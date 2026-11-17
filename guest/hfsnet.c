/*
 * hfsnet.c - HostFS 経由 inetd.x 互換ドライバ (ゲスト側)
 *
 * 計測技研 inetd.x 互換の TI エントリを実装する常駐プログラム。
 * 実処理はすべて共有フォルダ内のファイル (HFSREQ.BIN / HFSRES.BIN) 経由で
 * ホスト側の相方プログラム (hfsnethost.py) に依頼する。
 * ファイルプロトコルの仕様は ../README.md を参照。
 *
 * ビルドは elf2x68k。入口の置き場所は head.S を参照。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x68k/dos.h>
#include <x68k/iocs.h>

/* ------------------------------------------------------------------ */
/* TI 関数番号 (計測技研 tcpipdrv.h の _TI_ 列挙順に一致させる)        */
/* ------------------------------------------------------------------ */
enum {
    TI_get_version = 0,
    TI_add_arp_table, TI_del_arp_table, TI_search_arp_table,
    TI_get_arp_table_top, TI_arp_request,
    TI_get_iface_list, TI_get_new_iface, TI_link_new_iface,
    TI_rt_top, TI_rt_lookup, TI_rt_lookupb, TI_rt_drop, TI_rt_add,
    TI_dns_add, TI_dns_drop, TI_dns_get,
    TI_set_domain_name, TI_get_domain_name,
    TI_res_query, TI_res_search, TI_res_mkquery, TI_res_sendquery,
    TI_get_MIB,
    TI_socket, TI_bind, TI_listen, TI_accept, TI_connect,
    TI_read_s, TI_write_s, TI_recvfrom, TI_sendto, TI_close_s,
    TI_socklen, TI_getsockname, TI_getpeername, TI_sockkick,
    TI_shutdown, TI_usesock, TI_recvline, TI_sendline,
    TI_rrecvchar, TI_recvchar, TI_usflush, TI_seteol, TI_sockmode,
    TI_setflush, TI_psocket, TI_sockerr, TI_sockstate,
    TI_sock_top, TI_ntoa_sock,
    TI_gethostbyname, TI_gethostbyaddr, TI_getnetbyname, TI_getnetbyaddr,
    TI_getservbyname, TI_getservbyport, TI_getprotobyname,
    TI_getprotobynumber,
    TI_rip
};

/* ------------------------------------------------------------------ */
/* ファイルプロトコル (../README.md と一致させること)                  */
/* ------------------------------------------------------------------ */
#define OP_OPEN     1
#define OP_CLOSE    2
#define OP_CONNECT  3
#define OP_SEND     4
#define OP_RECV     5
#define OP_STATE    6
#define OP_RESOLVE  7
#define OP_BIND     8
#define OP_LISTEN   9
#define OP_ACCEPT   10
#define OP_SHUTDOWN 11

/* 状態コード */
#define ST_CLOSED       0
#define ST_LISTEN       1
#define ST_SYN_SENT     2
#define ST_SYN_RECEIVED 3
#define ST_ESTABLISHED  4
#define ST_CLOSE_WAIT   5
#define ST_FIN_WAIT     6
#define ST_TIME_WAIT    7
#define ST_CLOSING      8

static const char * const state_names[] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECEIVED", "ESTABLISHED",
    "CLOSE_WAIT", "FIN_WAIT", "TIME_WAIT", "CLOSING"
};
#define NSTATES (sizeof(state_names) / sizeof(state_names[0]))

#define NSOCK       32
#define SOCKBASE    128
#define MAXDATA     2048        /* 1回のファイル往復で運ぶ最大バイト数 */
#define RPC_TIMEOUT 1500        /* 応答待ちの上限 (1/100秒単位で15秒) */

/* ------------------------------------------------------------------ */
/* ソケット状態構造体                                                  */
/*                                                                     */
/* 配置は libsocket / libnetwork が前提とする usock と一致させる。     */
/* 特に noblock は libsocket が sock_top 経由で直接読み書きする。      */
/* ------------------------------------------------------------------ */
struct usock {
    int refcnt;
    char noblock;
    char type;                  /* 0 = 未使用, 1 = TCP, 2 = UDP */
    int rdysock;
    void *p;
    char *name;
    int namelen;
    char *peername;
    int peernamelen;
    char errcodes[4];
    void *obuf;
    void *ibuf;
    char eol[3];
    int flag;
    int flush;
    int event;
};

