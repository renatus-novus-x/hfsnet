#!/usr/bin/env python3
# hfsnet ホスト側 GUI 版 設計スライド生成スクリプト
# 仕様: 全スライドに右下「n / 総数」のページ番号（保存直前の後処理で付与）。
# 表は図形（四角形＋テキスト）で描く（アプリ側ビューアは pptx の表オブジェクトを表示しないため）。
from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE
from pptx.oxml.ns import qn
import copy

FONT = "Noto Sans CJK JP"
INK = RGBColor(0x1A, 0x1D, 0x21)
GRAY = RGBColor(0x5A, 0x63, 0x6D)
ACCENT = RGBColor(0x0E, 0x7C, 0x86)
LIGHT_BG = RGBColor(0xF2, 0xF4, 0xF6)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)

# GUI モックアップ用カラー（ImGui 風ダークテーマ）
WIN_BG = RGBColor(0x23, 0x27, 0x2E)
WIN_TITLE = RGBColor(0x2F, 0x35, 0x40)
FIELD_BG = RGBColor(0x17, 0x19, 0x1D)
FIELD_LINE = RGBColor(0x3A, 0x3F, 0x47)
BTN_BG = RGBColor(0x3A, 0x3F, 0x47)
TXT_LIGHT = RGBColor(0xDD, 0xE3, 0xE8)
TXT_DIM = RGBColor(0x9A, 0xA4, 0xAE)
GREEN = RGBColor(0x4C, 0xC3, 0x6B)
TX_COL = RGBColor(0x38, 0xBD, 0xD8)
RX_COL = RGBColor(0xF0, 0xA0, 0x4A)

prs = Presentation()
prs.slide_width = Inches(13.333)
prs.slide_height = Inches(7.5)
BLANK = prs.slide_layouts[6]


def slide_new():
    return prs.slides.add_slide(BLANK)


def rect(s, x, y, w, h, fill=None, line=None, line_w=0.75):
    shp = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, Inches(x), Inches(y), Inches(w), Inches(h))
    shp.shadow.inherit = False
    if fill is None:
        shp.fill.background()
    else:
        shp.fill.solid()
        shp.fill.fore_color.rgb = fill
    if line is None:
        shp.line.fill.background()
    else:
        shp.line.color.rgb = line
        shp.line.width = Pt(line_w)
    return shp


def text(s, x, y, w, h, txt, size=14, color=INK, bold=False, align=PP_ALIGN.LEFT,
         anchor=MSO_ANCHOR.TOP, font=FONT):
    tb = s.shapes.add_textbox(Inches(x), Inches(y), Inches(w), Inches(h))
    tf = tb.text_frame
    tf.word_wrap = True
    tf.vertical_anchor = anchor
    tf.margin_left = 0
    tf.margin_right = 0
    tf.margin_top = 0
    tf.margin_bottom = 0
    lines = txt.split("\n")
    for i, ln in enumerate(lines):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = align
        r = p.add_run()
        r.text = ln
        r.font.size = Pt(size)
        r.font.bold = bold
        r.font.color.rgb = color
        r.font.name = font
    return tb


def header(s, title, sub=None):
    text(s, 0.7, 0.45, 11.9, 0.7, title, size=25, bold=True)
    rect(s, 0.72, 1.18, 1.6, 0.045, fill=ACCENT)
    if sub:
        text(s, 0.7, 1.32, 11.9, 0.45, sub, size=13.5, color=GRAY)


def bullets(s, items, x=0.85, y=1.95, w=11.6, size=15.5, gap=0.62, lh=1.0):
    cy = y
    for head, body in items:
        text(s, x, cy, 0.35, 0.4, "・", size=size, color=ACCENT, bold=True)
        t = head if not body else head + "：" + body
        tb = text(s, x + 0.35, cy + 0.02, w - 0.35, 0.6, t, size=size)
        nlines = max(1, int(len(t) / (52 if size <= 15.5 else 46)) + 1)
        cy += gap * nlines * lh
    return cy


def notes(s, txt):
    s.notes_slide.notes_text_frame.text = txt


# ---------- 1. 表紙 ----------
s = slide_new()
rect(s, 0, 0, 13.333, 7.5, fill=WHITE)
rect(s, 0, 2.02, 13.333, 0.06, fill=ACCENT)
text(s, 0.9, 2.3, 11.5, 1.1, "hfsnet ホスト側 GUI 版 設計", size=38, bold=True)
text(s, 0.9, 3.35, 11.5, 0.6, "仕様と画面イメージ", size=20, color=GRAY)
text(s, 0.9, 4.6, 11.5, 1.2,
     "エンジンを C++ 化し、Dear ImGui の GUI と同一プロセスで動かす\n"
     "セッションは単一（2026-11-18 決定）／ ファイルプロトコルとゲスト側は無変更",
     size=15, color=INK)
