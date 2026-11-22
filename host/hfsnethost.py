#!/usr/bin/env python3
# hfsnethost.py - hfsnet のホスト側相方プログラム
#
# 共有フォルダ内の HFSREQ.BIN を見張り、実際の TCP ソケットを操作して
# 結果を HFSRES.BIN に書く。Python 3 標準ライブラリのみで動く。
# ファイルプロトコルの仕様は ../README.md を参照。
#
# 使い方: python3 hfsnethost.py <共有フォルダのパス>

import os
import select
import socket
import struct
import sys
import time

REQ_HDR = struct.Struct(">2sBBIHHII")   # 20 バイト
RES_HDR = struct.Struct(">2sBBIiHHII")  # 24 バイト

OP_OPEN, OP_CLOSE, OP_CONNECT, OP_SEND, OP_RECV = 1, 2, 3, 4, 5
OP_STATE, OP_RESOLVE, OP_BIND, OP_LISTEN, OP_ACCEPT = 6, 7, 8, 9, 10
OP_SHUTDOWN = 11
OP_HOSTINFO = 12
OP_NAMES = {1: "OPEN", 2: "CLOSE", 3: "CONNECT", 4: "SEND", 5: "RECV",
            6: "STATE", 7: "RESOLVE", 8: "BIND", 9: "LISTEN", 10: "ACCEPT",
            11: "SHUTDOWN", 12: "HOSTINFO"}

ST_CLOSED, ST_LISTEN, ST_SYN_SENT, ST_SYN_RECEIVED = 0, 1, 2, 3
ST_ESTABLISHED, ST_CLOSE_WAIT = 4, 5

NSOCK = 32
RECV_WAIT = 0.3     # RECV/ACCEPT でデータ・接続を待つ上限 (秒)
SEND_WAIT = 3.0     # SEND で送信可能になるのを待つ上限 (秒)


def ipstr(n):
    return socket.inet_ntoa(struct.pack(">I", n))


class Slot:
    def __init__(self):
        self.sock = None        # socket オブジェクト
        self.state = ST_CLOSED
        self.listening = False
        self.eof = False        # 相手が閉じて読み切った
        self.udp = False        # True ならデータグラムソケット


