#!/usr/bin/env python3
# sim_test.py - hfsnethost.py のエンドツーエンドテスト
#
# 疑似ゲスト (ファイルプロトコルを喋る) から、ホスト側相方経由で
# ローカルの TCP エコーサーバおよび待ち受けソケットと通信できることを確かめる。
#
# 使い方: python3 sim_test.py   (相方をサブプロセスで起動して自動で終わる)

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
ST_ESTABLISHED, ST_LISTEN = 4, 1

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (f" ({detail})" if detail else ""))
    if not cond:
        failures.append(name)


class SimGuest:
    """ゲスト側ドライバのファイル往復をそのまま真似る"""
    def __init__(self, folder):
        self.req = os.path.join(folder, "HFSREQ.BIN")
        self.res = os.path.join(folder, "HFSRES.BIN")
        self.seq = 0

    def rpc(self, op, sock=0, port=0, arg=0, addr=0, length=0, payload=b"",
            timeout=10.0):
        self.seq += 1
        raw = REQ_HDR.pack(b"HN", op, sock, self.seq, port, arg, addr,
                           length if payload == b"" else len(payload)) + payload
        # RECV のときは length が受け取り上限、payload は空
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


def echo_server(port):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    conn, _ = srv.accept()
    while True:
        data = conn.recv(4096)
        if not data:
            break
        conn.sendall(data)
    conn.close()
    srv.close()


def ipint(s):
    return struct.unpack(">I", socket.inet_aton(s))[0]


def main():
    folder = tempfile.mkdtemp(prefix="hfsnet-test-")
    proc = subprocess.Popen([sys.executable, HOSTAPP, folder],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    try:
        time.sleep(0.5)
        g = SimGuest(folder)

        # エコーサーバを立てる
        echo_port = 18777
        th = threading.Thread(target=echo_server, args=(echo_port,),
                              daemon=True)
        th.start()
        time.sleep(0.2)

        # RESOLVE
        r, _, _, aux, _ = g.rpc(OP_RESOLVE, payload=b"localhost\x00")
        check("RESOLVE localhost", r == 0 and aux == ipint("127.0.0.1"),
              f"aux={aux:#x}")

        # OPEN / CONNECT
        r, *_ = g.rpc(OP_OPEN, sock=0)
        check("OPEN", r == 0)
        r, *_ = g.rpc(OP_CONNECT, sock=0, port=echo_port,
                      addr=ipint("127.0.0.1"))
        check("CONNECT (即時または接続中)", r in (0, 1), f"result={r}")
        if r == 1:
            for _ in range(50):
                r, *_ = g.rpc(OP_STATE, sock=0)
                if r == ST_ESTABLISHED:
                    break
                time.sleep(0.1)
            check("STATE -> ESTABLISHED", r == ST_ESTABLISHED, f"state={r}")

        # SEND / RECV (エコー往復)
        msg = b"Hello, hfsnet!"
        r, *_ = g.rpc(OP_SEND, sock=0, payload=msg)
        check("SEND", r == len(msg), f"sent={r}")
        got = b""
        for _ in range(50):
            r, _, _, _, payload = g.rpc(OP_RECV, sock=0, length=2048)
            if r > 0:
                got = payload
                break
            if r == 0:
                break
            time.sleep(0.05)
        check("RECV echo", got == msg, f"got={got!r}")

        # 大きめのデータ (チャンク境界 2048 をまたがない範囲で複数回)
        msg2 = b"x" * 1500
        r, *_ = g.rpc(OP_SEND, sock=0, payload=msg2)
        got = b""
        deadline = time.time() + 5
        while len(got) < len(msg2) and time.time() < deadline:
            r, _, _, _, payload = g.rpc(OP_RECV, sock=0, length=2048)
            if r > 0:
                got += payload
        check("SEND/RECV 1500 bytes", r == len(msg2) and got == msg2,
              f"sent={r} got={len(got)}")

        # CLOSE
        r, *_ = g.rpc(OP_CLOSE, sock=0)
        check("CLOSE", r == 0)

        # サーバ側: BIND / LISTEN / ACCEPT
        srv_port = 18778
        r, *_ = g.rpc(OP_OPEN, sock=1)
        check("OPEN (server)", r == 0)
        r, *_ = g.rpc(OP_BIND, sock=1, port=srv_port, addr=ipint("127.0.0.1"))
        check("BIND", r == 0)
        r, *_ = g.rpc(OP_LISTEN, sock=1, arg=5)
        check("LISTEN", r == 0)
        r, *_ = g.rpc(OP_STATE, sock=1)
        check("STATE -> LISTEN", r == ST_LISTEN, f"state={r}")

        cli = socket.create_connection(("127.0.0.1", srv_port))
        r = -2
        for _ in range(50):
            r, rport, _, aux, _ = g.rpc(OP_ACCEPT, sock=1, arg=2)
            if r == 0:
                break
            time.sleep(0.05)
        check("ACCEPT", r == 0, f"peer port={rport}")
        cli.sendall(b"ping from client")
        got = b""
        for _ in range(50):
            r, _, _, _, payload = g.rpc(OP_RECV, sock=2, length=2048)
            if r > 0:
                got = payload
                break
            time.sleep(0.05)
        check("RECV on accepted socket", got == b"ping from client",
              f"got={got!r}")
        r, *_ = g.rpc(OP_SEND, sock=2, payload=b"pong")
        cli.settimeout(3)
        check("SEND on accepted socket", cli.recv(100) == b"pong")
        cli.close()
        g.rpc(OP_CLOSE, sock=2)
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
