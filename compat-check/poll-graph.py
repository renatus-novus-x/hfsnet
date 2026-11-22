#!/usr/bin/env python3
"""poll-graph.py - POLL-BENCH 用の確定グラフ（2軸）を日英で描く。

棒 = スループット（送信/受信）、赤線 = アイドル時ホスト CPU。
データは POLL-BENCH.md の表（2 回の平均）。
出力: poll-graph.png（日本語）/ poll-graph.en.png（英語）
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.font_manager as fm
import numpy as np

FONT = "/home/hatch/workspace/x68k/hfsnet/gui/release-staging/pkg-linux/fonts/ipaexg.ttf"
fm.fontManager.addfont(FONT)
plt.rcParams["font.family"] = fm.FontProperties(fname=FONT).get_name()
plt.rcParams["axes.unicode_minus"] = False

OUT = "/home/hatch/workspace/x68k/hfsnet/compat-check"
intervals = ["10 ms", "5 ms", "2 ms", "1 ms", "0 ms"]
x = np.arange(5)
send = np.array([79.7, 76.1, 95.6, 89.1, 108.7])
recv = np.array([110.8, 113.4, 123.9, 127.0, 155.7])
idle = np.array([0.6, 1.2, 1.2, 2.0, 19.6])
C_SEND, C_RECV, C_CPU = "#3b6fb5", "#2e9e7b", "#d9534f"


def draw(labels, path):
    fig, ax = plt.subplots(figsize=(7.2, 4.4), dpi=150)
    w = 0.38
    ax.bar(x - w / 2, send, w, color=C_SEND, label=labels["send"])
    ax.bar(x + w / 2, recv, w, color=C_RECV, label=labels["recv"])
    ax.set_xticks(x, intervals)
    ax.set_xlabel(labels["xlabel"])
    ax.set_ylabel(labels["ylabel"])
    ax.set_ylim(0, 175)
    ax2 = ax.twinx()
    ax2.plot(x, idle, color=C_CPU, marker="o", lw=2, label=labels["cpu"])
    ax2.set_ylabel(labels["cpulabel"], color=C_CPU)
    ax2.tick_params(axis="y", colors=C_CPU)
    ax2.set_ylim(0, 24)
    for xi, v in zip(x, idle):
        ax2.annotate(f"{v}%", (xi, v), textcoords="offset points",
                     xytext=(0, 7), ha="center", color=C_CPU, fontsize=9)
    ax.set_title(labels["title"])
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper left", fontsize=9)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


draw({
    "title": "ポーリング間隔と速度・アイドル時 CPU",
    "xlabel": "ホスト側ポーリング間隔",
    "ylabel": "スループット (KB/s)",
    "cpulabel": "アイドル時ホスト CPU (%)",
    "send": "送信 (KB/s)", "recv": "受信 (KB/s)", "cpu": "アイドル時 CPU (%)",
}, f"{OUT}/poll-graph.png")

draw({
    "title": "Polling interval vs. throughput and idle CPU",
    "xlabel": "Host polling interval",
    "ylabel": "Throughput (KB/s)",
    "cpulabel": "Host CPU at idle (%)",
    "send": "Send (KB/s)", "recv": "Receive (KB/s)", "cpu": "Idle CPU (%)",
}, f"{OUT}/poll-graph.en.png")

print("done")
