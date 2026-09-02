#!/usr/bin/env bash
#
# mk-release.sh — build and package a CrossMosa release.
#
# WHAT IT DOES
#   1. Pins SOURCE_DATE_EPOCH (default: this commit's own timestamp) so the build is
#      byte-for-byte reproducible — see docs/reproducible-builds.md.
#   2. Clean-builds the `gh_release` environment and records the firmware sha256.
#   3. Builds a SECOND time from clean and asserts the sha256 is identical. This is the
#      whole point of the epoch: a release that cannot be reproduced is a release nobody
#      can verify, and the failure is otherwise silent. (--no-verify skips it.)
#   4. Packages  crossmosa-<ver>-firmware.zip    (firmware/bootloader/partitions + 刷機說明.txt
#                                                 + the onboarding book),
#                crossmosa-<ver>-sd-fonts.zip    (the .cpfont reading fonts, if available)
#            and crossmosa-<ver>-wallpapers.zip  (the rendered sleep wallpapers, if available),
#      and copies 歡迎使用CrossMosa.epub out on its own as a fourth, linkable asset.
#
# WHO RUNS THIS
#   The maintainer, from a clean checkout of the release tag. The firmware half runs
#   anywhere. The sd-fonts half needs the generated .cpfont files, which are ~157 MB of
#   build output and are therefore NOT in this repository — they are a release asset,
#   produced separately by lib/EpdFont/scripts/fontconvert_sdcard.py. When that directory
#   is absent the script says so and still produces the firmware zip; it does not fail.
#   Point it at the fonts with --sd-fonts DIR or SD_FONTS_DIR=...
#
#   The wallpaper half is the same shape: the rendered .bmp files are build output of
#   wallpapers/make-art-wallpapers.py, not repository content. Absent directory = skip,
#   not failure. Point it at them with --wallpapers DIR or WALLPAPERS_DIR=...
#
# FONT LICENSING — NOT OPTIONAL
#   SIL OFL 1.1 requires the licence to travel with the font. Before zipping, every family
#   folder is checked for a licence file; a missing one is generated from the OFL 1.1 body
#   already in this repo plus that family's copyright line (transcribed from the source
#   font's own name table, nameID 0). A family this script does not recognise AND that has
#   no licence file of its own ABORTS the packaging — shipping fonts without their licence
#   is not a thing to do quietly.
#
# USAGE
#   scripts/mk-release.sh [--no-verify] [--out DIR] [--sd-fonts DIR] [--wallpapers DIR]
#
# ENVIRONMENT
#   SOURCE_DATE_EPOCH  override the epoch (default: git log -1 --format=%ct)
#   PIO                path to the platformio executable (default: pio)
#   OUT_DIR            output directory (default: <repo>/release-out)
#   SD_FONTS_DIR       directory holding the .cpfont family folders
#   WALLPAPERS_DIR     directory holding the rendered sleep wallpaper .bmp files

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ENV_NAME="gh_release"
PIO="${PIO:-pio}"
OUT_DIR="${OUT_DIR:-$ROOT/release-out}"
SD_FONTS_DIR="${SD_FONTS_DIR:-}"
WALLPAPERS_DIR="${WALLPAPERS_DIR:-}"
VERIFY_REPRODUCIBLE=1

die() { printf '\nERROR: %s\n' "$*" >&2; exit 1; }
say() { printf '\n=== %s\n' "$*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --no-verify)  VERIFY_REPRODUCIBLE=0; shift ;;
    --out)        OUT_DIR="${2:?--out needs a directory}"; shift 2 ;;
    --sd-fonts)   SD_FONTS_DIR="${2:?--sd-fonts needs a directory}"; shift 2 ;;
    --wallpapers) WALLPAPERS_DIR="${2:?--wallpapers needs a directory}"; shift 2 ;;
    -h|--help)    sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//;$d'; exit 0 ;;
    *)            die "unknown argument: $1 (try --help)" ;;
  esac
done

