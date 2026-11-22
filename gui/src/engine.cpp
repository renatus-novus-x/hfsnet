// engine.cpp - hfsnet ホスト側エンジンの実装
// Python 版 hfsnethost.py の忠実な移植 + SOCK_RAW (ICMP 代理) + 統計。
#include "engine.h"
#include "platform.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace {

// ファイルプロトコルの操作コード (Python 版と同じ)
enum {
    OP_OPEN = 1, OP_CLOSE = 2, OP_CONNECT = 3, OP_SEND = 4, OP_RECV = 5,
    OP_STATE = 6, OP_RESOLVE = 7, OP_BIND = 8, OP_LISTEN = 9,
    OP_ACCEPT = 10, OP_SHUTDOWN = 11, OP_HOSTINFO = 12
};
const char *OP_NAMES[] = {"",    "OPEN",  "CLOSE", "CONNECT", "SEND",
                          "RECV", "STATE", "RESOLVE", "BIND",  "LISTEN",
                          "ACCEPT", "SHUTDOWN", "HOSTINFO"};

enum { ST_CLOSED = 0, ST_LISTEN = 1, ST_SYN_SENT = 2, ST_SYN_RECEIVED = 3,
       ST_ESTABLISHED = 4, ST_CLOSE_WAIT = 5 };

constexpr int NSOCK = 32;
constexpr int RECV_WAIT_MS = 300;       // RECV/ACCEPT の待ち上限
constexpr int SEND_WAIT_MS = 3000;      // SEND の待ち上限
constexpr int ICMP_TIMEOUT_MS = 3000;

uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

uint16_t inet_checksum(const uint8_t *p, size_t len)
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

struct RawPacket {
    std::vector<uint8_t> bytes;
    uint32_t src_ip = 0;
};

} // namespace

// エコー応答スレッドと共有する raw 受信キュー (Engine の寿命と切り離す)
struct RawPacketQ {
    struct Q {
        std::deque<RawPacket> q;
        uint32_t gen = 0;
    };
    std::mutex m;
    std::array<Q, NSOCK> slots;
    std::atomic<uint32_t> epoch{0};
};

struct Engine::Slot {
    plat::sock_t sock = plat::kInvalidSock;
    int state = ST_CLOSED;
    bool listening = false;
    bool eof = false;
    int kind = 0;               // 0=TCP, 1=UDP, 2=RAW
    int proto = 0;              // RAW のプロトコル番号 (1=ICMP)
    uint32_t peer_ip = 0;       // RAW の宛先 (connect 記録用)
    uint16_t peer_port = 0;
    bool raw_ip_style = false;  // ゲストが IP ヘッダ付きで送ってきたか
    uint32_t raw_guest_src = 0; // その IP ヘッダの送信元
};

Engine::Engine() : slots_(NSOCK) {}
Engine::~Engine() { stop(); }

void Engine::set_poll_ms(int ms)
{
    poll_ms_ = ms < 0 ? 0 : ms;
}

bool Engine::start(const std::string &folder)
{
    stop();
    plat::net_init();
    folder_ = folder;
    req_path_ = folder + "/HFSREQ.BIN";
    res_path_ = folder + "/HFSRES.BIN";
    tmp_path_ = folder + "/HFSRES.TMP";
    slots_.assign(NSOCK, Slot());
    {
        std::lock_guard<std::mutex> lk(stat_mtx_);
        tx_total_ = rx_total_ = 0;
        state_polls_ = 0;
        last_sec_tx_ = last_sec_rx_ = 0;
        cur_rate_tx_ = cur_rate_rx_ = 0;
        history_.clear();
        slot_info_.fill(SlotInfo());
    }
    raw_ = std::make_shared<RawPacketQ>();
    have_seq_ = false;
    last_history_ms_ = plat::now_ms();
    running_ = true;
    worker_ = std::thread(&Engine::loop, this);
    log_line("hfsnet host: watching " + folder);
    return true;
}

void Engine::stop()
{
    if (raw_)
        raw_->epoch++;
    running_ = false;
    if (worker_.joinable())
        worker_.join();
    for (auto &sl : slots_)
        if (sl.sock != plat::kInvalidSock)
            plat::close_sock(sl.sock);
    slots_.assign(NSOCK, Slot());
}

void Engine::wait()
{
    while (running_)
        plat::sleep_ms(100);
}

void Engine::set_log_callback(std::function<void(const std::string &)> cb)
{
    std::lock_guard<std::mutex> lk(log_mtx_);
    log_cb_ = std::move(cb);
}

