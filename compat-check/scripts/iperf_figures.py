#!/usr/bin/env python3
"""iperf 実機実測の図2点を日英で作る。
- iperf-chart-realhw.png / .en.png : 速度の棒グラフ（実機2構成＋参考 XEiJ）
- iperf-why-recv-faster.png / .en.png : 受信が送信より速い理由の概念図
数値は 2026-11-19 の実測（各構成1回、TOTAL バイト数と所要時間から算出）。
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")

# 日本語フォント（Sans があれば Sans、無ければ Serif）
for name in ("Noto Sans CJK JP", "Noto Serif CJK JP"):
    if any(name in f.name for f in font_manager.fontManager.ttflist):
        plt.rcParams["font.family"] = name
        break
plt.rcParams["axes.unicode_minus"] = False

SEND_COLOR = "#4C78A8"   # 送信（ゲスト→ホスト）
RECV_COLOR = "#F58518"   # 受信（ホスト→ゲスト）

STR = {
    "ja": dict(
        suffix="",
        suptitle="hfsnet の iperf スループット（単一 TCP ストリーム、各構成1回の測定、TOTAL バイト数と所要時間から算出）",
        realhw_title="実機 X68000Z XVI",
        emu_title="参考：エミュレータ",
        ylabel="スループット (B/s)",
        legend_send="送信（ゲスト→ホスト）",
        legend_recv="受信（ホスト→ゲスト）",
        cfg_remote="x68kzremotedrv\n(Raspberry Pi)",
        cfg_builtin="純正 Windows ファイル共有\n(Nano ITX)",
        xeij_label="XEiJ（エミュレータ、参考）",
        approx="約 ",
        send_head="送信（ゲスト → ホスト）：1チャンクごとにファイル往復が同期的にはさまる",
        recv_head="受信（ホスト → ゲスト）：ホスト側に先に届いて貯まっているので、受け取りは即座",
        box_guest_write="ゲスト\nHFSREQ.BIN に書く",
        box_host_send="ホスト\n検出してソケット送信",
        box_host_write="ホスト\nHFSRES.BIN に書く",
        box_guest_next="ゲスト\n次へ",
        link_label="回線の稼働：",
        link_note="実際にデータを送っているのは一部だけ。残りはファイル往復の待ち時間で回線が遊ぶ",
        buffer_text="ホスト OS の受信バッファ：相手からのデータが先に届いて貯まっている",
        box_recv="ゲストの RECV 要求\n→ 貯まった分をすぐ返す",
        side_note="ゲストが前のチャンクを処理している間も、\nホスト側では受信が進み続ける",
        bottom="だから受信は、どちらの構成でも送信より速い（実測で約1.4〜2.0倍）",
        box_fs=9.5,
    ),
    "en": dict(
        suffix=".en",
        suptitle="hfsnet iperf throughput (single TCP stream, one run per setup, from TOTAL bytes and elapsed time)",
        realhw_title="Real hardware: X68000Z XVI",
        emu_title="Reference: emulator",
        ylabel="Throughput (B/s)",
        legend_send="Send (guest → host)",
        legend_recv="Receive (host → guest)",
        cfg_remote="x68kzremotedrv\n(Raspberry Pi)",
        cfg_builtin="Built-in Windows file sharing\n(Nano ITX)",
        xeij_label="XEiJ (emulator, reference)",
        approx="approx. ",
        send_head="Send (guest → host): every chunk waits for a synchronous file round trip",
        recv_head="Receive (host → guest): data arrives early and is buffered on the host, so pickup is immediate",
        box_guest_write="Guest\nwrites HFSREQ.BIN",
        box_host_send="Host\ndetects it, sends via socket",
        box_host_write="Host\nwrites HFSRES.BIN",
        box_guest_next="Guest\nnext chunk",
        link_label="Link activity:",
        link_note="The link carries data only part of the time; the rest is spent waiting for the file round trip",
        buffer_text="Host OS receive buffer: data from the peer arrives early and accumulates",
        box_recv="Guest RECV request\n→ buffered data returned at once",
        side_note="While the guest processes the previous chunk,\nthe host keeps receiving",
        bottom="That is why receiving is faster than sending in both setups (measured: about 1.4–2.0x)",
        box_fs=8.5,
    ),
}


def chart(s):
    # (ラベル, 送信 B/s, 受信 B/s, 送信表示, 受信表示)
    realhw = [
        (s["cfg_remote"], 3297, 6593, "3,297 B/s\n(0.03 Mb/s)", "6,593 B/s\n(0.05 Mb/s)"),
        (s["cfg_builtin"], 20775, 28326, "20,775 B/s\n(0.17 Mb/s)", "28,326 B/s\n(0.23 Mb/s)"),
    ]
    xeij = (s["xeij_label"], 66000, 112500,
            f"{s['approx']}66,000 B/s\n(0.53 Mb/s)", f"{s['approx']}112,500 B/s\n(0.90 Mb/s)")

    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.6),
                             gridspec_kw={"width_ratios": [2, 1]})

    ax = axes[0]
    x = range(len(realhw))
    w = 0.38
    b1 = ax.bar([i - w / 2 for i in x], [r[1] for r in realhw], w,
                color=SEND_COLOR, label=s["legend_send"])
    b2 = ax.bar([i + w / 2 for i in x], [r[2] for r in realhw], w,
                color=RECV_COLOR, label=s["legend_recv"])
    for bars, key in ((b1, 3), (b2, 4)):
        for bar, r in zip(bars, realhw):
            ax.annotate(r[key], (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                        ha="center", va="bottom", fontsize=8.5)
    ax.set_xticks(list(x))
    ax.set_xticklabels([r[0] for r in realhw], fontsize=9.5)
    ax.set_ylabel(s["ylabel"], fontsize=10)
    ax.set_title(s["realhw_title"], fontsize=11)
    ax.set_ylim(0, 33000)
    ax.grid(axis="y", alpha=0.3)
    ax.legend(fontsize=9, loc="upper left")

    ax = axes[1]
    b1 = ax.bar([0 - w / 2], [xeij[1]], w, color=SEND_COLOR)
    b2 = ax.bar([0 + w / 2], [xeij[2]], w, color=RECV_COLOR, alpha=0.85)
    for bar, txt in ((b1[0], xeij[3]), (b2[0], xeij[4])):
        ax.annotate(txt, (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    ha="center", va="bottom", fontsize=8.5)
    ax.set_xticks([0])
    ax.set_xticklabels([xeij[0]], fontsize=9.5)
    ax.set_title(s["emu_title"], fontsize=11)
    ax.set_ylim(0, 132000)
    ax.grid(axis="y", alpha=0.3)

    fig.suptitle(s["suptitle"], fontsize=11.5)
    fig.tight_layout(rect=[0, 0, 1, 0.93])
    fig.savefig(os.path.join(OUT, f"iperf-chart-realhw{s['suffix']}.png"), dpi=150)
    plt.close(fig)


def diagram(s):
    fig, ax = plt.subplots(figsize=(10.5, 5.6))
    ax.set_xlim(0, 100)
    ax.set_ylim(0, 56)
    ax.axis("off")

    def box(x, y, w_, h, text, fc, ec="#333333", fs=None, tc="#111111"):
        ax.add_patch(FancyBboxPatch((x, y), w_, h,
                                      boxstyle="round,pad=0.4,rounding_size=1.2",
                                      fc=fc, ec=ec, lw=1.0))
        ax.text(x + w_ / 2, y + h / 2, text, ha="center", va="center",
                fontsize=fs or s["box_fs"], color=tc)

    def arrow(x1, y1, x2, y2, text=None, ty=0):
        ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="->",
                                     mutation_scale=13, color="#333333", lw=1.1))
        if text:
            ax.text((x1 + x2) / 2, (y1 + y2) / 2 + ty, text, ha="center",
                    fontsize=8.5, color="#333333")

    ax.text(2, 53.5, s["send_head"], fontsize=11.5, weight="bold")
    ax.text(2, 26.5, s["recv_head"], fontsize=11.5, weight="bold")

    # --- 送信の段
    y = 44
    box(2, y, 20, 6, s["box_guest_write"], "#DCE6F1")
    arrow(23, y + 3, 30, y + 3)
    box(31, y, 20, 6, s["box_host_send"], "#FDEBD0")
    arrow(52, y + 3, 59, y + 3)
    box(60, y, 20, 6, s["box_host_write"], "#FDEBD0")
    arrow(81, y + 3, 88, y + 3)
    box(89, y, 9, 6, s["box_guest_next"], "#DCE6F1")
    # 回線の稼働 strip
    ax.text(2, 40.5, s["link_label"], fontsize=9.5)
    ax.add_patch(plt.Rectangle((2, 37.2), 96, 2.6, fc="#EEEEEE", ec="#999999", lw=0.6, zorder=0))
    ax.add_patch(plt.Rectangle((31, 37.2), 20, 2.6, fc=SEND_COLOR, ec="none", zorder=1))
    ax.text(50, 33.8, s["link_note"],
            ha="center", fontsize=9.5, color="#8A3324")

    # --- 受信の段
    y = 17
    ax.add_patch(plt.Rectangle((2, y + 1.2), 62, 3.4, fc=RECV_COLOR, ec="#333333", lw=0.8))
    ax.text(33, y + 2.9, s["buffer_text"],
            ha="center", va="center", fontsize=9.5, color="white", weight="bold")
    arrow(33, y + 1.0, 33, y - 3.4)
    box(18, y - 10, 30, 6, s["box_recv"], "#DCE6F1")
    ax.text(66, y - 4.5, s["side_note"],
            fontsize=9.5, va="center")
    ax.text(50, 1.5, s["bottom"],
            ha="center", fontsize=10.5, weight="bold")

    fig.tight_layout()
    fig.savefig(os.path.join(OUT, f"iperf-why-recv-faster{s['suffix']}.png"), dpi=150)
    plt.close(fig)


for lang in ("ja", "en"):
    chart(STR[lang])
    diagram(STR[lang])
print("figures written")