PY="${PYTHON:-python3}"
command -v "$PIO" >/dev/null 2>&1 || die "platformio not found (set PIO=/path/to/pio)"
command -v "$PY"  >/dev/null 2>&1 || die "python3 not found (set PYTHON=/path/to/python3)"

# Deterministic zip: entries sorted, timestamps pinned to SOURCE_DATE_EPOCH, fixed modes.
# Done with python's zipfile rather than zip(1) because the latter is not installed
# everywhere and gives no ordering or timestamp guarantee without extra work.
make_zip() {  # make_zip <out.zip> <dir>
  "$PY" - "$1" "$2" "$SOURCE_DATE_EPOCH" <<'PYEOF'
import os, shutil, sys, time, zipfile
out, root, epoch = sys.argv[1], sys.argv[2], int(sys.argv[3])
dt = time.gmtime(epoch)[:6]
if dt[0] < 1980:                       # the zip format cannot express earlier dates
    dt = (1980, 1, 1, 0, 0, 0)
files = []
for base, dirs, names in os.walk(root):
    dirs.sort()
    for n in names:
        p = os.path.join(base, n)
        files.append((os.path.relpath(p, root).replace(os.sep, '/'), p))
files.sort()
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    for arc, p in files:
        zi = zipfile.ZipInfo(arc, date_time=dt)
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = 0o644 << 16
        with open(p, 'rb') as src, z.open(zi, 'w') as dst:
            shutil.copyfileobj(src, dst)
print('  %d entries' % len(files))
PYEOF
}

# ---------------------------------------------------------------------------
# Version — single source of truth is platformio.ini
# ---------------------------------------------------------------------------
VERSION_FULL="$(sed -n 's/^[[:space:]]*version[[:space:]]*=[[:space:]]*//p' platformio.ini | head -1)"
[ -n "$VERSION_FULL" ] || die "could not read 'version =' from platformio.ini"
VERSION="${VERSION_FULL%% *}"          # "1.0.0 (cp1.4.1 build 113)" -> "1.0.0"
case "$VERSION" in
  [0-9]*.[0-9]*.[0-9]*) : ;;
  *) die "unexpected version '$VERSION' parsed from '$VERSION_FULL'" ;;
esac

# ---------------------------------------------------------------------------
# SOURCE_DATE_EPOCH — the release commit's own timestamp, so that anyone who
# checks out the tag derives the same value without being told what it was.
# ---------------------------------------------------------------------------
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
  SOURCE_DATE_EPOCH="$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null)" \
    || die "not a git checkout and SOURCE_DATE_EPOCH is unset — set it explicitly"
fi
export SOURCE_DATE_EPOCH
[ -n "$SOURCE_DATE_EPOCH" ] || die "SOURCE_DATE_EPOCH is empty"

if [ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null || true)" ]; then
  printf '\nWARNING: working tree is dirty. The published sha256 will not correspond to\n'
  printf '         any commit, and nobody will be able to reproduce it.\n'
fi

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

say "CrossMosa release packaging"
printf '  version           : %s\n' "$VERSION_FULL"
printf '  commit            : %s\n' "$COMMIT"
printf '  SOURCE_DATE_EPOCH : %s  (%s UTC)\n' \
       "$SOURCE_DATE_EPOCH" "$(date -u -d "@$SOURCE_DATE_EPOCH" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo '?')"
printf '  output            : %s\n' "$OUT_DIR"

BUILD_DIR="$ROOT/.pio/build/$ENV_NAME"
FW="$BUILD_DIR/firmware.bin"

build_clean() {
  rm -rf "$BUILD_DIR"
  "$PIO" run -e "$ENV_NAME"
}
sha_of() { sha256sum "$1" | awk '{print $1}'; }

say "build 1/$(( VERIFY_REPRODUCIBLE + 1 )) (clean)"
build_clean
[ -f "$FW" ] || die "build produced no $FW"
SHA1="$(sha_of "$FW")"
printf '  firmware.bin sha256: %s\n' "$SHA1"

