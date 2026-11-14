#!/usr/bin/env python3
"""產生單字卡專用的 SD 字型 AnkiCJK（繁中＋日文假名／漢字＋韓文＋英文音標）。

單字卡畫面會優先找 SD 卡上的 /fonts/AnkiCJK/（或 /.fonts/AnkiCJK/），找不到才用閱讀字型。
這支程式用 Windows 內建字型疊出一套涵蓋四種文字的字型（照順序補字，前面的優先）：
    微軟正黑體 msjh.ttc   繁體、簡體、假名、日文漢字
    游ゴシック YuGothR.ttc 日文漢字的漏網之魚、IPA
    맑은 고딕 malgun.ttf   韓文
    Segoe UI segoeui.ttf  其餘 IPA 音標
⚠️ 這些是 Windows 授權字型：產出只供自己的裝置使用，不要放進 git 或公開散布。
    想要可散布的版本，把 --regular／--fallback 換成 Noto Sans CJK（OFL）即可。

用法：
    python scripts/anki/build_anki_font.py -o "H:/我的文件/Anki/X3/fonts/AnkiCJK"
輸出：AnkiCJK_14.cpfont（內文）、AnkiCJK_22.cpfont（標題字）。複製到 SD 卡 /fonts/AnkiCJK/。
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CONVERTER = os.path.join(HERE, "..", "..", "lib", "EpdFont", "scripts", "fontconvert_sdcard.py")
WIN_FONTS = os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts")

# tc-reading 已含繁中閱讀會遇到的符號區塊與假名；補上韓文與完整 IPA
INTERVALS = "tc-reading,hangul,ipa-chars"
SIZES = "14,22"  # 要跟 src/anki/AnkiFonts.h 的 kBodyPt／kHeadPt 一致（找不到確切尺寸會取最接近的）


def ttc_to_ttf(path: str, tmp: str) -> str:
    """fontconvert_sdcard 的連字分析用 fontTools 開檔、不吃 .ttc：取第 0 個字面存成 .ttf。"""
    if not path.lower().endswith(".ttc"):
        return path
    from fontTools.ttLib import TTCollection

    out = os.path.join(tmp, os.path.splitext(os.path.basename(path))[0] + ".ttf")
    TTCollection(path).fonts[0].save(out)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output-dir", required=True)
    ap.add_argument("--sizes", default=SIZES)
    ap.add_argument("--regular", default=os.path.join(WIN_FONTS, "msjh.ttc"))
    ap.add_argument("--fallback", default=",".join(
        os.path.join(WIN_FONTS, f) for f in ("YuGothR.ttc", "malgun.ttf", "segoeui.ttf")))
    args = ap.parse_args()

    for p in [args.regular] + args.fallback.split(","):
        if not os.path.exists(p):
            print(f"找不到字型檔：{p}", file=sys.stderr)
            return 1
    os.makedirs(args.output_dir, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        regular = ttc_to_ttf(args.regular, tmp)
        fallback = ",".join(ttc_to_ttf(p, tmp) for p in args.fallback.split(","))
        cmd = [sys.executable, CONVERTER, "--intervals", INTERVALS, "--sizes", args.sizes, "--name", "AnkiCJK",
               "--regular", regular, "--fallback-regular", fallback, "--output-dir", args.output_dir]
        return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
