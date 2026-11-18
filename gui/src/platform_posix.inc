// platform_posix.inc - POSIX (Linux / macOS) 共通の実装
// platform_linux.cpp と platform_mac.cpp から include される。
// OS 差はこのファイル内の小さな分岐 (SIGPIPE の扱い) だけに閉じ込める。
#include "platform.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

namespace plat {
namespace {

int to_fd(sock_t s) { return (int)s; }

void set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

sock_t make_socket(int type)
{
    int fd = ::socket(AF_INET, type, 0);
    if (fd < 0)
        return kInvalidSock;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#if defined(__APPLE__)
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    set_nonblocking(fd);
    return (sock_t)fd;
}

sockaddr_in make_addr(uint32_t ip_be, uint16_t port_host)
{
    // ip_be は「最初のオクテットが最上位バイト」の整数 (ファイル
    // プロトコルと同じ解釈)。s_addr へは htonl して入れる。
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip_be);
    a.sin_port = htons(port_host);
    return a;
}

bool select_wait(sock_t s, bool for_write, int timeout_ms)
{
    int fd = to_fd(s);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int n = ::select(fd + 1, for_write ? nullptr : &fds,
                     for_write ? &fds : nullptr, nullptr, &tv);
    return n > 0;
}

int io_result(int n)
{
    if (n >= 0)
        return n;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return -2;
    return -1;
}

#if defined(__APPLE__)
const int kSendFlags = 0;               // SO_NOSIGPIPE を付けている
#else
const int kSendFlags = MSG_NOSIGNAL;
#endif

bool write_all(const std::string &path, const uint8_t *data, size_t len)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    bool ok = len == 0 || std::fwrite(data, 1, len, f) == len;
    if (std::fclose(f) != 0)
        ok = false;
    return ok;
}

uint16_t cksum(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    while (len >= 2) {
        sum += (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint16_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

} // namespace

bool net_init() { return true; }
void net_cleanup() {}

sock_t tcp_socket() { return make_socket(SOCK_STREAM); }
sock_t udp_socket() { return make_socket(SOCK_DGRAM); }
void close_sock(sock_t s) { if (s != kInvalidSock) ::close(to_fd(s)); }
bool readable(sock_t s, int timeout_ms) { return select_wait(s, false, timeout_ms); }
bool writable(sock_t s, int timeout_ms) { return select_wait(s, true, timeout_ms); }

int connect_ex(sock_t s, uint32_t ip_be, uint16_t port_host)
{
    sockaddr_in a = make_addr(ip_be, port_host);
    int n = ::connect(to_fd(s), (sockaddr *)&a, sizeof(a));
    if (n == 0)
        return 0;
    if (errno == EINPROGRESS)
        return 1;
    return -1;
}

int send_data(sock_t s, const uint8_t *data, int len)
{
    return io_result((int)::send(to_fd(s), data, len, kSendFlags));
}

int recv_data(sock_t s, uint8_t *buf, int len)
{
    return io_result((int)::recv(to_fd(s), buf, len, 0));
}

int send_to(sock_t s, const uint8_t *data, int len,
            uint32_t ip_be, uint16_t port_host)
{
    sockaddr_in a = make_addr(ip_be, port_host);
    int n = (int)::sendto(to_fd(s), data, len, kSendFlags,
                          (sockaddr *)&a, sizeof(a));
    if (n < 0 && (errno == EPERM || errno == EACCES || errno == EISCONN)) {
        // 制限の強いサンドボックスでは未接続の sendto が塞がれることが
        // ある (Python 版テストのシムと同じ事情)。connect + send で代用
        // する。通常の環境では本物の sendto が成功し、ここには来ない。
        if (::connect(to_fd(s), (sockaddr *)&a, sizeof(a)) == 0)
            n = (int)::send(to_fd(s), data, len, kSendFlags);
    }
    return io_result(n);
}

int recv_from(sock_t s, uint8_t *buf, int len,
              uint32_t *src_ip_be, uint16_t *src_port_host)
{
    sockaddr_in a;
    socklen_t alen = sizeof(a);
    int n = (int)::recvfrom(to_fd(s), buf, len, 0, (sockaddr *)&a, &alen);
    if (n >= 0) {
        if (src_ip_be)
            *src_ip_be = ntohl(a.sin_addr.s_addr);
        if (src_port_host)
            *src_port_host = ntohs(a.sin_port);
    }
    return io_result(n);
}

int peek_one(sock_t s)
{
    uint8_t b;
    int n = (int)::recv(to_fd(s), &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 1)
        return 1;
    if (n == 0)
        return 0;
    return -1;
}

int peek_avail(sock_t s)
{
    static thread_local uint8_t buf[65536];
    int n = (int)::recv(to_fd(s), buf, sizeof(buf), MSG_PEEK | MSG_DONTWAIT);
    return n > 0 ? n : 0;
}

bool bind_sock(sock_t s, uint32_t ip_be, uint16_t port_host)
{
    sockaddr_in a = make_addr(ip_be, port_host);
    return ::bind(to_fd(s), (sockaddr *)&a, sizeof(a)) == 0;
}

bool listen_sock(sock_t s, int backlog) { return ::listen(to_fd(s), backlog) == 0; }

sock_t accept_sock(sock_t s, uint32_t *peer_ip_be, uint16_t *peer_port_host)
{
    sockaddr_in a;
    socklen_t alen = sizeof(a);
    int fd = ::accept(to_fd(s), (sockaddr *)&a, &alen);
    if (fd < 0)
        return kInvalidSock;
    set_nonblocking(fd);
    if (peer_ip_be)
        *peer_ip_be = ntohl(a.sin_addr.s_addr);
    if (peer_port_host)
        *peer_port_host = ntohs(a.sin_port);
    return (sock_t)fd;
}

bool shutdown_sock(sock_t s, int how) { return ::shutdown(to_fd(s), how) == 0; }

bool local_endpoint(sock_t s, uint32_t *ip_be, uint16_t *port_host)
{
    sockaddr_in a;
    socklen_t alen = sizeof(a);
    if (::getsockname(to_fd(s), (sockaddr *)&a, &alen) != 0)
        return false;
    if (ip_be)
        *ip_be = ntohl(a.sin_addr.s_addr);
    if (port_host)
        *port_host = ntohs(a.sin_port);
    return true;
}

bool peer_endpoint(sock_t s, uint32_t *ip_be, uint16_t *port_host)
{
    sockaddr_in a;
    socklen_t alen = sizeof(a);
    if (::getpeername(to_fd(s), (sockaddr *)&a, &alen) != 0)
        return false;
    if (ip_be)
        *ip_be = ntohl(a.sin_addr.s_addr);
    if (port_host)
        *port_host = ntohs(a.sin_port);
    return true;
}

int socket_error(sock_t s)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(to_fd(s), SOL_SOCKET, SO_ERROR, &err, &len) != 0)
        return -1;
    return err;
}