text(s, 0.9, 6.7, 11.5, 0.4, "2026-11-18", size=12.5, color=GRAY)
notes(s, "hfsnet ホスト側アプリ（hfsnethost.py）の GUI 版の設計まとめ。仕様の要点と画面イメージを示す。")

# ---------- 2. 目的と前提 ----------
s = slide_new()
header(s, "目的と前提", "Python 版ホストを、運用しやすい GUI アプリに置き換える")
bullets(s, [
    ("目的", "共有フォルダの選択、開始・停止、通信の監視を GUI で行う"),
    ("変えないもの 1", "ファイルプロトコル（HFSREQ.BIN / HFSRES.BIN、ビッグエンディアン）"),
    ("変えないもの 2", "ゲスト側 hfsnet.x（常駐ドライバ）は無変更"),
    ("変えないもの 3", "Python 版はリファレンス実装として残す"),
    ("回帰テスト", "既存の test/sim_test.py と test/udp_test.py を C++ エンジンの検証に使う"),
    ("対象 OS", "Windows / macOS / Ubuntu"),
], y=2.0)
notes(s, "GUI 化はホスト側だけの話で、ゲストとプロトコルには触らない。Python 版は残し、テスト資産をそのまま回帰テストに使う。")

# ---------- 3. 決定事項：単一セッション ----------
s = slide_new()
header(s, "決定事項：セッションは単一", "複数フォルダの同時監視は行わない（2026-11-18 決定）")
bullets(s, [
    ("想定した用途", "複数セッションが要るのは「複数のエミュレータを同時に通信させる」ケース"),
    ("IP の重なり", "ゲストは固有の IP を持たず、ホストの実ソケットを借りる"),
    ("待ち受けの競合", "同じポートを複数セッションで掴めず、サーバ用途が衝突する"),
    ("区別できない", "LAN から見ると全ゲストが同じホスト IP になる"),
    ("判断", "今の検証の進め方では同時通信は不要。必要になったら、そのフォルダ向けに Python 版ホストを別途起動すれば足りる"),
], y=2.0)
notes(s, "外向き接続だけならホスト OS が接続ごとに別ポートを割り当てるため共存できるが、待ち受けポートの競合と同一 IP の問題があり、素直にはできない。ユーザーの指摘どおり。")

# ---------- 4. 構成と技術選定 ----------
s = slide_new()
header(s, "構成と技術選定", "エンジンは GUI 非依存の C++ クラス、描画はスナップショット")
bullets(s, [
    ("エンジン", "C++ 化して GUI と同一プロセスに組み込む。ワーカースレッドで動作"),
    ("ポーリング", "共有フォルダを約10ms 間隔でポーリング（Python 版と同じ）。ファイル通知 API は使わない"),
    ("GUI", "GLFW + GLEW + Dear ImGui の OpenGL3 バックエンド。グラフは ImDrawList で描く"),
    ("フォルダ選択", "nativefiledialog-extended（OS 標準ダイアログ）"),
    ("ビルド", "CMake、依存は vendoring"),
    ("ソケット層", "Winsock2 と BSD ソケットの差を薄い層で吸収"),
], y=2.0)
notes(s, "エンジンと GUI の分離で、エンジン単体のテスト（sim_test / udp_test 相当）を効かせる。")