if [ "$VERIFY_REPRODUCIBLE" -eq 1 ]; then
  say "build 2/2 (clean) — reproducibility check"
  build_clean
  SHA2="$(sha_of "$FW")"
  printf '  firmware.bin sha256: %s\n' "$SHA2"
  [ "$SHA1" = "$SHA2" ] || die "NOT REPRODUCIBLE: $SHA1 != $SHA2
Two clean builds at the same SOURCE_DATE_EPOCH produced different binaries.
Do not publish. See docs/reproducible-builds.md — and note that a diff showing
only the app_elf_sha256 and the appended image hash is telling you the ELF
changed, not why."
  printf '  reproducible: YES (two clean builds, identical sha256)\n'
else
  printf '  reproducibility check SKIPPED (--no-verify)\n'
fi

FLASH_BYTES="$(stat -c%s "$FW")"

# ---------------------------------------------------------------------------
# Firmware zip
# ---------------------------------------------------------------------------
mkdir -p "$OUT_DIR"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

FW_STAGE="$STAGE/firmware"
mkdir -p "$FW_STAGE"
# The main image ships as update.bin: the stock-firmware SD flash path (our primary
# install route) requires that exact name, and every other route (in-app SD update,
# rescue picker, web flasher, esptool) accepts any name. Same bytes as firmware.bin.
cp "$BUILD_DIR/firmware.bin" "$FW_STAGE/update.bin"
for f in bootloader.bin partitions.bin; do
  [ -f "$BUILD_DIR/$f" ] || die "missing build artifact: $BUILD_DIR/$f"
  cp "$BUILD_DIR/$f" "$FW_STAGE/$f"
done

# The onboarding book. Unlike the fonts and wallpapers this one IS repository content
# (52 KB, built by guide/build-guide-epub.py and committed), so a missing file is a
# mistake and not a normal skip — die rather than quietly ship a release whose README
# tells people to copy a book that is not in it. It goes in TWICE on purpose: inside
# the firmware zip, so that anyone who only downloads the thing they must download
# already has it; and as a standalone asset, so it can be linked and updated on its
# own. 52 KB is not worth a decision.
GUIDE_EPUB="$ROOT/guide/歡迎使用CrossMosa.epub"
[ -f "$GUIDE_EPUB" ] || die "missing $GUIDE_EPUB — build it with: python3 guide/build-guide-epub.py"
cp "$GUIDE_EPUB" "$FW_STAGE/"
cp "$GUIDE_EPUB" "$OUT_DIR/"
# Bare update.bin as its own asset too: existing users upgrade phone-only
# (download -> web upload -> Settings -> SD firmware update) with no unzip step.
cp "$FW_STAGE/update.bin" "$OUT_DIR/update.bin"
say "guide: $(basename "$GUIDE_EPUB") ($(stat -c%s "$GUIDE_EPUB") bytes, in the firmware zip and standalone)"

cat > "$FW_STAGE/刷機說明.txt" <<EOF
CrossMosa $VERSION_FULL — 刷機說明
================================================================

⚠️ 兩件事都要做,少做一件中文書就是滿頁方塊
----------------------------------------------------------------
刷韌體只解決「介面」。書的內文字型不在韌體裡,它在 SD 卡上。
韌體內建的閱讀備援字型只有拉丁文,沒有複製 SD 字型的話,選單是正常
中文,但打開任何中文書,內文會整頁都是方塊(□□□□)。

  步驟 1  刷這個 zip 裡的 update.bin(方法 A 直接用,不用改名)
  步驟 2  把 crossmosa-$VERSION-sd-fonts.zip 解開,整個字型資料夾
          複製到 SD 卡的 /.fonts/ 底下(例如 /.fonts/NotoSerifTC/)
  步驟 3  把這個 zip 裡的《歡迎使用CrossMosa.epub》一起複製到 SD 卡,
          刷完之後第一本就讀它——十三章,一邊讀一邊按,約 10 分鐘。