void Engine::log_line(const std::string &s)
{
    std::function<void(const std::string &)> cb;
    {
        std::lock_guard<std::mutex> lk(log_mtx_);
        cb = log_cb_;
    }
    if (cb)
        cb(s);
}

Snapshot Engine::snapshot()
{
    Snapshot sn;
    sn.running = running_;
    sn.folder = folder_;
    std::lock_guard<std::mutex> lk(stat_mtx_);
    sn.tx_total = tx_total_;
    sn.rx_total = rx_total_;
    sn.tx_rate = cur_rate_tx_;
    sn.rx_rate = cur_rate_rx_;
    sn.state_polls = state_polls_;
    sn.history.assign(history_.begin(), history_.end());
    sn.slots.assign(slot_info_.begin(), slot_info_.end());
    return sn;
}

// ---------------- ワーカーループ ----------------

void Engine::loop()
{
    while (running_) {
        uint64_t now = plat::now_ms();
        if (now - last_history_ms_ >= 1000) {
            std::lock_guard<std::mutex> lk(stat_mtx_);
            uint64_t dtx = tx_total_ - last_sec_tx_;
            uint64_t drx = rx_total_ - last_sec_rx_;
            last_sec_tx_ = tx_total_;
            last_sec_rx_ = rx_total_;
            cur_rate_tx_ = dtx;
            cur_rate_rx_ = drx;
            history_.push_back({dtx, drx});
            while (history_.size() > 60)
                history_.pop_front();
            last_history_ms_ = now;
        }

        std::vector<uint8_t> raw;
        if (plat::read_file(req_path_, &raw) && raw.size() >= 20 &&
            raw[0] == 'H' && raw[1] == 'N') {
            uint8_t op = raw[2];
            uint32_t seq = be32(&raw[4]);
            uint32_t length = be32(&raw[16]);
            uint32_t plen = (op == OP_SEND || op == OP_RESOLVE) ? length : 0;
            if ((!have_seq_ || seq != last_seq_) &&
                raw.size() == 20 + plen) {
                have_seq_ = true;
                last_seq_ = seq;
                try {
                    process(raw);
                } catch (...) {
                    log_line("error: exception in request processing");
                }
            }
        }
        plat::sleep_ms(poll_ms_);
    }
}

// ---------------- スロット操作 ----------------

void Engine::refresh_state(Slot &sl)
{
    if (sl.kind == 2) {                 // RAW は常に ESTABLISHED
        sl.state = ST_ESTABLISHED;
        return;
    }
    if (sl.sock == plat::kInvalidSock) {
        sl.state = ST_CLOSED;
        return;
    }
    if (sl.listening) {
        sl.state = ST_LISTEN;
        return;
    }
    if (sl.kind == 1) {                 // UDP
        sl.state = ST_ESTABLISHED;
        return;
    }
    if (sl.state == ST_SYN_SENT) {
        if (plat::writable(sl.sock, 0)) {
            if (plat::socket_error(sl.sock) == 0)
                sl.state = ST_ESTABLISHED;
            else {
                sl.state = ST_CLOSED;
                close_slot(sl);
            }
        }
        return;
    }
    if (sl.state == ST_ESTABLISHED || sl.state == ST_CLOSE_WAIT) {
        if (sl.eof) {
            sl.state = ST_CLOSE_WAIT;
            return;
        }
        if (plat::readable(sl.sock, 0) && plat::peek_one(sl.sock) == 0)
            sl.state = ST_CLOSE_WAIT;
    }
}

void Engine::close_slot(Slot &sl)
{
    if (sl.sock != plat::kInvalidSock)
        plat::close_sock(sl.sock);
    sl.sock = plat::kInvalidSock;
    sl.state = ST_CLOSED;
    sl.listening = false;
    sl.eof = false;
    sl.kind = 0;
}

int Engine::recv_avail(Slot &sl)
{
    if (sl.sock == plat::kInvalidSock || sl.listening)
        return 0;
    return plat::peek_avail(sl.sock);
}

// ---------------- 要求の処理 ----------------

namespace {
struct Res {
    long result = -1;
    uint32_t rlen = 0;
    uint32_t aux = 0;
    uint16_t port = 0;          // 応答ヘッダの port 欄 (extra_port)
    std::vector<uint8_t> payload;
};
} // namespace

