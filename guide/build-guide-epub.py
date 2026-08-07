#!/usr/bin/env python3
"""build-guide-epub.py — build 《歡迎使用 CrossMosa》, the on-device onboarding book.

WHAT THIS IS
  A 12-chapter EPUB that teaches the device by making the reader operate it. Each
  chapter ends with one instruction whose natural result moves the reader forward.
  It ships as a release asset and inside the firmware zip.

USAGE
  python3 guide/build-guide-epub.py                 # build + verify, writes guide/歡迎使用CrossMosa.epub
  python3 guide/build-guide-epub.py --out DIR       # write the .epub somewhere else
  python3 guide/build-guide-epub.py --no-verify     # skip the conformance pass (don't)

REQUIREMENTS
  Python 3.8+ and Pillow (already in scripts/requirements.txt). Nothing else — the
  EPUB is written straight with zipfile, the way scripts/generate_test_epub.py does,
  rather than through ebooklib. Two reasons: ebooklib is not a declared dependency of
  this repo, and writing the zip directly is what lets the build be byte-for-byte
  reproducible (see below).

REPRODUCIBLE
  Same input tree, same output bytes. Entries are written in a fixed order with
  timestamps pinned to GUIDE_EPOCH (overridable with SOURCE_DATE_EPOCH). Unlike
  scripts/mk-release.sh this does NOT track HEAD — see source_date_epoch(). Verify
  with two builds and sha256sum.

THE VERIFY PASS IS THE POINT
  This book makes operational claims about a device nobody can unit-test here, and its
  footnote chapter only works if the markup matches what lib/Epub actually parses. So
  the build asserts, against the firmware source in this same tree:

    * every footnote <a href> resolves the way Epub::resolveHrefToSpineIndex resolves
      (filename-only match), and its target id sits on an element the parser will
      record as an anchor (ChapterHtmlSlimParser skips ids on <span>);
    * every character of the book title and of every TOC label is present in the
      built-in UI font — TOC entries and titles render with the *UI* font, not the SD
      reading font, so a character outside the 7,413-Han set is a box on screen;
    * the cover and figure PNGs are inside what lib/PngToBmpConverter accepts;
    * FootnoteEntry's fixed buffers (href 96 bytes, label 32) are not overrun;
    * the container/OPF/spine/manifest agree and every XHTML file is well-formed.

  A failure here is a book that looks fine in Calibre and misbehaves on the X3.
"""

import argparse
import io
import os
import re
import struct
import sys
import unicodedata
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

GUIDE_DIR = Path(__file__).resolve().parent
ROOT = GUIDE_DIR.parent

CHAPTER_DIR = GUIDE_DIR / "chapters"
STYLE_FILE = GUIDE_DIR / "style.css"

LOGO_PNG = ROOT / "docs/promo/logo.png"
COVER_FONT = ROOT / "fonts/ui-subset/UI-NotoSans-Bold.otf"
UI_FONT_HEADER = ROOT / "lib/EpdFont/builtinFonts/ubuntu_14_regular.h"

BOOK_TITLE = "歡迎使用 CrossMosa"
BOOK_AUTHOR = "CrossMosa contributors"
BOOK_ID = "urn:uuid:crossmosa-guide-1-0-0"
BOOK_LANG = "zh-TW"

# Pinned publication timestamp for the archive entries — the CrossMosa 1.0.0 release
# commit's own %ct. See source_date_epoch() for why this is a constant and not HEAD.
GUIDE_EPOCH = 1786082852

# Filename rationale is in guide/README.md. Short version: the firmware writes CJK
# filenames itself (StringUtils::sanitizeFilename keeps every codepoint >= 128), the
# no-spaces rule is about /.fonts/ FOLDER names, and a leading dot would make the file
# hidden. So a space-free CJK name is the safe and friendly choice.
OUTPUT_NAME = "歡迎使用CrossMosa.epub"

# X3 panel, portrait. The cover doubles as the home-screen thumbnail source.
COVER_W, COVER_H = 528, 792