struct sockaddr_in_h {
    short sin_family;
    unsigned short sin_port;    /* ネットワークバイトオーダのまま */
    unsigned long sin_addr;     /* ネットワークバイトオーダのまま */
    char sin_zero[8];
};

struct hostent_h {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};

static struct usock usocks[NSOCK];
static unsigned char namebuf[NSOCK][16];    /* getsockname 用 */
static unsigned char peerbuf[NSOCK][16];    /* getpeername 用 */
static unsigned char sock_state[NSOCK];     /* 最後に STATE で得た状態 */

static char path_req[160];
static char path_res[160];

static unsigned long rpc_seq;
static unsigned char reqbuf[20 + MAXDATA];
static unsigned char resbuf[24 + MAXDATA];
static unsigned char databuf[MAXDATA];

static struct hostent_h hostentbuf;
static char host_namebuf[128];
static unsigned long host_addr;
static char *host_addr_list[2];
static char *host_aliases[1];

/* ------------------------------------------------------------------ */
/* バイトオーダ変換 (ファイル内はビッグエンディアンで統一)             */
/* ------------------------------------------------------------------ */
static void put16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static unsigned int get16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

static unsigned long get32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------------ */
/* 時刻 (IOCS ONTIME は 1/100 秒単位、日内で循環する)                  */
/* ------------------------------------------------------------------ */
static long now100(void)
{
    struct iocs_time t = _iocs_ontime();
    return t.sec;
}

static long elapsed100(long t0, long t1)
{
    long d = t1 - t0;
    if (d < 0)
        d += 8640000L;          /* 24時間 = 8640000 (1/100秒) */
    return d;
}

/* ------------------------------------------------------------------ */
/* ファイル往復 (要求を書いて、通し番号が一致する応答が来るまで待つ)   */
/* ------------------------------------------------------------------ */
struct resinfo {
    long result;
    unsigned short port;
    unsigned long len;
    unsigned long aux;
};

/* 戻り値: 0 = 応答あり, -1 = 伝送エラー/時間切れ
 * hdrlen はヘッダの長さ欄に入れる値 (SEND ではペイロード長、
 * RECV では受け取り上限)。payload がある場合は payloadlen 分を送る。
 * 応答ペイロードが要る場合は respayload に最大 MAXDATA バイト入る。 */
static int rpc(int op, int sock, unsigned short port, unsigned short arg,
               unsigned long addr, unsigned long hdrlen,
               const void *payload, unsigned long payloadlen,
               struct resinfo *res, void *respayload)
{
    int h, n, got;
    long t0;

    rpc_seq++;
    if (rpc_seq == 0)
        rpc_seq = 1;

    reqbuf[0] = 'H';
    reqbuf[1] = 'N';
    reqbuf[2] = (unsigned char)op;
    reqbuf[3] = (unsigned char)sock;
    put32(reqbuf + 4, rpc_seq);
    put16(reqbuf + 8, port);
    put16(reqbuf + 10, arg);
    put32(reqbuf + 12, addr);
    put32(reqbuf + 16, hdrlen);
    if (payloadlen)
        memcpy(reqbuf + 20, payload, payloadlen);

    h = _dos_create(path_req, 0x20);
    if (h < 0) {
        _dos_delete(path_req);
        h = _dos_create(path_req, 0x20);
        if (h < 0)
            return -1;
    }
    if (_dos_write(h, (const char *)reqbuf, 20 + (int)payloadlen) < 0) {
        _dos_close(h);
        return -1;
    }
    _dos_close(h);

    t0 = now100();
    for (;;) {
        h = _dos_open(path_res, 0);
        if (h >= 0) {
            got = 0;
            while (got < (int)sizeof(resbuf)) {
                n = _dos_read(h, (char *)resbuf + got,
                              (int)sizeof(resbuf) - got);
                if (n <= 0)
                    break;
                got += n;
            }
            _dos_close(h);
            if (got >= 24 && resbuf[0] == 'H' && resbuf[1] == 'N' &&
                get32(resbuf + 4) == rpc_seq) {
                res->result = (long)get32(resbuf + 8);
                res->port = get16(resbuf + 12);
                res->len = get32(resbuf + 16);
                res->aux = get32(resbuf + 20);
                if (respayload) {
                    if (res->len > MAXDATA ||
                        got < 24 + (int)res->len)
                        return -1;
                    if (res->len)
                        memcpy(respayload, resbuf + 24, res->len);
                }
                return 0;
            }
        }
        _dos_change_pr();
        if (elapsed100(t0, now100()) > RPC_TIMEOUT)
            return -1;
    }
}

