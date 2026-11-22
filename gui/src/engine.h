// engine.h - hfsnet ホスト側エンジン (GUI 非依存)
//
// 共有フォルダ内の HFSREQ.BIN を見張り、実ソケットを操作して
// HFSRES.BIN に結果を書く。Python 版 hfsnethost.py の C++ 移植で、
// ファイルプロトコルは同一。加えて SOCK_RAW (ICMP エコーの代理) と
// トラフィック計数・ログ・スナップショット (GUI 表示用) を持つ。
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct SlotInfo {
    bool used = false;
    int kind = 0;               // 0=TCP, 1=UDP, 2=RAW
    int state = 0;              // ST_* (ファイルプロトコルと同じ番号)
    uint32_t peer_ip = 0;       // ネットワークバイトオーダ
    uint16_t peer_port = 0;     // ホストバイトオーダ
    uint64_t tx = 0, rx = 0;    // ペイロードのバイト数
};

struct Snapshot {
    bool running = false;
    std::string folder;
    uint64_t tx_total = 0, rx_total = 0;
    uint64_t tx_rate = 0, rx_rate = 0;          // バイト/秒 (直近1秒の差分)
    // 直近60秒の1秒ごとの (tx, rx) 差分。先頭が古い。
    std::vector<std::pair<uint64_t, uint64_t>> history;
    uint64_t state_polls = 0;
    std::vector<SlotInfo> slots;
};

struct RawPacketQ;              // engine.cpp で定義 (エコー応答キュー)

class Engine {
public:
    Engine();
    ~Engine();

    // 監視を開始する。folder は UTF-8 パス。
    bool start(const std::string &folder);

    // 要求ファイルの監視間隔 (ミリ秒)。既定は 10。短くするとゲストの
    // 応答が速くなる代わりにホストの CPU 使用率が上がる。0 で待ちなし。
    // start() の前に呼ぶこと。
    void set_poll_ms(int ms);
    void stop();
    bool running() const { return running_.load(); }

    // ログ行の受け取り先 (STATE ポーリングは行にならず回数だけ数える)
    void set_log_callback(std::function<void(const std::string &)> cb);

    Snapshot snapshot();

    // ヘッドレス用: start() のあと止まるまでこのスレッドで待つ
    void wait();

private:
    struct Slot;
    void loop();
    void process(const std::vector<uint8_t> &raw);
    void log_line(const std::string &s);

    // スロット操作 (ワーカースレッドからのみ呼ぶ)
    void refresh_state(Slot &sl);
    void close_slot(Slot &sl);
    int recv_avail(Slot &sl);
    long raw_send(int idx, Slot &sl, const std::vector<uint8_t> &payload,
                  uint32_t addr, uint16_t port);
    std::pair<long, uint32_t> raw_recv(int idx, int maxlen);

    std::atomic<bool> running_{false};
    std::thread worker_;
    int poll_ms_ = 10;
    std::string folder_;
    std::string req_path_, res_path_, tmp_path_;
    std::vector<Slot> slots_;
    uint32_t last_seq_ = 0;
    bool have_seq_ = false;
    std::shared_ptr<RawPacketQ> raw_;
    std::vector<uint8_t> raw_last_payload_;

    std::mutex stat_mtx_;
    uint64_t tx_total_ = 0, rx_total_ = 0;
    uint64_t state_polls_ = 0;
    uint64_t last_sec_tx_ = 0, last_sec_rx_ = 0;
    uint64_t cur_rate_tx_ = 0, cur_rate_rx_ = 0;
    std::deque<std::pair<uint64_t, uint64_t>> history_;
    std::array<SlotInfo, 32> slot_info_;
    uint64_t last_history_ms_ = 0;

    std::function<void(const std::string &)> log_cb_;
    std::mutex log_mtx_;
};
