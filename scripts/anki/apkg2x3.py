#!/usr/bin/env python3
"""apkg2x3 — 把 Anki 的 .apkg 牌組轉成 X3 單字卡用的 .xdk 檔。

為什麼不在機器上直接讀 apkg：apkg 是 zip＋SQLite，卡面靠 HTML 模板＋JavaScript 拼出來，
音檔佔了九成以上的體積；ESP32-C3 沒有 SQLite、記憶體也只有幾百 KB。這支程式在電腦上把
卡面先排成「一行一個樣式碼＋文字」的純文字，去掉音檔，韌體只需要逐行畫出來。

用法：
    python apkg2x3.py <deck.apkg> [<deck2.apkg> ...] -o <輸出資料夾>
    python apkg2x3.py "H:/我的文件/Anki/新标准日本语 · 初级 (假名到释义) 教材原版发音.apkg" -o out/anki

把輸出資料夾裡的 .xdk 複製到 SD 卡的 /anki/。複習進度（.srs）由韌體在第一次打開時建立，
重新轉換、覆蓋 .xdk 不會動到進度（卡片順序與 deckId 只由原始牌組決定）。

支援的筆記類型（以「欄位名稱」判斷，不看模板名）：
    新標日：  排序编号 日文 词性 释义 课号 音频
    實用英語：id word AmEPhonetic usage definition def_cn examples AmEPronunciation
其他筆記類型走「通用」：正面＝第一個欄位，背面＝其餘欄位（去掉 HTML 與 [sound:]）。

.xdk 格式（little-endian），韌體端在 src/anki/AnkiDeckFile.h：
    0   4  magic "XDK1"
    4   2  version (1)
    6   2  headerSize (128)
    8   4  cardCount
    12  4  deckId（所有來源卡片 id 的 FNV-1a 32）
    16  4  indexOffset
    20  4  dataOffset
    24  2  建議每日新卡數
    26  2  建議每日複習上限
    28  1  nameLen
    29  1  保留
    30  98 牌組名稱（UTF-8，不含結尾 0）
    index: cardCount × u32（記錄的絕對位移）
    record: u16 frontLen, u16 backLen, front bytes, back bytes
卡面文字：以 \\n 分行，每行第一個字元是樣式碼：
    H 標題字（大字、置中）   S 副標（置中）   T 標籤（小字、置中）
    B 內文                   E 例句（縮排）   C 例句翻譯（縮排、小字）
    M 附註（小字、靠右）     R 分隔線（後面沒有文字）
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import os
import re
import sqlite3
import struct
import sys
import tempfile
import zipfile

try:
    import opencc  # opencc-python-reimplemented
except ImportError:  # pragma: no cover
    opencc = None

HEADER_SIZE = 128
NAME_MAX = 98
MAX_SIDE_BYTES = 6000  # 單面上限（韌體端一次讀進一張卡，兩面合計不超過 12 KB）

# ---------------------------------------------------------------- 文字處理

_s2t = None
_s2t_chars = None


def to_trad_chars(text: str) -> str:
    """只轉字、不換詞（牌組名稱用：s2twp 會把「高级」換成「高階」）。"""
    global _s2t_chars
    if opencc is None or not text:
        return text
    if _s2t_chars is None:
        _s2t_chars = opencc.OpenCC("s2t")
    return _s2t_chars.convert(text)


def to_trad(text: str) -> str:
    """簡體 → 臺灣正體（含用詞，s2twp）。沒裝 OpenCC 就原樣返回並警告一次。"""
    global _s2t
    if not text:
        return text
    if _s2t is None:
        if opencc is None:
            print("警告：沒有安裝 opencc-python-reimplemented，中文不會轉繁體", file=sys.stderr)
            _s2t = False
        else:
            _s2t = opencc.OpenCC("s2twp")
    if _s2t is False:
        return text
    return _s2t.convert(text)


_TAG_BR = re.compile(r"<\s*(br|/div|/p|/li)\s*/?>", re.I)
_TAG = re.compile(r"<[^>]+>")
_SOUND = re.compile(r"\[sound:[^\]]*\]")
_WS = re.compile(r"[ \t\u00a0]+")


def clean(text: str) -> str:
    """去 HTML、去 [sound:]、解實體、合併空白；保留換行。"""
    if not text:
        return ""
    t = _SOUND.sub("", text)
    t = _TAG_BR.sub("\n", t)
    t = _TAG.sub("", t)
    t = html.unescape(t)
    lines = [_WS.sub(" ", ln).strip() for ln in t.replace("\r", "").split("\n")]
    return "\n".join(ln for ln in lines if ln)


# Anki 的 furigana 語法：" 漢字[かな]"，前面的空白是分界、會被吃掉
_FURI = re.compile(r" ?([^ >\[\]]+?)\[([^\]]+)\]")
# 兩個非 ASCII 字元之間的空白（日文裡只是分界，不是真的空格）
_JP_SPACE = re.compile(r"(?<=[^\x00-\x7f]) +(?=[^\x00-\x7f])")


def furi_kanji(text: str) -> str:
    return _JP_SPACE.sub("", _FURI.sub(r"\1", text)).strip()


def furi_kana(text: str) -> str:
    return _JP_SPACE.sub("", _FURI.sub(r"\2", text)).strip()


class Face:
    """一面卡的組裝器：一行一個樣式碼。"""

    def __init__(self) -> None:
        self.lines: list[str] = []

    def add(self, style: str, text: str) -> None:
        if style == "R":
            if self.lines and self.lines[-1] != "R":
                self.lines.append("R")
            return
        for ln in clean(text).split("\n"):
            if ln:
                self.lines.append(style + ln)

    def text(self) -> str:
        while self.lines and self.lines[-1] == "R":
            self.lines.pop()
        out = "\n".join(self.lines)
        b = out.encode("utf-8")
        if len(b) > MAX_SIDE_BYTES:
            b = b[:MAX_SIDE_BYTES]
            # 切在 UTF-8 字元邊界、整行
            out = b.decode("utf-8", "ignore")
            out = out[: out.rfind("\n")] if "\n" in out else out
        return out


# ---------------------------------------------------------------- 筆記類型

def fields_of(model) -> list[str]:
    return [f["name"] for f in model["flds"]]


def is_shinhyonichi(names) -> bool:
    return {"日文", "释义"} <= set(names)


def is_kexp(names) -> bool:
    return {"word", "def_cn", "examples"} <= set(names)


def render_shinhyonichi(f: dict, tmpl_ord: int):
    jp = clean(f.get("日文", ""))
    pos = to_trad(clean(f.get("词性", "")))
    meaning = to_trad(f.get("释义", ""))
    lesson = clean(f.get("课号", ""))
    kana = furi_kana(jp)
    kanji = furi_kanji(jp)

    front = Face()
    front.add("H", kana)
    if pos:
        front.add("T", pos)

    back = Face()
    back.add("H", kanji)
    if kana != kanji:
        back.add("S", kana)
    if pos:
        back.add("T", pos)
    back.add("R", "")
    back.add("B", meaning)
    if lesson:
        back.add("R", "")
        back.add("M", f"第 {lesson} 課")
    return front.text(), back.text()


def _pick_example(examples: list, seed: str) -> int:
    if not examples:
        return -1
    h = int(hashlib.md5(seed.encode("utf-8")).hexdigest()[:8], 16)
    return h % len(examples)


def _pos_tag(tags: str) -> str:
    out = []
    for t in tags.split():
        if t.startswith("POS_"):
            out.append(t[4:])
        elif t.startswith("CEFR_"):
            out.append(t[5:])
    return " · ".join(out)


def _example_lines(face: Face, ex: dict, want_en=True, want_cn=True) -> None:
    lead = " ".join(x for x in (ex.get("labels", ""), ex.get("usage", "")) if x)
    if lead:
        face.add("T", lead)
    if want_en and ex.get("en"):
        face.add("E", ex["en"])
    if want_cn and ex.get("cn"):
        face.add("C", to_trad(ex["cn"]))


def render_kexp(f: dict, tags: str, tmpl_ord: int, card_id: int):
    word = clean(f.get("word", ""))
    phon = clean(f.get("AmEPhonetic", "") or f.get("BrEPhonetic", ""))
    usage = clean(f.get("usage", ""))
    definition = clean(f.get("definition", ""))
    def_cn = to_trad(clean(f.get("def_cn", "")))
    try:
        raw = f.get("examples", "").strip()
        if raw.startswith('"') and raw.endswith('"'):
            raw = raw[1:-1]
        examples = json.loads(html.unescape(raw)) if raw else []
        if not isinstance(examples, list):
            examples = []
    except Exception:
        examples = []
    pick = _pick_example(examples, f"{card_id}")
    tag = _pos_tag(tags)

    front = Face()
    back = Face()
    if tmpl_ord == 0:  # Read：看英文想意思
        front.add("H", word)
        if phon:
            front.add("S", phon)
        if usage:
            front.add("S", usage)
        if tag:
            front.add("T", tag)
        if pick >= 0:
            front.add("R", "")
            _example_lines(front, examples[pick], want_en=True, want_cn=False)

        back.add("H", word)
        if phon:
            back.add("S", phon)
        if usage:
            back.add("S", usage)
        back.add("R", "")
        back.add("B", def_cn)
        back.add("B", definition)
    else:  # Write：看意思想英文
        if tag:
            front.add("T", tag)
        front.add("B", def_cn)
        front.add("B", definition)
        if pick >= 0:
            front.add("R", "")
            _example_lines(front, examples[pick], want_en=False, want_cn=True)

        back.add("H", word)
        if phon:
            back.add("S", phon)
        if usage:
            back.add("S", usage)
        back.add("R", "")
        back.add("B", def_cn)
        back.add("B", definition)

    # 背面的例句：先放正面用過的那一句（中英都給），再補最多兩句
    order = ([pick] if pick >= 0 else []) + [i for i in range(len(examples)) if i != pick]
    if order:
        back.add("R", "")
    for i in order[:3]:
        _example_lines(back, examples[i])
    return front.text(), back.text()


def render_generic(names, values, tmpl_ord: int):
    front = Face()
    back = Face()
    if values:
        front.add("H", to_trad(values[0]))
        back.add("H", to_trad(values[0]))
        back.add("R", "")
    for n, v in zip(names[1:], values[1:]):
        v = clean(v)
        if v:
            back.add("B", to_trad(v))
    return front.text(), back.text()


# ---------------------------------------------------------------- apkg 讀取

def open_collection(apkg: str, workdir: str) -> sqlite3.Connection:
    z = zipfile.ZipFile(apkg)
    names = z.namelist()
    if "collection.anki21b" in names and "collection.anki21" not in names:
        raise SystemExit(f"{apkg}: 這是新版 Anki（2.1.50+）壓縮格式，請在 Anki 匯出時勾選「支援舊版 Anki」")
    for cand in ("collection.anki21", "collection.anki2"):
        if cand in names:
            path = z.extract(cand, workdir)
            return sqlite3.connect(path)
    raise SystemExit(f"{apkg}: 找不到 collection.anki2*")


def fnv1a32(data: bytes, h: int = 0x811C9DC5) -> int:
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def nice_deck_name(full: str) -> str:
    """「KEXP3-AmE::常用词::1. 读」→「實用英語·常用詞·讀」；其餘轉繁體、:: 換成 ·"""
    parts = [p for p in full.split("::") if p and p != "Default"]
    if parts and parts[0].startswith("KEXP3"):
        parts[0] = "实用英语"
    parts = [re.sub(r"^\d+\.\s*", "", p) for p in parts]
    name = to_trad_chars("·".join(parts))
    name = name.replace("(假名到解釋)", "").replace("（假名到解釋）", "")
    name = re.sub(r"^新標準日本語", "新標日·", name)
    return name.strip()


def safe_filename(name: str) -> str:
    return re.sub(r'[\\/:*?"<>|\s]+', "_", name).strip("_") or "deck"


def convert(apkg: str, out_dir: str, new_per_day: int, max_reviews: int) -> list[str]:
    written = []
    with tempfile.TemporaryDirectory() as tmp:
        col = open_collection(apkg, tmp)
        models = json.loads(col.execute("select models from col").fetchone()[0])
        decks = json.loads(col.execute("select decks from col").fetchone()[0])
        notes = {}
        for nid, mid, flds, tags in col.execute("select id, mid, flds, tags from notes"):
            notes[nid] = (str(mid), flds.split("\x1f"), tags)

        # 卡片分到各自的牌組（英文卡組的「讀」「寫」本來就是兩個子牌組）
        by_deck: dict[int, list] = {}
        for cid, nid, did, ord_, ctype, due in col.execute(
                "select id, nid, did, ord, type, due from cards"):
            if nid not in notes:
                continue
            by_deck.setdefault(did, []).append((cid, nid, ord_, ctype, due))

        for did, cards in sorted(by_deck.items()):
            deck_name = nice_deck_name(decks.get(str(did), {}).get("name", "Deck"))
            # 新卡順序：Anki 的 new position（due）→ 排序編號（新標日）→ 卡片 id
            def sort_key(c):
                cid, nid, ord_, ctype, due = c
                mid, vals, _ = notes[nid]
                names = fields_of(models[mid])
                if is_shinhyonichi(names) and "排序编号" in names:
                    try:
                        return (0, int(vals[names.index("排序编号")]), ord_, cid)
                    except ValueError:
                        pass
                return (1, due if ctype == 0 else 0, ord_, cid)

            cards.sort(key=sort_key)
            records = []
            id_bytes = bytearray()
            for cid, nid, ord_, ctype, due in cards:
                mid, vals, tags = notes[nid]
                names = fields_of(models[mid])
                f = dict(zip(names, vals))
                if is_shinhyonichi(names):
                    front, back = render_shinhyonichi(f, ord_)
                elif is_kexp(names):
                    front, back = render_kexp(f, tags, ord_, cid)
                else:
                    front, back = render_generic(names, vals, ord_)
                records.append((front.encode("utf-8"), back.encode("utf-8")))
                id_bytes += struct.pack("<q", cid)

            path = write_xdk(out_dir, deck_name, records, fnv1a32(bytes(id_bytes)), new_per_day, max_reviews)
            written.append(path)
            print(f"  {deck_name}: {len(records)} 張 → {path} ({os.path.getsize(path) / 1024:.0f} KB)")
        col.close()
    return written


def write_xdk(out_dir, name, records, deck_id, new_per_day, max_reviews) -> str:
    os.makedirs(out_dir, exist_ok=True)
    n = len(records)
    index_off = HEADER_SIZE
    data_off = index_off + 4 * n
    name_b = name.encode("utf-8")
    if len(name_b) > NAME_MAX:
        name_b = name_b[:NAME_MAX].decode("utf-8", "ignore").encode("utf-8")
    header = struct.pack("<4sHHIIIIHHBB", b"XDK1", 1, HEADER_SIZE, n, deck_id, index_off, data_off,
                         new_per_day, max_reviews, len(name_b), 0)
    header += name_b
    header += b"\0" * (HEADER_SIZE - len(header))
    offsets = []
    blob = bytearray()
    for front, back in records:
        offsets.append(data_off + len(blob))
        blob += struct.pack("<HH", len(front), len(back)) + front + back
    path = os.path.join(out_dir, safe_filename(name) + ".xdk")
    with open(path, "wb") as fp:
        fp.write(header)
        fp.write(struct.pack(f"<{n}I", *offsets))
        fp.write(blob)
    return path


def dump(path: str, count: int) -> None:
    """除錯：印出 .xdk 的前幾張卡。"""
    with open(path, "rb") as fp:
        data = fp.read()
    magic, ver, hsize, n, deck_id, ioff, doff, npd, mrv, nlen, _ = struct.unpack_from("<4sHHIIIIHHBB", data)
    print(magic, ver, n, hex(deck_id), npd, mrv, data[30:30 + nlen].decode("utf-8"))
    for i in range(min(n, count)):
        off = struct.unpack_from("<I", data, ioff + 4 * i)[0]
        fl, bl = struct.unpack_from("<HH", data, off)
        print(f"--- #{i} front")
        print(data[off + 4:off + 4 + fl].decode("utf-8"))
        print("--- back")
        print(data[off + 4 + fl:off + 4 + fl + bl].decode("utf-8"))


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("apkg", nargs="+", help=".apkg 檔（或用 --dump 看 .xdk）")
    ap.add_argument("-o", "--out", default="x3_anki", help="輸出資料夾（預設 ./x3_anki）")
    ap.add_argument("--new-per-day", type=int, default=20, help="建議每日新卡數（預設 20，機器上可改）")
    ap.add_argument("--max-reviews", type=int, default=200, help="建議每日複習上限（預設 200，機器上可改）")
    ap.add_argument("--dump", type=int, metavar="N", help="印出 .xdk 前 N 張卡（除錯用）")
    args = ap.parse_args()
    if args.dump:
        for p in args.apkg:
            dump(p, args.dump)
        return 0
    for p in args.apkg:
        print(os.path.basename(p))
        convert(p, args.out, args.new_per_day, args.max_reviews)
    return 0


if __name__ == "__main__":
    sys.exit(main())
