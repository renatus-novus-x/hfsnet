#!/usr/bin/env python3
# hfsnet ホスト側 GUI 版 プロトタイプ実装仕様スライド生成スクリプト
# 仕様: 全スライドに右下「n / 総数」のページ番号（保存直前の後処理で付与）。
# 表は図形（四角形＋テキスト）で描く（アプリ側ビューアは pptx の表オブジェクトを表示しないため）。
from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE

FONT = "Noto Sans CJK JP"
INK = RGBColor(0x1A, 0x1D, 0x21)
GRAY = RGBColor(0x5A, 0x63, 0x6D)
ACCENT = RGBColor(0x0E, 0x7C, 0x86)
WHITE = RGBColor(0xFF, 0xFF, 0xFF)

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

# OS 標準ダイアログ用（ライトテーマ）
DLG_BG = RGBColor(0xF0, 0xF0, 0xF0)
DLG_TITLE = RGBColor(0xE5, 0xE5, 0xE5)
DLG_NAV = RGBColor(0xF5, 0xF6, 0xF7)
DLG_LINE = RGBColor(0xC8, 0xC8, 0xC8)
FOLDER_COL = RGBColor(0xF7, 0xD9, 0x6B)

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


def bullets(s, items, x=0.85, y=1.95, w=11.6, size=15.5, gap=0.62):
    cy = y
    for head, body in items:
        text(s, x, cy, 0.35, 0.4, "・", size=size, color=ACCENT, bold=True)
        t = head if not body else head + "：" + body
        text(s, x + 0.35, cy + 0.02, w - 0.35, 0.6, t, size=size)
        nlines = max(1, int(len(t) / 52) + 1)
        cy += gap * nlines
    return cy


def notes(s, txt):
    s.notes_slide.notes_text_frame.text = txt


# ---------- 1. 表紙 ----------
s = slide_new()
rect(s, 0, 0, 13.333, 7.5, fill=WHITE)
rect(s, 0, 2.02, 13.333, 0.06, fill=ACCENT)
text(s, 0.9, 2.3, 11.9, 1.1, "hfsnet ホスト側 GUI 版", size=38, bold=True)
text(s, 0.9, 3.3, 11.9, 0.7, "プロトタイプ実装仕様", size=30, bold=True)
text(s, 0.9, 4.35, 11.5, 0.6, "仕様と画面イメージ（ファイル選択ダイアログを含む）", size=17, color=GRAY)
text(s, 0.9, 5.35, 11.9, 1.2,
     "第一目標は ICMP 対応で ping.x を動かすこと\n"
     "プラットフォーム依存部は platform.h に分離 ／ セッションは単一",
     size=15, color=INK)
text(s, 0.9, 6.7, 11.5, 0.4, "2026-11-18", size=12.5, color=GRAY)
notes(s, "プロトタイプ実装の仕様まとめ。ICMP 対応と platform.h の方針（2026-11-18 指示）を反映した版。")

# ---------- 2. 目的と前提 ----------
s = slide_new()
header(s, "目的と前提", "Python 版ホストを GUI アプリに置き換える。ICMP 対応のため前提を1つ変更")
bullets(s, [
    ("目的", "共有フォルダの選択、開始・停止、通信の監視を GUI で行う"),
    ("プロトコル形式", "HFSREQ.BIN / HFSRES.BIN の形式は変えない"),
    ("Python 版", "リファレンス実装として残す（ICMP は入れない）"),
    ("前提の変更", "当初はゲスト無変更だったが、ICMP に必要な SOCK_RAW の受理だけゲストへ手を入れる"),
    ("回帰テスト", "既存の test/sim_test.py と test/udp_test.py を C++ エンジンの検証に使う"),
    ("対象 OS", "Windows / macOS / Ubuntu"),
], y=2.0)
notes(s, "ゲスト変更は SOCK_RAW 受理の1点だけ。プロトコル形式は不変で、ソケット種別の値が増えるだけ。")