USB-locked 機器須知
----------------------------------------------------------------
部分第三方通路的機器出廠鎖住 USB 燒錄(直購 xteink.com 的沒有)。
方法 A 的 SD 首刷不受鎖定影響——機器本身不接電腦(社群文件實證)。不要用 Xteink Unlocker
刷本韌體(該工具官方只支援 CrossPoint 與 CrossInk)。退路:本韌體保留
完整 SD 救援模式,任何時候可經它刷回官方 CrossPoint release。

刷韌體的五種方法
----------------------------------------------------------------
A. SD 卡首刷(推薦;機器不用接電腦,檔案用讀卡機放進 SD 卡即可)
   update.bin → SD 卡根目錄 → 關機 →
   按住左側「上一頁」鍵+電源鍵到出現載入畫面 → 約五分鐘刷完自動開機。
   檔名已預先改好——這顆就是其他教學裡說要改名的 firmware.bin。
   X3 限定;失敗就長按電源強制重開、重新下載再試。

B. 網頁 flasher(USB 偵測得到時)
   USB-C 接電腦並喚醒裝置 → https://crosspointreader.com/#flash-tools
   → 選 X3 → Custom .bin → 上傳本 zip 的 update.bin

C. SD 卡更新(已裝 CrossMosa 之後的升級)
   update.bin 複製到 SD 卡根目錄 → 裝置上「設定 → 系統 →
   SD 卡韌體更新」→ 選該檔案。韌體會先完整驗證映像檔才寫入。

D. 救援開機(進不了設定時)
   關機 → 按住左側的「上一頁」鍵不放 → 按右上的電源鍵 → 直接進 SD 韌體選擇畫面。

E. 命令列
   pip install esptool
   esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \\
              write_flash 0x10000 update.bin

bootloader.bin 與 partitions.bin 只有在做完整重刷(從 0x0 起)時才需要;
一般更新只要 update.bin。

第一次開機
----------------------------------------------------------------
1. 開機就是繁體中文(要英文介面:設定 → 系統 → 語言 → English)
2. 選內文字型:設定 → 閱讀器 → 閱讀字型(選你複製進去的那套)
3. 版號應顯示 $VERSION_FULL(開機畫面下緣與設定頁右上角)

本次建置(可重現)
----------------------------------------------------------------
  commit             $COMMIT
  SOURCE_DATE_EPOCH  $SOURCE_DATE_EPOCH
  update.bin         $FLASH_BYTES bytes
  sha256             $SHA1

設定同一個 SOURCE_DATE_EPOCH 重建這個 commit,會得到 sha256 完全
相同的映像檔。做法見 repo 的 docs/reproducible-builds.md。

刷機有風險,自負。與 Xteink 及上游 CrossPoint 專案均無隸屬關係。
授權見 repo 的 LICENSE 與 NOTICE.md(含 GPLv2 / LGPL-2.1 元件的發佈義務)。
EOF

(cd "$FW_STAGE" && sha256sum update.bin bootloader.bin partitions.bin > SHA256SUMS)

FW_ZIP="$OUT_DIR/crossmosa-$VERSION-firmware.zip"
rm -f "$FW_ZIP"
make_zip "$FW_ZIP" "$FW_STAGE"
say "wrote $FW_ZIP ($(stat -c%s "$FW_ZIP") bytes)"

# ---------------------------------------------------------------------------
# SD reading fonts — licence-checked, then zipped
# ---------------------------------------------------------------------------

# The SIL OFL 1.1 body, taken verbatim from a copy already in this repo. Both in-repo
# copies were verified byte-identical from this anchor line onward, so either serves as
# the canonical text; only the copyright line above it differs per font.
OFL_SOURCE="$ROOT/fonts/ui-subset/OFL.txt"

ofl_body() {
  [ -f "$OFL_SOURCE" ] || die "cannot find the OFL 1.1 text at $OFL_SOURCE"
  sed -n '/^This Font Software is licensed under the SIL Open Font License/,$p' "$OFL_SOURCE"
}