/* ------------------------------------------------------------------ */
/* 内部ヘルパ                                                          */
/* ------------------------------------------------------------------ */
static int sock_index(long fd)
{
    int idx = (int)fd - SOCKBASE;
    if (idx < 0 || idx >= NSOCK || usocks[idx].type == 0)
        return -1;
    return idx;
}

static int alloc_sock(void)
{
    int i;
    for (i = 0; i < NSOCK; i++) {
        if (usocks[i].type == 0)
            return i;
    }
    return -1;
}

static void init_sock(int idx, int type)
{
    struct usock *u = &usocks[idx];
    memset(u, 0, sizeof(*u));
    memset(namebuf[idx], 0, 16);
    memset(peerbuf[idx], 0, 16);
    /* sin_family は 0 (= AF_INET, elf2x68k の規約) なので、
       上の memset で 0 埋めされたままでよい */
    u->refcnt = 1;
    u->type = (char)type;       /* 1 = TCP, 2 = UDP */
    u->name = (char *)namebuf[idx];
    u->namelen = 16;
    u->peername = (char *)peerbuf[idx];
    u->peernamelen = 16;
    sock_state[idx] = ST_CLOSED;
}

/* STATE を問い合わせて状態キャッシュを更新する。失敗したら -1 */
static int query_state(int idx, struct resinfo *res)
{
    if (rpc(OP_STATE, idx, 0, 0, 0, 0, NULL, 0, res, NULL) < 0)
        return -1;
    if (res->result >= 0 && res->result < (long)NSTATES)
        sock_state[idx] = (unsigned char)res->result;
    return 0;
}

/* ------------------------------------------------------------------ */
/* TI 関数の実装                                                       */
/* ------------------------------------------------------------------ */
static long ti_socket(long *arg)
{
    struct resinfo res;
    int idx;

    /* elf2x68k の <sys/socket.h> の規約では PF_INET = 0, SOCK_STREAM = 0,
       SOCK_DGRAM = 1 (BSD の 2, 1 ではない)。libsocket は引数を素通し
       するので、アプリからは (0, 0, 0) や (0, 1, 0) が届く */
    if (arg[0] != 0)            /* PF_INET のみ */
        return -1;
    if (arg[1] != 0 && arg[1] != 1)     /* STREAM / DGRAM のみ (RAW は不可) */
        return -1;
    idx = alloc_sock();
    if (idx < 0)
        return -1;
    /* ソケット種別は要求ヘッダの arg 欄でホストへ伝える */
    if (rpc(OP_OPEN, idx, 0, (unsigned short)arg[1], 0, 0, NULL, 0,
            &res, NULL) < 0 || res.result < 0)
        return -1;
    init_sock(idx, arg[1] == 1 ? 2 : 1);
    return SOCKBASE + idx;
}

static long ti_bind(long *arg)
{
    struct resinfo res;
    struct sockaddr_in_h *sin = (struct sockaddr_in_h *)arg[1];
    int idx = sock_index(arg[0]);
    if (idx < 0 || sin == NULL)
        return -1;
    if (rpc(OP_BIND, idx, sin->sin_port, 0, sin->sin_addr, 0,
            NULL, 0, &res, NULL) < 0 || res.result < 0)
        return -1;
    memcpy(namebuf[idx], sin, 16);
    return 0;
}

static long ti_listen(long *arg)
{
    struct resinfo res;
    int idx = sock_index(arg[0]);
    if (idx < 0)
        return -1;
    if (rpc(OP_LISTEN, idx, 0, (unsigned short)arg[1], 0, 0,
            NULL, 0, &res, NULL) < 0 || res.result < 0)
        return -1;
    sock_state[idx] = ST_LISTEN;
    return 0;
}

