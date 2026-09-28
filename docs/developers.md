# 給開發者

這一頁是給想自己建置、想知道 CrossMosa 跟原版差在哪、或想補介面字型的人。一般使用者不需要讀。

## 自行建置

```bash
git submodule update --init --recursive --depth 1   # freeink-sdk 是 submodule，缺了會 link 失敗
pip install platformio
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)  # 見下方「可重現建置」
pio run -e gh_release                                # 產物在 .pio/build/gh_release/firmware.bin
```

### 可重現建置

**發佈的映像檔（`update.bin`，即建置產物 `firmware.bin` 改名）是逐位元組可重現的**——同一個 commit、同一組釘住版本的相依套件，
任何人都能建出 sha256 完全相同的檔案。條件只有一個：**必須設 `SOURCE_DATE_EPOCH`**。

不設的話，`__DATE__` / `__TIME__` 會把建置當下的時刻編進 binary（其中一處還在 Arduino
core 裡，不是本專案能改的），兩次建置就會差幾十個位元組。設了之後 GCC 會用這個值取代那兩個
巨集，同時本專案的網頁資產壓縮也會用它當 gzip 的 mtime。

**每個 Release 都會公佈當次使用的 `SOURCE_DATE_EPOCH` 與 firmware 的 sha256。**
打包腳本 [`scripts/mk-release.sh`](../scripts/mk-release.sh) 預設直接取 release commit 自己的
時間戳（`git log -1 --format=%ct`），所以只要 checkout 同一個 tag 就會自動得到同一個值。
機制與判讀方式寫在 [`reproducible-builds.md`](reproducible-builds.md)。

---

## 與原版 CrossPoint 的關係

**CrossMosa 的一切都建立在 [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) 上面。**
閱讀引擎、EPUB 解析、排版、活動框架、網頁介面、OPDS、Calibre 流程——這些都是原版寫的，
本分支只是在上面做中文化與 X3、X4 的調校。