# ---------- 5. 画面イメージ ----------
s = slide_new()
header(s, "画面イメージ", "Phase 1 の画面（ソケット表は Phase 2）")
# ウィンドウ
rect(s, 0.7, 1.8, 8.7, 5.15, fill=WIN_BG, line=FIELD_LINE)
rect(s, 0.7, 1.8, 8.7, 0.44, fill=WIN_TITLE)
text(s, 0.92, 1.9, 6.5, 0.3, "hfsnet host", size=11.5, color=TXT_LIGHT, bold=True)
text(s, 8.75, 1.88, 0.45, 0.3, "×", size=12, color=TXT_DIM, align=PP_ALIGN.RIGHT)
# フォルダ行
text(s, 0.92, 2.48, 1.5, 0.3, "共有フォルダ", size=11, color=TXT_DIM)
rect(s, 2.35, 2.42, 4.35, 0.4, fill=FIELD_BG, line=FIELD_LINE)
text(s, 2.48, 2.51, 4.2, 0.3, "D:\\X68000\\HFS\\HFSNET", size=10.5, color=TXT_LIGHT)
rect(s, 6.85, 2.42, 0.95, 0.4, fill=BTN_BG)
text(s, 6.85, 2.51, 0.95, 0.3, "参照", size=10.5, color=TXT_LIGHT, align=PP_ALIGN.CENTER)
rect(s, 7.95, 2.42, 0.95, 0.4, fill=BTN_BG)
text(s, 7.95, 2.51, 0.95, 0.3, "停止", size=10.5, color=TXT_LIGHT, align=PP_ALIGN.CENTER)
dot = s.shapes.add_shape(MSO_SHAPE.OVAL, Inches(2.42), Inches(2.98), Inches(0.14), Inches(0.14))
dot.shadow.inherit = False
dot.fill.solid(); dot.fill.fore_color.rgb = GREEN; dot.line.fill.background()
text(s, 2.66, 2.9, 3.0, 0.3, "実行中", size=10.5, color=TXT_LIGHT)
# トラフィック
text(s, 0.92, 3.3, 8.0, 0.3,
     "TX  累計 3,842,112 B    レート 9.6 KB/s", size=11, color=TX_COL)
text(s, 0.92, 3.58, 8.0, 0.3,
     "RX  累計 1,205,440 B    レート 3.1 KB/s", size=11, color=RX_COL)
rect(s, 0.92, 3.9, 8.26, 0.98, fill=FIELD_BG, line=FIELD_LINE)
# グラフ折れ線（フリーフォーム）
tx_pts = [(1.05, 4.68), (1.9, 4.63), (2.7, 4.32), (3.5, 4.47), (4.3, 4.12),
          (5.1, 4.26), (5.9, 4.0), (6.7, 4.22), (7.5, 4.08), (8.3, 4.3), (9.0, 4.17)]
rx_pts = [(1.05, 4.76), (1.9, 4.73), (2.7, 4.6), (3.5, 4.66), (4.3, 4.5),
          (5.1, 4.57), (5.9, 4.43), (6.7, 4.54), (7.5, 4.47), (8.3, 4.56), (9.0, 4.5)]
for pts, col in ((tx_pts, TX_COL), (rx_pts, RX_COL)):
    fb = s.shapes.build_freeform(Emu(int(pts[0][0] * 914400)), Emu(int(pts[0][1] * 914400)))
    fb.add_line_segments([(Emu(int(px * 914400)), Emu(int(py * 914400))) for px, py in pts[1:]], close=False)
    shp = fb.convert_to_shape()
    shp.shadow.inherit = False
    shp.fill.background()
    shp.line.color.rgb = col
    shp.line.width = Pt(1.5)
text(s, 8.35, 4.9, 0.85, 0.25, "60秒", size=9, color=TXT_DIM, align=PP_ALIGN.RIGHT)
# ソケット表（図形で描く）
text(s, 0.92, 5.18, 4.0, 0.28, "ソケット", size=11, color=TXT_DIM)
text(s, 2.05, 5.18, 1.6, 0.28, "（Phase 2）", size=9.5, color=TXT_DIM)
cols = [(0.92, 0.55, "#"), (1.47, 1.35, "状態"), (2.82, 2.6, "相手"), (5.42, 1.85, "TX"), (7.27, 1.91, "RX")]
rect(s, 0.92, 5.44, 8.26, 0.28, fill=WIN_TITLE)
for cx, cw, cl in cols:
    text(s, cx + 0.06, 5.49, cw - 0.1, 0.24, cl, size=9.5, color=TXT_DIM)
rows = [("0", "ESTABLISHED", "93.184.216.34:80", "3,802,112", "1,198,204"),
        ("1", "UDP", "192.168.1.10:123", "40,000", "7,236")]
ry = 5.74
for r_i, row in enumerate(rows):
    if r_i % 2 == 0:
        rect(s, 0.92, ry, 8.26, 0.28, fill=RGBColor(0x2A, 0x2F, 0x36))
    for (cx, cw, _), val in zip(cols, row):
        text(s, cx + 0.06, ry + 0.045, cw - 0.1, 0.24, val, size=9.5, color=TXT_LIGHT)
    ry += 0.28
# ログペイン
rect(s, 0.92, 6.38, 8.26, 0.44, fill=FIELD_BG, line=FIELD_LINE)
text(s, 1.04, 6.44, 8.0, 0.36,
     "[15:01:22] CONNECT 93.184.216.34:80 -> OK\n[15:01:23] STATE polls: 1,204",
     size=8.5, color=TXT_DIM)
