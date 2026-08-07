# fonts/ — 內建 UI 字型的字集與重產工具

`lib/EpdFont/builtinFonts/ubuntu_1{0,4}_{regular,bold}.h` 這四個字面是**產生物**。
它們涵蓋哪些字,由這個目錄裡的字集檔決定。要改字集就改這裡再重產,
**不要手改 .h 的十六進位陣列**。

> UI 字型編進韌體,**只能重刷韌體才會變**;SD 卡上的 `/.fonts/` 只影響書的內文。

---

## 這個目錄有什麼

| 路徑 | 是什麼 |
|---|---|
| `charsets/charset-ui-v5.txt` | **現行 UI 字集**(7,683 碼位 / 7,413 漢字)。檔頭寫著完整出處、選字規則與已知缺口 |
| `charsets/charset-ui-v4.txt` | v5 的前身(5,683 碼位 / 5,413 漢字)。v5 逐字包含它,所以 `diff` 兩者剛好是「新增的 2,000 字」 |
| `charsets/iansui-charset.txt` | SD 卡「硬筆楷書」(芫荽 Iansui)`.cpfont` 的字集(5,366 常用漢字)。跟 UI 字型無關,收在這裡是為了讓那顆 SD 字型也可重現 |
| `pick-big5-l2-chars.py` | 選字程式:從 BIG5 次常用字挑出最高頻、尚未涵蓋的漢字。`charset-ui-v5.txt` 末行的 2,000 字就是它的產出 |
| `regen-ui-fonts.sh` | 重產四個字面的腳本 |
| `ui-subset/` | 三層來源子集(見下)+ 授權全文 |

### ⚠️ 字集檔的 ASCII 檔頭規則

`charset-ui-v5.txt` **整個檔案(含檔頭)**都會餵給 `pyftsubset --text-file`。
可列印 ASCII 本來就在字集內,所以 ASCII 檔頭不花錢;
但**檔頭裡一旦出現任何 CJK 字元,它就會無聲地進到字型裡**。
`regen-ui-fonts.sh` 的第 2 階段會斷言最終 cmap,那才是抓得到這種事的地方。

`charset-ui-v4.txt` 與 `iansui-charset.txt` 沒有檔頭,整個檔案就是字元本身。

---

## 三層疊加

每個字面都是多個來源字型疊出來的(順序即優先序,前者先取):

```
Ubuntu(拉丁)→ NotoSansHebrew → NotoSansArabic → Ubuntu-Vietnamese
  → UI-NotoSans(主漢字層)→ UI-CJKextra → UI-Symextra → NotoSans(U+FFFD)
```

前四層與最後一層的來源字型在 `lib/EpdFont/builtinFonts/source/`(已收進 repo)。
中間三層是子集,收在 `ui-subset/`:

| 檔案 | 從哪裡子集出來 | 為什麼需要 |
|---|---|---|
| `UI-NotoSans-{Regular,Bold}.otf` | Noto Sans TC | 主漢字層,由 `charset-ui-v5.txt` 決定 |
| `UI-CJKextra-{Regular,Bold}.otf` | Noto Sans CJK TC 的 face[3] | 357 個 Noto Sans TC 沒有的字(日文變體、注音、假名、符號) |
| `UI-Symextra-{Regular,Bold}.ttf` | DejaVu Sans | 三個符號 `∎ ► ▾`,兩顆 Noto 都沒有 |

**這三層收進 repo,是為了讓 `.h` 不必下載完整的 Noto 就能重產。**
完整的 Noto Sans TC 一顆約 16 MB,收進 repo 不合理;子集加起來不到 4 MB。

---

## 重產

從 **repo 根目錄**執行。

