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

/* リリースバージョン (組み込み時メッセージで表示する。
   TI_get_version が返す 0x00010001 は inetd.x インターフェースの
   版数なので、こことは別に固定のままにすること) */
#define HFSNET_VERSION "0.6.0"

/* 常駐コピーの識別マーカー。常駐解除のときに、"TCP/IP" 署名を持つ
   常駐ブロックの中から、この文字列を含むものを自ドライバとみなす。
   ブロック内をスキャンして探すので配置アドレスには依存せず、
   版の違う常駐コピーでも識別できる */
static const char resident_marker[] = "hfsnet-resident-driver";

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
#define OP_HOSTINFO 12

/* sockmode のモード値 (FCT socket.h) */
#define SOCK_BINARY     0       /* 生データのまま */
#define SOCK_ASCII      1       /* EOL 変換あり */
#define SOCK_QUERY      2       /* 変更せず現在値を返す */

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
    char type;                  /* 0 = 未使用, 1 = TCP, 2 = UDP, 3 = RAW */
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

/* TI_get_MIB が返す MIB 一覧。実データは持たないが、ping.x など
   戻り値をポインタとして参照するアプリが落ちないよう、joynetd と
   同じ形のダミー表 (IP / ICMP / UDP / TCP の4グループ) を返す */
struct mib_entry;
struct mib_array {
    struct mib_entry *mib;
    int mib_size;
    char *name;
};
static struct mib_array mib_array[4] = {
    { NULL, 0, "IP" },
    { NULL, 0, "ICMP" },
    { NULL, 0, "UDP" },
    { NULL, 0, "TCP" },
};

/* ------------------------------------------------------------------ */
/* 管理系 TI (iface / route / dns) が返す構造体群。レイアウトは       */
/* 計測技研 network.h の定義と一致させる (オフセットは実測で確認し、  */
/* 静的アサートで固定する)。アプリは戻り値のポインタをそのまま        */
/* 参照し、netstat は iface の update 関数ポインタまで呼ぶ            */
/* ------------------------------------------------------------------ */
struct iface_h {
    struct iface_h *next;
    char *name;
    int (*config)(struct iface_h *);
    int (*stop)(struct iface_h *);
    int (*update)(struct iface_h *);
    int (*send)(struct iface_h *, void *, long, int, int, int, int);
    int (*output)(struct iface_h *, char *, char *, long, void *);
    void (*input)(struct iface_h *, void *);
    int (*fprint)(void *, char *);
    int (*sprint)(char *, char *);
    int (*fscan)(void *, char *);
    int (*sscan)(char *, char *);
    int mtu;
    long my_ip_addr;
    long net_mask;
    long broad_cast;
    short arp_type;
    short arp_ip_type;
    short arp_hw_type;
    int hw_addr_len;
    char my_hw_addr[16];
    char my_hw_broad_addr[16];
    struct iface_h *forw;
    int flag;
    long ipsndcnt;
    long rawsndcnt;
    long snderrcnt;
    long iprcvcnt;
    long rawrcvcnt;
    long rcverrcnt;
    long collcnt;
    long lastsent;
    long data;
};
_Static_assert(__builtin_offsetof(struct iface_h, my_ip_addr) == 52,
               "iface layout");
_Static_assert(__builtin_offsetof(struct iface_h, flag) == 110,
               "iface layout");
_Static_assert(sizeof(struct iface_h) == 150, "iface layout");

struct route_h {
    struct route_h *prev;
    struct route_h *next;
    long target;
    unsigned int bits;
    long gateway;
    long metric;
    struct iface_h *iface;
    int flags;
    int ttl;
    int _ttl;
    long uses;
};
_Static_assert(sizeof(struct route_h) == 44, "route layout");

struct dns_h {
    struct dns_h *prev;
    struct dns_h *next;
    long address;
};

#define IFACE_UP        1
#define IFACE_RUNNING   2
#define IFACE_NOARP     4
#define IFACE_BROAD     16
#define IFACE_LOOPBACK  32

static char iface_name_lp0[] = "lp0";
static char iface_name_hf0[] = "hf0";
static struct iface_h if_lp0, if_hf0;
static struct route_h rt_default, rt_subnet, rt_loop;
static struct route_h *route_table[32][5];
static int subnet_bits = -1;    /* rt_subnet のバケツ番号 (-1 = 未登録) */
static struct dns_h dns_nodes[4];
static int dns_count;
static char domain_name[64];
/* ARP 表のバケツ列 (17本)。get_arp_table_top は先頭ポインタではなく
   この配列へのポインタを返すのが ABI の約束 (libnetwork の
   get_arp_array が _top[x] と添字参照する)。表自体は持たないので
   中身は常に NULL */
static void *arp_buckets[17];
static int rip_flag;            /* inetdconf が読み書きする RIP の台帳 */
static unsigned char namebuf[NSOCK][16];    /* getsockname 用 */
static unsigned char peerbuf[NSOCK][16];    /* getpeername 用 */
static unsigned char sock_state[NSOCK];     /* 最後に STATE で得た状態 */

/* 拡張 TI 関数 (37〜52) 用のスロットごとの状態 */
static const char *sock_errstr[NSOCK];      /* sockerr が返す直近のエラー文字列 */
/* 文字/行読み (recvchar / recvline) と read_s / recvfrom で共用する
   入力バッファ。1回の RECV 往復でまとめて取り寄せ、残りは次回に回す */
