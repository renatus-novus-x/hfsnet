#!/usr/bin/env python3
"""README 用の概要図（hfsnet の仕組み）を日英で作る。
出力: assets/overview.png / assets/overview.en.png
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

for name in ("Noto Sans CJK JP", "Noto Serif CJK JP"):
    if any(name in f.name for f in font_manager.fontManager.ttflist):
        plt.rcParams["font.family"] = name
        break
plt.rcParams["axes.unicode_minus"] = False

STR = {
    "ja": dict(
        suffix="",
        title="hfsnet の仕組み — ファイルのやり取りだけで TCP/IP",
        guest_label="ゲスト: X68000 / Human68k",
        apps="通信アプリ（TCPPACK 等）\nhttpclient.x ・ ftp.x ・ ping.x …",
        ti_label="inetd.x 互換 API (TI)",
        driver="hfsnet.x 常駐ドライバ\nTI 呼び出しをファイルの読み書きに翻訳",
        folder_label="共有フォルダ",
        folder_sub="HFS / HostFS / Windrv 等",
        req="HFSREQ.BIN（要求）",
        res="HFSRES.BIN（応答）",
        host_label="ホスト: PC (Windows / macOS / Linux)",
        hostapp="ホストアプリ（GUI 版 / Python 版）\n実際のソケットを操作",
        sock_label="OS のソケット",
        net="インターネット / LAN",
        point1="専用ハードウェアは不要。使うのは通常のファイル読み書きだけ",
        point2="動作環境: XEiJ / WebX68k / XM6 TypeG / X68000Z XVI（実機）",
        fs=9.0,
    ),
    "en": dict(
        suffix=".en",
        title="How hfsnet works — TCP/IP using only file exchange",
        guest_label="Guest: X68000 / Human68k",
        apps="Network apps (TCPPACK etc.)\nhttpclient.x · ftp.x · ping.x …",
        ti_label="inetd.x-compatible API (TI)",
        driver="hfsnet.x resident driver\ntranslates TI calls into file I/O",
        folder_label="Shared folder",
        folder_sub="HFS / HostFS / Windrv, etc.",
        req="HFSREQ.BIN (requests)",
        res="HFSRES.BIN (responses)",
        host_label="Host: PC (Windows / macOS / Linux)",
        hostapp="Host application (GUI / Python)\noperates the real sockets",
        sock_label="OS sockets",
        net="Internet / LAN",
        point1="No special hardware — plain file reads and writes only",
        point2="Runs on: XEiJ / WebX68k / XM6 TypeG / real X68000Z XVI",
        fs=8.5,
    ),
}


def draw(s):
    fig, ax = plt.subplots(figsize=(10.5, 5.0))
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 48)
    ax.axis("off")
    fs = s["fs"]

    def frame(x, y, w, h, fc, ec):
        ax.add_patch(FancyBboxPatch((x, y), w, h,
                                    boxstyle="round,pad=0.3,rounding_size=1.0",
                                    fc=fc, ec=ec, lw=1.2))

    def box(x, y, w, h, text, fc, ec="#333333"):
        ax.add_patch(FancyBboxPatch((x, y), w, h,
                                    boxstyle="round,pad=0.4,rounding_size=1.2",
                                    fc=fc, ec=ec, lw=1.0))
        ax.text(x + w / 2, y + h / 2, text, ha="center", va="center",
                fontsize=fs, color="#111111")

    def arrow(x1, y1, x2, y2):
        ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="->",
                                     mutation_scale=14, color="#333333", lw=1.2))

    ax.text(50, 45.3, s["title"], ha="center", fontsize=12.5, weight="bold")

    # ゲスト側
    frame(1, 10, 31, 30, "#F4F8FC", "#4C78A8")
    ax.text(3, 37.3, s["guest_label"], fontsize=fs + 0.5, weight="bold", color="#2F5578")
    box(3.5, 28.5, 26, 7.5, s["apps"], "#DCE6F1")
    arrow(10, 28.3, 10, 24.7)
    ax.text(11.2, 26.5, s["ti_label"], fontsize=fs - 0.5, va="center")
    box(3.5, 14.5, 26, 9.5, s["driver"], "#DCE6F1")

    # 共有フォルダ
    frame(38, 13, 24, 24, "#F5F5F5", "#888888")
    ax.text(50, 34.0, s["folder_label"], ha="center", fontsize=fs + 0.5, weight="bold", color="#444444")
    ax.text(50, 31.2, s["folder_sub"], ha="center", fontsize=fs - 1, color="#666666")
    box(40.5, 24.5, 19, 5.0, s["req"], "#FFFFFF")
    box(40.5, 16.5, 19, 5.0, s["res"], "#FFFFFF")

    # ホスト側
    frame(68, 10, 31, 30, "#FFFBF4", "#F58518")
    ax.text(70, 37.3, s["host_label"], fontsize=fs + 0.5, weight="bold", color="#9A5A12")
    box(70.5, 26.5, 26, 9.5, s["hostapp"], "#FDEBD0")
    arrow(77, 26.3, 77, 22.7)
    ax.text(78.2, 24.5, s["sock_label"], fontsize=fs - 0.5, va="center")
    box(70.5, 14.5, 26, 7.5, s["net"], "#FDEBD0")

    # パイプライン（要求は右へ、応答は左へ）
    arrow(32.6, 27.0, 37.4, 27.0)
    arrow(62.6, 27.0, 67.4, 27.0)
    arrow(67.4, 19.0, 62.6, 19.0)
    arrow(37.4, 19.0, 32.6, 19.0)

    ax.text(50, 6.3, s["point1"], ha="center", fontsize=10.5, weight="bold")
    ax.text(50, 2.8, s["point2"], ha="center", fontsize=9.5, color="#444444")

    fig.tight_layout()
    fig.savefig(os.path.join(ROOT, "assets", f"overview{s['suffix']}.png"), dpi=150)
    plt.close(fig)


for lang in ("ja", "en"):
    draw(STR[lang])
print("overview figures written")