```bash
# 準備工具(fontTools 附帶 pyftsubset;freetype-py 是 fontconvert.py 要的)
python3 -m venv .venv
.venv/bin/pip install -r lib/EpdFont/scripts/requirements.txt

# A) 只重建四個 .h(不需要下載 Noto,用 repo 裡的子集)
PYTHON=.venv/bin/python3 ./fonts/regen-ui-fonts.sh --faces-only

# B) 改了字集之後的完整重產(需要完整的 Noto Sans TC)
#    到 https://github.com/notofonts/noto-cjk/releases 抓 NotoSansTC-Regular.otf
#    與 NotoSansTC-Bold.otf,放同一個目錄:
PYTHON=.venv/bin/python3 PYFTSUBSET=.venv/bin/pyftsubset \
  NOTO_DIR=/path/to/noto ./fonts/regen-ui-fonts.sh
```

改完之後重新編譯韌體並重刷,新字才會出現。

**驗收**:`--faces-only` 在未改任何東西時,應該產出與 repo 裡**逐位元組相同**的
四個 `.h`(每個字面的 `fontconvert.py` 完整參數就記在該 `.h` 自己的
「Command used:」檔頭裡,腳本是從那裡撈出來重跑的)。

### ⚠️ 兩個踩過的坑

1. **charset 列了字 ≠ 字型認得**。`fontconvert.py` 只 export
   `--additional-intervals` 涵蓋到的碼位;字集檔列了但區間沒涵蓋的字**不會**進字型。
   要找缺字請對「字型實際的 interval 表」掃,不要對 charset 檔掃。
   `pick-big5-l2-chars.py` 因此直接解析已產好的 `.h`,而不是讀 charset。
2. **絕對不要在 `--unicodes` 加整段 `4E00-9FFF`**。那會把兩萬個漢字全灌回來,
   韌體就塞不進 flash 了。`regen-ui-fonts.sh` 的檔頭有完整說明。

### 選字程式

```bash
# 需要三份不在 repo 裡的外部資料,見腳本檔頭的 URL
.venv/bin/python3 fonts/pick-big5-l2-chars.py \
    sorted.txt hanzi-chars/data-charlist out.txt --noto-dir /path/to/noto
```

⚠️ 它必須在**重產字型之前**跑:它的「目前涵蓋了什麼」是從
`lib/EpdFont/builtinFonts/ubuntu_14_regular.h` 讀出來的,字型一重產,基準就變了。

---

## 授權

| 檔案 | 授權 | 著作權 |
|---|---|---|
| `ui-subset/UI-NotoSans-*.otf` | **SIL OFL 1.1**(`ui-subset/OFL.txt`) | © 2014-2021 Adobe(Noto Sans TC) |
| `ui-subset/UI-CJKextra-*.otf` | **SIL OFL 1.1**(`ui-subset/OFL.txt`) | © 2014-2021 Adobe(Noto Sans CJK TC) |
| `ui-subset/UI-Symextra-*.ttf` | **Bitstream Vera / DejaVu 授權**(`ui-subset/LICENSE-DejaVu.txt`) | © 2003 Bitstream, Inc.;© 2006 Tavmjong Bah |
| `charsets/*.txt`、兩支腳本 | 與本 repo 相同(MIT) | — |

這六顆字型都是**子集**(衍生物)。子集過程會丟掉字型 name 表裡的授權欄位
(nameID 13/14),所以授權全文以本目錄的獨立檔案隨附 —— OFL 1.1 第 2 條允許
以獨立文字檔的形式附帶。Noto 系列的 OFL **沒有宣告 Reserved Font Name**,
所以子集沿用原家族名是允許的。

`ui-subset/OFL.txt` 的授權本文取自 repo 內既有的
`lib/EpdFont/builtinFonts/source/NotoSans/OFL.txt`(同一份 OFL 1.1 標準文本),
只把開頭的著作權行換成 Noto CJK 的。
`ui-subset/LICENSE-DejaVu.txt` 是 Debian `fonts-dejavu-core` 套件的
`copyright` 檔逐字複製。

字型本身**不是**本專案的作品;完整的第三方元件清單見 repo 根目錄的 `NOTICE`。