static unsigned char cbuf[NSOCK][MAXDATA];
static int cpos[NSOCK];                     /* 読み出し位置 */
static int clen[NSOCK];                     /* 有効バイト数 */
static int ceof[NSOCK];                     /* EOF を観測済みか */
static unsigned short cpeer_port[NSOCK];    /* バッファ内データの送信元 (UDP) */
static unsigned long cpeer_addr[NSOCK];

static char path_req[160];
static char path_res[160];

static unsigned long rpc_seq;
static long rpc_timeout = RPC_TIMEOUT;  /* 応答待ちの上限 (通常は RPC_TIMEOUT) */
static unsigned char reqbuf[20 + MAXDATA];
static unsigned char resbuf[24 + MAXDATA];

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
        if (elapsed100(t0, now100()) > rpc_timeout)
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
    u->type = (char)type;       /* 1 = TCP, 2 = UDP, 3 = RAW */
    u->name = (char *)namebuf[idx];
    u->namelen = 16;
    u->peername = (char *)peerbuf[idx];
    u->peernamelen = 16;
    /* EOL の既定は CRLF (inetd.x / joynetd と同じ)。recvchar が使う */
    u->eol[0] = '\r';
    u->eol[1] = '\n';
    u->eol[2] = '\0';
    sock_state[idx] = ST_CLOSED;
    sock_errstr[idx] = "";
    cpos[idx] = clen[idx] = 0;
    ceof[idx] = 0;
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
    if (arg[1] != 0 && arg[1] != 1 && arg[1] != 2)
        return -1;              /* STREAM / DGRAM / RAW のみ */
    idx = alloc_sock();
    if (idx < 0)
        return -1;
    /* ソケット種別は要求ヘッダの arg 欄、プロトコル番号 (RAW のとき
       IPPROTO_ICMP = 1 など) は port 欄でホストへ伝える */
    if (rpc(OP_OPEN, idx, (unsigned short)arg[2], (unsigned short)arg[1],
            0, 0, NULL, 0, &res, NULL) < 0 || res.result < 0)
        return -1;
    init_sock(idx, arg[1] == 1 ? 2 : arg[1] == 2 ? 3 : 1);
    return SOCKBASE + idx;
}

/* CONNECT の応答に含まれるローカル側エンドポイントを namebuf へ
   反映する (getsockname が実際のアドレス/ポートを返すため。
   FTP クライアントが PORT の組み立てに使う) */
static void set_namebuf_local(int idx, const struct resinfo *res)
{
    struct sockaddr_in_h *me = (struct sockaddr_in_h *)namebuf[idx];
    if (res->aux == 0 && res->port == 0)
        return;
    me->sin_family = 0;                 /* AF_INET (elf2x68k の規約) */
    me->sin_port = res->port;
    me->sin_addr = res->aux;
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
    if (res.port)       /* port=0 で割り当てられた実際のポートを反映 */
        ((struct sockaddr_in_h *)namebuf[idx])->sin_port = res.port;
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
            /* EOL とモードは聞き側ソケットから引き継ぐ (joynetd と同じ) */
            memcpy(usocks[nidx].eol, usocks[idx].eol, 3);
            usocks[nidx].flag = usocks[idx].flag;
            usocks[nidx].flush = usocks[idx].flush;
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
            NULL, 0, &res, NULL) < 0) {
        sock_errstr[idx] = "Connection refused";
        return -1;
    }
    if (res.result == 0) {
        memcpy(peerbuf[idx], sin, 16);
        set_namebuf_local(idx, &res);
        sock_state[idx] = ST_ESTABLISHED;
        sock_errstr[idx] = "";
        return 0;
    }
    if (res.result != 1) {
        sock_errstr[idx] = "Connection refused";
        return -1;
    }
    /* 接続中 */
    memcpy(peerbuf[idx], sin, 16);
    set_namebuf_local(idx, &res);
    sock_state[idx] = ST_SYN_SENT;
    if (usocks[idx].noblock)
        return -1;              /* libsocket が状態を見て EINPROGRESS にする */
    t0 = now100();
    for (;;) {
        if (query_state(idx, &res) == 0) {
            if (sock_state[idx] == ST_ESTABLISHED) {
                sock_errstr[idx] = "";
                return 0;
            }
            if (sock_state[idx] == ST_CLOSED) {
                sock_errstr[idx] = "Connection refused";
                return -1;
            }
        }
        if (elapsed100(t0, now100()) > 3000) {  /* 30秒 */
            sock_errstr[idx] = "Connection timed out";
            return -1;
        }
        _dos_change_pr();
    }
}

/* 入力バッファへの取り寄せ。戻り値:
 *   1 = cbuf にデータがある, 0 = EOF (ホスト側の失敗も従来方針で EOF 扱い),
 *  -1 = まだデータが無い (ホストが -2 を返した), -2 = 伝送エラー */
static int cfill(int idx)
{
    struct resinfo res;
    if (cpos[idx] < clen[idx])
        return 1;
    if (ceof[idx])
        return 0;
    if (rpc(OP_RECV, idx, 0, 0, 0, MAXDATA, NULL, 0, &res, cbuf[idx]) < 0)
        return -2;
    if (res.result > 0) {
        cpos[idx] = 0;
        clen[idx] = (int)res.result;
        cpeer_port[idx] = res.port;
        cpeer_addr[idx] = res.aux;
        return 1;
    }
    if (res.result == -2)
        return -1;
    ceof[idx] = 1;                      /* 0 = EOF、その他の負値も EOF 扱い */
    return 0;
}

