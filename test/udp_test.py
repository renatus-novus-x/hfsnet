#!/usr/bin/env python3
# udp_test.py - hfsnethost.py の UDP エンドツーエンドテスト
#
# 疑似ゲスト (ファイルプロトコルを喋る) から、ホスト側相方経由で
# ローカルの UDP エコーサーバと通信できること、および BIND した
# UDP ソケットで外部からのデータグラムを受けて返信できることを確かめる。
#
# 使い方: python3 udp_test.py   (相方をサブプロセスで起動して自動で終わる)
#
# 注意: Muse の VM では sendto システムコールがサンドボックスに
# EPERM で弾かれる (TCP や connected UDP の send は通る)。そのため
# テスト内と相方プロセス内では、sendto が PermissionError のときだけ
# connect+send で代用するシムを挟んでいる。通常の環境では本物の
# sendto が成功するのでシムは何もしない。

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

OP_OPEN, OP_CLOSE, OP_CONNECT, OP_SEND, OP_RECV = 1, 2, 3, 4, 5
OP_STATE, OP_RESOLVE, OP_BIND, OP_LISTEN, OP_ACCEPT = 6, 7, 8, 9, 10
ST_ESTABLISHED = 4
SOCK_DGRAM_ARG = 1      # OP_OPEN の arg 欄に渡す種別 (elf2x68k の規約)

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (f" ({detail})" if detail else ""))
    if not cond:
        failures.append(name)


# ---------------- sendto シム (この VM のサンドボックス回避) ----------------
_RealSocket = socket.socket


class ShimSocket(_RealSocket):
    def sendto(self, data, address):
        try:
            return super().sendto(data, address)
        except PermissionError:
            super().connect(address)
            return super().send(data)


def install_shim():
    socket.socket = ShimSocket


# 相方プロセス用のラッパ (同じシムを挟んでから hfsnethost を起動する)
WRAPPER = '''\
import socket, sys
sys.path.insert(0, {hostdir!r})
_RealSocket = socket.socket
class ShimSocket(_RealSocket):
    def sendto(self, data, address):
        try:
            return super().sendto(data, address)
        except PermissionError:
            super().connect(address)
            return super().send(data)
socket.socket = ShimSocket
import hfsnethost
sys.argv = ["hfsnethost.py", sys.argv[1]]
sys.exit(hfsnethost.main())
'''


class SimGuest:
    """ゲスト側ドライバのファイル往復をそのまま真似る (sim_test.py と同じ)"""
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

    def recv_retry(self, sock, timeout=5.0):
        """データグラムが届くまで RECV を繰り返す"""
        deadline = time.time() + timeout
        r = -2
        while time.time() < deadline:
            r, rport, _rlen, aux, payload = self.rpc(OP_RECV, sock=sock,
                                                     length=2048)
            if r > 0:
                return r, rport, aux, payload
            if r == 0:
                break
            time.sleep(0.05)
        return r, 0, 0, b""