# 注記（右側）
text(s, 9.75, 2.0, 3.0, 1.3,
     "フォルダ選択と\n開始・停止", size=13, bold=True)
text(s, 9.75, 2.75, 3.05, 0.75,
     "パス欄＋参照ボタン。実行中は状態を表示", size=11.5, color=GRAY)
text(s, 9.75, 3.7, 3.0, 1.3,
     "トラフィック監視", size=13, bold=True)
text(s, 9.75, 4.12, 3.05, 1.1,
     "TX / RX の累計と現在のレート、60秒の折れ線グラフ", size=11.5, color=GRAY)
text(s, 9.75, 5.05, 3.0, 1.3,
     "ログペイン", size=13, bold=True)
text(s, 9.75, 5.47, 3.05, 1.1,
     "操作ログを表示。STATE ポーリングは行に出さず回数だけ数える", size=11.5, color=GRAY)
notes(s, "画面イメージはモックアップ。ウィンドウ内にフォルダ選択・開始停止・状態、トラフィック（累計/レート/60秒グラフ）、ソケット表（Phase 2）、ログペインを配置した。")

# ---------- 6. 監視の仕様 ----------
s = slide_new()
header(s, "監視の仕様", "数えるのはペイロードだけ、ポーリングは行ログに出さない")
bullets(s, [
    ("計数の対象", "ペイロードのバイト数だけ（SEND で実際に送れた分、RECV でゲストへ渡した分）"),
    ("含めないもの", "プロトコルヘッダ（要求20B / 応答24B）は数えない"),
    ("レート", "1秒差分を取り、数値とグラフで共有する"),
    ("STATE ポーリング", "行ログに出さず回数として数える"),
    ("理由", "Python 版は全操作を印字し、sntp 検証で約1,900行のポーリング行が出た"),
], y=2.0)
notes(s, "監視はペイロード基準で統一。STATE は接続維持の確認で頻繁に飛ぶため、行ではなくカウンタにする。")

# ---------- 7. プラットフォーム注意点 ----------
s = slide_new()
header(s, "プラットフォーム注意点", "Python 版で踏んだ Windows の差異を C++ でも再現する")
bullets(s, [
    ("応答ファイルの差し替え", "Windows では一時名→リネームをリトライし、ダメなら直接上書きにフォールバック（ゲストが開いていると置換が失敗する）"),
    ("日本語パス", "Windows ではワイド文字パスを使う"),
    ("macOS の OpenGL", "非推奨だが 4.1 まで使用可。GL_SILENCE_DEPRECATION と vendoring した GLEW で対応"),
    ("ポーリング維持", "ファイル通知 API は使わない（環境差を避ける）"),
], y=2.0)
notes(s, "Windows の差し替え失敗（WinError 5）と MSG_DONTWAIT の不在は、2026-11-17 に実地で踏んだ教訓。C++ 版でも同じ対策を入れる。")

# ---------- 8. フェーズ分け ----------
s = slide_new()
header(s, "フェーズ分け", "Phase 1 をまず完成させる")
bullets(s, [
    ("Phase 1", "エンジン移植 + 単一セッション + フォルダ選択 + 開始・停止 + トラフィック／グラフ + ログ"),
    ("Phase 2", "ソケット表（相手アドレスとスロット別カウンタ）+ 設定保存"),
    ("Phase 3", "トレイ常駐、ドラッグ＆ドロップ"),
    ("次の一手", "Phase 1 の実装に着手（エンジン移植から）。完了後に sim_test.py / udp_test.py で回帰確認"),
], y=2.0)
notes(s, "複数セッションは今回の決定でフェーズから外した。")

# ---------- ページ番号（全スライド、右下「n / 総数」） ----------
total = len(prs.slides._sldIdLst)
for i, s in enumerate(prs.slides, start=1):
    tb = s.shapes.add_textbox(Inches(12.3), Inches(7.08), Inches(0.9), Inches(0.32))
    tf = tb.text_frame
    tf.word_wrap = False
    tf.margin_left = 0
    tf.margin_right = 0
    tf.margin_top = 0
    tf.margin_bottom = 0
    p = tf.paragraphs[0]
    p.alignment = PP_ALIGN.RIGHT
    r = p.add_run()
    r.text = f"{i} / {total}"
    r.font.size = Pt(10.5)
    r.font.color.rgb = GRAY
    r.font.name = FONT

out = "/home/hatch/workspace/x68k/hfsnet/gui/design-slides.pptx"
prs.save(out)
print("saved:", out, "slides:", total)