class Host:
    def __init__(self, folder):
        self.folder = folder
        self.req_path = os.path.join(folder, "HFSREQ.BIN")
        self.res_path = os.path.join(folder, "HFSRES.BIN")
        self.tmp_path = os.path.join(folder, "HFSRES.TMP")
        self.slots = [Slot() for _ in range(NSOCK)]
        self.last_seq = None

    # ---------------- ソケット状態の追随 ----------------
    def refresh_state(self, sl):
        """ノンブロッキングソケットの状態を select で判定する"""
        if sl.sock is None:
            sl.state = ST_CLOSED
            return
        if sl.listening:
            sl.state = ST_LISTEN
            return
        if sl.udp:
            sl.state = ST_ESTABLISHED   # UDP は接続という概念がない
            return
        if sl.state == ST_SYN_SENT:
            _, w, _ = select.select([], [sl.sock], [], 0)
            if w:
                err = sl.sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
                sl.state = ST_ESTABLISHED if err == 0 else ST_CLOSED
                if err != 0:
                    self.close_slot(sl)
            return
        if sl.state in (ST_ESTABLISHED, ST_CLOSE_WAIT):
            if sl.eof:
                sl.state = ST_CLOSE_WAIT
                return
            r, _, _ = select.select([sl.sock], [], [], 0)
            if r:
                try:
                    data = sl.sock.recv(1, socket.MSG_PEEK)
                except (BlockingIOError, InterruptedError):
                    data = None
                except OSError:
                    data = b""
                if data == b"":
                    sl.state = ST_CLOSE_WAIT

    def close_slot(self, sl):
        if sl.sock is not None:
            try:
                sl.sock.close()
            except OSError:
                pass
        sl.sock = None
        sl.state = ST_CLOSED
        sl.listening = False
        sl.eof = False
        sl.udp = False

    def recv_avail(self, sl):
        if sl.sock is None or sl.listening:
            return 0
        try:
            data = sl.sock.recv(65536, socket.MSG_PEEK)
            return len(data)
        except (BlockingIOError, InterruptedError):
            return 0
        except OSError:
            return 0

    # ---------------- 各操作 ----------------
    def op_open(self, idx, arg=0):
        # arg はソケット種別 (elf2x68k の規約: 0 = SOCK_STREAM, 1 = SOCK_DGRAM,
        # 2 = SOCK_RAW)。RAW (ICMP) は C++ 版ホストのみ対応するため、ここでは
        # 明示的に拒否する (黙って TCP に化けると ping.x の失敗の仕方が
        # 分かりにくくなる)。
        sl = self.slots[idx]
        self.close_slot(sl)
        if arg == 2:
            return -1, 0, 0, b""
        if arg == 1:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sl.udp = True
            sl.state = ST_ESTABLISHED   # UDP は開いた時点で送受信できる
        else:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sl.state = ST_CLOSED
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.setblocking(False)
        sl.sock = s
        return 0, 0, 0, b""

    def op_close(self, idx):
        self.close_slot(self.slots[idx])
        return 0, 0, 0, b""

    def op_shutdown(self, idx, how):
        sl = self.slots[idx]
        if sl.sock is None:
            return -1, 0, 0, b""
        hows = (socket.SHUT_RD, socket.SHUT_WR, socket.SHUT_RDWR)
        try:
            sl.sock.shutdown(hows[how] if 0 <= how <= 2
                             else socket.SHUT_RDWR)
        except OSError:
            return -1, 0, 0, b""
        return 0, 0, 0, b""

    def op_connect(self, idx, addr, port):
        sl = self.slots[idx]
        if sl.sock is None:
            return -1, 0, 0, b"", 0
        try:
            err = sl.sock.connect_ex((ipstr(addr), port))
        except OSError:
            return -1, 0, 0, b"", 0
        # ローカル側エンドポイント (getsockname 用) を aux/port で返す。
        # ゲストが FTP の PORT などを組み立てるのに必要
        la = sl.sock.getsockname()
        la_ip = struct.unpack(">I", socket.inet_aton(la[0]))[0]
        if err == 0:
            sl.state = ST_ESTABLISHED
            return 0, 0, la_ip, b"", la[1]
        if err in (36, 115, 10035, 11):     # EINPROGRESS / EWOULDBLOCK / EAGAIN
            sl.state = ST_SYN_SENT
            return 1, 0, la_ip, b"", la[1]
        sl.state = ST_CLOSED
        return -1, 0, 0, b"", 0

    def op_send(self, idx, payload, addr=0, port=0):
        sl = self.slots[idx]
        if sl.sock is None:
            return -1, 0, 0, b""
        if sl.udp:
            # 宛先指定があれば sendto、無ければ connect 済みの相手へ send
            try:
                if addr or port:
                    n = sl.sock.sendto(payload, (ipstr(addr), port))
                else:
                    n = sl.sock.send(payload)
            except (BlockingIOError, InterruptedError):
                return -2, 0, 0, b""
            except OSError:
                return -1, 0, 0, b""
            return n, 0, 0, b""
        self.refresh_state(sl)
        if sl.state not in (ST_ESTABLISHED, ST_CLOSE_WAIT):
            return -1, 0, 0, b""
        sent = 0
        deadline = time.time() + SEND_WAIT
        while sent < len(payload):
            remain = deadline - time.time()
            if remain <= 0:
                break
            _, w, _ = select.select([], [sl.sock], [], remain)
            if not w:
                break
            try:
                n = sl.sock.send(payload[sent:])
            except (BlockingIOError, InterruptedError):
                continue
            except OSError:
                return (-1 if sent == 0 else sent), 0, 0, b""
            if n == 0:
                break
            sent += n
        if sent == 0 and len(payload) > 0:
            return -2, 0, 0, b""
        return sent, 0, 0, b""

    def op_recv(self, idx, maxlen):
        # 戻り値は (result, rlen, aux, payload, extra_port)。
        # UDP では送信元 IP を aux、送信元ポートを extra_port に載せる。
        sl = self.slots[idx]
        if sl.sock is None:
            return -1, 0, 0, b"", 0
        if sl.udp:
            r, _, _ = select.select([sl.sock], [], [], RECV_WAIT)
            if not r:
                return -2, 0, 0, b"", 0
            try:
                data, peer = sl.sock.recvfrom(maxlen)
            except (BlockingIOError, InterruptedError):
                return -2, 0, 0, b"", 0
            except OSError:
                return -1, 0, 0, b"", 0
            peer_ip = struct.unpack(">I", socket.inet_aton(peer[0]))[0]
            return len(data), 0, peer_ip, data, peer[1]
        if sl.eof:
            return 0, 0, 0, b"", 0
        r, _, _ = select.select([sl.sock], [], [], RECV_WAIT)
        if not r:
            self.refresh_state(sl)
            if sl.state == ST_CLOSE_WAIT:
                sl.eof = True
                return 0, 0, 0, b"", 0
            return -2, 0, 0, b"", 0
        try:
            data = sl.sock.recv(maxlen)
        except (BlockingIOError, InterruptedError):
            return -2, 0, 0, b"", 0
        except OSError:
            return -1, 0, 0, b"", 0
        if data == b"":
            sl.eof = True
            sl.state = ST_CLOSE_WAIT
            return 0, 0, 0, b"", 0
        return len(data), 0, 0, data, 0

    def op_state(self, idx):
        sl = self.slots[idx]
        self.refresh_state(sl)
        avail = self.recv_avail(sl) if sl.state in (ST_ESTABLISHED, ST_CLOSE_WAIT) else 0
        return sl.state, avail, 4096, b""

    def op_resolve(self, payload):
        name = payload.split(b"\x00")[0].decode("ascii", "replace")
        try:
            ip = socket.gethostbyname(name)
        except OSError:
            return -1, 0, 0, b""
        return 0, 0, struct.unpack(">I", socket.inet_aton(ip))[0], b""

    def op_hostinfo(self):
        # ゲストの常駐時表示用に、このホストの代表 IPv4 を aux で返す。
        # 外向きの経路で使われるアドレスを UDP connect の要領で調べる
        # (connect だけでは実際には何も送られない)。取れなければ 0。
        ip = None
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
        except OSError:
            ip = None
        finally:
            s.close()
        if ip is None:
            try:
                cand = socket.gethostbyname(socket.gethostname())
                if not cand.startswith("127."):
                    ip = cand
            except OSError:
                ip = None
        if ip is None:
            return 0, 0, 0, b""
        return 0, 0, struct.unpack(">I", socket.inet_aton(ip))[0], b""

    def op_bind(self, idx, addr, port):
        sl = self.slots[idx]
        if sl.sock is None:
            return -1, 0, 0, b"", 0
        try:
            sl.sock.bind((ipstr(addr), port))
        except OSError:
            return -1, 0, 0, b"", 0
        # port=0 で割り当てられた実際のポートを RES の port で返す
        # (ゲストの getsockname が返すため)
        return 0, 0, 0, b"", sl.sock.getsockname()[1]

    def op_listen(self, idx, backlog):
        sl = self.slots[idx]
        if sl.sock is None:
            return -1, 0, 0, b""
        try:
            sl.sock.listen(backlog if backlog > 0 else 5)
        except OSError:
            return -1, 0, 0, b""
        sl.listening = True
        sl.state = ST_LISTEN
        return 0, 0, 0, b""

    def op_accept(self, idx, newidx):
        sl = self.slots[idx]
        if sl.sock is None or not sl.listening:
            return -1, 0, 0, b"", 0
        if not (0 <= newidx < NSOCK):
            return -1, 0, 0, b"", 0
        r, _, _ = select.select([sl.sock], [], [], RECV_WAIT)
        if not r:
            return -2, 0, 0, b"", 0
        try:
            conn, peer = sl.sock.accept()
        except OSError:
            return -1, 0, 0, b"", 0
        conn.setblocking(False)
        nsl = self.slots[newidx]
        self.close_slot(nsl)
        nsl.sock = conn
        nsl.state = ST_ESTABLISHED
        peer_ip = struct.unpack(">I", socket.inet_aton(peer[0]))[0]
        return 0, 0, peer_ip, b"", peer[1]

    # ---------------- 要求の処理 ----------------
    def process(self, raw):
        magic, op, idx, seq, port, arg, addr, length = REQ_HDR.unpack(raw[:20])
        # ペイロードを伴うのは SEND と RESOLVE だけ。それ以外で length は
        # RECV の受け取り上限など、操作ごとの意味を持つ。
        payload = raw[20:] if op in (OP_SEND, OP_RESOLVE) else b""
        extra_port = 0
        if op == OP_OPEN:
            result, rlen, aux, rpayload = self.op_open(idx, arg)
        elif op == OP_CLOSE:
            result, rlen, aux, rpayload = self.op_close(idx)
        elif op == OP_CONNECT:
            result, rlen, aux, rpayload, extra_port = self.op_connect(idx, addr, port)
        elif op == OP_SEND:
            result, rlen, aux, rpayload = self.op_send(idx, payload, addr, port)
        elif op == OP_RECV:
            result, rlen, aux, rpayload, extra_port = self.op_recv(idx, length)
        elif op == OP_STATE:
            result, rlen, aux, rpayload = self.op_state(idx)
        elif op == OP_RESOLVE:
            result, rlen, aux, rpayload = self.op_resolve(payload)
        elif op == OP_BIND:
            result, rlen, aux, rpayload, extra_port = self.op_bind(idx, addr, port)
        elif op == OP_LISTEN:
            result, rlen, aux, rpayload = self.op_listen(idx, arg)
        elif op == OP_ACCEPT:
            result, rlen, aux, rpayload, extra_port = self.op_accept(idx, arg)
        elif op == OP_SHUTDOWN:
            result, rlen, aux, rpayload = self.op_shutdown(idx, arg)
        elif op == OP_HOSTINFO:
            result, rlen, aux, rpayload = self.op_hostinfo()
        else:
            result, rlen, aux, rpayload = -1, 0, 0, b""

        if op != OP_STATE:
            rlen = len(rpayload)

        res = RES_HDR.pack(b"HN", op, idx, seq, result, extra_port, 0,
                           rlen, aux) + rpayload
        with open(self.tmp_path, "wb") as f:
            f.write(res)
        self.publish_res(res)
        print(f"[{time.strftime('%H:%M:%S')}] seq={seq} "
              f"{OP_NAMES.get(op, op)} sock={idx} -> result={result} "
              f"len={rlen} aux={aux}", flush=True)

    def publish_res(self, res):
        """応答ファイルを差し替える。

        通常は一時ファイルからの rename で原子的に差し替える。ただし
        Windows では、ゲスト（エミュレータ）が応答ファイルを開いている
        間は rename がアクセス拒否で失敗する。ゲストはポーリングで
        開いては閉じるので、しばらく再試行すれば合間に差し替えられる。
        それでも駄目なら直接上書きする。ゲスト側はマジック・通し番号・
        長さで応答を検証するので、書きかけを読んでも取り違えは起きない。
        """
        for _ in range(40):
            try:
                os.replace(self.tmp_path, self.res_path)
                return
            except OSError:
                time.sleep(0.05)
        with open(self.res_path, "wb") as f:
            f.write(res)

    def run(self):
        print(f"hfsnethost: watching {self.folder}", flush=True)
        while True:
            try:
                with open(self.req_path, "rb") as f:
                    raw = f.read()
            except OSError:
                raw = b""
            if len(raw) >= REQ_HDR.size and raw[:2] == b"HN":
                _, op, _, seq, _, _, _, length = REQ_HDR.unpack(raw[:20])
                # 書きかけ (ヘッダの長さと実長が合わない) は読み直す。
                # ペイロード長として length を使うのは SEND / RESOLVE のみ。
                plen = length if op in (OP_SEND, OP_RESOLVE) else 0
                if seq != self.last_seq and len(raw) == 20 + plen:
                    self.last_seq = seq
                    try:
                        self.process(raw)
                    except Exception as e:  # 1件の失敗で止めない
                        print(f"error: {e}", flush=True)
            time.sleep(0.01)


def main():
    if len(sys.argv) < 2:
        print("usage: python3 hfsnethost.py <shared folder>")
        return 1
    folder = sys.argv[1]
    if not os.path.isdir(folder):
        print(f"not a folder: {folder}")
        return 1
    try:
        Host(folder).run()
    except KeyboardInterrupt:
        print("\nhfsnethost: bye")
    return 0


if __name__ == "__main__":
    sys.exit(main())
