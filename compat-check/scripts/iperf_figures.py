#!/usr/bin/env python3
"""iperf 実機実測の図2点を作る。
- iperf-chart-realhw.png : 速度の棒グラフ（実機2構成＋参考 XEiJ）
- iperf-why-recv-faster.png : 受信が送信より速い理由の概念図
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

# ---------------------------------------------------------------- 棒グラフ
# (ラベル, 送信 B/s, 受信 B/s, 送信表示, 受信表示)
realhw = [
    ("x68kzremotedrv\n(Raspberry Pi)", 3297, 6593, "3,297 B/s\n(0.03 Mb/s)", "6,593 B/s\n(0.05 Mb/s)"),
    ("純正 Windows ファイル共有\n(Nano ITX)", 20775, 28326, "20,775 B/s\n(0.17 Mb/s)", "28,326 B/s\n(0.23 Mb/s)"),
]
xeij = ("XEiJ（エミュレータ、参考）", 66000, 112500, "約 66,000 B/s\n(0.53 Mb/s)", "約 112,500 B/s\n(0.90 Mb/s)")

fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.6),
                         gridspec_kw={"width_ratios": [2, 1]})

ax = axes[0]
x = range(len(realhw))
w = 0.38
b1 = ax.bar([i - w / 2 for i in x], [r[1] for r in realhw], w,
            color=SEND_COLOR, label="送信（ゲスト→ホスト）")
b2 = ax.bar([i + w / 2 for i in x], [r[2] for r in realhw], w,
            color=RECV_COLOR, label="受信（ホスト→ゲスト）")
for bars, key in ((b1, 3), (b2, 4)):
    for bar, r in zip(bars, realhw):
        ax.annotate(r[key], (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                    ha="center", va="bottom", fontsize=8.5)
ax.set_xticks(list(x))
ax.set_xticklabels([r[0] for r in realhw], fontsize=9.5)
ax.set_ylabel("スループット (B/s)", fontsize=10)
ax.set_title("実機 X68000Z XVI", fontsize=11)
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
ax.set_title("参考：エミュレータ", fontsize=11)
ax.set_ylim(0, 132000)
ax.grid(axis="y", alpha=0.3)

fig.suptitle("hfsnet の iperf スループット（単一 TCP ストリーム、各構成1回の測定、TOTAL バイト数と所要時間から算出）",
             fontsize=11.5)
fig.tight_layout(rect=[0, 0, 1, 0.93])
fig.savefig(os.path.join(OUT, "iperf-chart-realhw.png"), dpi=150)
plt.close(fig)

# ---------------------------------------------------------------- 概念図
fig, ax = plt.subplots(figsize=(10.5, 5.6))
ax.set_xlim(0, 100)
ax.set_ylim(0, 56)
ax.axis("off")

def box(x, y, w_, h, text, fc, ec="#333333", fs=9.5, tc="#111111"):
    ax.add_patch(FancyBboxPatch((x, y), w_, h,
                                  boxstyle="round,pad=0.4,rounding_size=1.2",
                                  fc=fc, ec=ec, lw=1.0))
    ax.text(x + w_ / 2, y + h / 2, text, ha="center", va="center",
            fontsize=fs, color=tc)

def arrow(x1, y1, x2, y2, text=None, ty=0):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle="->",
                                 mutation_scale=13, color="#333333", lw=1.1))
    if text:
        ax.text((x1 + x2) / 2, (y1 + y2) / 2 + ty, text, ha="center",
                fontsize=8.5, color="#333333")

ax.text(2, 53.5, "送信（ゲスト → ホスト）：1チャンクごとにファイル往復が同期的にはさまる",
        fontsize=11.5, weight="bold")
ax.text(2, 26.5, "受信（ホスト → ゲスト）：ホスト側に先に届いて貯まっているので、受け取りは即座",
        fontsize=11.5, weight="bold")

# --- 送信の段
y = 44
box(2, y, 20, 6, "ゲスト\nHFSREQ.BIN に書く", "#DCE6F1")
arrow(23, y + 3, 30, y + 3)
box(31, y, 20, 6, "ホスト\n検出してソケット送信", "#FDEBD0")
arrow(52, y + 3, 59, y + 3)
box(60, y, 20, 6, "ホスト\nHFSRES.BIN に書く", "#FDEBD0")
arrow(81, y + 3, 88, y + 3)
box(89, y, 9, 6, "ゲスト\n次へ", "#DCE6F1")
# 回線の稼働 strip
ax.text(2, 40.5, "回線の稼働：", fontsize=9.5)
for i, (sx, sw) in enumerate(((31, 20),)):
    ax.add_patch(plt.Rectangle((sx, 37.2), sw, 2.6, fc=SEND_COLOR, ec="none"))
ax.add_patch(plt.Rectangle((2, 37.2), 96, 2.6, fc="#EEEEEE", ec="#999999", lw=0.6, zorder=0))
ax.add_patch(plt.Rectangle((31, 37.2), 20, 2.6, fc=SEND_COLOR, ec="none", zorder=1))
ax.text(50, 33.8, "実際にデータを送っているのは一部だけ。残りはファイル往復の待ち時間で回線が遊ぶ",
        ha="center", fontsize=9.5, color="#8A3324")

# --- 受信の段
y = 17
ax.add_patch(plt.Rectangle((2, y + 1.2), 62, 3.4, fc=RECV_COLOR, ec="#333333", lw=0.8))
ax.text(33, y + 2.9, "ホスト OS の受信バッファ：相手からのデータが先に届いて貯まっている",
        ha="center", va="center", fontsize=9.5, color="white", weight="bold")
arrow(33, y + 1.0, 33, y - 3.4)
box(18, y - 10, 30, 6, "ゲストの RECV 要求\n→ 貯まった分をすぐ返す", "#DCE6F1")
ax.text(66, y - 4.5, "ゲストが前のチャンクを処理している間も、\nホスト側では受信が進み続ける",
        fontsize=9.5, va="center")
ax.text(50, 1.5, "だから受信は、どちらの構成でも送信より速い（実測で約1.4〜2.0倍）",
        ha="center", fontsize=10.5, weight="bold")

fig.tight_layout()
fig.savefig(os.path.join(OUT, "iperf-why-recv-faster.png"), dpi=150)
plt.close(fig)
print("figures written")
