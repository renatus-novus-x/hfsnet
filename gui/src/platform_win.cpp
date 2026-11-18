// platform_win.cpp - Windows 用のプラットフォーム実装
// Winsock2 + IcmpSendEcho (iphlpapi) + ワイド文字ファイル API。
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#include "platform.h"

#include <atomic>
#include <chrono>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace plat {
namespace {

SOCKET to_sock(sock_t s) { return (SOCKET)s; }

std::wstring widen(const std::string &utf8)
{
    if (utf8.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(),
                                nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], n);
    return w;
}

void set_nonblocking(SOCKET s)
{
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
}

sock_t make_socket(int type)
{
    SOCKET s = ::socket(AF_INET, type, 0);
    if (s == INVALID_SOCKET)
        return kInvalidSock;
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    set_nonblocking(s);
    return (sock_t)s;
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
    SOCKET fd = to_sock(s);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int n = ::select(0, for_write ? nullptr : &fds,
                     for_write ? &fds : nullptr, nullptr, &tv);
    return n > 0;
}

int io_result(int n)
{
    if (n >= 0)
        return n;
    if (WSAGetLastError() == WSAEWOULDBLOCK)
        return -2;
    return -1;
}

bool write_all(const std::string &path, const uint8_t *data, size_t len)
{
    std::wstring w = widen(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    size_t off = 0;
    while (off < len) {
        DWORD wrote = 0;
        if (!WriteFile(h, data + off, (DWORD)(len - off), &wrote, nullptr) ||
            wrote == 0) {
            ok = false;
            break;
        }
        off += wrote;
    }
    CloseHandle(h);
    return ok;
}

} // namespace

