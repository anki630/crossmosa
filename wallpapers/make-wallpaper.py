#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make-wallpaper.py — 把 sleep/ 資料夾裡的任何圖片,一鍵轉成 Xteink X3 待機(輪播)合規 BMP。

韌體格式要求(lib/GfxRenderer/Bitmap.cpp parseHeaders 驗證):
  * 未壓縮 BMP:compression == 0 (BI_RGB)   ← 最常踩的雷(匯出時別選 RLE 壓縮)
  * 8-bit 灰階(bpp 屬於 {1,2,4,8,24,32})
  * 528x792 直向(1:1 對上待機畫面,裝置不縮放;尺寸上限 2048x3072)

用法:
  python3 make-wallpaper.py                # 處理預設資料夾(../sleep)裡所有圖
  python3 make-wallpaper.py --dir 路徑      # 換一個資料夾
  python3 make-wallpaper.py a.jpg b.png    # 只處理指定檔案(輸出放在原檔旁)
  選項:
    --mode cover|fit   cover=裁切填滿整個畫面(預設)  fit=完整放入、四周留邊
    --pad  black|white fit 模式的留邊顏色(預設 black)
    --size WxH         目標尺寸(預設 528x792)
    --tidy             把「轉檔用過的非-bmp原檔」移到 _src/ 保留(不刪除)

轉好後:資料夾裡的 .bmp 就是可用的。整批複製到 SD 卡的 /.sleep/ 即可(放 >=2 張才會輪播)。
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageOps
except ImportError:
    sys.exit("需要 Pillow:pip install pillow")

try:
    RESAMPLE = Image.Resampling.LANCZOS   # Pillow >= 9.1
except AttributeError:
    RESAMPLE = Image.LANCZOS              # 舊版

HERE = Path(__file__).resolve().parent
DEFAULT_DIR = HERE.parent / "sleep"       # <repo>/sleep
IMG_EXTS = {".png", ".jpg", ".jpeg", ".bmp", ".webp", ".gif", ".tif", ".tiff"}
VALID_BPP = {1, 2, 4, 8, 24, 32}


def parse_size(s):
    w, _, h = s.lower().partition("x")
    return int(w), int(h)


def verify(path):
    """直接解析輸出 BMP 標頭,回傳 (是否合格, 說明) — 比對韌體規則。"""
    with open(path, "rb") as f:
        h = f.read(34)
    if h[0:2] != b"BM":
        return False, "不是 BMP"
    w = struct.unpack("<i", h[18:22])[0]
    ht = struct.unpack("<i", h[22:26])[0]
    bpp = struct.unpack("<H", h[28:30])[0]
    comp = struct.unpack("<I", h[30:34])[0]
    ok = (bpp in VALID_BPP) and (comp == 0 or (bpp == 32 and comp == 3)) \
        and abs(w) <= 2048 and abs(ht) <= 3072
    return ok, "%dx%d bpp=%d compression=%d" % (w, ht, bpp, comp)


def convert(src, dst, size, mode, pad):
    im = Image.open(src)
    im = ImageOps.exif_transpose(im)          # 修正手機照片的旋轉方向
    im = im.convert("L")                      # 8-bit 灰階(e-ink 用)
    if mode == "cover":
        im = ImageOps.fit(im, size, RESAMPLE, centering=(0.5, 0.5))
    else:                                     # fit:完整放入,四周留邊
        bg = Image.new("L", size, 0 if pad == "black" else 255)
        thumb = im.copy()
        thumb.thumbnail(size, RESAMPLE)
        bg.paste(thumb, ((size[0] - thumb.width) // 2,
                         (size[1] - thumb.height) // 2))
        im = bg
    im.save(dst, format="BMP")                # Pillow 'L' -> 8-bit 未壓縮 BI_RGB


def gather(dir_path):
    """收集資料夾內可處理的圖(跳過 _src/、隱藏檔)。"""
    out = []
    for p in sorted(dir_path.iterdir()):
        if p.is_dir():
            continue
        if p.name.startswith(".") or p.name.startswith("_"):
            continue
        if p.suffix.lower() in IMG_EXTS:
            out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("images", nargs="*", help="指定檔案;省略則掃 --dir 整個資料夾")
    ap.add_argument("--dir", default=str(DEFAULT_DIR))
    ap.add_argument("--mode", choices=["cover", "fit"], default="cover")
    ap.add_argument("--pad", choices=["black", "white"], default="black")
    ap.add_argument("--size", default="528x792")
    ap.add_argument("--tidy", action="store_true")
    a = ap.parse_args()

    size = parse_size(a.size)

    if a.images:
        inputs = [Path(p) for p in a.images]
    else:
        d = Path(a.dir)
        if not d.is_dir():
            sys.exit("找不到資料夾:%s" % d)
        inputs = gather(d)
        if not inputs:
            sys.exit("%s 裡沒有圖片。把圖丟進去再跑一次。" % d)
        print("處理資料夾:%s" % d)

    ok_all = True
    made = 0
    for src in inputs:
        if not src.exists():
            print("✗ 找不到 %s" % src)
            ok_all = False
            continue
        dst = src.with_suffix(".bmp")
        try:
            convert(src, dst, size, a.mode, a.pad)
            made += 1
        except Exception as e:                # noqa: BLE001
            print("✗ %s:轉檔失敗 %s" % (src.name, e))
            ok_all = False
            continue
        ok, info = verify(dst)
        ok_all = ok_all and ok
        print("%s %s -> %s  (%s)" % ("✅" if ok else "❌", src.name, dst.name, info))

        # 把非-bmp 原檔挪到 _src/ 保留(可選)
        if a.tidy and src.suffix.lower() != ".bmp":
            srcdir = src.parent / "_src"
            srcdir.mkdir(exist_ok=True)
            src.rename(srcdir / src.name)

    print()
    if ok_all:
        print("全部合格(%d 張)。把 .bmp 整批複製到 SD 卡 /.sleep/ 即可。" % made)
    else:
        print("有檔案不合格,請看上面。")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
