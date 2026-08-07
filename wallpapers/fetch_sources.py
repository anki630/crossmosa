#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fetch_sources.py — 從 Wikimedia Commons(公共領域)下載 artworks.py 清單裡
每幅畫的原圖到 sleep/_src/,供 make-art-wallpapers.py 使用。

用法:
  python3 fetch_sources.py            # 下載清單裡所有缺檔的原圖
  python3 fetch_sources.py --force    # 強制重新下載(覆蓋已存在的)
"""
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from artworks import ARTWORKS

HERE = Path(__file__).resolve().parent
SRC_DIR = HERE.parent / "sleep" / "_src"

UA = "crossmosa-wallpaper-fetch/1.0 (personal e-reader wallpaper project; https://github.com/anki630/crossmosa)"
WIDTH = 1600  # 夠印到 792px 長邊還有餘裕,又不會抓到動輒上百 MB 的原始掃描檔
REQUEST_GAP = 3.0  # 秒,每次請求間隔,避免踩 Wikimedia rate limit(踩過 429)
MAX_RETRIES = 5


def fetch(commons_filename, dst):
    encoded = urllib.parse.quote(commons_filename.replace(" ", "_"))
    url = "https://commons.wikimedia.org/wiki/Special:FilePath/%s?width=%d" % (encoded, WIDTH)
    req = urllib.request.Request(url, headers={"User-Agent": UA})

    last_err = None
    for attempt in range(MAX_RETRIES):
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = resp.read()
            dst.write_bytes(data)
            return len(data)
        except urllib.error.HTTPError as e:
            last_err = e
            if e.code == 429:
                wait = float(e.headers.get("Retry-After", 5 * (attempt + 1)))
                print("   (429 rate limited, 等 %.0fs 重試 %d/%d)" % (wait, attempt + 1, MAX_RETRIES))
                time.sleep(wait)
                continue
            raise
    raise last_err


def main():
    force = "--force" in sys.argv
    SRC_DIR.mkdir(parents=True, exist_ok=True)

    ok = 0
    for art in ARTWORKS:
        dst = SRC_DIR / (art["slug"] + ".jpg")
        if dst.exists() and not force:
            print("=  已存在,跳過: %s" % dst.name)
            ok += 1
            continue
        try:
            n = fetch(art["commons_file"], dst)
            print("✓  %-32s %8d bytes  <- %s" % (dst.name, n, art["commons_file"]))
            ok += 1
        except urllib.error.HTTPError as e:
            print("✗  %-32s HTTP %s  <- %s" % (art["slug"], e.code, art["commons_file"]))
        except Exception as e:  # noqa: BLE001
            print("✗  %-32s 下載失敗: %s" % (art["slug"], e))
        time.sleep(REQUEST_GAP)

    print("\n完成 %d / %d" % (ok, len(ARTWORKS)))
    sys.exit(0 if ok == len(ARTWORKS) else 1)


if __name__ == "__main__":
    main()