bool net_init()
{
    static std::atomic<bool> done{false};
    if (done.exchange(true))
        return true;
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
void net_cleanup() {}

sock_t tcp_socket() { return make_socket(SOCK_STREAM); }
sock_t udp_socket() { return make_socket(SOCK_DGRAM); }
void close_sock(sock_t s)
{
    if (s != kInvalidSock)
        closesocket(to_sock(s));
}
bool readable(sock_t s, int timeout_ms) { return select_wait(s, false, timeout_ms); }
bool writable(sock_t s, int timeout_ms) { return select_wait(s, true, timeout_ms); }

int connect_ex(sock_t s, uint32_t ip_be, uint16_t port_host)
{
    sockaddr_in a = make_addr(ip_be, port_host);
    int n = ::connect(to_sock(s), (sockaddr *)&a, sizeof(a));
    if (n == 0)
        return 0;
    if (WSAGetLastError() == WSAEWOULDBLOCK)
        return 1;
    return -1;
}

int send_data(sock_t s, const uint8_t *data, int len)
{
    return io_result(::send(to_sock(s), (const char *)data, len, 0));
}

int recv_data(sock_t s, uint8_t *buf, int len)
{
    return io_result(::recv(to_sock(s), (char *)buf, len, 0));
}

int send_to(sock_t s, const uint8_t *data, int len,
            uint32_t ip_be, uint16_t port_host)
{
    sockaddr_in a = make_addr(ip_be, port_host);
    return io_result(::sendto(to_sock(s), (const char *)data, len, 0,
                              (sockaddr *)&a, sizeof(a)));
}

int recv_from(sock_t s, uint8_t *buf, int len,
              uint32_t *src_ip_be, uint16_t *src_port_host)
{
    sockaddr_in a;
    int alen = sizeof(a);
    int n = ::recvfrom(to_sock(s), (char *)buf, len, 0, (sockaddr *)&a, &alen);
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
    char b;
    int n = ::recv(to_sock(s), &b, 1, MSG_PEEK);
    if (n == 1)
        return 1;
    if (n == 0)
        return 0;
    return -1;
}

int peek_avail(sock_t s)
{
    static thread_local char buf[65536];
    int n = ::recv(to_sock(s), buf, sizeof(buf), MSG_PEEK);
    return n > 0 ? n : 0;
}

bool bind_sock(sock_t s, uint32_t ip_be, uint16_t port_host)
{
    sockaddr_in a = make_addr(ip_be, port_host);
    return ::bind(to_sock(s), (sockaddr *)&a, sizeof(a)) == 0;
}

bool listen_sock(sock_t s, int backlog) { return ::listen(to_sock(s), backlog) == 0; }

sock_t accept_sock(sock_t s, uint32_t *peer_ip_be, uint16_t *peer_port_host)
{
    sockaddr_in a;
    int alen = sizeof(a);
    SOCKET c = ::accept(to_sock(s), (sockaddr *)&a, &alen);
    if (c == INVALID_SOCKET)
        return kInvalidSock;
    set_nonblocking(c);
    if (peer_ip_be)
        *peer_ip_be = ntohl(a.sin_addr.s_addr);
    if (peer_port_host)
        *peer_port_host = ntohs(a.sin_port);
    return (sock_t)c;
}

bool shutdown_sock(sock_t s, int how)
{
    // ゲストの how (0=RD, 1=WR, 2=RDWR) は Winsock の SD_* と値が違う
    static const int map[3] = {SD_RECEIVE, SD_SEND, SD_BOTH};
    if (how < 0 || how > 2)
        return false;
    return ::shutdown(to_sock(s), map[how]) == 0;
}

bool local_endpoint(sock_t s, uint32_t *ip_be, uint16_t *port_host)
{
    sockaddr_in a;
    int alen = sizeof(a);
    if (::getsockname(to_sock(s), (sockaddr *)&a, &alen) != 0)
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
    int alen = sizeof(a);
    if (::getpeername(to_sock(s), (sockaddr *)&a, &alen) != 0)
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
    int len = sizeof(err);
    if (::getsockopt(to_sock(s), SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0)
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
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
        return 0;
    sockaddr_in a = make_addr(dest_ip_be, 80);
    uint32_t out = 0;
    if (::connect(s, (sockaddr *)&a, sizeof(a)) == 0) {
        sockaddr_in me;
        int len = sizeof(me);
        if (::getsockname(s, (sockaddr *)&me, &len) == 0)
            out = ntohl(me.sin_addr.s_addr);
    }
    closesocket(s);
    return out;
}

// IcmpSendEcho (iphlpapi) による特権なしエコー。管理者権限は不要で、
// Windows 2000 以降で使える。識別子・シーケンスは API 側が付ける。
bool icmp_echo(uint32_t dest_ip_be, const uint8_t *data, int len,
               int timeout_ms, int *rtt_ms)
{
    static HANDLE handle = IcmpCreateFile();
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    DWORD reply_size = sizeof(ICMP_ECHO_REPLY) + (DWORD)len + 8;
    std::vector<uint8_t> replybuf(reply_size);
    // IPAddr は inet_addr() と同じくネットワークバイトオーダの値
    DWORD n = IcmpSendEcho(handle, (IPAddr)htonl(dest_ip_be),
                           const_cast<uint8_t *>(data), (WORD)len, nullptr,
                           replybuf.data(), reply_size, (DWORD)timeout_ms);
    if (n == 0)
        return false;
    auto *reply = (ICMP_ECHO_REPLY *)replybuf.data();
    if (reply->Status != IP_SUCCESS)
        return false;
    if (rtt_ms)
        *rtt_ms = (int)reply->RoundTripTime;
    return true;
}

bool publish_file(const std::string &tmp_path, const std::string &final_path,
                  const uint8_t *data, size_t len)
{
    if (!write_all(tmp_path, data, len))
        return false;
    std::wstring wt = widen(tmp_path);
    std::wstring wf = widen(final_path);
    // ゲストが応答ファイルを開いている間は差し替えが失敗する (Python 版
    // で実証済み) ため、再試行してから直接上書きにフォールバックする
    for (int i = 0; i < 40; i++) {
        if (MoveFileExW(wt.c_str(), wf.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        Sleep(50);
    }
    return write_all(final_path, data, len);
}

bool read_file(const std::string &path, std::vector<uint8_t> *out)
{
    std::wstring w = widen(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    out->clear();
    uint8_t buf[8192];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof(buf), &n, nullptr)) {
            CloseHandle(h);
            return false;
        }
        if (n == 0)
            break;
        out->insert(out->end(), buf, buf + n);
    }
    CloseHandle(h);
    return true;
}

uint64_t now_ms() { return (uint64_t)GetTickCount64(); }
void sleep_ms(int ms) { Sleep((DWORD)ms); }

} // namespace plat
