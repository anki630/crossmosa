# NOTICE — 授權與第三方元件清單

本檔列出 CrossMosa 1.0.0 所含的全部第三方程式碼與字型、各自的授權,以及**發佈韌體
binary 時的義務**。

> 這份清單裡的每一條授權都是從本機檔案實際查證的(授權檔、原始碼檔頭、套件中繼資料、
> 字型 `name` 表第 13/14 欄)。**「進 binary」欄是用 `riscv32-esp-elf-nm` 對實際建置出來的
> `firmware.elf` 數符號得到的**,不是從相依清單推論的。查證方式見本檔最後一節。

---

## 1. 本專案

| | |
|---|---|
| **上游 CrossPoint** | MIT License,Copyright (c) 2025 **Dave Allie**。授權全文保留在 `LICENSE`,未經修改。 |
| **CrossMosa 的修改** | MIT License,Copyright (c) 2026 **CrossMosa contributors**。 |

CrossMosa 是 CrossPoint 的分支。所有未特別標示的原始碼都源自上游或為本分支的修改,
兩者同為 MIT。

---

## 2. ⚠️ 發佈義務(請先讀這節)

**發佈出來的 `firmware.bin` 裡有 GPLv2 與 LGPL-2.1 的程式碼。**
本專案的原始碼是 MIT,但**編出來的 binary 是個組合作品**,受其中最嚴格的條款約束。

### 2.1 GPLv2 — wolfSSL

`Arduino-wolfSSL` 套件的中繼資料明確宣告 `"license": "GPL-2.0-only"`,
而它有 **204 個符號連結進發佈的 binary**(TLS 是 OPDS 與 Calibre 連線用的)。

實務上的意思:

- MIT 與 GPLv2 相容,所以這個組合作品**可以**依 GPLv2 的條款散布。
- 條件是**提供完整的對應原始碼**(complete corresponding source)。
  本專案以「公開的原始碼 repo + `platformio.ini` 裡逐一釘住版本的相依套件 + 可重現的
  建置指令」履行這一點:任何人都能從本 repo 重建出同一份 binary。
- 因此**發佈 binary 時必須同時保持這個 repo 公開可取得**。若日後把 repo 轉回私有,
  就不能再發佈編好的 binary。

> wolfSSL 本身採雙授權(GPLv2 或商業授權)。本專案使用的是**開源那一側**,
> 所以適用的是 GPLv2。上游 CrossPoint 連結同一個套件、同樣發佈編好的 binary。

### 2.2 LGPL-2.1 — arduino-esp32、libsmb2、WebSockets

這三個是 LGPL-2.1(或 -or-later),都靜態連結進 binary。LGPL 第 6 條要求
**讓使用者能夠用修改過的函式庫版本重新連結**。本專案同樣以「公開完整原始碼 +
釘住版本的相依套件 + 標準的 PlatformIO 建置流程」履行:使用者可以自行替換任一函式庫
再重建韌體。

**libsmb2 另有一項標示義務**:本專案對 vendored 的 libsmb2 做了 **5 處**修改。
每一處的內容、理由與上游對照都記錄在 `docs/third-party/libsmb2-vendoring.md`,
並由 `scripts/verify_libsmb2_patch.py` 逐位元組釘住(該腳本會即時抓取上游 tarball
比對全部檔案,並斷言「恰好 5 處差異」)。

> **注意**:雖然 SMB2 功能在 1.0.0 是關閉的(選單裡沒有入口、伺服器不會啟動),
> **libsmb2 的程式碼仍然被編譯並連結進 binary**(211 個符號)——那是連結器沒有回收掉的
> 死碼。所以 LGPL 的義務**照樣成立**,這裡不含糊帶過。

### 2.3 不構成法律意見

以上是依各套件宣告的授權所做的整理,不是法律意見。
如果你要以任何超出「個人使用 / 非商業散布」的方式散布本韌體,請自行確認。

---

## 3. 隨原始碼一起附上的第三方程式碼(vendored,在樹裡)