# ---------- 3. 決定事項：単一セッション ----------
s = slide_new()
header(s, "決定事項：セッションは単一", "複数フォルダの同時監視は行わない（2026-11-18 決定）")
bullets(s, [
    ("用途の想定", "複数セッションが要るのは「複数のエミュレータを同時に通信させる」ケース"),
    ("難しい理由", "ゲストは固有の IP を持たずホストの実ソケットを借りるため、待ち受けポートが競合し、LAN から区別できない"),
    ("判断", "単一で確定。必要になったら、そのフォルダ向けに Python 版ホストを別途起動すれば足りる"),
], y=2.0)
notes(s, "外向き接続だけなら共存できるが、待ち受けと同一 IP の問題がある。今の検証の進め方では同時通信は不要。")

# ---------- 4. 第一目標：ICMP 対応と ping.x ----------
s = slide_new()
header(s, "第一目標：ICMP 対応で ping.x を動かす", "プロトタイプの到達点は、ping.x が応答を受け取れること")
bullets(s, [
    ("現状", "ping.x は SOCK_RAW を使うが、現行ドライバは -1 を返し未対応"),
    ("ゲスト側の変更", "SOCK_RAW を受理し、種別を OP_OPEN でホストへ伝える（形式は不変、種別の値が1つ増えるだけ）"),
    ("ホスト側の方式", "エコー要求（タイプ8）から識別子・シーケンス・データを取り出して実際に打ち、応答をゲストの形式に組み立て直して返す"),
    ("権限の方針", "生の raw ソケットは管理者権限が要るため使わない。OS ごとの特権なし方式を使う（次々枚）"),
    ("調査事項", "ping.x の送受信パケット形式（IP ヘッダの有無など）を、実物と libsocket の逆アセンブルで確認する"),
], y=2.0)
notes(s, "ホスト側は ICMP エコーの代理。応答の組み立てではチェックサムを再計算する。")

# ---------- 5. プラットフォーム抽象：platform.h ----------
s = slide_new()
header(s, "プラットフォーム抽象：platform.h", "本体は3プラットフォームを意識せずに書く")
bullets(s, [
    ("方針", "エンジンと GUI は platform.h だけを見て書き、本体に #ifdef を散らさない"),
    ("隠すもの", "ソケットの初期化・送受信・エラー正規化、ICMP エコー、応答ファイルの差し替え、ワイド文字パス、高精度時刻"),
    ("実装", "platform_win.cpp / platform_mac.cpp / platform_linux.cpp と、3プラットフォーム分を個別に書く"),
    ("効果", "プラットフォーム差は platform 層に閉じ、本体のコードとテストが1本で済む"),
], y=2.0)
notes(s, "機種依存コードを3プラットフォーム書くことは厭わない、という指示を受けての構成。差し替え（リネームのリトライと直接上書き）も platform 層の責務。")

# ---------- 6. ICMP のプラットフォーム別実装 ----------
s = slide_new()
header(s, "ICMP エコーのプラットフォーム別実装", "どの OS でも管理者権限なしで打てる方式にする")
cols = [(0.85, 2.1, "OS"), (2.95, 4.6, "方式"), (7.55, 5.1, "備考")]
rect(s, 0.85, 2.05, 11.8, 0.44, fill=WIN_TITLE)
for cx, cw, cl in cols:
    text(s, cx + 0.12, 2.15, cw - 0.2, 0.3, cl, size=13, color=TXT_LIGHT, bold=True)
rows = [
    ("Windows", "IcmpSendEcho（iphlpapi）", "管理者権限は不要。OS の ping 機能をそのまま使う"),
    ("Linux", "ping ソケット\n（SOCK_DGRAM + IPPROTO_ICMP）", "net.ipv4.ping_group_range の設定に依存。カーネルが識別子を書き換えるため、応答の組み立てで戻す"),
    ("macOS", "ping ソケット\n（SOCK_DGRAM + IPPROTO_ICMP）", "第一候補。特権なしで使えるかをプロトタイプで実測する"),
]
ry = 2.49
for r_i, row in enumerate(rows):
    rh = 0.92
    if r_i % 2 == 0:
        rect(s, 0.85, ry, 11.8, rh, fill=RGBColor(0xEE, 0xF1, 0xF3))
    else:
        rect(s, 0.85, ry, 11.8, rh, fill=WHITE, line=RGBColor(0xDD, 0xE2, 0xE6))
    for (cx, cw, _), val in zip(cols, row):
        text(s, cx + 0.12, ry + 0.1, cw - 0.2, rh - 0.15, val, size=12.5)
    ry += rh
