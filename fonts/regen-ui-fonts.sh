#!/bin/bash
# Regenerate the four built-in UI font faces (ubuntu_10/14 x regular/bold).
#
# Run from the REPOSITORY ROOT:
#     ./fonts/regen-ui-fonts.sh              # full run (needs Noto Sans TC, see below)
#     ./fonts/regen-ui-fonts.sh --faces-only # skip re-subsetting; use the shipped subsets
#
# WHAT THIS DOES
#   Stage 1  re-subsets the main Han layer (fonts/ui-subset/UI-NotoSans-{Regular,Bold}.otf)
#            from full Noto Sans TC, using fonts/charsets/charset-ui-v5.txt as the set.
#   Stage 2  asserts the resulting cmap is exactly (charset union ranges) intersect NotoSansTC.
#   Stage 3  rebuilds the four lib/EpdFont/builtinFonts/ubuntu_*.h faces.
#
#   Only stage 1 needs the full Noto Sans TC download. If you have not changed the
#   character set and only want to rebuild the .h files, use --faces-only: the subset
#   fonts are shipped in fonts/ui-subset/ precisely so that works offline.
#
# REQUIREMENTS
#   * Python 3.10+ with the packages in lib/EpdFont/scripts/requirements.txt
#     (fontTools and freetype-py; pyftsubset comes with fontTools):
#         python3 -m venv .venv && .venv/bin/pip install -r lib/EpdFont/scripts/requirements.txt
#     Point this script at it with:  PYTHON=.venv/bin/python3 PYFTSUBSET=.venv/bin/pyftsubset
#   * Stage 1 only: NotoSansTC-Regular.otf and NotoSansTC-Bold.otf. They are NOT in this
#     repo (about 16 MB each; the shipped subsets are the point). Download the OTF release
#     from  https://github.com/notofonts/noto-cjk/releases  (Sans -> NotoSansTC OTFs, or
#     the "05_NotoSansCJK-OTF-VF" bundle) and put both files in one directory, then:
#         NOTO_DIR=/path/to/that/directory ./fonts/regen-ui-fonts.sh
#
# THE THREE SUBSET LAYERS (all three feed every face; see the "Command used:" header
# inside each .h for the exact argument list)
#   UI-NotoSans-*   main Han layer, subset of Noto Sans TC        -- regenerated here
#   UI-CJKextra-*   357 chars Noto Sans TC lacks, from Noto Sans CJK TC face[3]
#   UI-Symextra-*   3 symbols neither Noto face has, from DejaVu Sans
#   The latter two are static inputs; this script does not touch them.
#
# WARNING -- the pyftsubset --unicodes list below is not a guess. It was derived by
# reverse-engineering the cmap of the shipped UI-NotoSans-Regular.otf and verified
# codepoint by codepoint (charset-ui-v4 plus these ranges, intersected with NotoSansTC,
# reproduced the shipped cmap exactly). Dropping FE50-FE6F loses 26 small-form
# punctuation variants. NEVER add the whole 4E00-9FFF range: that pulls all ~20,000
# Han back in and the firmware no longer fits in flash.
set -e

FONTS=fonts
SRC=lib/EpdFont/builtinFonts
CHARSET="$FONTS/charsets/charset-ui-v5.txt"

PYTHON="${PYTHON:-python3}"
PYFTSUBSET="${PYFTSUBSET:-pyftsubset}"
NOTO_DIR="${NOTO_DIR:-$FONTS/source}"

FACES_ONLY=0
[ "${1:-}" = "--faces-only" ] && FACES_ONLY=1

if [ ! -d "$SRC" ] || [ ! -f "$CHARSET" ]; then
  echo "error: run this from the repository root (expected $SRC and $CHARSET)" >&2
  exit 1
fi