| 元件 | 版本 | 授權 | 位置 | 進 binary |
|---|---|---|---|---|
| **libsmb2** | 3.0.1 | **LGPL-2.1-or-later**(`lib/` 與非 DCE/RPC 標頭);`libdcerpc/`、`include/smb2/libsmb2-dcerpc*.h` 與 `examples/` 為 **2-Clause BSD** | `lib/smb2/`(授權全文:`lib/smb2/COPYING`、`lib/smb2/LICENCE-LGPL-2.1.txt`) | 是(211 個符號,功能關閉但死碼保留) |
| **Expat** | 2.7.3 | **MIT** | `lib/expat/`(授權在每個原始碼檔頭) | 是 |
| **miniz** | 3.1.2(本地套件版號標 11.3.2) | **MIT**(`miniz.c` 檔頭的授權條款);檔案另含一段 Unlicense / public-domain 聲明 | `lib/miniz/third_party/miniz.{c,h}` | 是(只用 inflate) |
| **freeink-sdk** | git submodule | **MIT**,Copyright (c) 2026 FreeInk | `freeink-sdk/`(`LICENSE` + `NOTICE`) | 是 |

**libsmb2 著作權**:Copyright (C) 2016 Ronnie Sahlberg,部分 2017 Primary Data Inc.,
另有其他貢獻者(見各檔案檔頭)。

**Expat 著作權**:Copyright (c) 1999-2000 Thai Open Source Software Center Ltd、
2000 Clark Cooper、2002 Fred L. Drake Jr.、2007 Karl Waclawek、2017 Sebastian Pipping 等。

**miniz 著作權**:Copyright 2013-2014 RAD Game Tools 與 Valve Software;
2010-2014 Rich Geldreich 與 Tenacious Software LLC。

**freeink-sdk 的再歸屬**:該 SDK 的 `NOTICE` 說明它衍生自 OpenX4 E-Paper Community SDK
(MIT,Copyright (c) 2025 Open X4 E-Paper Contributors),SSD1677 與 UC8253 的面板初始化
序列與波形 LUT 出自該專案,原始的電子紙驅動作者為 CidVonHighwind。

---

## 4. 建置時取得的相依套件(釘在 `platformio.ini`)

這些**不在 repo 裡**,由 PlatformIO 依 `platformio.ini` 的 `lib_deps` 逐一釘住版本後
取得到 `.pio/libdeps/`,再連結進 binary。

| 套件 | 版本 | 授權 | 進 binary |
|---|---|---|---|
| **Arduino-wolfSSL** (wolfSSL Inc.) | 5.7.2 | **GPL-2.0-only**(見 §2.1) | 是(204 個符號) |
| **ArduinoJson** (Benoit Blanchon) | 7.4.2 | MIT | 是 |
| **WebSockets** (links2004 / Markus Sattler) | 2.7.3 | **LGPL-2.1**(見 §2.2) | 是 |
| **PNGdec** (Larry Bank / BitBank Software) | 1.1.6 | Apache-2.0 | 是 |
| **JPEGDEC** (Larry Bank / BitBank Software) | 1.8.4(釘 git commit `8628297`) | Apache-2.0 | 是 |
| **QRCode** (Richard Moore) | 0.0.1 | MIT,Copyright (c) 2017 Richard Moore | 是 |
| **SdFat** (Bill Greiman) | 2.3.1 | MIT | 是(經 freeink-sdk 的 SDCardManager) |

> **關於 NimBLE-Arduino**:BLE 翻頁遙控器預設不編入,`platformio.ini` 的 `lib_deps` 已把
> 該行註解掉,所以**預設建置根本不會下載這個套件,發佈的 binary 裡也一個符號都沒有**
> (`nm` 實測 0)。因此本清單不列它。若你自行加上 `-DCROSSMOSA_BLE` 編第二份韌體,
> PlatformIO 會把它拉進來,**那份 binary 的授權義務要你自己確認**(它不是本專案發佈的產物)。

---

## 5. 平台與框架

由 pioarduino 的 `platform-espressif32` 55.03.37 提供。同樣不在 repo 裡。

| 元件 | 版本 | 授權 |
|---|---|---|
| **arduino-esp32**(Arduino core for ESP32) | 3.3.7 | **LGPL-2.1-or-later**(套件 `package.json` 宣告,原始碼檔頭一致;見 §2.2) |
| **ESP-IDF**(Espressif) | 5.5.2 | Apache-2.0 |
| **FreeRTOS Kernel**(ESP-IDF SMP 修改版) | V10.5.1 | MIT,Copyright (C) 2021 Amazon.com, Inc. |
| **lwIP** | 隨 ESP-IDF | BSD 3-Clause 型(`components/lwip/lwip/COPYING`) |

