#!/usr/bin/env python3
"""Formosa Cover：沒有封面的書用的三種歐風底框 → src/components/covers/CoverFrames.h（2026-10-07）。

素材：codex 的圖片生成工具依外部 AI 設計稿重畫的三張 1024×1536 灰階線稿（A 古典花紋、B 三段式、C 花卉邊框）。
不放進 repo（每張 1.7 MB）；在工作區 work/home-cover/art/{A,B,C}-raw.png。重產：
    python3 scripts/gen_cover_frames.py <art 目錄>

每種底框 × 三種尺寸（v373 起十七種底框）（正在閱讀 160×240、書架 144×216、最近閱讀 128×192；v367 起，原 112×168）：
  縮放（LANCZOS）→ 書名區清白 →（A、B 門檻 170／C Floyd–Steinberg 抖色）→ 1-bit（列優先、高位在前、每列補到整數位元組，1＝黑）
  → raw deflate（wbits=-15）壓縮。畫的時候用 InflateReader 一次解到暫存區（輸出緩衝區就是字典，不配 32 KB 窗口）。
書名區（textX／Y／W／H）跟點陣一起輸出，韌體在裡面排書名（平均分行、14 粗 → 10 粗 → 截斷）。
圓角與 1 px 外框由主題在執行期畫（跟真封面同一套），這裡不畫。
"""
import sys
import zlib
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [(160, 240), (144, 216), (128, 192)]
# 花紋整張縮進來的白邊（v370）：封面四角是半徑 12 的圓角遮罩，原圖最外圈的直角框線會被切成缺口（維護者實機看到）。
#   縮進 4 px 之後，花紋的直角落在圓角裡面（(4,4) 離圓心 (12,12) 只有 11.3 < 12），只有白邊被切。
MARGIN = 4
# (名稱, 檔案, 書名區比例, 轉法, 小尺寸（寬 < 150：書架 144、最近閱讀 128）時的書名區, 自己重畫的框：0＝不畫、1＝粗題簽框、2＝細雙線)
#   v370 新增八款、v372 改版型（codex 生成；維護者挑過五輪：要有文化、圖案簡單、縮成 1-bit 不能變雜、插畫要有 IP 質感）。
#   v372 版型（維護者：保留三層、書名區放大偏上）：layout＝(上方花紋帶在原圖的結束位置, 下方主圖在原圖的開始位置)，
#   由 compose() 重排 —— AI 畫的書名框不一定照比例，所以書名區由我們決定（約 40%–45% 高、88% 寬）。
#   最後一欄 label：1＝粗框（插畫、像素）、2＝細雙線（花紋）。
STYLES = [
    ("Classic", "A-raw.png", (0.17, 0.22, 0.83, 0.78), "threshold", None, 0, None),
    ("ThreeBand", "B-raw.png", (0.03, 0.345, 0.97, 0.655), "threshold", None, 0, None),
    ("Floral", "C-raw.png", (0.21, 0.21, 0.79, 0.79), "dither", (0.13, 0.15, 0.87, 0.85), 0, None),
    ("NordicReindeer", "L-v5-raw.png", None, "threshold", None, 2, (0.135, 0.573)),
    ("NordicMoose", "L2-v5-raw.png", None, "threshold", None, 2, (0.13, 0.51)),
    ("NordicCottage", "L3-v5-raw.png", None, "threshold", None, 2, (0.19, 0.556)),
    ("NordicFolk", "M-v5-raw.png", None, "threshold", None, 2, (0.128, 0.49)),
    ("TaiwanWeave", "Q-v5-raw.png", None, "threshold", None, 2, (0.168, 0.605)),
    ("Cat", "S2-v5-raw.png", None, "threshold", None, 1, (0.10, 0.50)),
    ("Shiba", "V2-v5-raw.png", None, "threshold", None, 1, (0.08, 0.484)),
    ("PixelDragon", "T-v5-raw.png", None, "threshold", None, 1, (0.18, 0.516)),
    ("PixelPrincess", "U-v5-raw.png", None, "threshold", None, 1, (0.15, 0.495)),
    ("Zentangle", "W-v5-raw.png", None, "threshold", None, 2, (0.143, 0.48)),
    # v373 公有領域經典（維護者 2026-10-08；小王子因法國保護到 2032、美國到 2039＋角色商標而不做）：
    #   鳥獸戲畫（線條在原圖上先加粗 MinFilter 7，縮小才不會斷）、愛麗絲（Tenniel 風，不用迪士尼造型）、山海經九尾狐、福爾摩斯（Paget 時代剪影）
    ("Chojugiga", "X1-v6-raw.png", None, "threshold", None, 1, (0.145, 0.49)),
    ("Alice", "X2-v6-raw.png", None, "threshold", None, 1, (0.10, 0.475)),
    ("ShanHaiJing", "X3-v6-raw.png", None, "threshold", None, 2, (0.122, 0.443)),
    ("Holmes", "X4-v6-raw.png", None, "threshold", None, 2, (0.14, 0.496)),
]

