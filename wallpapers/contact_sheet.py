#!/usr/bin/env python3
"""桌布總覽圖：把某個機型資料夾裡的 2-bit BMP 解碼成裝置實顯的樣子，排成一張總覽（2026-09-29，X4 桌布審閱用）。

用法：python3 contact_sheet.py <桌布資料夾> <輸出.png>
直式、橫式分兩區；橫式檔案是轉 90° 存的（ROTATE_270），總覽裡轉回正向方便看。
⚠️ 縮圖只適合看構圖與裁切 —— dither 的顆粒縮小後會被平均掉（看起來比實機糊或更灰），明暗與細節要在機器上看。
"""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from artworks import ARTWORKS  # noqa: E402

PAL = np.array([0, 85, 170, 255], dtype=np.uint8)


def decode_2bit(path):
    d = np.frombuffer(Path(path).read_bytes(), dtype=np.uint8)
    off = int(struct.unpack("<I", d[10:14].tobytes())[0])
    w = int(struct.unpack("<i", d[18:22].tobytes())[0])
    h = int(struct.unpack("<i", d[22:26].tobytes())[0])
    row_bytes = ((w * 2 + 31) // 32) * 4
    raw = d[off:off + row_bytes * h].reshape(h, row_bytes)
    b = raw[:, :(w + 3) // 4]
    idx = np.stack([(b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3], axis=2).reshape(h, -1)[:, :w]
    return Image.fromarray(PAL[idx[::-1]], "L")


def main():
    folder, out = Path(sys.argv[1]), Path(sys.argv[2])
    font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 12)
    head = ImageFont.truetype("/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", 18, index=3)  # index 3＝繁中
    sections = {"portrait": [], "landscape": []}
    for a in ARTWORKS:
        im = decode_2bit(folder / (a["slug"] + ".bmp"))
        if a["orientation"] == "landscape":
            im = im.transpose(Image.ROTATE_90)  # 抵銷存檔時的 ROTATE_270
        sections[a["orientation"]].append((a["slug"], im))
    w0, h0 = sections["portrait"][0][1].size
    s = 240 / h0  # 直式縮到 240 高
    specs = [("直式 portrait", sections["portrait"], 8), ("橫式 landscape（實機要把機器轉 90° 看）", sections["landscape"], 5)]
    tiles_h = []
    width = 0
    for title, items, per_row in specs:
        tw, th = round(items[0][1].width * s), round(items[0][1].height * s)
        rows = (len(items) + per_row - 1) // per_row
        width = max(width, per_row * (tw + 12) + 12)
        tiles_h.append(30 + rows * (th + 26))
    sheet = Image.new("L", (width, sum(tiles_h) + 20), 255)
    d = ImageDraw.Draw(sheet)
    y = 10
    for (title, items, per_row), sec_h in zip(specs, tiles_h):
        d.text((12, y), title, fill=0, font=head)
        y0 = y + 30
        tw, th = round(items[0][1].width * s), round(items[0][1].height * s)
        for i, (slug, im) in enumerate(items):
            r, c = divmod(i, per_row)
            x, yy = 12 + c * (tw + 12), y0 + r * (th + 26)
            sheet.paste(im.resize((tw, th), Image.BOX), (x, yy))
            d.rectangle([x - 1, yy - 1, x + tw, yy + th], outline=160)
            d.text((x, yy + th + 4), slug, fill=0, font=font)
        y += sec_h
    sheet.save(out)
    print(f"{out}  {sheet.size[0]}x{sheet.size[1]}  portrait={len(sections['portrait'])} landscape={len(sections['landscape'])}")


if __name__ == "__main__":
    main()