- 原版作者：**Dave Allie** 與 CrossPoint 貢獻者們。授權 MIT，`LICENSE` 原封保留。
- 原版的錯誤回報請發到[原版 repo](https://github.com/crosspoint-reader/crosspoint-reader/issues)，
  不要發到這裡；本 repo 只處理本分支自己改壞的東西。
- **想要完整功能（多語系、更多格式）的人應該用原版**，不是用這個分支。

### 與原版的差異

**已移除**（不是關閉，是程式碼層面拔掉入口讓連結器回收，換 flash 空間給中文字型）：

| 移除 | 原因 |
|---|---|
| English / 繁體中文以外的 **29 種 UI 語言** | 約 258 KB，換中文字型 |
| **KOReader 進度同步** | 沒有伺服器可同步 |
| **字典查詢**（StarDict） | 未使用 |
| **OTA 線上更新** | 會指向原版的 release 把本分支蓋掉；**SD 卡韌體更新保留** |
| **Classic / RoundedRaff 主題** | 字級與語系支援跟不上中文；留 Formosa、Formosa Extended 與 Formosa Pro |
| **非英文的斷字表**（9 種語言） | 中文不斷字，約 323 KB |
| **內建斜體字面** | 自動退回正體，約 544 KB |
| 內建閱讀字型縮成**單一 14px 備援** | 只在沒有 SD 字型時用得到，約 373 KB |
| **SMB2 伺服器**（iOS「檔案」App 直接管理 SD 卡） | 已移除，原始碼一併拿掉，無法再開編譯開關編回來。請改用網頁傳檔、Calibre、OPDS 或拔卡複製 |
| **BLE 翻頁遙控器** | 已移除。發佈韌體本來就沒有；原始碼也不再保留，無法自編加回 |

**保留**：Calibre 無線推書（相容原版外掛生態）、網頁設定與傳檔、WebDAV、OPDS、
傾斜翻頁（X3）、螢幕截圖、按鍵重配、待機畫面。

---

## 介面字型：收了哪些字、怎麼自己重做

介面字型（選單、檔名、書名、目錄用的字）編在韌體裡，收的是 **BIG5 一級全部，加上掃描整個電子書庫挑出來的常用字**，
不是全部的中文字。確切的字集以 `fonts/charsets/charset-ui-v5.txt` 為準。書的內文走 SD 卡字型，不受這個限制。

| 東西 | 路徑 |
|---|---|
| 目前的 UI 字集（含完整出處與選字規則，寫在檔頭） | `fonts/charsets/charset-ui-v5.txt` |
| 2.0.1 補的字（掃書庫挑出來的） | `fonts/charsets/charset-ui-v5-additions.txt` |
| UI 字型重產腳本 | `fonts/regen-ui-fonts.sh` |
| 二級字選字程式（候選池 + 字頻排序） | `fonts/pick-big5-l2-chars.py` |
| SD 卡字型產生器 | `lib/EpdFont/scripts/fontconvert_sdcard.py`（加了 `tc-reading` 字集） |

流程是：把字加進字集檔 → 跑重產腳本 → 重新編譯韌體 → 重刷。
**UI 字型無法用 SD 卡替換，只能重編韌體。**

> 字集檔的檔頭**必須全部是 ASCII**——整個檔案會餵給 `pyftsubset --text-file`，
> 檔頭裡的任何一個中文字都會悄悄進到字型裡。重產腳本有 cmap 斷言擋這件事。

### 為什麼不乾脆全部收進去

全 BIG5 加進 UI 字型大約要多 2 MB，而 app 分割區只有 6.5 MB，發佈的韌體已經用掉約 98%
（2.1.0-beta.5：剩不到 100 KB）。要放得下就得先重新分割 flash——有變磚風險。

---
---

# For developers (English)

This page is for people who want to build CrossMosa, understand how it differs from the original firmware, or add characters to the interface font. Most users do not need it.

## Build it yourself

```bash
git submodule update --init --recursive --depth 1   # freeink-sdk is a submodule; linking fails without it
pip install platformio
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)  # see “Reproducible builds” below
pio run -e gh_release                                # output: .pio/build/gh_release/firmware.bin
```

### Reproducible builds

**Release images are reproducible byte for byte.** The released `update.bin` is the build output `firmware.bin` under a different name. Anyone using the same commit and the same pinned dependencies can produce a file with the same sha256. There is one requirement: **you must set `SOURCE_DATE_EPOCH`**.

Without it, `__DATE__` and `__TIME__` place the build time in the binary. One occurrence is in the Arduino core and cannot be changed by this project. As a result, two builds differ by several dozen bytes. When `SOURCE_DATE_EPOCH` is set, GCC uses its value for both macros. CrossMosa also uses it as the gzip mtime when compressing web assets.

**Every release publishes its `SOURCE_DATE_EPOCH` and firmware sha256.** By default, the [`scripts/mk-release.sh`](../scripts/mk-release.sh) packaging script uses the release commit’s timestamp from `git log -1 --format=%ct`. Checking out the same tag therefore produces the same value automatically.

See [`reproducible-builds.md`](reproducible-builds.md) for details and verification instructions.

---

## Relationship to the original CrossPoint

**Everything in CrossMosa is built on [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader).** CrossPoint provides the reading engine, EPUB parsing, layout, activity framework, web interface, OPDS, and Calibre workflow. This fork adds Chinese support and adjustments for X3 and X4.

- The original authors are **Dave Allie** and the CrossPoint contributors. The project uses the MIT license, and `LICENSE` is preserved unchanged.
- Report bugs in the original firmware to the [original repository](https://github.com/crosspoint-reader/crosspoint-reader/issues), not here. This repository handles only problems introduced by this fork.
- **If you need the complete feature set, including multiple languages and more formats, use the original firmware instead of this fork.**

### Differences from the original firmware

The following features are **removed**. They are not merely disabled. Their entry points were removed in code so the linker can reclaim flash space for Chinese fonts.

| Removed | Reason |
|---|---|
| **29 interface languages** other than English and Traditional Chinese | Reclaims about 258 KB for Chinese fonts |
| **KOReader progress sync** | There is no server to sync with |
| **Dictionary lookup** using StarDict | Not used |
| **Online OTA updates** | They would point to the original releases and overwrite this fork. **SD Card Firmware Update remains available** |
| **Classic and RoundedRaff themes** | Their text sizes and language support do not work well with Chinese. Formosa, Formosa Extended, and Formosa Pro remain |
| **Non-English hyphenation tables** for 9 languages | Chinese does not use hyphenation. Reclaims about 323 KB |
| **Built-in italic faces** | Italic text falls back to regular. Reclaims about 544 KB |
| Built-in reader fonts reduced to **one 14 px fallback** | It is only used when no SD card font is available. Reclaims about 373 KB |
| **SMB2 server** for managing the SD card directly from the iOS Files app | Removed with its source code and cannot be restored with a build option. Use browser file transfer, Calibre, OPDS, or copy files from the card |
| **BLE page-turner remote** | Removed. Release firmware did not include it, and its source is no longer retained, so you cannot add it back in a custom build |

**Retained:** Calibre wireless book transfer, including compatibility with the original plugin ecosystem; web settings and file transfer; WebDAV; OPDS; tilt page turning on X3; screenshots; key remapping; and sleep screens.

---

## Interface fonts: included characters and rebuilding

The interface font is compiled into the firmware and is used for menus, filenames, book titles, and tables of contents. It contains **all BIG5 Level 1 characters plus common characters selected by scanning an entire ebook library**. It does not contain every Chinese character. The exact character set is defined in `fonts/charsets/charset-ui-v5.txt`.

Book text uses fonts from the SD card and is not affected by this limit.

| Item | Path |
|---|---|
| Current interface character set, with complete sources and selection rules in its header | `fonts/charsets/charset-ui-v5.txt` |
| Characters added in 2.0.1 after scanning the library | `fonts/charsets/charset-ui-v5-additions.txt` |
| Interface-font regeneration script | `fonts/regen-ui-fonts.sh` |
| BIG5 Level 2 selection program, including the candidate pool and frequency ranking | `fonts/pick-big5-l2-chars.py` |
| SD card font generator | `lib/EpdFont/scripts/fontconvert_sdcard.py`, with the `tc-reading` character set |

The process is: add characters to the character-set file → run the regeneration script → rebuild the firmware → reflash the device.

**You cannot replace interface fonts from the SD card. You must rebuild the firmware.**

> The character-set file’s header **must contain only ASCII**. The entire file is passed to `pyftsubset --text-file`. Any Chinese character in the header would silently be included in the font. The regeneration script includes a cmap assertion to prevent this.

### Why not include every character?

Adding all BIG5 characters to the interface font would require about 2 MB more space. The app partition is only 6.5 MB, and the released firmware already uses about 98% of it. Version 2.1.0-beta.5 has less than 100 KB free.

Making enough room would require repartitioning the flash, which carries a risk of bricking the device.