# Copyright lines transcribed from each source font's own name table (nameID 0).
# None of these declares a Reserved Font Name, which is why the .cpfont derivatives
# may keep their family names. Verified with fontTools; re-check if a family is added.
# GuanKiapTsingKhai-90 checked 2026-08-31: nameID 0 has no "Reserved Font Name", nameID 13/14 unset.
family_copyright() {
  case "$1" in
    NotoSerifTC)    echo "Copyright © 2017-2023 Adobe (http://www.adobe.com/).|Noto Serif CJK TC" ;;
    NotoSansTC)     echo "Copyright © 2014-2021 Adobe (http://www.adobe.com/).|Noto Sans TC" ;;
    Iansui)         echo "Copyright 2025 The Iansui Project Authors (https://github.com/ButTaiwan/iansui)|Iansui (芫荽), derived from Klee One" ;;
    IBMPlexSansTC)  echo "Copyright 2018 IBM Corp. All rights reserved.|IBM Plex Sans TC" ;;
    GuanKiapTsingKhai-90)
                    echo "Copyright 2022-2025 Tony Huang (https://github.com/tonyhuan/GuanKiapTsingKhai)|原俠正楷 GuanKiapTsingKhai（偽直排版，字身旋轉 90 度），由芫荽 Iansui 與 LXGW 文楷合併而來" ;;
    *)              return 1 ;;
  esac
}

find_sd_fonts() {
  [ -n "$SD_FONTS_DIR" ] && { [ -d "$SD_FONTS_DIR" ] && echo "$SD_FONTS_DIR"; return; }
  for cand in "$ROOT/sd-fonts"; do
    [ -d "$cand" ] && { echo "$cand"; return; }
  done
  return 0
}

SD_SRC="$(find_sd_fonts || true)"

if [ -z "$SD_SRC" ]; then
  say "SD fonts: SKIPPED"
  cat <<'EOF'
  No .cpfont directory found. The reading fonts are ~157 MB of generated bitmap data
  and are deliberately not in this repository — they are a release asset built by the
  maintainer with lib/EpdFont/scripts/fontconvert_sdcard.py.

  The firmware zip above is complete and usable. To also build the font zip, re-run with
      scripts/mk-release.sh --sd-fonts /path/to/sd-fonts
  where that directory holds one folder per family (NotoSerifTC/, NotoSansTC/, ...),
  each containing its *.cpfont files.
EOF
else
  say "SD fonts: $SD_SRC"
  SD_STAGE="$STAGE/sd-fonts"
  mkdir -p "$SD_STAGE"

  found_any=0
  for dir in "$SD_SRC"/*/; do
    [ -d "$dir" ] || continue
    fam="$(basename "$dir")"
    ls "$dir"/*.cpfont >/dev/null 2>&1 || { printf '  skip %s (no .cpfont files)\n' "$fam"; continue; }
    found_any=1
    mkdir -p "$SD_STAGE/$fam"
    cp "$dir"/*.cpfont "$SD_STAGE/$fam/"

    # Carry over any licence file the family already ships with.
    existing=""
    for lic in "$dir"OFL.txt "$dir"OFL.TXT "$dir"LICENSE "$dir"LICENSE.txt "$dir"UFL.txt; do
      [ -f "$lic" ] && { cp "$lic" "$SD_STAGE/$fam/"; existing="$(basename "$lic")"; break; }
    done

    if [ -n "$existing" ]; then
      printf '  %-16s %d fonts, licence: %s (carried over)\n' \
             "$fam" "$(ls "$SD_STAGE/$fam"/*.cpfont | wc -l)" "$existing"
      continue
    fi

    if ! meta="$(family_copyright "$fam")"; then
      die "font family '$fam' has no licence file and is not in this script's copyright
table. Add its copyright line (from the source font's name table, nameID 0) to
family_copyright(), or put an OFL.txt in $dir. Refusing to ship fonts without
their licence."
    fi
    copyright="${meta%%|*}"
    srcname="${meta##*|}"

    {
      echo "$copyright"
      echo "  -- applies to the ${fam}_*.cpfont bitmap fonts in this folder"
      echo "  -- source family: $srcname"
      echo "  -- The .cpfont files are bitmap derivatives generated for the CrossMosa"
      echo "     firmware. The copyright line above is transcribed from the source"
      echo "     font's name table (nameID 0); it declares no Reserved Font Name,"
      echo "     so the derivative keeps the family name."
      echo
      ofl_body
    } > "$SD_STAGE/$fam/OFL.txt"

    printf '  %-16s %d fonts, licence: OFL.txt (generated)\n' \
           "$fam" "$(ls "$SD_STAGE/$fam"/*.cpfont | wc -l)"
  done

  [ "$found_any" -eq 1 ] || die "no font families with .cpfont files found under $SD_SRC"

  # Every family folder must now contain a licence — check, do not assume.
  for fam_dir in "$SD_STAGE"/*/; do
    fam="$(basename "$fam_dir")"
    have_lic=0
    for lic in "$fam_dir"OFL.txt "$fam_dir"OFL.TXT "$fam_dir"LICENSE "$fam_dir"LICENSE.txt "$fam_dir"UFL.txt; do
      [ -f "$lic" ] && have_lic=1
    done
    [ "$have_lic" -eq 1 ] || die "no licence file ended up in $fam — packaging aborted"
  done

  cat > "$SD_STAGE/README.txt" <<EOF