# 書名區直接指定（v378）：古典款是拱形窗，加大白框會切到拱線，只能在拱窗裡面量一塊沒有墨的長方形。
#   最近閱讀 128×192 的原書名區只有 79 px 寬（排版可用 71），英文「Sample」（10 號粗 74 px）也放不下 → 從字中間斷。
#   改成拱窗中段（量到 x 22..105 全白）：寬 84、高 77（三行 10 號字 66 px 放得下）。不畫任何東西，只換書名區；下面會驗證全白。
BOX_OVERRIDE = {("Classic", 128): (22, 57, 106, 134)}


def compose(src, top_end, art_top, iw, ih, panel_gap=0.02, bottom_frac=0.40):
    """v372 版型（維護者：保留三層，但書名區放大、偏上）：AI 畫的書名框比例不一定照提示詞，
    所以從原圖裁出「上方花紋帶」（0..top_end）與「下方主圖」（art_top..1），自己重排：
    花紋帶貼頂（同寬）、主圖等比縮進底部 bottom_frac 的高度（置中、貼底），中間全部留給書名區。
    回傳 (灰階畫布, 書名區比例 area)。"""
    W, H = src.size
    canvas = Image.new("L", (iw, ih), 255)
    top = src.crop((0, 0, W, int(H * top_end)))
    th = max(1, round(top.height * iw / W))
    canvas.paste(top.resize((iw, th), Image.LANCZOS), (0, 0))
    bot = src.crop((0, int(H * art_top), W, H))
    bh_max = int(ih * bottom_frac)
    sc = min(iw / bot.width, bh_max / bot.height)
    bw, bh = max(1, round(bot.width * sc)), max(1, round(bot.height * sc))
    canvas.paste(bot.resize((bw, bh), Image.LANCZOS), ((iw - bw) // 2, ih - bh))
    y0 = (th + panel_gap * ih) / ih
    y1 = (ih - bh - panel_gap * ih) / ih
    return canvas, (0.06, y0 + 0.02, 0.94, y1 - 0.02)


def render(src, area, mode, grow, w, h, label=0, layout=None):
    iw, ih = w - 2 * MARGIN, h - 2 * MARGIN
    base = Image.new("L", (w, h), 255)
    if layout:  # v372：自己重排（見 compose）；書名區由版型決定，不看 area
        canvas, area = compose(src, layout[0], layout[1], iw, ih)
        base.paste(canvas, (MARGIN, MARGIN))
    else:
        base.paste(src.resize((iw, ih), Image.LANCZOS), (MARGIN, MARGIN))
    # v378：門檻 140 → 150。140 是書架還用 112 時定的；v367 書架改成 144 之後花卉框在書架上書名區只剩 79 px，
    #   英文單字連 10 號粗體都放不下（「Sample」74 px）→ 從字中間斷成「Sampl／e」（維護者實機截圖）。
    small = grow is not None and w < 150
    if small:
        area = grow
    box = [MARGIN + int(iw * area[0]), MARGIN + int(ih * area[1]), MARGIN + int(iw * area[2]), MARGIN + int(ih * area[3])]
    d = ImageDraw.Draw(base)
    if label == 1:  # 自己畫題簽：黑框（約原圖比例的粗細）＋裡面全白
        t = max(2, round(w / 48))
        d.rectangle([box[0] - t, box[1] - t, box[2] + t, box[3] + t], fill=0)
    elif label == 2:  # 細雙線框（裝飾藝術）：外線、白縫、內線
        d.rectangle([box[0] - 5, box[1] - 5, box[2] + 5, box[3] + 5], fill=0)
        d.rectangle([box[0] - 4, box[1] - 4, box[2] + 4, box[3] + 4], fill=255)
        d.rectangle([box[0] - 2, box[1] - 2, box[2] + 2, box[3] + 2], fill=0)
    d.rectangle(box, fill=255)
    if small:  # 擴大的白框要一圈細線收邊（原圖的白框邊線被蓋掉了）
        d.rectangle([box[0] - 1, box[1] - 1, box[2], box[3]], outline=0)
    img = base.point(lambda v: 0 if v < 170 else 255, "1") if mode == "threshold" else base.convert(
        "1", dither=Image.FLOYDSTEINBERG)
    # 書名區內一律白（抖色可能在邊緣撒點）
    ImageDraw.Draw(img).rectangle([box[0] + 1, box[1] + 1, box[2] - 1, box[3] - 1], fill=1)
    return img, box


def pack(img):
    w, h = img.size
    stride = (w + 7) // 8
    out = bytearray(stride * h)
    px = img.load()
    for y in range(h):
        for x in range(w):
            if px[x, y] == 0:
                out[y * stride + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


def main():
    art = Path(sys.argv[1] if len(sys.argv) > 1 else "")
    out_path = Path(__file__).resolve().parent.parent / "src/components/covers/CoverFrames.h"
    lines = [
        "#pragma once",
        "// ⚠️ 自動產生：scripts/gen_cover_frames.py（素材與說明見該腳本開頭）。不要手改。",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace coverframes {",
        "",
        "struct Frame {",
        "  uint16_t width;",
        "  uint16_t height;",
        "  uint16_t textX, textY, textW, textH;  // 書名區（點陣座標）",
        "  const uint8_t* deflated;              // raw deflate；解開＝((width+7)/8)*height bytes，1＝黑",
        "  uint32_t deflatedLen;",
        "};",
        "",
    ]
    table = []
    total = 0
    for name, fname, area, mode, grow, label, layout in STYLES:
        src = Image.open(art / fname).convert("L")
        for (w, h) in SIZES:
            img, box = render(src, area, mode, grow, w, h, label, layout)
            if (name, w) in BOX_OVERRIDE:
                box = list(BOX_OVERRIDE[(name, w)])
                px = img.load()
                assert all(px[x, y] for x in range(box[0], box[2]) for y in range(box[1], box[3])), f"{name}{w} 書名區有墨"
            raw = pack(img)
            c = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp = c.compress(raw) + c.flush()
            total += len(comp)
            ident = f"k{name}{w}x{h}"
            hexs = ", ".join(f"0x{b:02x}" for b in comp)
            lines.append(f"static const uint8_t {ident}[{len(comp)}] = {{{hexs}}};")
            table.append(f"    {{{w}, {h}, {box[0]}, {box[1]}, {box[2] - box[0]}, {box[3] - box[1]}, {ident}, {len(comp)}}},")
    lines += [
        "",
        f"constexpr int kStyles = {len(STYLES)};",
        f"constexpr int kSizes = {len(SIZES)};",
        "// [style * kSizes + size]；size 0＝160×240、1＝144×216、2＝128×192",
        "static const Frame kFrames[kStyles * kSizes] = {",
        *table,
        "};",
        f"// 合計 {total} bytes（壓縮後）",
        "",
        "}  // namespace coverframes",
        "",
    ]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")
    print(out_path, total, "bytes")


if __name__ == "__main__":
    main()
