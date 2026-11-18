// platform_mac.cpp - macOS 用のプラットフォーム実装
// 実装は POSIX 共通部 (platform_posix.inc) を使う。SIGPIPE の扱いなど
// macOS 固有の分岐は共通部の側に閉じ込めてある。
#include "platform_posix.inc"