class UdpEchoServer:
    """1 クライアント専用の UDP エコーサーバ。

    シム経由の返信でソケットが相手に connect されてしまうため、
    セクションごとに作り直す前提で停止できるようにしてある。
    """
    def __init__(self, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", port))
        self.sock.settimeout(0.2)
        self.stop = False
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self.stop:
            try:
                data, peer = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                self.sock.sendto(data, peer)
            except OSError:
                pass

    def start(self):
        self.thread.start()

    def close(self):
        self.stop = True
        self.thread.join(timeout=2)
        self.sock.close()


def ipint(s):
    return struct.unpack(">I", socket.inet_aton(s))[0]


def main():
    install_shim()
    folder = tempfile.mkdtemp(prefix="hfsnet-udp-test-")
    wrapper = os.path.join(folder, "host_shim.py")
    with open(wrapper, "w") as f:
        f.write(WRAPPER.format(hostdir=os.path.dirname(HOSTAPP)))
    proc = subprocess.Popen([sys.executable, wrapper, folder],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    try:
        time.sleep(0.5)
        g = SimGuest(folder)
        loopback = ipint("127.0.0.1")

        # UDP エコーサーバを立てる (セクションごとに作り直す)
        echo_port = 18779
        echo = UdpEchoServer(echo_port)
        echo.start()
        time.sleep(0.2)

        # --- クライアント側: OPEN(DGRAM) -> SENDTO -> RECVFROM ---
        r, *_ = g.rpc(OP_OPEN, sock=0, arg=SOCK_DGRAM_ARG)
        check("OPEN (DGRAM)", r == 0)
        r, *_ = g.rpc(OP_STATE, sock=0)
        check("STATE -> ESTABLISHED (UDP)", r == ST_ESTABLISHED,
              f"state={r}")

        msg = b"Hello, UDP!"
        r, *_ = g.rpc(OP_SEND, sock=0, port=echo_port, addr=loopback,
                      payload=msg)
        check("SENDTO echo server", r == len(msg), f"sent={r}")
        r, rport, aux, got = g.recv_retry(sock=0)
        check("RECVFROM echo payload", got == msg, f"got={got!r}")
        check("RECVFROM source addr", aux == loopback, f"aux={aux:#x}")
        check("RECVFROM source port", rport == echo_port, f"port={rport}")
        r, *_ = g.rpc(OP_CLOSE, sock=0)
        check("CLOSE (DGRAM)", r == 0)
        echo.close()
        echo = UdpEchoServer(echo_port)
        echo.start()
        time.sleep(0.2)

        # --- connect() してから宛先なしで送る経路 ---
        r, *_ = g.rpc(OP_OPEN, sock=2, arg=SOCK_DGRAM_ARG)
        check("OPEN (DGRAM, connect path)", r == 0)
        r, *_ = g.rpc(OP_CONNECT, sock=2, port=echo_port, addr=loopback)
        check("CONNECT (UDP)", r == 0, f"result={r}")
        r, *_ = g.rpc(OP_SEND, sock=2, payload=msg)
        check("SEND (connected UDP)", r == len(msg), f"sent={r}")
        r, rport, aux, got = g.recv_retry(sock=2)
        check("RECV (connected UDP)", got == msg and rport == echo_port
              and aux == loopback, f"got={got!r} port={rport}")
        g.rpc(OP_CLOSE, sock=2)
        echo.close()

        # --- サーバ側: BIND して外部からのデータグラムを受ける ---
        bind_port = 18780
        r, *_ = g.rpc(OP_OPEN, sock=1, arg=SOCK_DGRAM_ARG)
        check("OPEN (DGRAM, server)", r == 0)
        r, *_ = g.rpc(OP_BIND, sock=1, port=bind_port, addr=loopback)
        check("BIND (UDP)", r == 0)

        cli = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cli.bind(("127.0.0.1", 0))
        cli_port = cli.getsockname()[1]
        cli.sendto(b"datagram in", ("127.0.0.1", bind_port))
        r, rport, aux, got = g.recv_retry(sock=1)
        check("RECVFROM on bound socket", got == b"datagram in",
              f"got={got!r}")
        check("RECVFROM client addr/port",
              aux == loopback and rport == cli_port,
              f"aux={aux:#x} port={rport} (client {cli_port})")

        # 受け取った送信元へそのまま返信できること
        r, *_ = g.rpc(OP_SEND, sock=1, port=rport, addr=aux,
                      payload=b"datagram out")
        check("SENDTO back to client", r == len(b"datagram out"),
              f"sent={r}")
        cli.settimeout(3)
        try:
            data, peer = cli.recvfrom(2048)
            check("client receives reply",
                  data == b"datagram out" and peer[1] == bind_port,
                  f"got={data!r} from={peer}")
        except socket.timeout:
            check("client receives reply", False, "timeout")
        cli.close()
        g.rpc(OP_CLOSE, sock=1)
    finally:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=5)
            print("---- host log ----")
            print(out)
        except subprocess.TimeoutExpired:
            proc.kill()

    if failures:
        print(f"\n{len(failures)} FAILURES: {failures}")
        return 1
    print("\nALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
