#!/usr/bin/env python3
"""選字:從 BIG5 次常用字裡挑 2,000 個最高頻、目前還沒涵蓋的漢字。

產出 fonts/charsets/charset-ui-v5.txt 的「新增」那 2,000 字。完整的來源、規則
與計數寫在 charset-ui-v5.txt 的 ASCII 檔頭裡,這支腳本是它的可執行版本。

從 repo 根目錄執行。需要 fontTools(見 lib/EpdFont/scripts/requirements.txt)。

需要三份不在 repo 裡的外部資料:
  sorted.txt    ← https://technology.chtsai.org/charfreq/sorted.zip 解開(BIG5 編碼)
  hanzi-chars/  ← git clone https://github.com/zispace/hanzi-chars
  NotoSansTC-{Regular,Bold}.otf
                ← https://github.com/notofonts/noto-cjk/releases
                  (約 16 MB/顆,所以不收進 repo;放同一個目錄再用 --noto-dir 指過去)

用法:
  fonts/pick-big5-l2-chars.py <sorted.txt> <hanzi-chars/data-charlist> [輸出檔] \\
      [--noto-dir DIR] [--font-h PATH]

⚠️ 「目前涵蓋了什麼」的事實來源是 ubuntu_14_regular.h 的 interval + glyph 表,
   不是任何 charset 檔——charset 列了字不代表字型認得。所以本腳本直接解析已產好
   的 .h,而且必須在重產字型【之前】跑。
"""
import argparse
import re
from pathlib import Path

# repo-root-relative; override with --font-h / --noto-dir
FONT_H = 'lib/EpdFont/builtinFonts/ubuntu_14_regular.h'
NOTO_DIR = 'fonts/source'
NONLIST_MIN_FREQ = 200   # 規則 B:非表列但實際常見的門檻
TARGET = 2000


def covered_codepoints(path=FONT_H):
    """回傳這個字面【真的畫得出來】的碼位(有 glyph 且 dataLength/advanceX > 0)。"""
    txt = Path(path).read_text(encoding='utf-8', errors='replace')
    stem = re.search(r'static const EpdUnicodeInterval (\w+?)Intervals\[\]', txt).group(1)
    iv_body = re.search(re.escape(stem) + r'Intervals\[\] = \{(.*?)\};', txt, re.S).group(1)
    intervals = [tuple(int(x, 0) for x in m) for m in re.findall(
        r'\{\s*([0-9xXa-fA-F]+)\s*,\s*([0-9xXa-fA-F]+)\s*,\s*([0-9xXa-fA-F]+)\s*\}', iv_body)]
    gl_body = re.search(re.escape(stem) + r'Glyphs\[\] = \{(.*?)\};', txt, re.S).group(1)
    glyphs = [tuple(int(x, 0) for x in m) for m in re.findall(
        r'\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', gl_body)]
    out = set()
    for first, last, off in intervals:
        for cp in range(first, last + 1):
            i = off + (cp - first)
            if i < len(glyphs) and (glyphs[i][5] > 0 or glyphs[i][2] > 0):
                out.add(cp)
    return out


def load_moe_list(path):
    """每行一字;※ 標記非正字,〔〕內是異體字。取每行第一個漢字。"""
    out = set()
    for ln in Path(path).read_text(encoding='utf-8').splitlines():
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        base = ln.replace('※', '').strip().split('〔')[0].strip()
        if base:
            out.add(base[0])
    return out