# The charset header is ASCII-only, and the WHOLE file -- header included -- is fed to
# pyftsubset via --text-file. ASCII is already in the set, so the header costs nothing;
# but a single CJK character in the header would silently enter the font. Stage 2 is what
# catches that.
UNICODES="2150-218F,2460-2473,2500-25FF,3000-303F,30FB,203B,3013,FF00-FFEF,FFFD,FE50-FE6F"

if [ "$FACES_ONLY" -eq 0 ]; then
  echo "=== 1/3 re-subsetting the main Han layer from Noto Sans TC ==="
  for w in Regular Bold; do
    if [ ! -f "$NOTO_DIR/NotoSansTC-$w.otf" ]; then
      echo "error: $NOTO_DIR/NotoSansTC-$w.otf not found." >&2
      echo "       Download Noto Sans TC (see header) and set NOTO_DIR, or pass" >&2
      echo "       --faces-only to rebuild the .h files from the shipped subsets." >&2
      exit 1
    fi
    "$PYFTSUBSET" "$NOTO_DIR/NotoSansTC-$w.otf" \
      --text-file="$CHARSET" \
      --unicodes="$UNICODES" \
      --layout-features='' --no-hinting \
      --output-file="$FONTS/ui-subset/UI-NotoSans-$w.otf"
    echo "  UI-NotoSans-$w.otf: $(wc -c < "$FONTS/ui-subset/UI-NotoSans-$w.otf") bytes"
  done

  echo "=== 2/3 verifying cmap == (charset union ranges) intersect NotoSansTC ==="
  "$PYTHON" - "$CHARSET" "$UNICODES" "$FONTS/ui-subset" "$NOTO_DIR" <<'PY'
import sys
from fontTools.ttLib import TTFont
charset, ranges, uidir, notodir = sys.argv[1:5]
want_txt = set(ord(c) for c in open(charset, encoding='utf-8').read())
rng = set()
for p in ranges.split(','):
    if '-' in p:
        a, b = p.split('-'); rng.update(range(int(a, 16), int(b, 16) + 1))
    else:
        rng.add(int(p, 16))
for w in ('Regular', 'Bold'):
    noto = set(TTFont(f'{notodir}/NotoSansTC-{w}.otf').getBestCmap())
    got = set(TTFont(f'{uidir}/UI-NotoSans-{w}.otf').getBestCmap())
    want = (want_txt | rng) & noto
    assert got == want, (f'{w}: extra={sorted(got-want)[:20]} missing={sorted(want-got)[:20]}')
    han = len([c for c in got if 0x4E00 <= c <= 0x9FFF])
    print(f'  UI-NotoSans-{w}: cmap {len(got)} (Han {han})  OK')
PY
else
  echo "=== 1-2/3 skipped (--faces-only): using the shipped fonts/ui-subset/ files ==="
fi

echo "=== 3/3 rebuilding the four faces ==="
# The exact fontconvert.py invocation for each face is recorded in that face's own .h
# header. Re-running it from there beats transcribing 45 --additional-intervals flags by
# hand, and it keeps the header honest: fontconvert.py writes its own argv back out.
# Paths in that header are repo-root-relative, so this must run from the repo root.
for face in ubuntu_10_regular ubuntu_10_bold ubuntu_14_regular ubuntu_14_bold; do
  CMD=$(grep -m1 "Command used:" "$SRC/$face.h" | sed 's/^.*Command used: //')
  echo "=== $face ==="
  "$PYTHON" $CMD > "$SRC/$face.h.new"
  if grep -q "Glyphs\[\]" "$SRC/$face.h.new" && [ "$(wc -c < "$SRC/$face.h.new")" -gt 100000 ]; then
    mv "$SRC/$face.h.new" "$SRC/$face.h"
    echo "  OK: $(wc -c < "$SRC/$face.h") bytes"
  else
    echo "  FAILED: output looks wrong, keeping the original"; rm -f "$SRC/$face.h.new"; exit 1
  fi
done
echo "done -- rebuild the firmware to pick the new faces up"