# Chapter order and TOC labels. Explicit rather than derived from filenames so that
# renaming a source file cannot silently reorder the book.
CHAPTERS = [
    ("01-keys.xhtml", "1　你已經會翻頁了"),
    ("02-fonts.xhtml", "2　換一套字"),
    ("03-bookmark.xhtml", "3　書籤"),
    ("04-contents.xhtml", "4　選擇章節"),
    ("05-footnote.xhtml", "5　註腳"),
    ("06-power-key.xhtml", "6　右上角那顆鍵"),
    ("07-images.xhtml", "7　圖片與灰階"),
    ("08-getting-books.xhtml", "8　書怎麼進來"),
    ("09-wallpaper.xhtml", "9　名畫待機"),
    ("10-prefetch.xhtml", "10　為什麼翻頁變快了"),
    ("11-missing-glyphs.xhtml", "11　遇到方塊字"),
    ("12-support.xhtml", "12　如果它讓你的 X3 變好用了"),
    ("13-closing.xhtml", "13　把機器還給書"),
    ("90-notes.xhtml", "註釋"),
]

FIGURE_NAME = "greyscale.png"


# ---------------------------------------------------------------------------
# Generated images
# ---------------------------------------------------------------------------


def _load_pil():
    try:
        from PIL import Image, ImageDraw, ImageFont  # noqa: F401
    except ImportError:
        sys.exit("Pillow is required (pip install -r scripts/requirements.txt)")
    from PIL import Image, ImageDraw, ImageFont

    return Image, ImageDraw, ImageFont