/* RECV の中核。戻り値: >0 受信バイト数, 0 EOF, -1 今は読めない/エラー
 * info が非 NULL なら、取り寄せたデータの送信元 (port / aux) を返す
 * (UDP の recvfrom が送信元アドレスを取り出すために使う) */
static long recv_core(int idx, char *buf, long len, struct resinfo *info)
{
    for (;;) {
        int st = cfill(idx);
        if (st == 1) {
            int avail = clen[idx] - cpos[idx];
            long n = len;
            if (n > avail)
                n = avail;
            memcpy(buf, cbuf[idx] + cpos[idx], (size_t)n);
            cpos[idx] += (int)n;
            if (info) {
                info->result = n;
                info->port = cpeer_port[idx];
                info->aux = cpeer_addr[idx];
            }
            return n;
        }
        if (st == 0)
            return 0;                   /* EOF */
        if (st == -2)
            return -1;                  /* 伝送エラー */
        /* st == -1: まだデータが無い */
        if (usocks[idx].noblock)
            return -1;
        /* ブロッキング時は再試行 (ホスト側で 300ms 待ってから返る) */
    }
}

/* 1バイト受信 (rrecvchar / recvchar の中核)。戻り値: 0〜255, -1 = 読めない */
static int recv_one(int idx)
{
    for (;;) {
        int st = cfill(idx);
        if (st == 1)
            return cbuf[idx][cpos[idx]++];
        if (st == 0 || st == -2)
            return -1;
        if (usocks[idx].noblock)
            return -1;
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
        if ((usocks[idx].type == 2 || usocks[idx].type == 3) && r > 0) {
            /* UDP / RAW: 送信元は RECV 応答の aux (IP) と port 欄に載っている */
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
    /* FCT の規約: close_s は参照カウントを下げ、0 になったときだけ
       実際に閉じる (libnetwork マニュアル)。usesock と対になる */
    if (usocks[idx].refcnt > 1) {
        usocks[idx].refcnt--;
        return 0;
    }
    rpc(OP_CLOSE, idx, 0, 0, 0, 0, NULL, 0, &res, NULL);    /* 結果は不問 */
    usocks[idx].type = 0;
    usocks[idx].refcnt = 0;
    sock_state[idx] = ST_CLOSED;
    cpos[idx] = clen[idx] = 0;
    ceof[idx] = 0;
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
    if (arg[1] == 0) {
        /* TCP で相手が切断し受信データも残っていない場合は -1 を返す。
           socklen の正の値だけを見て受信を判断するアプリ (FCT telnet は
           切断後も socklen を回し続ける) が切断を検出できるようにする
           ための意味づけで、本家ドライバの挙動からの推定である。 */
        if (usocks[idx].type == 1 && res.len == 0 &&
            (sock_state[idx] == ST_CLOSE_WAIT ||
             sock_state[idx] == ST_CLOSED))
            return -1;
        return (long)res.len;           /* 受信可能バイト数 */
    }
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

/* ------------------------------------------------------------------ */
/* 拡張 TI 関数 (37〜52 の未実装分)                                     */
/*                                                                     */
/* 引数の渡し方は FCT libnetwork のソース (TCPPACKB) で確定した:       */
/*   配列渡し: recvline / sendline / seteol / sockmode / setflush /    */
/*             ntoa_sock (arg[0] = fd, 以降は関数ごと)                 */
/*   fd を A0 に直接: sockkick / usesock / rrecvchar / recvchar /      */
/*             usflush / sockerr (close_s / sockstate と同じ流儀)      */
/*   ポインタを A0 に直接: psocket (sockaddr へのポインタ)             */
/* 意味は libnetwork マニュアルと joynetd の実装に従った。             */
/* ------------------------------------------------------------------ */

/* sockkick (37): stream の強制再送が本来の意味だが、再送はホスト側の
   TCP スタックの仕事なので、ここでは成功を返すだけ (joynetd も同じ) */
static long ti_sockkick(long fd)
{
    if (sock_index(fd) < 0)
        return -1;
    return 0;
}

/* usesock (39): 参照カウントを1つ上げる */
static long ti_usesock(long fd)
{
    int idx = sock_index(fd);
    if (idx < 0)
        return -1;
    usocks[idx].refcnt++;
    return 0;
}

/* rrecvchar (42): EOL 変換なしの1バイト受信。引数は fd そのもの */
static long ti_rrecvchar(long fd)
{
    int idx = sock_index(fd);
    if (idx < 0)
        return -1;
    return recv_one(idx);
}

/* recvchar (43): 1バイト受信。ASCII モードでは EOL シーケンスを
   '\n' に変換する (joynetd の変換規則どおり)。引数は fd そのもの */
static long ti_recvchar(long fd)
{
    int idx = sock_index(fd);
    int c;
    if (idx < 0)
        return -1;
    if (usocks[idx].flag == SOCK_BINARY)
        return recv_one(idx);
    c = recv_one(idx);
    if (c < 0)
        return -1;
    if (c != (unsigned char)usocks[idx].eol[0])
        return c;
    if (usocks[idx].eol[1] == '\0')
        return '\n';
    c = recv_one(idx);
    if (c < 0)
        return -1;
    if (c != (unsigned char)usocks[idx].eol[1])
        return c;               /* EOL の1文字目は捨て、2文字目を返す */
    return '\n';
}

/* recvline (40): '\n' まで (含む) または len バイトまで読む。
   終端の NUL は付けない。戻り値は読んだバイト数 */
static long ti_recvline(long *arg)
{
    char *p = (char *)arg[1];
    long len = arg[2], n = 0;
    if (sock_index(arg[0]) < 0 || p == NULL || len <= 0)
        return -1;
    while (n < len) {
        long c = ti_recvchar(arg[0]);
        if (c < 0)
            return -1;
        p[n++] = (char)c;
        if (c == '\n')
            break;
    }
    return n;
}

/* sendline (41): ASCII モードでは '\n' を EOL シーケンスに展開して送る。
   戻り値は元のバッファのバイト数。FCT libnetwork にラッパは無いため
   意味は KA9Q 由来の流儀からの推定 */
static long ti_sendline(long *arg)
{
    int idx = sock_index(arg[0]);
    const char *buf = (const char *)arg[1];
    long len = arg[2], i, start;
    if (idx < 0 || buf == NULL || len < 0)
        return -1;
    if (usocks[idx].flag == SOCK_BINARY || len == 0)
        return send_core(idx, buf, len, 0, 0);
    start = 0;
    for (i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            char eolbuf[2];
            int en = 0;
            if (i > start &&
                send_core(idx, buf + start, i - start, 0, 0) < 0)
                return -1;
            if (usocks[idx].eol[0])
                eolbuf[en++] = usocks[idx].eol[0];
            if (usocks[idx].eol[1])
                eolbuf[en++] = usocks[idx].eol[1];
            if (en == 0)
                eolbuf[en++] = '\n';
            if (send_core(idx, eolbuf, en, 0, 0) < 0)
                return -1;
            start = i + 1;
        }
    }
    if (len > start && send_core(idx, buf + start, len - start, 0, 0) < 0)
        return -1;
    return len;
}

/* usflush (44): 送信バッファのフラッシュ。hfsnet の送信は即時なので
   成功を返すだけ。引数は fd そのもの、戻り値は使われない */
static long ti_usflush(long fd)
{
    if (sock_index(fd) < 0)
        return -1;
    return 0;
}

/* seteol (45): EOL シーケンス (最大2文字) を設定する */
static long ti_seteol(long *arg)
{
    int idx = sock_index(arg[0]);
    const char *seq = (const char *)arg[1];
    if (idx < 0 || seq == NULL)
        return -1;
    usocks[idx].eol[0] = seq[0];
    usocks[idx].eol[1] = seq[0] ? seq[1] : '\0';
    usocks[idx].eol[2] = '\0';
    return 0;
}

/* sockmode (46): モードを設定し、以前のモードを返す。
   SOCK_QUERY では変更しない */
static long ti_sockmode(long *arg)
{
    int idx = sock_index(arg[0]);
    int old;
    if (idx < 0)
        return -1;
    old = usocks[idx].flag;
    if (arg[1] == SOCK_BINARY || arg[1] == SOCK_ASCII)
        usocks[idx].flag = (int)arg[1];
    return old;
}

/* setflush (47): フラッシュ文字の設定。hfsnet は出力バッファを持たない
   ので、値を保持するだけ */
static long ti_setflush(long *arg)
{
    int idx = sock_index(arg[0]);
    if (idx < 0)
        return -1;
    usocks[idx].flush = (int)arg[1];
    return 0;
}

/* sockaddr を "a.b.c.d:port" に整形する (psocket / ntoa_sock 共用)。
   sin_addr / sin_port はネットワークバイトオーダのまま保持されており、
   ビッグエンディアンの 68k では値がそのまま各要素になる */
static void fmt_addr(char *out, const void *sa)
{
    const struct sockaddr_in_h *sin = (const struct sockaddr_in_h *)sa;
    unsigned long a = sin->sin_addr;
    sprintf(out, "%lu.%lu.%lu.%lu:%u",
            (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff,
            sin->sin_port);
}

static char psockbuf[64];

/* psocket (48): sockaddr へのポインタを A0 で受け取り、整形した文字列
   へのポインタを返す (KA9Q 由来の意味づけ) */
static long ti_psocket(const void *p)
{
    if (p == NULL)
        return (long)"";
    fmt_addr(psockbuf, p);
    return (long)psockbuf;
}

/* sockerr (49): そのソケットで最後に起きたエラーを表す文字列を返す。
   引数は fd そのもの。文字列の文言は近似 (接続失敗は "Connection
   refused"、時間切れは "Connection timed out") */
static long ti_sockerr(long fd)
{
    int idx = sock_index(fd);
    if (idx < 0)
        return 0;                       /* NULL (joynetd と同じ) */
    return (long)sock_errstr[idx];
}

/* ntoa_sock (52): ソケットの状態を netstat 風の1行に整形して
   呼び出し側のバッファに書き、バッファへのポインタを返す。
   mode (名前解決の抑止) は無視し、常に数値で整形する */
static long ti_ntoa_sock(long *arg)
{
    struct resinfo res;
    char local[64], foreign[64];
    char *buf = (char *)arg[2];
    long rq = 0, sq = 0;
    int idx = sock_index(arg[0]);
    if (idx < 0 || buf == NULL)
        return 0;
    if (arg[1] != 0)                    /* AF_INET のみ */
        return 0;
    if (query_state(idx, &res) == 0) {
        rq = (long)res.len;
        sq = (long)res.aux;
    }
    fmt_addr(local, namebuf[idx]);
    fmt_addr(foreign, peerbuf[idx]);
    sprintf(buf, "%-5s %6ld %6ld %-22s %-22s %s",
            usocks[idx].type == 2 ? "udp" : "tcp", rq, sq, local, foreign,
            usocks[idx].type == 2 ? "" : state_names[sock_state[idx]]);
    return (long)buf;
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
/* 管理系 TI の実装                                                    */
/*                                                                     */
/* hfsnet のゲスト側に実インターフェースや経路表は存在しない (実通信   */
/* はホスト側のスタックが行う)。ここで返すのは、ifconfig / netstat /   */
/* inetdconf が表示に必要とする範囲の台帳である:                       */
/*   - lp0: ループバック (127.0.0.1)                                   */
/*   - hf0: ホストの IP アドレスを自分のアドレスとして見せる仮想口。   */
/*     アドレスは常駐時の OP_HOSTINFO 応答で得たもの。ネットマスクは   */
/*     ホストから取得できないためクラス別デフォルトを使う              */
/*   - 経路: デフォルト (rt_top の *def で返す) と、hf0 のサブネット、 */
/*     127.0.0.0/8 の3件だけを持つ                                     */
/*   - DNS サーバ一覧とドメイン名: inetdconf が読み書きする台帳として  */
/*     ゲスト内に保持する (名前解決自体はホスト側が行う)               */
/* ------------------------------------------------------------------ */
static int iface_update_noop(struct iface_h *p)
{
    (void)p;
    return 0;
}

/* ifconfig の down が呼ぶ停止処理。台帳なのでフラグを落とすだけ */
static int iface_stop(struct iface_h *p)
{
    p->flag &= ~(IFACE_UP | IFACE_RUNNING);
    return 0;
}

/* 無条件に呼ばれる側のポインタは空関数で埋める。ただし
   fprint / sprint / fscan / sscan は「その形式のハードウェア
   アドレスを扱えるか」を示す能力ポインタで、呼び出し側は
   NULL かどうかを確認してから使う (arp.x の -s が sscan を
   確認する)。hfsnet はハードウェアアドレスを持たないので、
   この4つは NULL のままにして「扱えない」と答えるのが正しい */
static int iface_send_noop(struct iface_h *p, void *a, long b,
                           int c, int d, int e, int f)
{
    (void)p; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return 0;
}
static int iface_output_noop(struct iface_h *p, char *a, char *b,
                             long c, void *d)
{
    (void)p; (void)a; (void)b; (void)c; (void)d;
    return 0;
}
static void iface_input_noop(struct iface_h *p, void *a)
{
    (void)p; (void)a;
}

static void iface_fill_fptrs(struct iface_h *p)
{
    p->config = iface_update_noop;
    p->stop = iface_stop;
    p->update = iface_update_noop;
    p->send = iface_send_noop;
    p->output = iface_output_noop;
    p->input = iface_input_noop;
    /* fprint / sprint / fscan / sscan は NULL のまま */
}

/* ホスト IP を hf0 とサブネット経路へ反映する */
static void apply_host_ip(unsigned long ip)
{
    unsigned long mask;
    int bits;

    if (ip == 0)
        return;
    if ((ip >> 24) < 128)           { mask = 0xff000000UL; bits = 8; }
    else if ((ip >> 24) < 192)      { mask = 0xffff0000UL; bits = 16; }
    else                            { mask = 0xffffff00UL; bits = 24; }
    if_hf0.my_ip_addr = (long)ip;
    if_hf0.net_mask = (long)mask;
    if_hf0.broad_cast = (long)(ip | ~mask);
    if (subnet_bits >= 0)
        route_table[subnet_bits][0] = NULL;
    rt_subnet.target = (long)(ip & mask);
    rt_subnet.bits = (unsigned int)bits;
    route_table[bits][0] = &rt_subnet;
    subnet_bits = bits;
}

/* インターフェースと経路表の初期化 (常駐時に main から呼ぶ) */
static void mgmt_init(unsigned long host_ip)
{
    memset(&if_lp0, 0, sizeof(if_lp0));
    if_lp0.next = &if_hf0;
    if_lp0.name = iface_name_lp0;
    iface_fill_fptrs(&if_lp0);
    if_lp0.mtu = 16436;
    if_lp0.my_ip_addr = 0x7f000001L;
    if_lp0.net_mask = (long)0xff000000UL;
    if_lp0.broad_cast = 0x7fffffffL;
    if_lp0.flag = IFACE_UP | IFACE_RUNNING | IFACE_LOOPBACK;

    memset(&if_hf0, 0, sizeof(if_hf0));
    if_hf0.name = iface_name_hf0;
    iface_fill_fptrs(&if_hf0);
    if_hf0.mtu = 1500;
    if_hf0.flag = IFACE_UP | IFACE_RUNNING | IFACE_NOARP | IFACE_BROAD;

    memset(&rt_default, 0, sizeof(rt_default));
    rt_default.iface = &if_hf0;
    memset(&rt_subnet, 0, sizeof(rt_subnet));
    rt_subnet.iface = &if_hf0;
    memset(&rt_loop, 0, sizeof(rt_loop));
    rt_loop.target = 0x7f000000L;
    rt_loop.bits = 8;
    rt_loop.iface = &if_lp0;
    route_table[8][0] = &rt_loop;
    if (host_ip != 0)
        apply_host_ip(host_ip);
}

/* ホスト IP が未判明なら、短い待ち時間で1回だけ問い合わせる。
   常駐時にホストが起動していなかった場合の後追い用 */
static void refresh_host_iface(void)
{
    struct resinfo res;
    long saved_timeout;
    int r;

    if (if_hf0.my_ip_addr != 0)
        return;
    saved_timeout = rpc_timeout;
    rpc_timeout = 200;              /* 2秒だけ待つ */
    r = rpc(OP_HOSTINFO, 0, 0, 0, 0, 0, NULL, 0, &res, NULL);
    rpc_timeout = saved_timeout;
    if (r == 0 && res.result == 0 && res.aux != 0)
        apply_host_ip(res.aux);
}

static struct iface_h *iface_by_name(const char *name)
{
    if (strcmp(name, iface_name_lp0) == 0)
        return &if_lp0;
    if (strcmp(name, iface_name_hf0) == 0)
        return &if_hf0;
    return NULL;
}

static struct route_h *route_lookup_h(long ip)
{
    if (((unsigned long)ip & 0xff000000UL) == 0x7f000000UL)
        return &rt_loop;
    if (if_hf0.my_ip_addr != 0 &&
        ((unsigned long)ip & (unsigned long)if_hf0.net_mask) ==
        (unsigned long)rt_subnet.target)
        return &rt_subnet;
    return &rt_default;
}

static struct route_h *route_lookupb_h(long target, unsigned int bits)
{
    if (rt_loop.target == target && rt_loop.bits == bits)
        return &rt_loop;
    if (subnet_bits >= 0 && rt_subnet.target == target &&
        rt_subnet.bits == bits)
        return &rt_subnet;
    if (target == 0 && bits == 0)
        return &rt_default;
    return NULL;
}

static void dns_relink(void)
{
    int j;
    for (j = 0; j < dns_count; j++) {
        dns_nodes[j].prev = j ? &dns_nodes[j - 1] : NULL;
        dns_nodes[j].next = (j + 1 < dns_count) ? &dns_nodes[j + 1] : NULL;
    }
    if (dns_count == 0)
        dns_nodes[0].prev = dns_nodes[0].next = NULL;
}

static int dns_add_h(long addr)
{
    int i;
    for (i = 0; i < dns_count; i++)
        if (dns_nodes[i].address == addr)
            return 0;
    if (dns_count >= 4)
        return -1;
    dns_nodes[dns_count++].address = addr;
    dns_relink();
    return 0;
}

static int dns_drop_h(long addr)
{
    int i, j;
    for (i = 0; i < dns_count; i++) {
        if (dns_nodes[i].address != addr)
            continue;
        for (j = i; j + 1 < dns_count; j++)
            dns_nodes[j].address = dns_nodes[j + 1].address;
        dns_count--;
        dns_relink();
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* TI エントリ (head.S の jsr_entry から呼ばれる)                       */
/*   呼ばれ方: D0 = 関数番号, A0 = 引数 (配列へのポインタ。ただし       */
/*   close_s / sockstate / sockkick / usesock / rrecvchar / recvchar / */
/*   usflush / sockerr は fd の値そのもの、psocket は sockaddr への     */
/*   ポインタ、gethostbyname は名前へのポインタ)。結果は D0 で返す。   */
/* ------------------------------------------------------------------ */
#ifdef TRACE
/* 切り分け用: 全 TI 呼び出しを共有フォルダの HFSTRACE.BIN に
   「関数番号 A0の値」の行で追記する (通常ビルドには含めない) */
static void trace_call(int cmd, long a0)
{
    char path[sizeof(path_res)];
    char line[128];
    int h, n;
    strcpy(path, path_res);
    n = (int)strlen(path);
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/')
        n--;
    strcpy(path + n, "HFSTRACE.BIN");
    h = _dos_open(path, 1);
    if (h < 0)
        h = _dos_create(path, 0x20);
    if (h < 0)
        return;
    _dos_seek(h, 0, 2);
    if (cmd == TI_socket || cmd == TI_bind || cmd == TI_connect ||
        cmd == TI_sendto || cmd == TI_recvfrom || cmd == TI_read_s ||
        cmd == TI_write_s) {
        /* ポインタ引数の関数だけは中身の先頭も記録する (調査用) */
        long *a = (long *)a0;
        n = snprintf(line, sizeof(line),
                     "%d %ld %ld %ld %ld %ld %ld\r\n", cmd, a0,
                     a[0], a[1], a[2], a[3], a[4]);
    } else {
        n = snprintf(line, sizeof(line), "%d %ld\r\n", cmd, a0);
    }
    _dos_write(h, line, n);
    _dos_close(h);
}
#endif

int do_command(void)
{
    int cmd;
    long *arg;

    __asm__ volatile (
        "move.l %%d0,%0\n"
        "move.l %%a0,%1\n"
        : "=r" (cmd), "=r" (arg) : : "d0", "a0"
    );

#ifdef TRACE
    trace_call(cmd, (long)arg);
#endif

    switch (cmd) {
    case -1:                    /* trap 番号の問い合わせには「無し」で答える */
        return -1;
    case TI_get_version:
        return 0x00010001;
    case TI_get_MIB:
        return (long)mib_array;
    case TI_add_arp_table:
    case TI_del_arp_table:
        return 0;               /* ARP 表は持たない (受け付けて何もしない) */
    case TI_search_arp_table:
        return 0;               /* 該当なし (NULL) */
    case TI_get_arp_table_top:
        return (long)arp_buckets;   /* バケツ列へのポインタ (中身は空) */
    case TI_arp_request:
        return -1;
    case TI_get_iface_list:
        refresh_host_iface();
        return (long)&if_lp0;
    case TI_get_new_iface:
        refresh_host_iface();
        return (long)iface_by_name((const char *)arg);
    case TI_link_new_iface:
        return 0;               /* 実ノードを返しているので確定不要 */
    case TI_rt_top:
        if (arg != NULL)
            *(struct route_h **)arg = &rt_default;
        return (long)route_table;
    case TI_rt_lookup:
        return (long)route_lookup_h((long)arg);
    case TI_rt_lookupb:
        return (long)route_lookupb_h(arg[0], (unsigned int)arg[1]);
    case TI_rt_drop:
        /* デフォルト経路の削除だけは台帳として受ける */
        if (arg[0] == 0 && arg[1] == 0) {
            rt_default.gateway = 0;
            return 0;
        }
        return -1;              /* ホスト側の経路はゲストから変えられない */
    case TI_rt_add:
        /* デフォルト経路の追加だけは台帳として受ける */
        if (arg[0] == 0 && arg[1] == 0) {
            rt_default.gateway = arg[2];
            if (arg[3])
                rt_default.iface = (struct iface_h *)arg[3];
            rt_default.metric = arg[4];
            return (long)&rt_default;
        }
        return 0;
    case TI_rip:
        /* 引数は値そのもの: 0=RIP_ON, 1=RIP_OFF, 2=RIP_STAT。
           RIP 自体はゲストでは動かないので、表示用の台帳として保持する */
        if ((long)arg == 0) { rip_flag = 1; return 1; }
        if ((long)arg == 1) { rip_flag = 0; return 0; }
        return rip_flag;
    case TI_dns_add:
        return dns_add_h((long)arg);
    case TI_dns_drop:
        return dns_drop_h((long)arg);
    case TI_dns_get:
        return dns_count ? (long)&dns_nodes[0] : 0;
    case TI_set_domain_name:
        strncpy(domain_name, (const char *)arg, sizeof(domain_name) - 1);
        domain_name[sizeof(domain_name) - 1] = '\0';
        return 0;
    case TI_get_domain_name:
        return (long)domain_name;
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
    case TI_read_s: {
        int r = ti_read_s(arg);
        if (r > 0) { if_hf0.rawrcvcnt++; if_hf0.iprcvcnt++; }
        return r;
    }
    case TI_write_s: {
        int r = ti_write_s(arg);
        if (r > 0) { if_hf0.rawsndcnt++; if_hf0.ipsndcnt++; }
        return r;
    }
    case TI_recvfrom: {
        int r = ti_recvfrom(arg);
        if (r > 0) { if_hf0.rawrcvcnt++; if_hf0.iprcvcnt++; }
        return r;
    }
    case TI_sendto: {
        int r = ti_sendto(arg);
        if (r > 0) { if_hf0.rawsndcnt++; if_hf0.ipsndcnt++; }
        return r;
    }
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
    case TI_sockkick:
        return ti_sockkick((long)arg);
    case TI_usesock:
        return ti_usesock((long)arg);
    case TI_recvline:
        return ti_recvline(arg);
    case TI_sendline:
        return ti_sendline(arg);
    case TI_rrecvchar:
        return ti_rrecvchar((long)arg);
    case TI_recvchar:
        return ti_recvchar((long)arg);
    case TI_usflush:
        return ti_usflush((long)arg);
    case TI_seteol:
        return ti_seteol(arg);
    case TI_sockmode:
        return ti_sockmode(arg);
    case TI_setflush:
        return ti_setflush(arg);
    case TI_psocket:
        return ti_psocket(arg);
    case TI_sockerr:
        return ti_sockerr((long)arg);
    case TI_sockstate:
        return ti_sockstate((long)arg);
    case TI_sock_top:
        return (long)usocks;
    case TI_ntoa_sock:
        return ti_ntoa_sock(arg);
    case TI_gethostbyname:
        return ti_gethostbyname((const char *)arg);
    case TI_gethostbyaddr:
    case TI_getnetbyname:
    case TI_getnetbyaddr:
    case TI_getservbyname:
    case TI_getservbyport:
    case TI_getprotobyname:
    case TI_getprotobynumber:
        /* ポインタを返す名前引き関数は、引けない場合に -1 ではなく
           NULL を返すのが ABI の約束 (joynetd の同関数も NULL を返す)。
           -1 を返すと FCT libnetwork の _copyhost が
           (struct hostent *)-1 を参照して SIGBUS になる
           (ftp.x の open がこの経路で落ちていた) */
        return 0;
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

/* ------------------------------------------------------------------ */
/* 常駐解除                                                            */
/*                                                                     */
/* アプリ側 (libsocket の search_ti_entry) は、自分の PSP からメモリ   */
/* 管理ブロック (MEP) の prev_mp チェーンをさかのぼり、常駐フラグの    */
/* 立ったブロックでイメージ先頭に "TCP/IP" 署名があるものをドライバ   */
/* として見つける。したがって解除とは、そのブロックを DOS の MFREE    */
/* で解放してチェーンから外すことに等しい (joynetd の常駐解除と同じ   */
/* 方式)。常駐ブロックの PSP は MEP の直後 (MEP+0x10) にある。         */
/* ------------------------------------------------------------------ */

/* 常駐している hfsnet の MEP を探す。他の TCP/IP ドライバ (joynetd 等)
   と区別するため、署名に加えてマーカー文字列を含むブロックだけを
   自ドライバとみなす。見つからなければ NULL を返す */
static struct dos_mep *find_resident(void)
{
    struct dos_psp *psp = _dos_getpdb();
    struct dos_mep *mep = &((struct dos_mep *)psp)[-1];
    size_t mlen = strlen(resident_marker);

    while ((mep = mep->prev_mp) != NULL) {
        char *img, *end, *p;
        if (((long)mep->parent_mp & 0xff000000L) != 0xff000000L)
            continue;                   /* 常駐ブロックでない */
        img = (char *)mep + 0x100;      /* PSP の後ろがイメージ先頭 */
        if (memcmp(img, "TCP/IP", 6) != 0)
            continue;
        end = (char *)mep->block_end;
        if (end <= img || end > img + 0x100000L)
            end = img + 0x100000L;      /* 念のためのスキャン上限 */
        for (p = img; p + mlen <= end; p++) {
            if (memcmp(p, resident_marker, mlen) == 0)
                return mep;
        }
    }
    return NULL;
}

/* 常駐コピーの usocks を読み、使用中ソケットの数を数える。
   常駐コピーとは同じバイナリなので、イメージ先頭からの相対位置は
   一致する。スロットが確保済み (type != 0) か、参照カウントが残って
   いれば使用中とみなす。RPC は使わないので、ホスト側の相方が
   動いていなくても判定できる */
static int count_busy_sockets(const char *img)
{
    const volatile struct usock *ru = (const volatile struct usock *)
        (img + ((const char *)usocks - (const char *)&tcpip_magic));
    int i, busy = 0;

    for (i = 0; i < NSOCK; i++) {
        if (ru[i].type != 0 || ru[i].refcnt != 0)
            busy++;
    }
    return busy;
}

static int remove_driver(void)
{
    struct dos_mep *mep;
    int busy;

    (void)_dos_super(0);        /* joynetd の解除処理と同じく特権モードで */
    mep = find_resident();
    if (mep == NULL) {
        printf("hfsnet: not installed.\r\n");
        return 0;
    }
    busy = count_busy_sockets((const char *)mep + 0x100);
    if (busy > 0) {
        printf("hfsnet: cannot remove: %d socket(s) in use.\r\n", busy);
        return 1;
    }
    /* 解放するアドレスはブロックの PSP (joynetd が _dos_getpdb() の
       戻り値を控えて MFREE に渡すのと同じ位置) */
    if (_dos_mfree((char *)mep + sizeof(struct dos_mep)) < 0) {
        printf("hfsnet: remove failed.\r\n");
        return 1;
    }
    printf("hfsnet: removed.\r\n");
    return 0;
}

int main(int argc, char *argv[])
{
    const char *dir;
    char probe[192];
    size_t dl;
    int h;

    if (argc >= 2 &&
        (strcmp(argv[1], "/r") == 0 || strcmp(argv[1], "-r") == 0))
        return remove_driver();

    if (argc < 2) {
        printf("usage: hfsnet.x <folder>\r\n");
        printf("  e.g. hfsnet.x C:\\HFSNET\r\n");
        printf("  hfsnet.x /r  : remove the resident driver\r\n");
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

    /* ホスト側アプリの応答を一度だけ確認する。応答があり、ホストが
       自分の IP アドレスを返してきたらそれを表示する。応答自体が
       なければ、ホスト側アプリの起動を促す。旧いホストは
       OP_HOSTINFO を知らず result=-1 で応答するので、その場合は
       追加の行を出さない (応答があったことは分かるため)。
       確認の前に応答ファイルを消しておく。前回の応答が残っていると、
       通し番号が偶然一致したときにホスト不在でも「応答あり」と
       誤判定してしまうため */
    {
        struct resinfo res;
        long saved_timeout = rpc_timeout;
        unsigned long host_ip = 0;
        int r;
        _dos_delete(path_res);
        /* 通し番号を現在時刻で種付けする。ホストは通し番号が前回と
           同じ要求を処理しないため、種付けしないと、前回の常駐時に
           残った要求ファイルと番号が衝突して応答が返らない */
        rpc_seq = (unsigned long)now100();
        rpc_timeout = 500;              /* 常駐時の確認は5秒だけ待つ */
        r = rpc(OP_HOSTINFO, 0, 0, 0, 0, 0, NULL, 0, &res, NULL);
        rpc_timeout = saved_timeout;
        printf("hfsnet: installed (version %s, folder = %s)\r\n",
               HFSNET_VERSION, dir);
        if (r == 0 && res.result == 0 && res.aux != 0) {
            unsigned long ip = res.aux;
            host_ip = ip;
            printf("hfsnet: host IP address: %lu.%lu.%lu.%lu\r\n",
                   (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                   (ip >> 8) & 0xff, ip & 0xff);
        } else if (r != 0) {
            printf("hfsnet: no host application. "
                   "Please start the host application.\r\n");
        }
        /* 管理系 TI (iface / route) の台帳を初期化する。ホスト IP が
           取れていれば hf0 のアドレスとして反映する */
        mgmt_init(host_ip);
    }
    fflush(stdout);

    _dos_keeppr(_HEND - (int)&tcpip_magic, 0);
    return 0;
}
