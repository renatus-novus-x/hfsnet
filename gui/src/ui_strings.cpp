// strings.cpp - 日英文字列テーブル
#include "ui_strings.h"

#include <cstddef>

namespace {
Lang g_lang = Lang::Ja;

const char *JA[static_cast<size_t>(Str::Count)] = {
    "hfsnet ホスト",                       // Title
    "ファイル", "終了", "言語", "ヘルプ", "バージョン情報",
    "共有フォルダ", "参照...", "開始", "停止",
    "動作中", "停止中",
    "送信 累計", "受信 累計", "速度", "B/秒",
    "トラフィック (直近60秒)", "ソケット", "ログ",
    "#", "種別", "状態", "接続先", "送信", "受信",
    "STATE ポーリング", "ログ消去",
    "hfsnet ホスト (GUI 版プロトタイプ)\n"
    "X68000 の hfsnet.x と共有フォルダ経由で通信し、\n"
    "実ネットワークへの中継と ICMP (ping) の代理を行います。",
};

const char *EN[static_cast<size_t>(Str::Count)] = {
    "hfsnet Host",
    "File", "Quit", "Language", "Help", "About",
    "Shared folder", "Browse...", "Start", "Stop",
    "Running", "Stopped",
    "TX total", "RX total", "Rate", "B/s",
    "Traffic (last 60 s)", "Sockets", "Log",
    "#", "Kind", "State", "Peer", "TX", "RX",
    "STATE polls", "Clear log",
    "hfsnet Host (GUI prototype)\n"
    "Relays traffic for the X68000 hfsnet.x driver through a\n"
    "shared folder, and proxies ICMP (ping) requests.",
};
} // namespace

namespace i18n {
void set_lang(Lang l) { g_lang = l; }
Lang lang() { return g_lang; }
const char *s(Str id)
{
    return (g_lang == Lang::Ja ? JA : EN)[static_cast<size_t>(id)];
}
} // namespace i18n
