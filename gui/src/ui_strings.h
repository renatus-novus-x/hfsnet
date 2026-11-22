// strings.h - UI 文字列の日英対応
// 言語は実行時に切り替えられる。文字列は毎フレーム引くので、
// 切り替えは即座に画面へ反映される。
#pragma once

enum class Lang { Ja, En };

enum class Str {
    Title,
    MenuFile, MenuQuit, MenuLanguage, MenuHelp, MenuAbout,
    Folder, Browse, Start, Stop,
    StatusRunning, StatusStopped,
    TxTotal, RxTotal, Rate, PerSec,
    HistoryTitle, SlotsTitle, LogTitle,
    ColSlot, ColKind, ColState, ColPeer, ColTx, ColRx,
    StatePolls, ClearLog,
    AboutText,
    DialogFailed, FontMissing,
    Count
};

namespace i18n {
void set_lang(Lang l);
Lang lang();
const char *s(Str id);
} // namespace i18n
