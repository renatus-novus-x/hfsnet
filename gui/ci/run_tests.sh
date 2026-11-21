#!/bin/sh
# run_tests.sh - C++ 版ホストに対する回帰テスト一式 (CI / ローカル共用)
#
# 使い方: gui/ci/run_tests.sh /path/to/hfsnet-host
#
# リポジトリの test/sim_test.py と test/udp_test.py は Python 版ホスト
# 用なので、相方の起動部分だけを書き換えたコピーを一時フォルダで
# 実行する (リポジトリ側のテスト自体は変更しない)。加えて raw / ICMP
# のエンドツーエンド (gui/ci/raw_test.py) を実行する。
set -e
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
# Windows (Git Bash) ではネイティブ Python が MSYS 形式のパスを解釈できない
# ため、cygpath があれば Windows 形式 (スラッシュ区切り) に変換する。
if command -v cygpath >/dev/null 2>&1; then
    BIN=$(cygpath -m "$BIN")
fi
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
# WORK は生成する Python スクリプト内にもパスとして埋め込まれるため、
# BIN と同様に Windows 形式へ変換しておく。
if command -v cygpath >/dev/null 2>&1; then
    WORK=$(cygpath -m "$WORK")
fi

cat > "$WORK/host_wrapper.py" <<EOF
import os, sys
os.execv("$BIN", ["$BIN", "--headless", sys.argv[1]])
EOF

sed "s|^HOSTAPP = .*|HOSTAPP = \"$WORK/host_wrapper.py\"|" \
    "$ROOT/test/sim_test.py" > "$WORK/sim_test.py"
python3 "$WORK/sim_test.py"

sed "s|proc = subprocess.Popen(\[sys.executable, wrapper, folder\],|proc = subprocess.Popen([\"$BIN\", \"--headless\", folder],|" \
    "$ROOT/test/udp_test.py" > "$WORK/udp_test.py"
python3 "$WORK/udp_test.py"

python3 "$ROOT/gui/ci/raw_test.py" "$BIN"
