#!/usr/bin/env python3
# raw_test.py - C++ 版ホスト (hfsnet-host) の raw / ICMP エンドツーエンドテスト
#
# 疑似ゲスト (ファイルプロトコルを喋る) から raw ソケットを開き、
# ICMP エコー要求を 127.0.0.1 へ送って、ホスト側の代理送信で実際に
# 応答が返り、ゲストの識別子・シーケンス・データで組み立て直された
# エコー応答が RECV で受け取れることを確かめる。
#
# 使い方: python3 raw_test.py /path/to/hfsnet-host
#
# 注意: ホスト側で ICMP を打つため、通常の OS 権限で ping ソケット
# (SOCK_DGRAM / IPPROTO_ICMP) が使える環境が必要。使えない環境では
# 応答が届かず、このテストは FAIL になる (環境制約であり、ゲスト側の
# 実装不良とは限らない)。macOS / 通常の Linux ではそのまま通る。

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

REQ_HDR = struct.Struct(">2sBBIHHII")
RES_HDR = struct.Struct(">2sBBIiHHII")

OP_OPEN, OP_CLOSE, OP_CONNECT, OP_SEND, OP_RECV = 1, 2, 3, 4, 5
OP_STATE = 6
ST_ESTABLISHED = 4
SOCK_RAW_ARG = 2        # OP_OPEN の arg 欄に渡す種別 (elf2x68k の規約)
IPPROTO_ICMP = 1        # OP_OPEN の port 欄に渡すプロトコル番号

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name +
          (f" ({detail})" if detail else ""))
    if not cond:
        failures.append(name)


class SimGuest:
    """ゲスト側ドライバのファイル往復をそのまま真似る (udp_test.py と同じ)"""
    def __init__(self, folder):
        self.req = os.path.join(folder, "HFSREQ.BIN")
        self.res = os.path.join(folder, "HFSRES.BIN")
        self.seq = 0

    def rpc(self, op, sock=0, port=0, arg=0, addr=0, length=0, payload=b"",
            timeout=10.0):
        self.seq += 1
        raw = REQ_HDR.pack(b"HN", op, sock, self.seq, port, arg, addr,
                           length if payload == b"" else len(payload)) + payload
        if op == OP_RECV:
            raw = REQ_HDR.pack(b"HN", op, sock, self.seq, port, arg, addr,
                               length)
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


def ipint(s):
    return struct.unpack(">I", socket.inet_aton(s))[0]


def icmp_checksum(data):
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack(">%dH" % (len(data) // 2), data))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return (~s) & 0xFFFF


def main():
    if len(sys.argv) < 2:
        print("usage: raw_test.py /path/to/hfsnet-host")
        return 2
    hostbin = os.path.abspath(sys.argv[1])
    folder = tempfile.mkdtemp(prefix="hfsnet-raw-test-")
    env = dict(os.environ)
    env["HFSNET_DEBUG_RAW"] = "1"
    proc = subprocess.Popen([hostbin, "--headless", folder],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, env=env)
    try:
        time.sleep(0.5)
        g = SimGuest(folder)

        # raw ソケットを開く (種別=RAW は arg 欄、プロトコルは port 欄)
        r, *_ = g.rpc(OP_OPEN, sock=0, port=IPPROTO_ICMP, arg=SOCK_RAW_ARG)
        check("OPEN raw (ICMP)", r == 0, f"result={r}")

        # ping.x と同じ形のエコー要求: 識別子 0x0080、データは
        # clock_t 4 バイト + 位置パターン 52 バイト = 56 バイト
        ident, seqno = 0x0080, 7
        data = struct.pack(">I", 123456) + bytes(range(4, 56))
        pkt = struct.pack(">BBHHH", 8, 0, 0, ident, seqno) + data
        csum = icmp_checksum(pkt)
        pkt = struct.pack(">BBHHH", 8, 0, csum, ident, seqno) + data

        r, *_ = g.rpc(OP_SEND, sock=0, addr=ipint("127.0.0.1"), payload=pkt)
        check("SEND echo request", r == len(pkt), f"result={r}")

        # 応答がキューに届くまで STATE をポーリングする (ping.x と同じ)
        avail = 0
        deadline = time.time() + 10.0
        while time.time() < deadline:
            r, _port, rlen, _aux, _p = g.rpc(OP_STATE, sock=0)
            if r == ST_ESTABLISHED and rlen > 0:
                avail = rlen
                break
            time.sleep(0.1)
        check("STATE reports queued reply", avail == len(pkt),
              f"avail={avail}")

        if avail > 0:
            r, rport, _rlen, aux, payload = g.rpc(OP_RECV, sock=0,
                                                  length=2048)
            check("RECV echo reply", r == len(pkt), f"result={r}")
            if r >= 8:
                typ, code, _c, rid, rseq = struct.unpack(
                    ">BBHHH", payload[:8])
                check("reply is echo reply (type 0)", typ == 0 and code == 0,
                      f"type={typ} code={code}")
                check("reply id matches guest", rid == ident,
                      f"id={rid:#x}")
                check("reply seq matches guest", rseq == seqno,
                      f"seq={rseq}")
                check("reply data matches guest", payload[8:r] == data)
                check("reply source is 127.0.0.1",
                      aux == ipint("127.0.0.1"), f"aux={aux:#x}")

        r, *_ = g.rpc(OP_CLOSE, sock=0)
        check("CLOSE", r == 0, f"result={r}")
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