def load_freq(path):
    """chtsai sorted.txt:BIG5 編碼,欄位 = 字 / 頻次 / 筆畫。

    必須用 big5hkscs 解:標準 big5 codec 吃不下 F9D6-F9FE 那七個 ETen 擴充字。
    """
    raw = Path(path).read_bytes().decode('big5hkscs')
    freq = {}
    for ln in raw.split('\n'):
        m = re.match(r'^(\S)\s+(\d+)\s+(\d+)\s*$', ln.rstrip('\r'))
        if m:
            freq[m.group(1)] = int(m.group(2))
    return freq


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('sorted_txt', help='chtsai sorted.txt (BIG5-encoded)')
    ap.add_argument('charlist_dir', type=Path, help="hanzi-chars' data-charlist directory")
    ap.add_argument('out_path', nargs='?', help='write the picked characters here')
    ap.add_argument('--noto-dir', default=NOTO_DIR,
                    help=f'directory holding NotoSansTC-{{Regular,Bold}}.otf (default: {NOTO_DIR})')
    ap.add_argument('--font-h', default=FONT_H,
                    help=f'face whose intervals define "already covered" (default: {FONT_H})')
    args = ap.parse_args()
    sorted_txt, charlist_dir, out_path = args.sorted_txt, args.charlist_dir, args.out_path
    noto_tpl = str(Path(args.noto_dir) / 'NotoSansTC-{}.otf')

    from fontTools.ttLib import TTFont

    freq = load_freq(sorted_txt)
    assert len(freq) == 13060, f'頻率表應有 13,060 字,實得 {len(freq)}'

    jia = load_moe_list(charlist_dir / '臺灣《常用國字表》（1982年）.txt')
    yi = load_moe_list(charlist_dir / '臺灣《次常用國字表》（1982年）.txt')
    assert (len(jia), len(yi)) == (4808, 6343), (len(jia), len(yi))
    assert not (jia & yi), '甲乙表不該重疊'

    covered = covered_codepoints(args.font_h)
    noto = set(TTFont(noto_tpl.format('Regular')).getBestCmap())
    noto &= set(TTFont(noto_tpl.format('Bold')).getBestCmap())

    pool = set(jia) | set(yi) | {c for c, f in freq.items() if f >= NONLIST_MIN_FREQ}
    cands, dropped = [], []
    for ch in pool:
        cp = ord(ch)
        if cp in covered or not (0x4E00 <= cp <= 0x9FFF):
            continue
        if cp not in noto:
            dropped.append(ch)
            continue
        cands.append((ch, freq.get(ch, 0)))
    cands.sort(key=lambda x: (-x[1], ord(x[0])))
    picked = cands[:TARGET]

    print(f'目前涵蓋漢字 {len([c for c in covered if 0x4E00 <= c <= 0x9FFF])}')
    print(f'候選池 {len(cands)}(NotoSansTC 缺字丟棄 {len(dropped)}: '
          f'{" ".join("U+%04X" % ord(c) for c in sorted(dropped))})')
    print(f'選出 {len(picked)}:乙表 {sum(1 for c, _ in picked if c in yi)} / '
          f'甲表 {sum(1 for c, _ in picked if c in jia)} / '
          f'非表列 {sum(1 for c, _ in picked if c not in jia and c not in yi)}')
    print(f'頻次範圍 U+{ord(picked[0][0]):04X}={picked[0][1]} .. U+{ord(picked[-1][0]):04X}={picked[-1][1]}')
    all_unc = [(c, f) for c, f in freq.items()
               if ord(c) not in covered and 0x4E00 <= ord(c) <= 0x9FFF]
    print(f'涵蓋「所有未涵蓋漢字」語料出現次數的 '
          f'{sum(f for _, f in picked) / sum(f for _, f in all_unc):.1%}'
          f'(候選池內則是 {sum(f for _, f in picked) / sum(f for _, f in cands):.1%})')
    cut = picked[-1][1]
    tie = sum(1 for _, f in cands if f == cut)
    print(f'⚠️ 切點在頻次 {cut},該頻次共 {tie} 字、取了 '
          f'{sum(1 for _, f in picked if f == cut)} 字 —— 最後這批是按碼位切的,不是按語言學訊號')

    text = ''.join(sorted((c for c, _ in picked), key=ord))
    if out_path:
        Path(out_path).write_text(text, encoding='utf-8')
        print(f'寫出 {out_path}')
    return text


if __name__ == '__main__':
    main()
