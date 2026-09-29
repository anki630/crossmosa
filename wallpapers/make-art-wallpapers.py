#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make-art-wallpapers.py — 世界名畫清單批次產生 X3 待機壁紙(2-bit 原生四色,繞過韌體 dither)

正式管線(2026-07-22 定案,經實機驗證與多輪 A/B):
  原圖 → 灰階 → autocontrast(cutoff=1) → Lanczos 縮到 528×792(最終顯示解析度)
       → Floyd-Steinberg dither(**僅畫作區**,4 階 0/85/170/255)
       → 疊乾淨標籤(黑底白字,就近取整不 dither = 「先算圖再打字」)
       → 橫式最後無損旋轉 90°(整數影像)
       → 寫 2-bit 原生 BMP(色盤 0/85/170/255)

為什麼是這套(踩過的坑,別亂改):
  * **2-bit 而非 8-bit**:8-bit 灰階會被韌體用它自己的 Atkinson 重新 dither,電腦端演算法白做。
    2-bit(bpp≤2)→ 韌體判 nativePalette=true → 原樣顯示,演算法決定權才在電腦端。實機已驗證。
  * **Floyd-Steinberg 而非 Blue Noise/Atkinson/Jarvis**:誤差擴散(自適應)細節勝 blue noise;
    FS 暗部比 Jarvis 乾淨(小核、誤差不亂散);比韌體 Atkinson 色調更平滑(不丟誤差)。
  * **autocontrast**:對比/可見度這一軸跟演算法無關,靠它補;不加會顯得淡、費眼。
  * **先算圖再打字**:整張一起 dither 會把文字反鋸齒邊緣打散成雜點 → 文字糊。標籤區只做就近取整。
  * **dither 在最終 528×792 解析度、當最後一步**:裝置 1:1 不縮放顯示,dither 點才精準落像素。

橫式構圖(依畫作原始比例)先在 792×528 排版再整張轉 90° 存 528×792;看時把裝置逆時針轉 90°
(ROTATE_270:畫作的上方朝螢幕右邊。2026-09-29 更正,之前寫的「順時針」是改 ROTATE_270 前的舊說法)。

用法:
  python3 make-art-wallpapers.py            # X3 全部(輸出 sleep/)
  python3 make-art-wallpapers.py mona_lisa great_wave   # 只指定 slug
  python3 make-art-wallpapers.py --device x4             # X4 版(480×800,輸出 sleep-x4/)
輸出:<機型資料夾>/<slug>.bmp(2-bit)。先跑 fetch_sources.py 下載原圖。