static long ti_accept(long *arg)
{
    struct resinfo res;
    int idx = sock_index(arg[0]);
    int nidx;
    if (idx < 0)
        return -1;
    nidx = alloc_sock();
    if (nidx < 0)
        return -1;
    for (;;) {
        if (rpc(OP_ACCEPT, idx, 0, (unsigned short)nidx, 0, 0,
                NULL, 0, &res, NULL) < 0)
            return -1;
        if (res.result == 0) {
            struct sockaddr_in_h *sin;
            init_sock(nidx, 1);
            sin = (struct sockaddr_in_h *)peerbuf[nidx];
            sin->sin_family = 0;        /* AF_INET (elf2x68k の規約) */
            sin->sin_port = res.port;
            sin->sin_addr = res.aux;
            sock_state[nidx] = ST_ESTABLISHED;
            if (arg[1] && arg[2]) {
                memcpy((void *)arg[1], peerbuf[nidx], 16);
                *(int *)arg[2] = 16;
            }
            return SOCKBASE + nidx;
        }
        if (res.result == -2) {         /* まだ接続が来ていない */
            if (usocks[idx].noblock)
                return -1;
            continue;                   /* ホスト側で 300ms 待ってから返る */
        }
        return -1;
    }
}

static long ti_connect(long *arg)
{
    struct resinfo res;
    struct sockaddr_in_h *sin = (struct sockaddr_in_h *)arg[1];
    int idx = sock_index(arg[0]);
    long t0;
    if (idx < 0 || sin == NULL)
        return -1;
    if (rpc(OP_CONNECT, idx, sin->sin_port, 0, sin->sin_addr, 0,
            NULL, 0, &res, NULL) < 0)
        return -1;
    if (res.result == 0) {
        memcpy(peerbuf[idx], sin, 16);
        sock_state[idx] = ST_ESTABLISHED;
        return 0;
    }
    if (res.result != 1)
        return -1;
    /* 接続中 */
    memcpy(peerbuf[idx], sin, 16);
    sock_state[idx] = ST_SYN_SENT;
    if (usocks[idx].noblock)
        return -1;              /* libsocket が状態を見て EINPROGRESS にする */
    t0 = now100();
    for (;;) {
        if (query_state(idx, &res) == 0) {
            if (sock_state[idx] == ST_ESTABLISHED)
                return 0;
            if (sock_state[idx] == ST_CLOSED)
                return -1;
        }
        if (elapsed100(t0, now100()) > 3000)    /* 30秒 */
            return -1;
        _dos_change_pr();
    }
}

/* RECV の中核。戻り値: >0 受信バイト数, 0 EOF, -1 今は読めない/エラー
 * info が非 NULL なら、最後に応答した RECV の resinfo をコピーする
 * (UDP の recvfrom が送信元アドレスを取り出すために使う) */
static long recv_core(int idx, char *buf, long len, struct resinfo *info)
{
    struct resinfo res;
    if (len > MAXDATA)
        len = MAXDATA;
    for (;;) {
        if (rpc(OP_RECV, idx, 0, 0, 0, (unsigned long)len,
                NULL, 0, &res, databuf) < 0)
            return -1;
        if (info)
            *info = res;
        if (res.result > 0) {
            memcpy(buf, databuf, res.result);
            return res.result;
        }
        if (res.result == 0)
            return 0;                   /* EOF */
        if (res.result == -2) {         /* まだデータが無い */
            if (usocks[idx].noblock)
                return -1;
            continue;                   /* ホスト側で 300ms 待ってから返る */
        }
        return 0;                       /* それ以外の失敗は EOF として扱う */
    }
}

static long ti_read_s(long *arg)
{
    int idx = sock_index(arg[0]);
    if (idx < 0 || arg[2] <= 0)
        return -1;
    return recv_core(idx, (char *)arg[1], arg[2], NULL);
}

static long ti_recvfrom(long *arg)
{
    struct resinfo res;
    long r;
    int idx = sock_index(arg[0]);
    if (idx < 0 || arg[2] <= 0)
        return -1;
    r = recv_core(idx, (char *)arg[1], arg[2], &res);
    if (r >= 0 && arg[4]) {
        if (usocks[idx].type == 2 && r > 0) {
            /* UDP: 送信元は RECV 応答の aux (IP) と port 欄に載っている */
            struct sockaddr_in_h sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = 0;         /* AF_INET (elf2x68k の規約) */
            sin.sin_port = res.port;
            sin.sin_addr = res.aux;
            memcpy((void *)arg[4], &sin, 16);
        } else {
            memcpy((void *)arg[4], peerbuf[idx], 16);
        }
        if (arg[5])
            *(int *)arg[5] = 16;
    }
    return r;
}

/* SEND の中核。戻り値: 送信バイト数, -1 エラー
 * daddr / dport は sendto 用の宛先 (ネットワークバイトオーダのまま)。
 * 0 のときは宛先なし (接続済みソケット前提) */
