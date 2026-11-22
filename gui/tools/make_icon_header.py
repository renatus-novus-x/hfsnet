#!/usr/bin/env python3
# assets/icons/ の PNG から、実行時ウィンドウアイコン用の RGBA 配列
# ヘッダ (gui/src/icon_rgba.h) を生成する。
# 使い方: python3 gui/tools/make_icon_header.py
import os
from PIL import Image

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
SIZES = (32, 48, 64)

out = []
out.append("// icon_rgba.h - ウィンドウアイコン用 RGBA データ")
out.append("// 生成: gui/tools/make_icon_header.py (手で編集しないこと)")
out.append("#pragma once")
out.append("")
for size in SIZES:
    src = os.path.join(ROOT, "assets", "icons", f"hfsnet_{size}x{size}.png")
    img = Image.open(src).convert("RGBA")
    assert img.size == (size, size), (src, img.size)
    data = img.tobytes()
    out.append(f"static const unsigned char icon_rgba_{size}[{len(data)}] = {{")
    for i in range(0, len(data), 12):
        chunk = ", ".join(str(b) for b in data[i:i + 12])
        out.append(f"    {chunk},")
    out.append("};")
    out.append("")

dst = os.path.join(ROOT, "gui", "src", "icon_rgba.h")
with open(dst, "w") as f:
    f.write("\n".join(out))
print("wrote", dst)
