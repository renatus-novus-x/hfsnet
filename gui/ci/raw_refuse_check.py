#!/usr/bin/env python3
# raw_refuse_check.py - Python 版ホスト (host/hfsnethost.py) が
# OP_OPEN arg=2 (SOCK_RAW) を明示的に拒否することを確かめる。
# あわせて arg=0 (TCP) / arg=1 (UDP) は従来どおり開けることも確認する。
#
# 使い方: python3 raw_refuse_check.py
# 終了コード: 0 = 期待どおり, 1 = 不一致

import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "gui", "ci"))

from raw_test import SimGuest, OP_OPEN, OP_CLOSE, IPPROTO_ICMP, SOCK_RAW_ARG

HOSTAPP = os.path.join(ROOT, "host", "hfsnethost.py")

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name +
          (f" ({detail})" if detail else ""))
    if not cond:
        failures.append(name)


def main():
    folder = tempfile.mkdtemp(prefix="hfsnet-raw-refuse-")
    proc = subprocess.Popen([sys.executable, HOSTAPP, folder],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    try:
        time.sleep(0.5)
        g = SimGuest(folder)

        # RAW は拒否される (result != 0)
        r, *_ = g.rpc(OP_OPEN, sock=0, port=IPPROTO_ICMP, arg=SOCK_RAW_ARG)
        check("OPEN raw refused", r != 0, f"result={r}")

        # TCP は開ける
        r, *_ = g.rpc(OP_OPEN, sock=1, arg=0)
        check("OPEN tcp ok", r == 0, f"result={r}")
        r, *_ = g.rpc(OP_CLOSE, sock=1)
        check("CLOSE tcp", r == 0, f"result={r}")

        # UDP は開ける
        r, *_ = g.rpc(OP_OPEN, sock=2, arg=1)
        check("OPEN udp ok", r == 0, f"result={r}")
        r, *_ = g.rpc(OP_CLOSE, sock=2)
        check("CLOSE udp", r == 0, f"result={r}")
    finally:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
    if failures:
        print(f"\n{len(failures)} FAILURES: {failures}")
        print("---- host output ----")
        print(out)
        return 1
    print("\nALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