static long send_core(int idx, const char *buf, long len,
                      unsigned long daddr, unsigned short dport)
{
    struct resinfo res;
    long total = 0;
    while (total < len) {
        long chunk = len - total;
        if (chunk > MAXDATA)
            chunk = MAXDATA;
        if (rpc(OP_SEND, idx, dport, 0, daddr, (unsigned long)chunk,
                buf + total, (unsigned long)chunk, &res, NULL) < 0)
            return -1;
        if (res.result > 0) {
            total += res.result;
            continue;
        }
        if (res.result == -2) {         /* 今は送れない */
            if (usocks[idx].noblock)
                return total > 0 ? total : -1;
            continue;
        }
        return -1;
    }
    return total;
}

static long ti_write_s(long *arg)
{
    int idx = sock_index(arg[0]);
    if (idx < 0 || arg[2] < 0)
        return -1;
    if (arg[2] == 0)
        return 0;
    return send_core(idx, (const char *)arg[1], arg[2], 0, 0);
}

static long ti_sendto(long *arg)
{
    /* libsocket の sendto は arg[4] に宛先 sockaddr へのポインタを置く
       (recvfrom の arg[4] = 送信元と対称) */
    struct sockaddr_in_h *sin = (struct sockaddr_in_h *)arg[4];
    int idx = sock_index(arg[0]);
    if (idx < 0 || arg[2] < 0)
        return -1;
    if (arg[2] == 0)
        return 0;
    if (sin == NULL)            /* 宛先なしは接続済みの相手へ送る */
        return send_core(idx, (const char *)arg[1], arg[2], 0, 0);
    return send_core(idx, (const char *)arg[1], arg[2],
                     sin->sin_addr, sin->sin_port);
}

static long ti_close_s(long fd)         /* 引数は配列でなく fd そのもの */
{
    struct resinfo res;
    int idx = sock_index(fd);
    if (idx < 0)
        return -1;
    rpc(OP_CLOSE, idx, 0, 0, 0, 0, NULL, 0, &res, NULL);    /* 結果は不問 */
    usocks[idx].type = 0;
    usocks[idx].refcnt = 0;
    sock_state[idx] = ST_CLOSED;
    return 0;
}

static long ti_shutdown(long *arg)
{
    struct resinfo res;
    int idx = sock_index(arg[0]);
    if (idx < 0)
        return -1;
    /* how (0=RD, 1=WR, 2=RDWR) は listen の backlog と同じ arg 欄で渡す */
    if (rpc(OP_SHUTDOWN, idx, 0, (unsigned short)arg[1], 0, 0,
            NULL, 0, &res, NULL) < 0 || res.result < 0)
        return -1;
    return 0;
}

static long ti_socklen(long *arg)
{
    struct resinfo res;
    int idx = sock_index(arg[0]);
    if (idx < 0)
        return -1;
    if (query_state(idx, &res) < 0)
        return -1;
    if (arg[1] == 0)
        return (long)res.len;           /* 受信可能バイト数 */
    if (arg[1] == 1)
        return (long)res.aux;           /* 送信可能バイト数 */
    return -1;
}

static long ti_getsockname(long *arg, int peer)
{
    int idx = sock_index(arg[0]);
    if (idx < 0 || arg[1] == 0)
        return -1;
    memcpy((void *)arg[1], peer ? peerbuf[idx] : namebuf[idx], 16);
    if (arg[2])
        *(int *)arg[2] = 16;
    return 0;
}

static long ti_sockstate(long fd)       /* 引数は配列でなく fd そのもの */
{
    struct resinfo res;
    int idx = sock_index(fd);
    if (idx < 0)
        return 0;                       /* NULL: libsocket がエラーにする */
    query_state(idx, &res);             /* 失敗しても前回の状態を返す */
    return (long)state_names[sock_state[idx]];
}

static long ti_gethostbyname(const char *name)
{
    struct resinfo res;
    size_t n = strlen(name);
    if (n == 0 || n >= sizeof(host_namebuf))
        return 0;
    if (rpc(OP_RESOLVE, 0, 0, 0, 0, (unsigned long)(n + 1),
            name, (unsigned long)(n + 1), &res, NULL) < 0 ||
        res.result < 0)
        return 0;
    strcpy(host_namebuf, name);
    host_addr = res.aux;
    host_addr_list[0] = (char *)&host_addr;
    host_addr_list[1] = NULL;
    host_aliases[0] = NULL;
    hostentbuf.h_name = host_namebuf;
    hostentbuf.h_aliases = host_aliases;
    hostentbuf.h_addrtype = 0;          /* AF_INET (elf2x68k の規約) */
    hostentbuf.h_length = 4;
    hostentbuf.h_addr_list = host_addr_list;
    return (long)&hostentbuf;
}