def make_cover_png() -> bytes:
    """528x792 cover: the bear logo over the title, set in the repo's own UI subset.

    The face is fonts/ui-subset/UI-NotoSans-Bold.otf — the same subset the firmware's
    UI faces are cut from, and in this tree, so the cover renders identically on any
    machine instead of depending on whatever CJK font happens to be installed.
    """
    Image, ImageDraw, ImageFont = _load_pil()

    cover = Image.new("L", (COVER_W, COVER_H), color=255)
    draw = ImageDraw.Draw(cover)

    logo_side = 330
    with Image.open(LOGO_PNG) as raw:
        if raw.mode in ("RGBA", "LA", "P"):
            bg = Image.new("RGB", raw.size, (255, 255, 255))
            bg.paste(raw, mask=raw.convert("RGBA").split()[3])
            logo = bg
        else:
            logo = raw.convert("RGB")
        logo = logo.convert("L").resize((logo_side, logo_side), Image.LANCZOS)
    logo_y = 176
    cover.paste(logo, ((COVER_W - logo_side) // 2, logo_y))

    if not COVER_FONT.is_file():
        sys.exit(f"cover font missing: {COVER_FONT}")
    f_title = ImageFont.truetype(str(COVER_FONT), 62)
    f_sub = ImageFont.truetype(str(COVER_FONT), 24)

    # The logo already carries the "crossmosa" wordmark, so the title block only needs
    # the two words the logo does not say. Anchor "mm" keeps the maths honest — the
    # earlier bbox arithmetic put the subtitle on top of the title.
    title_y = logo_y + logo_side + 96
    draw.text((COVER_W // 2, title_y), "歡迎使用", font=f_title, fill=0, anchor="mm")

    rule_y = title_y + 74
    draw.line([(150, rule_y), (COVER_W - 150, rule_y)], fill=150, width=2)
    draw.text((COVER_W // 2, rule_y + 40), "十三章，一邊讀一邊按", font=f_sub, fill=90, anchor="mm")

    buf = io.BytesIO()
    # Non-interlaced 8-bit greyscale: what lib/PngToBmpConverter decodes most directly.
    cover.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def make_greyscale_figure() -> bytes:
    """The figure for chapter 6: the panel's four grey levels, plus an aliased edge.

    Deliberately a chart and not a screenshot. The chapter's claim is "this screen has
    exactly four greys"; a chart shows that, a screenshot of the UI does not. The
    diagonal is there so the reader has something whose edge they can actually look at
    while the chapter talks about anti-aliasing.
    """
    Image, ImageDraw, _ = _load_pil()

    W, H = 440, 300
    img = Image.new("L", (W, H), color=255)
    d = ImageDraw.Draw(img)

    # The four levels the panel can hold, black to white.
    levels = [0, 85, 170, 255]
    band_h = 110
    band_w = W // 4
    for i, v in enumerate(levels):
        d.rectangle([i * band_w, 0, (i + 1) * band_w - 1, band_h], fill=v)
    d.rectangle([0, 0, W - 1, band_h], outline=0, width=2)
    for i in range(1, 4):
        d.line([(i * band_w, 0), (i * band_w, band_h)], fill=0, width=2)

    # A hard diagonal and a circle: staircases you can see, which is the whole point.
    top = band_h + 40
    d.polygon([(20, H - 20), (200, H - 20), (200, top)], fill=0)
    d.ellipse([250, top, 250 + (H - 20 - top), H - 20], outline=0, width=6)

    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# EPUB assembly
# ---------------------------------------------------------------------------

XHTML_WRAPPER = """<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="{lang}" lang="{lang}">
<head>
<meta http-equiv="Content-Type" content="text/html; charset=utf-8"/>
<title>{title}</title>
<link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
{body}
</body>
</html>
"""


def read_chapter(filename: str) -> str:
    path = CHAPTER_DIR / filename
    if not path.is_file():
        sys.exit(f"missing chapter source: {path}")
    return path.read_text(encoding="utf-8").strip()


def build_documents() -> dict:
    """Return {archive path: bytes} for everything under OEBPS/, plus the shell."""
    docs = {}

    cover_png = make_cover_png()
    figure_png = make_greyscale_figure()

    docs["OEBPS/images/cover.png"] = cover_png
    docs[f"OEBPS/images/{FIGURE_NAME}"] = figure_png
    docs["OEBPS/style.css"] = STYLE_FILE.read_bytes()

    docs["OEBPS/cover.xhtml"] = XHTML_WRAPPER.format(
        lang=BOOK_LANG,
        title=BOOK_TITLE,
        body='<div class="cover"><img src="images/cover.png" alt="'
        + BOOK_TITLE
        + '"/></div>',
    ).encode("utf-8")

    for filename, label in CHAPTERS:
        docs[f"OEBPS/{filename}"] = XHTML_WRAPPER.format(
            lang=BOOK_LANG, title=label, body=read_chapter(filename)
        ).encode("utf-8")

    manifest = [
        '    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
        '    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
        '    <item id="style" href="style.css" media-type="text/css"/>',
        # id="cover-image" is what the EPUB 2 <meta name="cover"> below points at;
        # ContentOpfParser only accepts it because the media-type is image/*.
        '    <item id="cover-image" href="images/cover.png" media-type="image/png" properties="cover-image"/>',
        f'    <item id="fig-greyscale" href="images/{FIGURE_NAME}" media-type="image/png"/>',
        '    <item id="coverpage" href="cover.xhtml" media-type="application/xhtml+xml"/>',
    ]
    spine = ['    <itemref idref="coverpage"/>']
    for filename, _ in CHAPTERS:
        item_id = "ch-" + filename.rsplit(".", 1)[0]
        manifest.append(f'    <item id="{item_id}" href="{filename}" media-type="application/xhtml+xml"/>')
        spine.append(f'    <itemref idref="{item_id}"/>')

    nl = "\n"
    docs["OEBPS/content.opf"] = f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">{BOOK_ID}</dc:identifier>
    <dc:title>{BOOK_TITLE}</dc:title>
    <dc:creator>{BOOK_AUTHOR}</dc:creator>
    <dc:language>{BOOK_LANG}</dc:language>
    <meta name="cover" content="cover-image"/>
  </metadata>
  <manifest>
{nl.join(manifest)}
  </manifest>
  <spine toc="ncx">
{nl.join(spine)}
  </spine>
</package>
""".encode(
        "utf-8"
    )

    nav_items = nl.join(
        f'      <li><a href="{fn}">{label}</a></li>' for fn, label in CHAPTERS
    )
    docs["OEBPS/nav.xhtml"] = f"""<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="{BOOK_LANG}" lang="{BOOK_LANG}">
<head><meta http-equiv="Content-Type" content="text/html; charset=utf-8"/><title>目錄</title></head>
<body>
  <nav epub:type="toc" id="toc">
    <h1>目錄</h1>
    <ol>
{nav_items}
    </ol>
  </nav>
</body>
</html>
""".encode(
        "utf-8"
    )

    nav_points = nl.join(
        f"""    <navPoint id="np{i + 1}" playOrder="{i + 1}">
      <navLabel><text>{label}</text></navLabel>
      <content src="{fn}"/>
    </navPoint>"""
        for i, (fn, label) in enumerate(CHAPTERS)
    )
    # NCX is the fallback path (Epub.cpp prefers the EPUB 3 nav and falls back to this).
    # Shipping both costs a kilobyte and removes a single point of failure.
    docs["OEBPS/toc.ncx"] = f"""<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="{BOOK_ID}"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>{BOOK_TITLE}</text></docTitle>
  <navMap>
{nav_points}
  </navMap>
</ncx>
""".encode(
        "utf-8"
    )

    docs["META-INF/container.xml"] = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
""".encode(
        "utf-8"
    )

    return docs


def source_date_epoch() -> int:
    """The book's own publication timestamp, pinned.

    Deliberately NOT `git log -1 --format=%ct` the way scripts/mk-release.sh does it.
    That rule is right for the firmware, whose bytes are supposed to be a function of
    the commit. The book's bytes should be a function of its *content*: pinning to HEAD
    would make an unrelated later commit change the .epub's sha256 and turn the checked-
    in artifact stale for no reason. Bump this only if the book is republished.
    """
    env = os.environ.get("SOURCE_DATE_EPOCH")
    return int(env) if env else GUIDE_EPOCH


def write_epub(path: Path, docs: dict, epoch: int) -> None:
    dt = list(__import__("time").gmtime(epoch)[:6])
    if dt[0] < 1980:
        dt = [1980, 1, 1, 0, 0, 0]
    dt = tuple(dt)

    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        # mimetype first, stored, no extra fields — OCF requires exactly this.
        zi = zipfile.ZipInfo("mimetype", date_time=dt)
        zi.compress_type = zipfile.ZIP_STORED
        zi.external_attr = 0o644 << 16
        z.writestr(zi, b"application/epub+zip")

        for name in ["META-INF/container.xml"] + sorted(
            k for k in docs if k != "META-INF/container.xml"
        ):
            zi = zipfile.ZipInfo(name, date_time=dt)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            with z.open(zi, "w") as dst:
                dst.write(docs[name])


# ---------------------------------------------------------------------------
# Verification — every rule below is transcribed from firmware source in this tree
# ---------------------------------------------------------------------------


def load_ui_font_coverage(header: Path):
    """Return a predicate over codepoints, read from the built-in UI font header.

    Mirrors the check the fork's own coverage scan does: a codepoint counts as covered
    when it falls in an EpdUnicodeInterval AND its glyph carries either bitmap data or
    an advance (the latter covers spaces). Titles, filenames and TOC labels are drawn
    with this font, so anything not covered is a box on screen.
    """
    txt = header.read_text(encoding="utf-8", errors="replace")
    stem = re.search(r"static const EpdUnicodeInterval (\w+?)Intervals\[\]", txt).group(1)
    iv_body = re.search(re.escape(stem) + r"Intervals\[\] = \{(.*?)\};", txt, re.S).group(1)
    intervals = [
        tuple(int(x, 0) for x in m)
        for m in re.findall(
            r"\{\s*([0-9xXa-fA-F]+)\s*,\s*([0-9xXa-fA-F]+)\s*,\s*([0-9xXa-fA-F]+)\s*\}", iv_body
        )
    ]
    gl_body = re.search(re.escape(stem) + r"Glyphs\[\] = \{(.*?)\};", txt, re.S).group(1)
    glyphs = [
        tuple(int(x, 0) for x in m)
        for m in re.findall(
            r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}",
            gl_body,
        )
    ]

    def covered(cp: int) -> bool:
        for first, last, off in intervals:
            if first <= cp <= last:
                idx = off + (cp - first)
                if idx >= len(glyphs):
                    return False
                g = glyphs[idx]
                return g[5] > 0 or g[2] > 0
        return False

    return covered


def check_png(name: str, data: bytes, problems: list) -> None:
    """lib/PngToBmpConverter's acceptance rules, applied before the device sees them."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        problems.append(f"{name}: not a PNG")
        return
    w, h, depth, colour, comp, filt, interlace = struct.unpack(">IIBBBBB", data[16:29])
    if comp != 0 or filt != 0:
        problems.append(f"{name}: unsupported compression/filter method")
    if interlace != 0:
        problems.append(f"{name}: interlaced — the decoder rejects these")
    if w == 0 or h == 0 or w > 2048 or h > 3072:
        problems.append(f"{name}: {w}x{h} outside the decoder's limits")
    if colour not in (0, 2, 3, 4, 6):
        # Unknown colour type: the row-size arithmetic below has nothing to work with,
        # and the decoder rejects the file anyway. Stop here rather than KeyError.
        problems.append(f"{name}: colour type {colour} unsupported")
        return
    bytes_per_px = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour] * (2 if depth == 16 else 1)
    if w * bytes_per_px > 16384:
        problems.append(f"{name}: row of {w * bytes_per_px} bytes exceeds the 16384 cap")


I18N_YAML = ROOT / "lib/I18n/translations/zh-hant.yaml"


def check_settings_paths(problems: list) -> int:
    """Assert every 「設定 → …」 in the book is the exact on-screen wording.

    Reads the labels straight out of lib/I18n/translations/zh-hant.yaml. A path segment
    that is not a translated string means the book is telling the reader to look for a
    menu item that does not exist under that name — the single most likely way for this
    book to rot, since UI wording moves and books do not.

    Chapters mark every path with <strong>, which is both the typographic convention and
    what makes the paths extractable without guessing where prose ends.
    """
    if not I18N_YAML.is_file():
        problems.append(f"cannot check settings paths: {I18N_YAML} missing")
        return 0
    values = {
        m.group(1)
        for m in (
            re.match(r'^STR_\w+:\s*"(.*)"\s*$', line)
            for line in I18N_YAML.read_text(encoding="utf-8").splitlines()
        )
        if m
    }
    values.add("設定")  # the app's own title for the settings activity root

    count = 0
    for src in sorted(CHAPTER_DIR.glob("*.xhtml")):
        for inner in re.findall(r"<strong>(.*?)</strong>", src.read_text(encoding="utf-8"), re.S):
            if "→" not in inner:
                continue
            count += 1
            for seg in (s.strip() for s in re.sub(r"<[^>]+>", "", inner).split("→")):
                if seg not in values:
                    problems.append(
                        f"{src.name}: 「{seg}」 in 「{inner}」 is not a string in zh-hant.yaml"
                    )
    return count


def verify(epub_path: Path) -> int:
    problems = []
    notes = []

    with zipfile.ZipFile(epub_path) as z:
        names = z.namelist()

        if names[0] != "mimetype":
            problems.append("mimetype is not the first archive entry")
        else:
            info = z.getinfo("mimetype")
            if info.compress_type != zipfile.ZIP_STORED:
                problems.append("mimetype entry is compressed (must be stored)")
            if z.read("mimetype") != b"application/epub+zip":
                problems.append("mimetype content is wrong")
        if "META-INF/container.xml" not in names:
            problems.append("META-INF/container.xml missing")

        opf_bytes = z.read("OEBPS/content.opf")
        opf = ET.fromstring(opf_bytes)
        ns = {
            "opf": "http://www.idpf.org/2007/opf",
            "dc": "http://purl.org/dc/elements/1.1/",
            "x": "http://www.w3.org/1999/xhtml",
            "epub": "http://www.idpf.org/2007/ops",
        }

        # -- manifest / spine agreement -------------------------------------
        manifest = {}
        for item in opf.findall(".//opf:manifest/opf:item", ns):
            manifest[item.get("id")] = item
            href = "OEBPS/" + item.get("href")
            if href not in names:
                problems.append(f"manifest item {item.get('id')} -> {href} is not in the archive")
        spine_files = []
        for ref in opf.findall(".//opf:spine/opf:itemref", ns):
            idref = ref.get("idref")
            if idref not in manifest:
                problems.append(f"spine references unknown id {idref}")
                continue
            spine_files.append(manifest[idref].get("href"))
        if len(spine_files) != len(CHAPTERS) + 1:
            problems.append(f"spine has {len(spine_files)} items, expected {len(CHAPTERS) + 1}")

        # -- cover: both the EPUB 2 and EPUB 3 declarations ------------------
        meta_cover = opf.find(".//opf:metadata/opf:meta[@name='cover']", ns)
        if meta_cover is None:
            problems.append("no EPUB 2 <meta name=\"cover\"> — the firmware looks here first")
        else:
            target = manifest.get(meta_cover.get("content"))
            if target is None or not target.get("media-type", "").startswith("image/"):
                problems.append("meta cover does not point at an image manifest item")
        if not any("cover-image" in (i.get("properties") or "") for i in manifest.values()):
            problems.append("no manifest item carries properties=\"cover-image\"")
        if not any((i.get("properties") or "") == "nav" for i in manifest.values()):
            problems.append("no manifest item carries properties=\"nav\"")

        # -- every XHTML parses, and collect ids + internal links -------------
        # nav.xhtml is read by TocNavParser, not by the chapter parser, so its <a>
        # elements are TOC entries and must not be judged as footnote links.
        ids_by_file = {}
        links = []
        spine_shortnames = {f.split("/")[-1] for f in spine_files}
        for name in sorted(n for n in names if n.endswith(".xhtml")):
            raw = z.read(name)
            try:
                tree = ET.fromstring(raw)
            except ET.ParseError as e:
                problems.append(f"{name}: not well-formed XML — {e}")
                continue
            short = name.split("/")[-1]
            found = set()
            for el in tree.iter():
                tag = el.tag.split("}")[-1]
                el_id = el.get("id")
                if el_id:
                    # ChapterHtmlSlimParser: ids on <span> are deliberately not recorded.
                    if tag == "span":
                        notes.append(f"{short}: id=\"{el_id}\" is on a <span> and will NOT be an anchor")
                    else:
                        found.add(el_id)
                if tag == "a" and el.get("href") and short in spine_shortnames:
                    links.append((short, el.get("href"), "".join(el.itertext())))
                if tag == "img":
                    src = el.get("src") or ""
                    if "OEBPS/" + src not in names:
                        problems.append(f"{short}: <img src=\"{src}\"> is not in the archive")
            ids_by_file[short] = found

        # -- footnote conformance --------------------------------------------
        # Any internal <a href> becomes a FootnoteEntry (ChapterHtmlSlimParser), so every
        # one of them must actually resolve, or the reader jumps nowhere and says nothing.
        external = ("http://", "https://", "mailto:", "ftp://", "tel:", "javascript:")
        footnotes = 0
        for src_file, href, text in links:
            if href.startswith(external):
                continue
            footnotes += 1
            if len(href.encode("utf-8")) > 95:
                problems.append(f"{src_file}: href {href!r} exceeds FOOTNOTE_HREF_LEN-1 (95 bytes)")
            label = text.strip().strip("[]").strip()
            if len(label.encode("utf-8")) > 31:
                problems.append(f"{src_file}: footnote label {label!r} exceeds FOOTNOTE_NUMBER_LEN-1")
            path, _, anchor = href.partition("#")
            target_file = src_file if not path else path.split("/")[-1]
            if path and target_file not in [f for f, _ in CHAPTERS] + ["cover.xhtml"]:
                problems.append(f"{src_file}: href {href!r} resolves to no spine document")
            elif anchor and anchor not in ids_by_file.get(target_file, set()):
                problems.append(
                    f"{src_file}: href {href!r} points at id \"{anchor}\", which is not a recorded anchor in {target_file}"
                )
        if footnotes == 0:
            problems.append("no internal links at all — chapter 5 cannot demonstrate footnotes")

        # -- UI-font coverage of everything drawn with the UI font ------------
        covered = load_ui_font_coverage(UI_FONT_HEADER)
        checked = {"書名": BOOK_TITLE, "作者": BOOK_AUTHOR}
        for _, label in CHAPTERS:
            checked[f"目錄「{label}」"] = label
        ui_chars = set()
        for where, text in checked.items():
            for ch in text:
                cp = ord(ch)
                if cp < 0x80 or unicodedata.category(ch) == "Zs":
                    continue
                ui_chars.add(ch)
                if not covered(cp):
                    problems.append(f"{where}: U+{cp:04X} '{ch}' is not in the built-in UI font — renders as a box")

        # -- images -----------------------------------------------------------
        for img in ("OEBPS/images/cover.png", f"OEBPS/images/{FIGURE_NAME}"):
            check_png(img, z.read(img), problems)

    # -- every 設定 → … path is on-screen wording, character for character ------
    # The book is only useful if its instructions match the device. UI strings move;
    # this makes the book fail the build instead of quietly going stale. The chapters
    # mark every path with <strong>, which is also what makes it extractable.
    paths = check_settings_paths(problems)

    print(f"\nverify: {epub_path.name}")
    print(f"  spine documents      {len(spine_files)}")
    print(f"  footnote links       {footnotes} (internal <a href> in spine documents)")
    print(f"  UI-font coverage     {len(ui_chars)} distinct non-ASCII characters checked")
    print(f"  settings paths       {paths} checked against zh-hant.yaml")
    for n in notes:
        print(f"  note   {n}")
    if problems:
        for p in problems:
            print(f"  FAIL   {p}")
        print(f"\n  {len(problems)} problem(s). Not shippable.")
        return 1
    print("  all checks passed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Build the CrossMosa onboarding EPUB.")
    ap.add_argument("--out", default=str(GUIDE_DIR), help="output directory (default: guide/)")
    ap.add_argument("--no-verify", action="store_true", help="skip the conformance pass")
    args = ap.parse_args()

    epoch = source_date_epoch()
    docs = build_documents()
    out_path = Path(args.out) / OUTPUT_NAME
    write_epub(out_path, docs, epoch)

    size = out_path.stat().st_size
    print(f"wrote {out_path}  ({size:,} bytes, SOURCE_DATE_EPOCH={epoch})")

    if args.no_verify:
        print("verification SKIPPED")
        return 0
    return verify(out_path)


if __name__ == "__main__":
    sys.exit(main())