CrossMosa $VERSION — SD 卡內文字型
================================================================
把你要用的**整個資料夾**複製到 SD 卡的 /.fonts/ 底下:

  SD 卡根目錄/
  └── .fonts/
      ├── NotoSerifTC/           ← 明體,建議先裝這套
      ├── NotoSansTC/            ← 黑體,有粗體
      ├── Iansui/                ← 硬筆楷書(芫荽),只有 Regular
      ├── GuanKiapTsingKhai-90/  ← 楷書·偽直排,見下
      └── IBMPlexSansTC/         ← 黑體,另一種風格

五套都是完整漢字涵蓋(約 21,000 字),台語文、古文、人名裡的冷僻字
都有字,不會顯示成黑框。

想直排讀中文:選 GuanKiapTsingKhai-90,再把螢幕轉成橫向,中文就會
由上而下、由右而左排列。它的字形是預先轉了 90 度的,所以正常橫排時
選它會整頁躺著——只在要直排時用。

只裝一套也可以(整包解開約 252 MB,多數人只會用一兩套)。
裝好後在裝置上:設定 → 閱讀器 → 閱讀字型。
資料夾名稱不可含空格(上游已知會 crash)。字型是按需從 SD 讀的,
不佔 RAM;裝多套只花 SD 空間。

每個資料夾內的 OFL.txt 是該字型的授權(SIL Open Font License 1.1),
散布時請一併保留。
EOF

  SD_ZIP="$OUT_DIR/crossmosa-$VERSION-sd-fonts.zip"
  rm -f "$SD_ZIP"
  make_zip "$SD_ZIP" "$SD_STAGE"
  say "wrote $SD_ZIP ($(stat -c%s "$SD_ZIP") bytes)"
fi

# ---------------------------------------------------------------------------
# Sleep wallpapers — the rendered 50 masterpieces (and anything else in the folder)
#
# Same shape as the fonts above: these .bmp files are output of
# wallpapers/make-art-wallpapers.py, not repository content, so an absent directory is a
# skip and not a failure. What is NOT optional is the format check — the firmware silently
# SKIPS a BMP it cannot decode (RLE compression is the classic way to get that), so a
# wallpaper zip that ships one is a zip that quietly does nothing on the device. The rules
# below are the ones lib/GfxRenderer/Bitmap.cpp actually applies.

find_wallpapers() {
  [ -n "$WALLPAPERS_DIR" ] && { [ -d "$WALLPAPERS_DIR" ] && echo "$WALLPAPERS_DIR"; return; }
  for cand in "$ROOT/sleep"; do
    [ -d "$cand" ] && { echo "$cand"; return; }
  done
  return 0
}

WP_SRC="$(find_wallpapers || true)"