ESP-IDF 本身還內含數十個各有授權的元件;Espressif 自己的 NOTICE 與各元件的授權檔
隨框架套件一起散布,此處不重複列舉。

> **一項本地修補**:本專案有一個建置期腳本(`scripts/patch_network_dns_lock.py`)會就地
> 修補 arduino-esp32 套件中的一行,把 `dns_clear_cache()` 包進 TCP/IP 核心鎖
> (修的是上游一個會造成偶發 panic 的競態)。這是對 LGPL 函式庫的修改,
> 腳本本身即是該修改的完整說明與可重現的套用方式,且會在框架版本漂移時硬擋建置。

---

## 6. 字型

### 6.1 編進韌體的 UI 字型

UI 字型是**多套字型的子集合併**後轉成點陣資料編進 binary 的。來源字型與各自授權:

| 來源字型 | 用途 | 授權 | 位置 |
|---|---|---|---|
| **Ubuntu** / Ubuntu-Vietnamese | 拉丁文字面 | **Ubuntu Font Licence 1.0** | `lib/EpdFont/builtinFonts/source/Ubuntu/`(`UFL.txt`) |
| **Noto Sans TC** | 主要漢字(7,413 字子集) | **SIL OFL 1.1** © 2014-2021 Adobe | 子集產物在 `lib/EpdFont/builtinFonts/` |
| **Noto Sans** | 備援與 U+FFFD | **SIL OFL 1.1** | `lib/EpdFont/builtinFonts/source/NotoSans/`(`OFL.txt`) |
| **Noto Sans Hebrew** / **Noto Sans Arabic** | 希伯來文 / 阿拉伯文 | **SIL OFL 1.1** | `.../source/NotoSansHebrew/`、`.../source/NotoSansArabic/`(各有 `OFL.txt`) |
| **Noto Sans CJK TC** | 補字(注音、假名、符號、變體漢字) | **SIL OFL 1.1** | 子集產物同上 |
| **DejaVu Sans** | 3 個符號字(∎ ► ▾) | **Bitstream Vera Fonts License**;DejaVu 自身的改動為 public domain。© 2003 Bitstream, Inc.;© 2006 Tavmjong Bah | 子集產物同上 |
| Noto Serif / OpenDyslexic | 上游附帶,本分支未編入 | **SIL OFL 1.1** | `.../source/`(各有 `OFL.txt`) |

> **Bitstream Vera 的條款有一項具體限制**:修改過的字型不得使用含 "Bitstream" 或 "Vera"
> 的名稱。本專案的 DejaVu 子集命名為 `UI-Symextra`,符合該限制。

### 6.2 SD 卡內文字型(Release 資產,不在 repo 裡)

以 `.cpfont` 點陣格式散布,由上游的 `lib/EpdFont/scripts/fontconvert_sdcard.py`
從下列字型產生:

| 字型 | 授權 | 著作權 |
|---|---|---|
| **Noto Serif CJK TC** | **SIL OFL 1.1** | © 2017-2023 Adobe |
| **Noto Sans TC** | **SIL OFL 1.1** | © 2014-2021 Adobe |
| **芫荽 Iansui** | **SIL OFL 1.1** | Copyright 2025 The Iansui Project Authors(https://github.com/ButTaiwan/iansui,衍生自 Klee One) |
| **IBM Plex Sans TC** | **SIL OFL 1.1** | Copyright 2018 IBM Corp. |

**SIL OFL 1.1 的兩項要求,本專案的處理方式**:

1. **保留授權聲明**:`crossmosa-1.0.0-sd-fonts.zip` 的**每一個字型資料夾內**都附有一份
   `OFL.txt`,含該字型的著作權行與 SIL OFL 1.1 全文。這件事不靠人記得——打包腳本
   `scripts/mk-release.sh` 會在壓縮**之前**逐一檢查每個家族目錄有沒有授權檔,缺的就補上,
   補不出來就**中止打包**而不是默默出貨一包沒有授權聲明的字型。
2. **不得單獨販售字型;衍生物不得使用 Reserved Font Name**。本專案的 `.cpfont` 是
   點陣衍生物,檔名沿用家族名(`NotoSerifTC_16.cpfont` 等)。
   **已查證:四套來源字型都沒有宣告 Reserved Font Name** ——
   逐顆讀 `name` 表 nameID 0(`NotoSerifTC-{Regular,Bold}`、`IBMPlexSansTC-{Regular,Bold}`、
   `Iansui-Regular`)都沒有 "with Reserved Font Name" 字樣,repo 內幾份 `OFL.txt` 的著作權行
   同樣沒有。因此沿用家族名合規,不需要改檔名。

---

## 7. 圖示 (Icons)

| 來源 | 版本 | 授權 | 位置 | 進 binary |
|---|---|---|---|---|
| **Tabler Icons** (Paweł Kuna) | **3.31.0** | **MIT**,Copyright (c) 2020-2026 Paweł Kuna | 來源 SVG 在 `icons/`(授權全文 `icons/LICENSE-tabler.txt`);轉出的點陣資料在 `src/components/icons/*.h` | 是(15 顆圖示編成點陣陣列) |

介面的 15 顆圖示是從 Tabler 的 SVG 轉成 1-bit 點陣陣列後編進韌體的。
`icons/README.md` 記錄了每顆圖示的 slot → 來源 → 尺寸 → 線寬對照,以及轉換指令。

**一個例外**:`icons/cover-crossmosa.svg`(書封佔位圖示)是**本專案自製**的——
以 Tabler 的 `book-2` 書封輪廓加上縮小的 `moon`(filled)組成,不是上游原檔,
但既然由 MIT 授權的圖形衍生而來,同樣以 MIT 提供。

---

## 8. 待機壁紙(Release 資產,不在 repo 裡)

| 內容 | 來源 | 授權狀態 | 位置 |
|---|---|---|---|
| **50 張世界名畫**的待機壁紙 | [Wikimedia Commons](https://commons.wikimedia.org/) | **公共領域(public domain)——無額外授權義務** | 產物在 `crossmosa-1.0.0-wallpapers.zip`;逐張出處與轉檔工具在 `wallpapers/` |

**為什麼沒有義務**:兩層都落在公共領域。

1. **畫作本身**已進入公共領域(著作權存續期間屆滿)。
2. **翻拍照片**——二維平面藝術品的忠實攝影翻拍**不產生新的著作權**
   (*Bridgeman Art Library v. Corel Corp.*,S.D.N.Y. 1999;Wikimedia 基金會亦採此立場),
   所以 Commons 上那些檔案的 PD 標示適用於整個檔案。

**逐張的出處檔名記錄在 [`wallpapers/artworks.py`](wallpapers/artworks.py)**(每一筆都有
Commons 的精確檔名),連同為什麼選這一張、以及為什麼換掉某幾張的理由。
挑選過程中曾發現數個檔名與內容不符的來源(展場遊客照、素描草稿、博物館展框翻拍),
**都已換成乾淨的平面掃描**——這既是畫質問題,也是授權問題:
遊客拍攝的展場照片是**攝影者的新作品**(常標成 CC-BY),不屬於上面第 2 點的範圍。

轉檔腳本本身(`wallpapers/*.py`)是本專案的一部分,MIT,與 §1 相同。
標籤文字用的 DejaVu Serif 字型見 §6.1。

---

## 9. 查證方式(可重跑)

「進 binary」欄的判定方式,對 `.pio/build/gh_release/firmware.elf` 執行:

```bash
NM=~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm
$NM --defined-only firmware.elf | grep -icE "smb2"                 # 211
$NM --defined-only firmware.elf | grep -icE " (wolfSSL|wolfTLS|wc_)" # 204
$NM --defined-only firmware.elf | grep -icE "NimBLE|ble_gap|BleRemote" # 0
```

各套件的授權宣告出處:

- `lib/smb2/COPYING`、各 `.c` 檔頭的 boilerplate
- `lib/expat/*.h` 檔頭、`lib/miniz/third_party/miniz.c` 檔頭
- `.pio/libdeps/<env>/<lib>/library.json` 或 `library.properties` 的 `license` 欄位
- `~/.platformio/packages/framework-arduinoespressif32/package.json`(`LGPL-2.1-or-later`)
- `~/.platformio/packages/framework-espidf/package.json`(`Apache-2.0`)+ `LICENSE`
- 字型:`name` 表第 13 欄(License Description)與第 14 欄(License URL),
  可用 `fontTools.ttLib` 讀出;另有隨附的 `OFL.txt` / `UFL.txt`

---

*維護:CrossMosa contributors*