機型(2026-09-29 加 X4):X4 螢幕 480×800,拿 X3 的 528×792 去放,韌體會整張縮成 480×720、
上下留白邊,縮放也打散 dither 點 → 必須從原圖在 X4 的最終解析度重算,不能把 X3 的成品縮小。
artworks.py 某筆要在 X4 上改裁切,就在那筆加 x4=dict(focus=(x, y)) 或 x4=dict(fit=True)。
"""
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageOps

from artworks import ARTWORKS

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "sleep" / "_src"
OUT = HERE.parent / "sleep"   # set_device() 依機型改
DEVICE = "x3"
BN = None  # 不再用 blue noise;保留欄位以防未來切換

LEVELS = np.array([0, 85, 170, 255], dtype=np.float32)
PORTRAIT = (528, 792)
# 標籤(2026-07-22 定案,選用 subtle + 右下右對齊):畫作滿版,右下角疊「白字黑描邊」
# 低調標籤,不佔黑條。字級/描邊刻意小=讀得到但不搶戲。文字在 dither 後疊、再就近取整保乾淨。
CAP_MR, CAP_MB, CAP_GAP = 20, 18, 3           # 右邊距 / 下邊距 / 兩行間距
# 字級用 X3 實體尺寸倒推(3.7"、~258 PPI、~0.099 mm/px、10px≈1mm;30cm 手持視角):
# 標題 21px = cap ~1.5mm ~17′(舒適);作者 16px = cap ~1.15mm ~13′(次級文字剛過舒適線)。
# 舊值 19/13 的作者行只有 ~0.92mm ~10.6′,踩在人眼舒適線下 → 認不出,故加大。
CAP_TITLE_SIZE, CAP_TITLE_STROKE = 21, 2      # 作品名(bold)字級 / 黑描邊粗細
CAP_ARTIST_SIZE, CAP_ARTIST_STROKE = 16, 2    # 藝術家(regular)字級 / 黑描邊粗細
# 機型設定。X3 的值就是上面 2026-07-22 定案的那組,不要動(改了就跟 sleep/ 裡在用的 50 張不同)。
# X4(2026-09-29):4.26"、800×480、~219 PPI、~0.116 mm/px(SDK BoardConfig.h 的註解)。標籤照實體大小換算,
#   讓字在兩台上看起來一樣大:px_X4 = px_X3 × 0.099 / 0.116 → 標題 21→18、作者 16→14、右邊距 20→17、下邊距 18→15;
#   描邊與行距維持 2／3 px(描邊再細就斷)。
DEVICES = {
    "x3": dict(portrait=(528, 792), out="sleep", mr=20, mb=18, gap=3, title=21, artist=16, stroke=2),
    "x4": dict(portrait=(480, 800), out="sleep-x4", mr=17, mb=15, gap=3, title=18, artist=14, stroke=2),
}


def set_device(name):
    """把機型的尺寸、輸出資料夾、標籤字級套到模組層的常數(其餘程式照舊讀它們)。"""
    global DEVICE, OUT, PORTRAIT, CAP_MR, CAP_MB, CAP_GAP
    global CAP_TITLE_SIZE, CAP_TITLE_STROKE, CAP_ARTIST_SIZE, CAP_ARTIST_STROKE
    d = DEVICES[name]
    DEVICE = name
    OUT = HERE.parent / d["out"]
    PORTRAIT = d["portrait"]
    CAP_MR, CAP_MB, CAP_GAP = d["mr"], d["mb"], d["gap"]
    CAP_TITLE_SIZE, CAP_ARTIST_SIZE = d["title"], d["artist"]
    CAP_TITLE_STROKE = CAP_ARTIST_STROKE = d["stroke"]


FONT_DIR = Path("/usr/share/fonts/truetype/dejavu")
FONT_BOLD = FONT_DIR / "DejaVuSerif-Bold.ttf"
FONT_REG = FONT_DIR / "DejaVuSerif.ttf"
try:
    RES = Image.Resampling.LANCZOS
except AttributeError:
    RES = Image.LANCZOS

FS_KERNEL = [(1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16)]


def _quant(v):
    return LEVELS[int(np.argmin(np.abs(LEVELS - v)))]


def fs_dither(imL):
    """Serpentine Floyd-Steinberg → 4 階 index(0..3)。畫作區用。"""
    a = np.asarray(imL, dtype=np.float32).copy()
    h, w = a.shape
    for y in range(h):
        serp = y & 1
        xr = range(w - 1, -1, -1) if serp else range(w)
        s = -1 if serp else 1
        row = a[y]
        for x in xr:
            old = row[x]
            new = _quant(old)
            err = old - new
            row[x] = new
            for dx, dy, wt in FS_KERNEL:
                xx, yy = x + dx * s, y + dy
                if 0 <= xx < w and 0 <= yy < h:
                    a[yy, xx] += err * wt
    return np.clip(np.round(a / 85.0), 0, 3).astype(np.uint8)


def nearest_idx(imL):
    """就近取整到 4 階(不 dither)。標籤區用,保文字乾淨。"""
    return np.clip(np.round(np.asarray(imL, dtype=np.float32) / 85.0), 0, 3).astype(np.uint8)


def _wrap_lines(font, text, max_w, stroke):
    """超過 max_w 才換行(含描邊寬度)。兩行時挑「最寬那行最窄」的斷點求平衡;
    更長才退到貪婪多行。單行放得下就原樣回傳=舊圖 byte 不變。"""
    def tw(s):
        b = font.getbbox(s)
        return b[2] - b[0] + 2 * stroke
    if tw(text) <= max_w:
        return [text]
    words = text.split()
    best = None
    for i in range(1, len(words)):
        l1, l2 = " ".join(words[:i]), " ".join(words[i:])
        m = max(tw(l1), tw(l2))
        if m <= max_w and (best is None or m < best[0]):
            best = (m, [l1, l2])
    if best:
        return best[1]
    lines, cur = [], words[0]
    for wd in words[1:]:
        if tw(cur + " " + wd) <= max_w:
            cur += " " + wd
        else:
            lines.append(cur)
            cur = wd
    lines.append(cur)
    return lines


def draw_caption_overlay(img, title, artist):
    """在已 dither 的 L 影像**右下角**疊低調白字黑描邊標籤(右對齊)。就地修改 img。
    長作品名自動斷成兩行(takiyasha_skeleton 曾超出左邊界被切)。"""
    w, h = img.size
    d = ImageDraw.Draw(img)
    tf = ImageFont.truetype(str(FONT_BOLD), CAP_TITLE_SIZE)
    af = ImageFont.truetype(str(FONT_REG), CAP_ARTIST_SIZE)
    max_w = w - 2 * CAP_MR
    tlines = _wrap_lines(tf, title, max_w, CAP_TITLE_STROKE)
    alines = _wrap_lines(af, artist, max_w, CAP_ARTIST_STROKE)
    ta, td = tf.getmetrics()
    aa, ad = af.getmetrics()
    tlh, alh = ta + td, aa + ad
    total = len(tlines) * tlh + len(alines) * alh + (len(tlines) + len(alines) - 1) * CAP_GAP
    y = h - CAP_MB - total
    for ln in tlines:
        d.text((w - CAP_MR, y), ln, font=tf, fill=255,
               stroke_width=CAP_TITLE_STROKE, stroke_fill=0, anchor="ra")
        y += tlh + CAP_GAP
    for ln in alines:
        d.text((w - CAP_MR, y), ln, font=af, fill=255,
               stroke_width=CAP_ARTIST_STROKE, stroke_fill=0, anchor="ra")
        y += alh + CAP_GAP


def compose_idx(art):
    art = {**art, **art.get(DEVICE, {})}  # 機型專屬的裁切設定(例如 x4=dict(focus=...))蓋過共用的
    slug, orient = art["slug"], art["orientation"]
    im = Image.open(SRC / (slug + ".jpg"))
    im = ImageOps.exif_transpose(im).convert("L")
    im = ImageOps.autocontrast(im, cutoff=1)
    cw, ch = PORTRAIT if orient == "portrait" else PORTRAIT[::-1]
    if art.get("fit"):
        # 完整呈現:主體橫跨整幅、怎麼裁都會切壞的畫,寧可補黑邊也要看到完整構圖。
        contain = ImageOps.contain(im, (cw, ch), RES)
        fitted = Image.new("L", (cw, ch), 0)
        fitted.paste(contain, ((cw - contain.width) // 2, (ch - contain.height) // 2))
    else:
        # cover 裁滿版;focus=(x,y) 0..1 指定裁切焦點,主體偏一邊時移過去(預設置中)。
        focus = art.get("focus", (0.5, 0.5))
        fitted = ImageOps.fit(im, (cw, ch), RES, centering=focus)
    art_idx = fs_dither(fitted)                                     # 畫作區:FS
    img = Image.fromarray(LEVELS[art_idx].astype(np.uint8), "L")
    draw_caption_overlay(img, art["title"], art["artist"])         # 疊低調右下標籤
    full = nearest_idx(img)  # 藝術區已是原生值→不變;文字反鋸齒邊緣就近取整→乾淨不 dither
    if orient == "landscape":
        # ROTATE_270 = 相對舊版(ROTATE_90)轉 180°:畫作的上方朝螢幕右邊,看的時候把裝置往逆時針轉 90 度
        full = np.asarray(Image.fromarray(full, "L").transpose(Image.ROTATE_270))
    return full


def write_2bit_bmp(idx, path):
    h, w = idx.shape
    padw = ((w + 3) // 4) * 4
    padded = np.zeros((h, padw), dtype=np.uint8)
    padded[:, :w] = idx
    g = padded.reshape(h, padw // 4, 4)
    packed = (g[:, :, 0] << 6) | (g[:, :, 1] << 4) | (g[:, :, 2] << 2) | g[:, :, 3]
    bpr = padw // 4
    row_bytes = ((bpr + 3) // 4) * 4
    out = np.zeros((h, row_bytes), dtype=np.uint8)
    out[:, :bpr] = packed
    out = out[::-1]  # bottom-up
    data = out.tobytes()
    pal = bytes([0, 0, 0, 0, 85, 85, 85, 0, 170, 170, 170, 0, 255, 255, 255, 0])
    off = 14 + 40 + len(pal)
    fh = b"BM" + struct.pack("<IHHI", off + len(data), 0, 0, off)
    ih = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 2, 0, len(data), 2835, 2835, 4, 0)
    Path(path).write_bytes(fh + ih + pal + data)


def verify_2bit(path, expected_idx):
    """向量化解碼(韌體同規則),確認 bytes 正確。回傳 (ok, w, h, bpp, comp)。"""
    d = np.frombuffer(Path(path).read_bytes(), dtype=np.uint8)
    off = int(struct.unpack("<I", d[10:14].tobytes())[0])
    w = int(struct.unpack("<i", d[18:22].tobytes())[0])
    h = int(struct.unpack("<i", d[22:26].tobytes())[0])
    bpp = int(struct.unpack("<H", d[28:30].tobytes())[0])
    comp = int(struct.unpack("<I", d[30:34].tobytes())[0])
    pal = np.array([(77 * int(d[56 + i * 4]) + 150 * int(d[55 + i * 4]) + 29 * int(d[54 + i * 4])) >> 8
                    for i in range(4)], dtype=np.uint8)
    row_bytes = ((w * 2 + 31) // 32) * 4
    raw = d[off:off + row_bytes * h].reshape(h, row_bytes)
    b = raw[:, :(w + 3) // 4]
    idx = np.stack([(b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3], axis=2).reshape(h, -1)[:, :w]
    idx = idx[::-1]
    ok = np.array_equal(pal[idx], LEVELS[expected_idx].astype(np.uint8))
    return ok, w, h, bpp, comp


def main():
    args = sys.argv[1:]
    if "--device" in args:
        i = args.index("--device")
        name = args[i + 1] if i + 1 < len(args) else ""
        if name not in DEVICES:
            sys.exit("--device 要是 %s 之一" % "／".join(DEVICES))
        set_device(name)
        del args[i:i + 2]
    OUT.mkdir(exist_ok=True)
    print("機型 %s:%d×%d → %s/" % (DEVICE, PORTRAIT[0], PORTRAIT[1], OUT.name))
    slugs = args
    todo = [a for a in ARTWORKS if not slugs or a["slug"] in slugs]
    t0 = time.time()
    ok_all = True
    landscape = []
    for a in todo:
        src = SRC / (a["slug"] + ".jpg")
        if not src.exists():
            print("✗ 缺原圖(先跑 fetch_sources.py):%s" % src.name)
            ok_all = False
            continue
        idx = compose_idx(a)
        dst = OUT / (a["slug"] + ".bmp")
        write_2bit_bmp(idx, str(dst))
        ok, w, h, bpp, comp = verify_2bit(str(dst), idx)
        ok_all = ok_all and ok
        tag = "[橫轉]" if a["orientation"] == "landscape" else "[直] "
        if a["orientation"] == "landscape":
            landscape.append(dst.name)
        print("%s %s %-26s -> %-28s %dx%d bpp=%d comp=%d %7dB roundtrip=%s (%.1fs)" % (
            "✅" if ok else "❌", tag, a["slug"], dst.name, w, h, bpp, comp,
            dst.stat().st_size, ok, time.time() - t0))

    print("\n完成 %d 張,總 %.1fs。" % (len(todo), time.time() - t0))
    if landscape:
        print("\n橫式構圖(看的時候把裝置逆時針轉 90°)共 %d 張:" % len(landscape))
        for f in landscape:
            print("  - " + f)
    print("\n全部合格。" if ok_all else "\n有檔案 roundtrip 不符,請看上面。")
    print("複製 %s/*.bmp 到 %s 的 SD 卡 /.sleep/ 即可(2-bit 原生格式,韌體原樣顯示、不再 dither)。"
          % (OUT.name, DEVICE.upper()))
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
