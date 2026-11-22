// strings.cpp - 日英文字列テーブル
#include "ui_strings.h"

#include <cstddef>

namespace {
Lang g_lang = Lang::Ja;

const char *JA[static_cast<size_t>(Str::Count)] = {
    "hfsnet ホスト",                       // Title
    "ファイル", "終了", "言語", "ヘルプ", "バージョン情報",
    "共有フォルダ", "参照...", "開始", "停止", "ポーリング間隔 (ミリ秒)",
    "動作中", "停止中",
    "送信 累計", "受信 累計", "速度", "B/秒",
    "トラフィック (直近60秒)", "ソケット", "ログ",
    "#", "種別", "状態", "接続先", "送信", "受信",
    "STATE ポーリング", "ログ消去",
    "hfsnet ホスト (GUI 版プロトタイプ)\n"
    "X68000 の hfsnet.x と共有フォルダ経由で通信し、\n"
    "実ネットワークへの中継と ICMP (ping) の代理を行います。\n"
    "使用ライブラリ等のライセンスは配布物の\n"
    "THIRD-PARTY-NOTICES.txt をご覧ください。",
    "フォルダ選択ダイアログを開けませんでした。パスを直接入力するか、"
    "フォルダを入力欄へドラッグ&ドロップしてください。",
    "日本語フォントが見つからないため、表示を English に切り替えました。"
    "日本語で使うには日本語フォント (例: fonts-noto-cjk) が必要です。",
};

const char *EN[static_cast<size_t>(Str::Count)] = {
    "hfsnet Host",
    "File", "Quit", "Language", "Help", "About",
    "Shared folder", "Browse...", "Start", "Stop", "Poll interval (ms)",
    "Running", "Stopped",
    "TX total", "RX total", "Rate", "B/s",
    "Traffic (last 60 s)", "Sockets", "Log",
    "#", "Kind", "State", "Peer", "TX", "RX",
    "STATE polls", "Clear log",
    "hfsnet Host (GUI prototype)\n"
    "Relays traffic for the X68000 hfsnet.x driver through a\n"
    "shared folder, and proxies ICMP (ping) requests.\n"
    "For third-party licenses, see THIRD-PARTY-NOTICES.txt\n"
    "in the distribution.",
    "Could not open the folder dialog. Type the path directly, or "
    "drag & drop a folder onto the path field.",
    "No Japanese font found, so the UI was switched to English. "
    "Install a Japanese font (e.g. fonts-noto-cjk) to use Japanese.",
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