if [ -z "$WP_SRC" ]; then
  say "wallpapers: SKIPPED"
  cat <<'EOF'
  No wallpaper directory found. The rendered .bmp files are build output, not repository
  content — regenerate them with wallpapers/fetch_sources.py + make-art-wallpapers.py,
  or point this script at an existing set:
      scripts/mk-release.sh --wallpapers /path/to/sleep
  The firmware zip above is unaffected.
EOF
elif ! ls "$WP_SRC"/*.bmp >/dev/null 2>&1; then
  say "wallpapers: SKIPPED"
  printf '  %s exists but holds no *.bmp files (only rendered wallpapers are packaged;\n' "$WP_SRC"
  printf '  the _src/ originals are deliberately left out).\n'
else
  say "wallpapers: $WP_SRC"
  WP_STAGE="$STAGE/wallpapers"
  mkdir -p "$WP_STAGE"
  cp "$WP_SRC"/*.bmp "$WP_STAGE/"

  # Verify every file against the firmware's own acceptance rules. Aborts, loudly:
  # a wallpaper the device skips looks exactly like a wallpaper the user installed wrong.
  "$PY" - "$WP_STAGE" <<'EOF' || die "wallpaper format check failed — see above"
import struct, sys, pathlib
bad = []
n = 0
for f in sorted(pathlib.Path(sys.argv[1]).glob("*.bmp")):
    b = f.read_bytes()
    if len(b) < 54 or b[:2] != b"BM":
        bad.append((f.name, "not a BMP")); continue
    w, h, planes, bpp, comp = struct.unpack_from("<iiHHI", b, 18)
    h = abs(h)
    if comp != 0:
        bad.append((f.name, f"compression={comp}, must be 0 (BI_RGB) — the device skips this"))
    if bpp not in (1, 2, 4, 8, 24, 32):
        bad.append((f.name, f"{bpp} bpp not supported"))
    if w > 2048 or h > 3072:
        bad.append((f.name, f"{w}x{h} exceeds 2048x3072"))
    n += 1
for name, why in bad:
    print(f"  REJECT {name}: {why}")
print(f"  checked {n} wallpapers, {len(bad)} rejected")
sys.exit(1 if bad else 0)
EOF

  cat > "$WP_STAGE/README.txt" <<EOF
CrossMosa $VERSION — 待機壁紙
================================================================
把 .bmp 檔複製到 SD 卡的 /.sleep/ 底下:

  SD 卡根目錄/
  └── .sleep/
      ├── mona_lisa.bmp
      ├── great_wave.bmp
      └── ...

放兩張以上才會輪播。裝好後在裝置上:設定 → 顯示 → 待機畫面 → 自訂。

⚠️ SD 根目錄不要放單獨一個 /sleep.bmp —— 它會優先、固定顯示、不輪播。

橫式構圖的那幾張是整張轉 90 度存的(待機畫面固定跑 528x792 直向,韌體不會
自動轉正)。看到橫的畫時,把裝置往順時針方向轉 90 度。

授權:畫作本身是公共領域,透過 Wikimedia Commons 取得;二維平面作品的忠實
攝影翻拍不產生新的著作權,沒有額外的授權義務。逐張出處與策展理由見原始碼
的 wallpapers/artworks.py 與 wallpapers/README.md。

要換成自己的圖:wallpapers/make-wallpaper.py 可以把任何圖片轉成這個格式。
EOF

  WP_ZIP="$OUT_DIR/crossmosa-$VERSION-wallpapers.zip"
  rm -f "$WP_ZIP"
  make_zip "$WP_ZIP" "$WP_STAGE"
  say "wrote $WP_ZIP ($(stat -c%s "$WP_ZIP") bytes)"
fi

# ---------------------------------------------------------------------------
say "done — publish these values with the release"
printf '  version            %s\n' "$VERSION_FULL"
printf '  commit             %s\n' "$COMMIT"
printf '  SOURCE_DATE_EPOCH  %s\n' "$SOURCE_DATE_EPOCH"
printf '  update.bin         %s bytes\n' "$FLASH_BYTES"
printf '  sha256             %s\n' "$SHA1"
printf '\n'
ls -la "$OUT_DIR"
