#!/usr/bin/env python3
# broadcast_test.py - hfsnet ホストの UDP ブロードキャスト送信テスト
#
# 疑似ゲスト (ファイルプロトコルを喋る) から、ホスト側相方経由で
# ブロードキャストアドレス (255.255.255.255) へ UDP 送信し、
# (1) ゲスト側の SEND が成功を返すこと (result = 送信バイト数)、
# (2) 同一マシンで待ち受けている受信ソケットに実際に届くこと
# を確かめる。SO_BROADCAST が無いホストでは (1) が -1 になる
# (macOS で OMNIMSG.X の UDP チャット送信が失敗した不具合の回帰テスト)。
#
# 使い方: python3 broadcast_test.py
# 相方は HOSTAPP (既定は ../host/hfsnethost.py) をサブプロセスで起動する。
# C++ ヘッドレス版を試すときは、build-and-test.sh の host_wrapper.py と
# 同じ要領のラッパを HOSTAPP に差し替えたコピーで実行する。
#
# 注意: Muse の VM では sendto がサンドボックスに EPERM で弾かれるため
# このテストは失敗する。実機 (macOS / Windows 等) で実行すること。

import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOSTAPP = os.path.join(HERE, "..", "host", "hfsnethost.py")

REQ_HDR = struct.Struct(">2sBBIHHII")
RES_HDR = struct.Struct(">2sBBIiHHII")

OP_OPEN, OP_CLOSE, OP_SEND = 1, 2, 4
SOCK_DGRAM_ARG = 1      # OP_OPEN の arg 欄に渡す種別 (elf2x68k の規約)

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (f" ({detail})" if detail else ""))
    if not cond:
        failures.append(name)


class SimGuest:
    """ゲスト側ドライバのファイル往復をそのまま真似る (udp_test.py と同じ)"""
    def __init__(self, folder):
        self.req = os.path.join(folder, "HFSREQ.BIN")
        self.res = os.path.join(folder, "HFSRES.BIN")
        self.seq = 0

    def rpc(self, op, sock=0, port=0, arg=0, addr=0, payload=b"",
            timeout=10.0):
        self.seq += 1
        raw = REQ_HDR.pack(b"HN", op, sock, self.seq, port, arg, addr,
                           len(payload)) + payload
        tmp = self.req + ".tmp"
        with open(tmp, "wb") as f:
            f.write(raw)
        os.replace(tmp, self.req)
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                with open(self.res, "rb") as f:
                    data = f.read()
            except OSError:
                data = b""
            if len(data) >= RES_HDR.size:
                magic, rop, rsock, rseq, result, rport, _f, rlen, aux = \
                    RES_HDR.unpack(data[:RES_HDR.size])
                if magic == b"HN" and rseq == self.seq:
                    return result, rport, rlen, aux, data[RES_HDR.size:]
            time.sleep(0.005)
        raise TimeoutError(f"op={op} seq={self.seq}")


class BroadcastReceiver:
    """0.0.0.0 で待ち受け、届いたデータグラムを集める"""
    def __init__(self, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.sock.settimeout(0.2)
        self.stop = False
        self.received = []
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self.stop:
            try:
                data, peer = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            self.received.append((data, peer))

    def start(self):
        self.thread.start()

    def close(self):
        self.stop = True
        self.thread.join(timeout=2)
        self.sock.close()


def ipint(s):
    return struct.unpack(">I", socket.inet_aton(s))[0]


def main():
    port = 18781
    rx = BroadcastReceiver(port)
    rx.start()
    folder = tempfile.mkdtemp(prefix="hfsnet-bcast-test-")
    proc = subprocess.Popen([sys.executable, HOSTAPP, folder],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    try:
        time.sleep(0.5)
        g = SimGuest(folder)
        r, *_ = g.rpc(OP_OPEN, sock=0, arg=SOCK_DGRAM_ARG)
        check("OPEN (DGRAM)", r == 0)

        msg = b"HFSNET-BROADCAST-TEST"
        r, *_ = g.rpc(OP_SEND, sock=0, port=port,
                      addr=ipint("255.255.255.255"), payload=msg)
        check("SEND to 255.255.255.255", r == len(msg), f"result={r}")

        deadline = time.time() + 3.0
        got = None
        while time.time() < deadline:
            for data, peer in rx.received:
                if data == msg:
                    got = peer
                    break
            if got:
                break
            time.sleep(0.05)
        check("broadcast datagram received", got is not None,
              f"from={got}" if got else "nothing received")

        r, *_ = g.rpc(OP_CLOSE, sock=0)
        check("CLOSE (DGRAM)", r == 0)
    finally:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        rx.close()
        if failures:
            print("---- host output (tail) ----")
            print("\n".join((out or "").splitlines()[-20:]))
    print("FAILURES:", failures if failures else "none")
    print("ALL PASS" if not failures else "SOME FAILED")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