/* ------------------------------------------------------------------ */
/* TI エントリ (head.S の jsr_entry から呼ばれる)                       */
/*   呼ばれ方: D0 = 関数番号, A0 = 引数 (配列へのポインタ。ただし       */
/*   close_s / sockstate は fd の値そのもの、gethostbyname は           */
/*   名前へのポインタ)。結果は D0 で返す。                             */
/* ------------------------------------------------------------------ */
int do_command(void)
{
    int cmd;
    long *arg;

    __asm__ volatile (
        "move.l %%d0,%0\n"
        "move.l %%a0,%1\n"
        : "=r" (cmd), "=r" (arg) : : "d0", "a0"
    );

    switch (cmd) {
    case -1:                    /* trap 番号の問い合わせには「無し」で答える */
        return -1;
    case TI_get_version:
        return 0x00010001;
    case TI_socket:
        return ti_socket(arg);
    case TI_bind:
        return ti_bind(arg);
    case TI_listen:
        return ti_listen(arg);
    case TI_accept:
        return ti_accept(arg);
    case TI_connect:
        return ti_connect(arg);
    case TI_read_s:
        return ti_read_s(arg);
    case TI_write_s:
        return ti_write_s(arg);
    case TI_recvfrom:
        return ti_recvfrom(arg);
    case TI_sendto:
        return ti_sendto(arg);
    case TI_close_s:
        return ti_close_s((long)arg);
    case TI_socklen:
        return ti_socklen(arg);
    case TI_getsockname:
        return ti_getsockname(arg, 0);
    case TI_getpeername:
        return ti_getsockname(arg, 1);
    case TI_shutdown:
        return ti_shutdown(arg);
    case TI_sockstate:
        return ti_sockstate((long)arg);
    case TI_sock_top:
        return (long)usocks;
    case TI_gethostbyname:
        return ti_gethostbyname((const char *)arg);
    default:
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* 常駐インストール                                                    */
/* ------------------------------------------------------------------ */
extern char tcpip_magic;                /* head.S: イメージ先頭の署名 */
extern int _HEND;                       /* crt0 が設定するヒープ終端 */

typedef long (*ti_func_t)(long, void *);
ti_func_t __sock_search_ti_entry(void); /* libsocket の既存ドライバ検索 */

int main(int argc, char *argv[])
{
    const char *dir;
    char probe[192];
    size_t dl;
    int h;

    if (argc < 2) {
        printf("usage: hfsnet.x <folder>\r\n");
        printf("  e.g. hfsnet.x C:\\HFSNET\r\n");
        return 1;
    }
    if (__sock_search_ti_entry() != NULL) {
        printf("hfsnet: TCP/IP driver is already installed.\r\n");
        return 1;
    }

    dir = argv[1];
    dl = strlen(dir);
    if (dl == 0 || dl > 100) {
        printf("hfsnet: folder path too long.\r\n");
        return 1;
    }
    snprintf(path_req, sizeof(path_req), "%s%sHFSREQ.BIN",
             dir, (dir[dl - 1] == '\\' || dir[dl - 1] == '/') ? "" : "\\");
    snprintf(path_res, sizeof(path_res), "%s%sHFSRES.BIN",
             dir, (dir[dl - 1] == '\\' || dir[dl - 1] == '/') ? "" : "\\");

    /* フォルダへ書けるか試してから常駐する */
    snprintf(probe, sizeof(probe), "%s%sHFSNPROB.BIN",
             dir, (dir[dl - 1] == '\\' || dir[dl - 1] == '/') ? "" : "\\");
    h = _dos_create(probe, 0x20);
    if (h < 0) {
        printf("hfsnet: cannot write files in %s\r\n", dir);
        return 1;
    }
    _dos_close(h);
    _dos_delete(probe);

    printf("hfsnet: installed (folder = %s)\r\n", dir);
    fflush(stdout);

    _dos_keeppr(_HEND - (int)&tcpip_magic, 0);
    return 0;
}