text(s, 0.85, ry + 0.25, 11.8, 0.6,
     "共通：生の raw ソケットは使わない。応答はゲストの識別子・シーケンス・データで組み立て直し、チェックサムを再計算する。",
     size=13, color=GRAY)
notes(s, "3プラットフォームとも特権なし方式。Linux は sysctl の設定に依存する点、macOS は実測待ちの点を明記した。")
# ---------- 7. 構成と技術選定 ----------
s = slide_new()
header(s, "構成と技術選定", "エンジンは GUI 非依存の C++ クラス、描画はスナップショット")
bullets(s, [
    ("エンジン", "C++ 化して GUI と同一プロセスに組み込む。ワーカースレッドで約10ms ポーリング"),
    ("GUI", "GLFW + GLEW + Dear ImGui の OpenGL3 バックエンド。グラフは ImDrawList で描く"),
    ("フォルダ選択", "nativefiledialog-extended（OS 標準ダイアログ、次々枚に画面イメージ）"),
    ("ビルド", "CMake、依存は vendoring"),
    ("プラットフォーム依存", "platform.h の裏に分離（5枚目）"),
], y=2.0)
notes(s, "エンジンと GUI の分離で、エンジン単体のテストを効かせる。")

# ---------- 8. 画面イメージ（本体） ----------
s = slide_new()
header(s, "画面イメージ", "Phase 1 の画面（ソケット表は Phase 2）")
rect(s, 0.7, 1.8, 8.7, 5.15, fill=WIN_BG, line=FIELD_LINE)
rect(s, 0.7, 1.8, 8.7, 0.44, fill=WIN_TITLE)
text(s, 0.92, 1.9, 6.5, 0.3, "hfsnet host", size=11.5, color=TXT_LIGHT, bold=True)
text(s, 8.75, 1.88, 0.45, 0.3, "×", size=12, color=TXT_DIM, align=PP_ALIGN.RIGHT)
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
text(s, 0.92, 3.3, 8.0, 0.3,
     "TX  累計 3,842,112 B    レート 9.6 KB/s", size=11, color=TX_COL)
text(s, 0.92, 3.58, 8.0, 0.3,
     "RX  累計 1,205,440 B    レート 3.1 KB/s", size=11, color=RX_COL)
rect(s, 0.92, 3.9, 8.26, 0.98, fill=FIELD_BG, line=FIELD_LINE)
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
text(s, 0.92, 5.18, 4.0, 0.28, "ソケット", size=11, color=TXT_DIM)
text(s, 2.05, 5.18, 1.6, 0.28, "（Phase 2）", size=9.5, color=TXT_DIM)
cols8 = [(0.92, 0.55, "#"), (1.47, 1.35, "状態"), (2.82, 2.6, "相手"), (5.42, 1.85, "TX"), (7.27, 1.91, "RX")]
rect(s, 0.92, 5.44, 8.26, 0.28, fill=WIN_TITLE)
for cx, cw, cl in cols8:
    text(s, cx + 0.06, 5.49, cw - 0.1, 0.24, cl, size=9.5, color=TXT_DIM)
rows8 = [("0", "ESTABLISHED", "93.184.216.34:80", "3,802,112", "1,198,204"),
         ("1", "UDP", "192.168.1.10:123", "40,000", "7,236")]
ry = 5.74
for r_i, row in enumerate(rows8):
    if r_i % 2 == 0:
        rect(s, 0.92, ry, 8.26, 0.28, fill=RGBColor(0x2A, 0x2F, 0x36))
    for (cx, cw, _), val in zip(cols8, row):
        text(s, cx + 0.06, ry + 0.045, cw - 0.1, 0.24, val, size=9.5, color=TXT_LIGHT)
    ry += 0.28
rect(s, 0.92, 6.38, 8.26, 0.44, fill=FIELD_BG, line=FIELD_LINE)
text(s, 1.04, 6.44, 8.0, 0.36,
     "[15:01:22] CONNECT 93.184.216.34:80 -> OK\n[15:01:23] STATE polls: 1,204",
     size=8.5, color=TXT_DIM)