void Engine::process(const std::vector<uint8_t> &raw)
{
    const uint8_t op = raw[2];
    const uint8_t idx = raw[3];
    const uint32_t seq = be32(&raw[4]);
    const uint16_t port = be16(&raw[8]);
    const uint16_t arg = be16(&raw[10]);
    const uint32_t addr = be32(&raw[12]);
    const uint32_t length = be32(&raw[16]);
    std::vector<uint8_t> payload;
    if ((op == OP_SEND || op == OP_RESOLVE) && raw.size() > 20)
        payload.assign(raw.begin() + 20, raw.end());

    Res r;
    const bool valid_idx = idx < NSOCK;
    Slot &sl = slots_[valid_idx ? idx : 0];

    if (!valid_idx) {
        r.result = -1;
    } else if (op == OP_OPEN) {
        // arg = ソケット種別 (0=STREAM, 1=DGRAM, 2=RAW)、
        // port = プロトコル番号 (RAW のとき。ゲストが渡す)
        close_slot(sl);
        {
            std::lock_guard<std::mutex> lk(raw_->m);
            raw_->slots[idx].q.clear();
            raw_->slots[idx].gen++;
        }
        sl.proto = port;
        sl.peer_ip = 0;
        sl.peer_port = 0;
        if (arg == 1) {
            sl.sock = plat::udp_socket();
            sl.kind = 1;
            sl.state = sl.sock != plat::kInvalidSock ? ST_ESTABLISHED
                                                     : ST_CLOSED;
            r.result = sl.sock != plat::kInvalidSock ? 0 : -1;
        } else if (arg == 2) {
            sl.kind = 2;                // RAW: OS ソケットは持たない
            sl.state = ST_ESTABLISHED;
            r.result = 0;
        } else {
            sl.sock = plat::tcp_socket();
            sl.kind = 0;
            sl.state = ST_CLOSED;
            r.result = sl.sock != plat::kInvalidSock ? 0 : -1;
        }
        {
            std::lock_guard<std::mutex> lk(stat_mtx_);
            slot_info_[idx] = SlotInfo();
            slot_info_[idx].used = r.result >= 0;
            slot_info_[idx].kind = sl.kind;
            slot_info_[idx].state = sl.state;
        }
    } else if (op == OP_CLOSE) {
        close_slot(sl);
        {
            std::lock_guard<std::mutex> lk(raw_->m);
            raw_->slots[idx].q.clear();
            raw_->slots[idx].gen++;
        }
        {
            std::lock_guard<std::mutex> lk(stat_mtx_);
            slot_info_[idx].used = false;
            slot_info_[idx].state = ST_CLOSED;
        }
        r.result = 0;
    } else if (op == OP_SHUTDOWN) {
        if (sl.kind == 2 || sl.sock == plat::kInvalidSock)
            r.result = -1;
        else
            r.result = plat::shutdown_sock(sl.sock, arg) ? 0 : -1;
    } else if (op == OP_CONNECT) {
        if (sl.kind == 2) {
            sl.peer_ip = addr;
            sl.peer_port = port;
            r.result = 0;
            r.aux = plat::local_addr_for(addr);
            r.port = 0;
            sl.state = ST_ESTABLISHED;
        } else if (sl.sock == plat::kInvalidSock) {
            r.result = -1;
        } else {
            int err = plat::connect_ex(sl.sock, addr, port);
            uint32_t lip = 0;
            uint16_t lport = 0;
            plat::local_endpoint(sl.sock, &lip, &lport);
            if (err == 0) {
                sl.state = ST_ESTABLISHED;
                r.result = 0;
                r.aux = lip;
                r.port = lport;
            } else if (err == 1) {
                sl.state = ST_SYN_SENT;
                r.result = 1;
                r.aux = lip;
                r.port = lport;
            } else {
                sl.state = ST_CLOSED;
                r.result = -1;
            }
        }
        if (r.result >= 0) {
            std::lock_guard<std::mutex> lk(stat_mtx_);
            slot_info_[idx].peer_ip = addr;
            slot_info_[idx].peer_port = port;
        }
    } else if (op == OP_SEND) {
        if (sl.kind == 2) {
            r.result = raw_send(idx, sl, payload, addr, port);
        } else if (sl.sock == plat::kInvalidSock) {
            r.result = -1;
        } else if (sl.kind == 1) {      // UDP
            int n;
            if (addr || port)
                n = plat::send_to(sl.sock, payload.data(),
                                  (int)payload.size(), addr, port);
            else
                n = plat::send_data(sl.sock, payload.data(),
                                    (int)payload.size());
            r.result = n;
        } else {                        // TCP
            refresh_state(sl);
            if (sl.state != ST_ESTABLISHED && sl.state != ST_CLOSE_WAIT) {
                r.result = -1;
            } else {
                int sent = 0;
                bool failed = false;
                uint64_t deadline = plat::now_ms() + SEND_WAIT_MS;
                while (sent < (int)payload.size()) {
                    uint64_t now = plat::now_ms();
                    if (now >= deadline)
                        break;
                    if (!plat::writable(sl.sock, (int)(deadline - now)))
                        break;
                    int n = plat::send_data(sl.sock, payload.data() + sent,
                                            (int)payload.size() - sent);
                    if (n == -2)
                        continue;
                    if (n < 0) {
                        failed = sent == 0;
                        break;
                    }
                    if (n == 0)
                        break;
                    sent += n;
                }
                if (failed)
                    r.result = -1;
                else if (sent == 0 && !payload.empty())
                    r.result = -2;
                else
                    r.result = sent;
            }
        }
        if (r.result > 0) {
            std::lock_guard<std::mutex> lk(stat_mtx_);
            tx_total_ += (uint64_t)r.result;
            slot_info_[idx].tx += (uint64_t)r.result;
        }
    } else if (op == OP_RECV) {
        if (sl.kind == 2) {
            auto got = raw_recv(idx, (int)length);
            r.result = got.first;
            r.aux = got.second;
            r.payload = std::move(raw_last_payload_);
        } else if (sl.sock == plat::kInvalidSock) {
            r.result = -1;
        } else if (sl.kind == 1) {      // UDP
            if (!plat::readable(sl.sock, RECV_WAIT_MS)) {
                r.result = -2;
            } else {
                std::vector<uint8_t> buf(length);
                uint32_t sip = 0;
                uint16_t sport = 0;
                int n = plat::recv_from(sl.sock, buf.data(), (int)length,
                                        &sip, &sport);
                if (n == -2)
                    r.result = -2;
                else if (n < 0)
                    r.result = -1;
                else {
                    r.result = n;
                    r.aux = sip;
                    r.port = sport;
                    buf.resize(n);
                    r.payload = std::move(buf);
                }
            }
        } else {                        // TCP
            if (sl.eof) {
                r.result = 0;
            } else if (!plat::readable(sl.sock, RECV_WAIT_MS)) {
                refresh_state(sl);
                if (sl.state == ST_CLOSE_WAIT) {
                    sl.eof = true;
                    r.result = 0;
                } else {
                    r.result = -2;
                }
            } else {
                std::vector<uint8_t> buf(length);
                int n = plat::recv_data(sl.sock, buf.data(), (int)length);
                if (n == -2)
                    r.result = -2;
                else if (n < 0)
                    r.result = -1;
                else if (n == 0) {
                    sl.eof = true;
                    sl.state = ST_CLOSE_WAIT;
                    r.result = 0;
                } else {
                    r.result = n;
                    buf.resize(n);
                    r.payload = std::move(buf);
                }
            }
        }
        if (r.result > 0) {
            std::lock_guard<std::mutex> lk(stat_mtx_);
            rx_total_ += (uint64_t)r.result;
            slot_info_[idx].rx += (uint64_t)r.result;
        }
    } else if (op == OP_STATE) {
        long st;
        uint32_t avail, sendable = 4096;
        if (sl.kind == 2) {
            size_t q = 0;
            {
                std::lock_guard<std::mutex> lk(raw_->m);
                for (auto &p : raw_->slots[idx].q)
                    q += p.bytes.size();
            }
            st = ST_ESTABLISHED;
            avail = (uint32_t)q;
        } else {
            refresh_state(sl);
            avail = (sl.state == ST_ESTABLISHED ||
                     sl.state == ST_CLOSE_WAIT)
                        ? (uint32_t)recv_avail(sl)
                        : 0;
            st = sl.state;
        }
        r.result = st;
        r.rlen = avail;
        r.aux = sendable;
        {
            std::lock_guard<std::mutex> lk(stat_mtx_);
            state_polls_++;
            slot_info_[idx].state = (int)st;
        }
    } else if (op == OP_RESOLVE) {
        std::string name((const char *)payload.data(), payload.size());
        name = name.c_str();            // NUL 終端まで
        bool ok = false;
        uint32_t ip = plat::resolve_ipv4(name, &ok);
        r.result = ok ? 0 : -1;
        r.aux = ok ? ip : 0;
    } else if (op == OP_BIND) {
        if (sl.kind == 2 || sl.sock == plat::kInvalidSock) {
            r.result = -1;
        } else if (!plat::bind_sock(sl.sock, addr, port)) {
            r.result = -1;
        } else {
            uint32_t ip;
            uint16_t bp = 0;
            plat::local_endpoint(sl.sock, &ip, &bp);
            r.result = 0;
            r.port = bp;
        }
    } else if (op == OP_LISTEN) {
        if (sl.kind != 0 || sl.sock == plat::kInvalidSock) {
            r.result = -1;
        } else if (!plat::listen_sock(sl.sock, arg > 0 ? arg : 5)) {
            r.result = -1;
        } else {
            sl.listening = true;
            sl.state = ST_LISTEN;
            r.result = 0;
        }
    } else if (op == OP_ACCEPT) {
        int newidx = arg;
        if (sl.sock == plat::kInvalidSock || !sl.listening ||
            newidx < 0 || newidx >= NSOCK) {
            r.result = -1;
        } else if (!plat::readable(sl.sock, RECV_WAIT_MS)) {
            r.result = -2;
        } else {
            uint32_t pip = 0;
            uint16_t pport = 0;
            plat::sock_t conn = plat::accept_sock(sl.sock, &pip, &pport);
            if (conn == plat::kInvalidSock) {
                r.result = -1;
            } else {
                Slot &nsl = slots_[newidx];
                close_slot(nsl);
                nsl.sock = conn;
                nsl.kind = 0;
                nsl.state = ST_ESTABLISHED;
                r.result = 0;
                r.aux = pip;
                r.port = pport;
                std::lock_guard<std::mutex> lk(stat_mtx_);
                slot_info_[newidx] = SlotInfo();
                slot_info_[newidx].used = true;
                slot_info_[newidx].kind = 0;
                slot_info_[newidx].state = ST_ESTABLISHED;
                slot_info_[newidx].peer_ip = pip;
                slot_info_[newidx].peer_port = pport;
            }
        }
    } else if (op == OP_HOSTINFO) {
        // ゲストの常駐時表示用に、このホストの代表 IPv4 を aux で返す
        // (8.8.8.8 へ送る場合に使われる送信元アドレス)
        r.result = 0;
        r.aux = plat::local_addr_for(0x08080808u);
    } else {
        r.result = -1;
    }

    if (op != OP_STATE)
        r.rlen = (uint32_t)r.payload.size();

    // 応答を書く (Python 版と同じ並び)
    std::vector<uint8_t> res(24);
    res[0] = 'H';
    res[1] = 'N';
    res[2] = op;
    res[3] = idx;
    put32(&res[4], seq);
    put32(&res[8], (uint32_t)(int32_t)r.result);
    put16(&res[12], r.port);
    put16(&res[14], 0);
    put32(&res[16], r.rlen);
    put32(&res[20], r.aux);
    res.insert(res.end(), r.payload.begin(), r.payload.end());
    plat::publish_file(tmp_path_, res_path_, res.data(), res.size());

    if (op != OP_STATE) {               // STATE は行ログに出さない
        char timestr[16];
        std::time_t t = std::time(nullptr);
        std::tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        std::strftime(timestr, sizeof(timestr), "%H:%M:%S", &tmv);
        char line[256];
        std::snprintf(line, sizeof(line), "[%s] seq=%u %s sock=%u -> "
                      "result=%ld len=%u aux=%u",
                      timestr, seq,
                      (op >= 1 && op <= 11) ? OP_NAMES[op] : "?",
                      idx, (long)r.result, r.rlen, r.aux);
        log_line(line);
    }
}

