// platform.h - プラットフォーム依存部の抽象層
//
// エンジンと GUI はこのヘッダだけを見て書く。OS ごとの実装は
// platform_win.cpp / platform_linux.cpp / platform_mac.cpp に置く。
// 本体コードに #ifdef を散らさないための境界。
//
// 文字列パスはすべて UTF-8 で受け渡す。Windows 実装の内側で
// ワイド文字へ変換する。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plat {

#if defined(_WIN32)
using sock_t = unsigned long long;              // SOCKET
inline const sock_t kInvalidSock = ~0ull;
#else
using sock_t = int;
inline const sock_t kInvalidSock = -1;
#endif

// ---- プロセス全体の初期化 (Winsock 等) ----
bool net_init();
void net_cleanup();

// ---- ソケット ----
// 戻り値の作法 (send/recv 系): >=0 = バイト数, -1 = エラー, -2 = 今はできない
sock_t tcp_socket();                            // ノンブロッキング, REUSEADDR
sock_t udp_socket();
void close_sock(sock_t s);
bool readable(sock_t s, int timeout_ms);
bool writable(sock_t s, int timeout_ms);
// 0 = 接続完了, 1 = 進行中, -1 = 失敗
int connect_ex(sock_t s, uint32_t ip_be, uint16_t port_host);
int send_data(sock_t s, const uint8_t *data, int len);
int recv_data(sock_t s, uint8_t *buf, int len);
int send_to(sock_t s, const uint8_t *data, int len,
            uint32_t ip_be, uint16_t port_host);
int recv_from(sock_t s, uint8_t *buf, int len,
              uint32_t *src_ip_be, uint16_t *src_port_host);
// 1 = データあり, 0 = 相手が閉じた (EOF), -1 = 判定できない/データなし
int peek_one(sock_t s);
// MSG_PEEK で見える最大バイト数 (受信可能量の概算)
int peek_avail(sock_t s);
bool bind_sock(sock_t s, uint32_t ip_be, uint16_t port_host);
bool listen_sock(sock_t s, int backlog);
sock_t accept_sock(sock_t s, uint32_t *peer_ip_be, uint16_t *peer_port_host);
bool shutdown_sock(sock_t s, int how);          // 0=RD, 1=WR, 2=RDWR
bool local_endpoint(sock_t s, uint32_t *ip_be, uint16_t *port_host);
bool peer_endpoint(sock_t s, uint32_t *ip_be, uint16_t *port_host);
int socket_error(sock_t s);                     // SO_ERROR の値 (0 = エラーなし)

// ---- 名前解決・自アドレス ----
uint32_t resolve_ipv4(const std::string &name, bool *ok);
// dest へ送るときに使われる自ホストの IPv4 (UDP connect の要領で調べる)
uint32_t local_addr_for(uint32_t dest_ip_be);

// ---- ICMP エコー (ping 専用の特権なし方式) ----
// data は ICMP ヘッダより後ろのデータ部。識別子・シーケンスは
// OS 側の仕組みが付けるため、呼び出し側は関知しない。
// 応答が返れば true。rtt_ms に往復時間 (ミリ秒) が入る。
bool icmp_echo(uint32_t dest_ip_be, const uint8_t *data, int len,
               int timeout_ms, int *rtt_ms);

// ---- ファイル ----
// 応答ファイルの差し替え: 一時ファイルへ書いてから rename で差し替える。
// Windows ではゲストが開いている間 rename が失敗するため再試行し、
// 最後は直接上書きにフォールバックする (Python 版 publish_res と同じ)。
bool publish_file(const std::string &tmp_path, const std::string &final_path,
                  const uint8_t *data, size_t len);
bool read_file(const std::string &path, std::vector<uint8_t> *out);

// ---- 時刻 ----
uint64_t now_ms();
void sleep_ms(int ms);

} // namespace plat