text(s, 9.75, 2.0, 3.0, 1.3, "フォルダ選択と\n開始・停止", size=13, bold=True)
text(s, 9.75, 2.75, 3.05, 0.75, "パス欄＋参照ボタン。実行中は状態を表示", size=11.5, color=GRAY)
text(s, 9.75, 3.7, 3.0, 1.3, "トラフィック監視", size=13, bold=True)
text(s, 9.75, 4.12, 3.05, 1.1, "TX / RX の累計と現在のレート、60秒の折れ線グラフ", size=11.5, color=GRAY)
text(s, 9.75, 5.05, 3.0, 1.3, "ログペイン", size=13, bold=True)
text(s, 9.75, 5.47, 3.05, 1.1, "操作ログを表示。STATE ポーリングは行に出さず回数だけ数える", size=11.5, color=GRAY)
notes(s, "本体ウィンドウのモックアップ。前回版から変更なし。")

# ---------- 9. 画面イメージ（ファイル選択ダイアログ） ----------
s = slide_new()
header(s, "画面イメージ：ファイル選択ダイアログ", "「参照」を押すと OS 標準のダイアログが本体の上に開く（Windows の例）")
# 背後の本体ウィンドウ（簡略）
rect(s, 0.55, 1.95, 8.3, 4.7, fill=WIN_BG, line=FIELD_LINE)
rect(s, 0.55, 1.95, 8.3, 0.44, fill=WIN_TITLE)
text(s, 0.77, 2.05, 6.0, 0.3, "hfsnet host", size=11.5, color=TXT_LIGHT, bold=True)
text(s, 0.77, 2.62, 1.5, 0.3, "共有フォルダ", size=11, color=TXT_DIM)
rect(s, 2.2, 2.56, 4.35, 0.4, fill=FIELD_BG, line=FIELD_LINE)
rect(s, 6.7, 2.56, 0.95, 0.4, fill=BTN_BG)
text(s, 6.7, 2.65, 0.95, 0.3, "参照", size=10.5, color=TXT_LIGHT, align=PP_ALIGN.CENTER)
# ダイアログ本体
DX, DY, DW, DH = 1.75, 2.6, 7.7, 4.25
rect(s, DX, DY, DW, DH, fill=DLG_BG, line=DLG_LINE, line_w=1.0)
rect(s, DX, DY, DW, 0.42, fill=DLG_TITLE)
text(s, DX + 0.18, DY + 0.09, 5.0, 0.28, "フォルダーの選択", size=11.5, color=INK)
text(s, DX + DW - 0.55, DY + 0.07, 0.4, 0.28, "×", size=12, color=GRAY, align=PP_ALIGN.RIGHT)
# ツールバー／アドレスバー
text(s, DX + 0.18, DY + 0.58, 0.9, 0.3, "← → ↑", size=11, color=GRAY)
rect(s, DX + 1.15, DY + 0.52, DW - 1.35, 0.38, fill=WHITE, line=DLG_LINE)
text(s, DX + 1.3, DY + 0.6, DW - 1.6, 0.28, "PC  >  X68000  >  HFS", size=10.5, color=INK)
# 左ナビ
rect(s, DX + 0.12, DY + 1.05, 1.75, DH - 1.85, fill=DLG_NAV, line=DLG_LINE)
text(s, DX + 0.28, DY + 1.18, 1.55, 1.6,
     "クイック アクセス\nデスクトップ\nダウンロード\nPC\nネットワーク", size=9.5, color=GRAY)
# 一覧ヘッダ
LX = DX + 2.0
LW = DW - 2.15
rect(s, LX, DY + 1.05, LW, 0.3, fill=WHITE, line=DLG_LINE)
text(s, LX + 0.1, DY + 1.1, 2.2, 0.24, "名前", size=9.5, color=GRAY)
text(s, LX + 2.5, DY + 1.1, 1.4, 0.24, "更新日時", size=9.5, color=GRAY)
text(s, LX + 4.0, DY + 1.1, 1.5, 0.24, "種類", size=9.5, color=GRAY)
# 一覧本体
rect(s, LX, DY + 1.35, LW, DH - 2.15, fill=WHITE, line=DLG_LINE)
folders = [("HFSNET", True), ("BACKUP", False), ("SHARED", False)]
fy = DY + 1.47
for name, sel in folders:
    if sel:
        rect(s, LX + 0.04, fy - 0.03, LW - 0.08, 0.32, fill=RGBColor(0xE5, 0xF1, 0xFB))
    rect(s, LX + 0.12, fy + 0.03, 0.2, 0.16, fill=FOLDER_COL, line=DLG_LINE)
    text(s, LX + 0.42, fy, 2.0, 0.26, name, size=10, color=INK)
    text(s, LX + 2.5, fy, 1.4, 0.26, "2026/11/18", size=9.5, color=GRAY)
    text(s, LX + 4.0, fy, 1.5, 0.26, "ファイル フォルダー", size=9.5, color=GRAY)
    fy += 0.34