// ---------------- RAW (ICMP 代理) ----------------

long Engine::raw_send(int idx, Slot &sl, const std::vector<uint8_t> &payload,
                      uint32_t addr, uint16_t /*port*/)
{
    if (sl.proto != 0 && sl.proto != 1)
        return -1;                      // ICMP 以外の raw は未対応
    if (payload.empty())
        return -1;

    // IP ヘッダ付きかどうかを見分ける
    size_t ihl = 0;
    bool ip_style = false;
    uint32_t ip_dst = 0, ip_src = 0;
    if (payload.size() >= 20 && (payload[0] >> 4) == 4 &&
        payload[9] == 1) {
        size_t h = (payload[0] & 0x0F) * 4;
        if (h >= 20 && payload.size() >= h + 8) {
            ihl = h;
            ip_style = true;
            ip_src = be32(&payload[12]);
            ip_dst = be32(&payload[16]);
        }
    }
    if (std::getenv("HFSNET_DEBUG_RAW")) {
        // 実測調査用: ゲストが raw で送ってきた中身をそのまま記録する
        std::string hex = "RAW SEND payload:";
        char tmp[4];
        for (size_t i = 0; i < payload.size() && i < 96; i++) {
            std::snprintf(tmp, sizeof(tmp), " %02x", payload[i]);
            hex += tmp;
        }
        log_line(hex);
    }
    const uint8_t *icmp = payload.data() + ihl;
    size_t icmp_len = payload.size() - ihl;
    if (icmp_len < 8)
        return -1;
    if (icmp[0] != 8)
        return -1;                      // エコー要求以外は送れない
    uint16_t gid = be16(icmp + 4);
    uint16_t gseq = be16(icmp + 6);
    std::vector<uint8_t> data(icmp + 8, icmp + icmp_len);

    uint32_t dest = addr ? addr : (ip_dst ? ip_dst : sl.peer_ip);
    if (!dest)
        return -1;
    sl.raw_ip_style = ip_style;
    sl.raw_guest_src = ip_src;

    uint32_t gen;
    {
        std::lock_guard<std::mutex> lk(raw_->m);
        gen = raw_->slots[idx].gen;
    }
    uint32_t epoch = raw_->epoch.load();
    auto shared = raw_;
    std::thread([shared, idx, gen, epoch, dest, data, gid, gseq, ip_style,
                 ip_src]() {
        int rtt = 0;
        if (!plat::icmp_echo(dest, data.data(), (int)data.size(),
                             ICMP_TIMEOUT_MS, &rtt))
            return;
        // ゲストの識別子・シーケンス・データで応答を組み立てる
        std::vector<uint8_t> pkt;
        if (ip_style) {
            pkt.resize(20, 0);
            pkt[0] = 0x45;
            uint16_t tot = (uint16_t)(20 + 8 + data.size());
            put16(&pkt[2], tot);
            pkt[8] = 64;                // TTL
            pkt[9] = 1;                 // ICMP
            put32(&pkt[12], dest);
            put32(&pkt[16], ip_src ? ip_src : plat::local_addr_for(dest));
            put16(&pkt[10], inet_checksum(pkt.data(), 20));
        }
        size_t base = pkt.size();
        pkt.resize(base + 8 + data.size(), 0);
        pkt[base + 0] = 0;              // エコー応答
        pkt[base + 1] = 0;
        put16(&pkt[base + 4], gid);
        put16(&pkt[base + 6], gseq);
        if (!data.empty())
            std::memcpy(&pkt[base + 8], data.data(), data.size());
        put16(&pkt[base + 2], inet_checksum(&pkt[base], 8 + data.size()));
        std::lock_guard<std::mutex> lk(shared->m);
        if (shared->epoch.load() != epoch)
            return;
        auto &q = shared->slots[idx];
        if (q.gen != gen)
            return;
        q.q.push_back(RawPacket{std::move(pkt), dest});
    }).detach();
    return (long)payload.size();
}

std::pair<long, uint32_t> Engine::raw_recv(int idx, int maxlen)
{
    // 最大 RECV_WAIT_MS まで、応答キューに届くのを待つ
    uint64_t deadline = plat::now_ms() + RECV_WAIT_MS;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(raw_->m);
            auto &q = raw_->slots[idx].q;
            if (!q.empty()) {
                RawPacket p = std::move(q.front());
                q.pop_front();
                uint32_t src = p.src_ip;
                if ((int)p.bytes.size() > maxlen)
                    p.bytes.resize(maxlen);
                if (std::getenv("HFSNET_DEBUG_RAW")) {
                    std::string hex = "RAW RECV payload:";
                    char tmp[4];
                    for (size_t i = 0; i < p.bytes.size() && i < 96; i++) {
                        std::snprintf(tmp, sizeof(tmp), " %02x", p.bytes[i]);
                        hex += tmp;
                    }
                    log_line(hex);
                }
                raw_last_payload_ = std::move(p.bytes);
                return {(long)raw_last_payload_.size(), src};
            }
        }
        if (plat::now_ms() >= deadline)
            break;
        plat::sleep_ms(5);
    }
    raw_last_payload_.clear();
    return {-2, 0};
}