uint32_t resolve_ipv4(const std::string &name, bool *ok)
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    addrinfo *res = nullptr;
    if (::getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0 || !res) {
        if (ok)
            *ok = false;
        return 0;
    }
    uint32_t ip = ntohl(((sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    ::freeaddrinfo(res);
    if (ok)
        *ok = true;
    return ip;
}

uint32_t local_addr_for(uint32_t dest_ip_be)
{
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    sockaddr_in a = make_addr(dest_ip_be, 80);
    uint32_t out = 0;
    if (::connect(fd, (sockaddr *)&a, sizeof(a)) == 0) {
        sockaddr_in me;
        socklen_t len = sizeof(me);
        if (::getsockname(fd, (sockaddr *)&me, &len) == 0)
            out = ntohl(me.sin_addr.s_addr);
    }
    ::close(fd);
    return out;
}

// ping ソケット (SOCK_DGRAM + IPPROTO_ICMP) による特権なしエコー。
// カーネルが識別子を付け替えチェックサムを計算するため、
// こちらは type=8 / シーケンス / データを渡すだけ。
bool icmp_echo(uint32_t dest_ip_be, const uint8_t *data, int len,
               int timeout_ms, int *rtt_ms)
{
    static std::atomic<uint16_t> seq_counter{0};
    int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0)
        return false;
    uint16_t seq = ++seq_counter;
    std::vector<uint8_t> pkt(8 + len, 0);
    pkt[0] = 8;                          // エコー要求
    pkt[1] = 0;
    pkt[4] = 0x48;                       // 識別子 'HF' (カーネルが付け替える)
    pkt[5] = 0x46;
    pkt[6] = (uint8_t)(seq >> 8);
    pkt[7] = (uint8_t)seq;
    if (len > 0)
        std::memcpy(&pkt[8], data, len);
    uint16_t c = cksum(pkt.data(), pkt.size());
    pkt[2] = (uint8_t)(c >> 8);
    pkt[3] = (uint8_t)c;

    sockaddr_in a = make_addr(dest_ip_be, 0);
    uint64_t t0 = now_ms();
    int n = (int)::sendto(fd, pkt.data(), pkt.size(), 0,
                          (sockaddr *)&a, sizeof(a));
    if (n < 0 && (errno == EPERM || errno == EACCES)) {
        // 制限の強いサンドボックスでは素の sendto が塞がれることが
        // あるため、connect してから send する形にフォールバックする
        if (::connect(fd, (sockaddr *)&a, sizeof(a)) == 0)
            n = (int)::send(fd, pkt.data(), pkt.size(), 0);
    }
    if (n < 0) {
        ::close(fd);
        return false;
    }
    uint8_t buf[2048];
    for (;;) {
        uint64_t elapsed = now_ms() - t0;
        if (elapsed > (uint64_t)timeout_ms) {
            ::close(fd);
            return false;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        timeval tv;
        int remain = timeout_ms - (int)elapsed;
        tv.tv_sec = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;
        if (::select(fd + 1, &fds, nullptr, nullptr, &tv) <= 0) {
            ::close(fd);
            return false;
        }
        int rn = (int)::recv(fd, buf, sizeof(buf), 0);
        if (rn >= 8 && buf[0] == 0) {    // エコー応答
            uint16_t rseq = (uint16_t)((buf[6] << 8) | buf[7]);
            if (rseq == seq) {
                if (rtt_ms)
                    *rtt_ms = (int)(now_ms() - t0);
                ::close(fd);
                return true;
            }
        }
    }
}

bool publish_file(const std::string &tmp_path, const std::string &final_path,
                  const uint8_t *data, size_t len)
{
    if (!write_all(tmp_path, data, len))
        return false;
    // POSIX では開いているファイルの rename が通るため通常は1回で成功
    for (int i = 0; i < 40; i++) {
        if (::rename(tmp_path.c_str(), final_path.c_str()) == 0)
            return true;
        sleep_ms(50);
    }
    return write_all(final_path, data, len);
}

bool read_file(const std::string &path, std::vector<uint8_t> *out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out->clear();
    uint8_t buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out->insert(out->end(), buf, buf + n);
    bool ok = std::ferror(f) == 0;
    std::fclose(f);
    return ok;
}

uint64_t now_ms()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

void sleep_ms(int ms) { ::usleep((useconds_t)ms * 1000); }

} // namespace plat