# 下部：選択中フォルダとボタン
text(s, DX + 0.18, DY + DH - 0.62, 1.1, 0.28, "フォルダー:", size=10.5, color=INK)
rect(s, DX + 1.3, DY + DH - 0.68, 3.1, 0.38, fill=WHITE, line=DLG_LINE)
text(s, DX + 1.42, DY + DH - 0.6, 2.9, 0.28, "HFSNET", size=10.5, color=INK)
rect(s, DX + DW - 3.05, DY + DH - 0.68, 1.85, 0.4, fill=WHITE, line=ACCENT, line_w=1.25)
text(s, DX + DW - 3.05, DY + DH - 0.6, 1.85, 0.28, "フォルダーの選択", size=10, color=INK, align=PP_ALIGN.CENTER)
rect(s, DX + DW - 1.05, DY + DH - 0.68, 0.9, 0.4, fill=WHITE, line=DLG_LINE)
text(s, DX + DW - 1.05, DY + DH - 0.6, 0.9, 0.28, "キャンセル", size=10, color=INK, align=PP_ALIGN.CENTER)
# 右の注記
text(s, 9.85, 2.1, 3.0, 0.4, "違和感はあるか", size=13.5, bold=True)
text(s, 9.85, 2.52, 3.05, 1.15,
     "見た目は OS 標準で、本体のダークな画面とは揃わない。ただし開くのは数秒のモーダル操作だけ",
     size=11.5, color=GRAY)
text(s, 9.85, 3.85, 3.0, 0.4, "標準を使う理由", size=13.5, bold=True)
text(s, 9.85, 4.27, 3.05, 1.15,
     "操作に迷わない。日本語パス・ネットワークドライブ・最近使ったフォルダを OS が正しく扱う",
     size=11.5, color=GRAY)
text(s, 9.85, 5.6, 3.0, 0.4, "代替案", size=13.5, bold=True)
text(s, 9.85, 6.02, 3.05, 1.0,
     "ImGui 内蔵のファイルブラウザは見た目が揃うが、操作性と正確さで劣る",
     size=11.5, color=GRAY)
notes(s, "ダイアログは Windows のフォルダー選択の模写。macOS では Finder 式のシートになる。見た目の不揃いはあるが一時的で、標準の正確さを優先する判断。")

# ---------- 10. 監視の仕様 ----------
s = slide_new()
header(s, "監視の仕様", "数えるのはペイロードだけ、ポーリングは行ログに出さない")
bullets(s, [
    ("計数の対象", "ペイロードのバイト数だけ（SEND で実際に送れた分、RECV でゲストへ渡した分）"),
    ("含めないもの", "プロトコルヘッダ（要求20B / 応答24B）は数えない"),
    ("レート", "1秒差分を取り、数値とグラフで共有する"),
    ("STATE ポーリング", "行ログに出さず回数として数える"),
    ("理由", "Python 版は全操作を印字し、sntp 検証で約1,900行のポーリング行が出た"),
], y=2.0)
notes(s, "ICMP のエコーも、ゲストへ渡した応答のペイロードとして RX に数える想定。")

# ---------- 11. フェーズ分け ----------
s = slide_new()
header(s, "フェーズ分け", "第一目標はプロトタイプで ping.x を動かすこと")
bullets(s, [
    ("第一目標（プロトタイプ）", "エンジン移植 + platform.h + 最小限の GUI（フォルダ選択・開始停止）+ ICMP 対応で、ping.x の実動確認まで"),
    ("Phase 2", "監視の仕上げ（グラフ・ソケット表）+ 設定保存"),
    ("Phase 3", "トレイ常駐、ドラッグ＆ドロップ"),
], y=2.0)
notes(s, "単一セッション確定により、複数セッションはフェーズから外れている。")

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

out = "/home/hatch/workspace/x68k/hfsnet/gui/prototype-spec-slides.pptx"
prs.save(out)
print("saved:", out, "slides:", total)
