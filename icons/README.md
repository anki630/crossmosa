# 圖示來源(v113 全機圖示換裝)

這裡放的是 `src/components/icons/*.h` 的**來源 SVG**。那些 .h 是產生物,
要改圖示請改這裡再重跑下面的指令,不要手改 .h 的十六進位陣列。

- **上游**:[Tabler Icons](https://github.com/tabler/tabler-icons) **v3.31.0**
- **授權**:MIT(全文見 `LICENSE-tabler.txt`,Copyright (c) 2020-2026 Paweł Kuna)
- **例外**:`cover-crossmosa.svg` 是**本專案自製**的品牌位圖示(Tabler `book-2`
  的書封輪廓 + Tabler `moon`(filled)縮小嵌進封面),不是上游檔案。

**選圖原則**:四個位置(`recent` / `transfer` / `settings2` / `hotspot`)在定案時
特意挑了**貼近換裝前既有隱喻**的 Tabler 對應(筆記本+書帶 / 紙飛機 / 直立滑桿 /
發射點),而不是換一套新語彙——換的是**線條品質與一致性**,不是使用者要重新學的符號。
`hotspot` 選 `access-point`(左右對稱雙弧)也讓它與 `wifi`(單向扇形)在 32px 下
明顯可辨,這兩顆在主畫面上會相鄰出現。

---

## 對應表(slot → 來源 → 尺寸 → 線寬)

| 目標 .h | 陣列名 | 來源 SVG | 尺寸 | 線寬 |
|---|---|---|---|---|
| `recent.h` | `RecentIcon` | `notebook.svg` | 32 | 2 → **1.5** |
| `folder.h` | `FolderIcon` | `folder.svg` | 32 | 2 → **1.5** |
| `library.h` | `LibraryIcon` | `books.svg` | 32 | 2 → **1.5** |
| `transfer.h` | `TransferIcon` | `send.svg` | 32 | 2 → **1.5** |
| `settings2.h` | `Settings2Icon` | `adjustments.svg` | 32 | 2 → **1.5** |
| `book.h` | `BookIcon` | `book-2.svg` | 32 | 2 → **1.5** |
| `bookmark.h` | `BookmarkIcon` | `bookmark-filled.svg` | 32 | —(實心,無描邊) |
| `wifi.h` | `WifiIcon` | `wifi.svg` | 32 | 2 → **1.5** |
| `hotspot.h` | `HotspotIcon` | `access-point.svg` | 32 | 2 → **1.5** |
| `cover.h` | `CoverIcon` | `cover-crossmosa.svg` | 32 | 2 → **1.5**(書封描邊;月牙是實心不受影響) |
| `book24.h` | `Book24Icon` | `book-2.svg` | 24 | 2(原樣) |
| `file24.h` | `File24Icon` | `file.svg` | 24 | 2(原樣) |
| `folder24.h` | `Folder24Icon` | `folder.svg` | 24 | 2(原樣) |
| `image24.h` | `Image24Icon` | `photo.svg` | 24 | 2(原樣) |
| `text24.h` | `Text24Icon` | `file-text.svg` | 24 | 2(原樣) |

---

## ⚠️ 線寬規則(為什麼 32px 要改 1.5,24px 不改)

Tabler 的原生格線是 **24×24、線寬 2**。原本那套圖示的視覺語言是
**「32px 下看起來 2px 寬」**,所以:

- **算到 32px**:24 的格線被放大 **32/24 = 4/3 倍** → 線寬 `1.5 × 4/3 = 2.0px` ✅
- **算到 24px**:1:1 不縮放 → 原生的 **2 就已經是 2px**,不要改 ✅

也就是說「32 用 1.5、24 用 2」不是兩套審美,是**同一個 2px 的螢幕線寬**在兩種
縮放比下的解。日後新增圖示照這條算,不要憑感覺挑。

線寬覆寫的做法是對 SVG 開一份暫時副本做字串取代(**不改本目錄的原始檔**):

```bash
sed 's/stroke-width="2"/stroke-width="1.5"/' <來源>.svg > /tmp/w15/<來源>.svg
```

`bookmark-filled.svg` 根本沒有 `stroke-width` 屬性(它是 fill-only),所以不套用。
`cover-crossmosa.svg` 的月牙包在 `<g stroke="none">` 裡且是 `fill="currentColor"`,
所以那個 sed 只會動到書封的描邊,月牙不受影響——這是刻意的。

---

## 重新產生(regen)

轉換器是 `scripts/convert_icon.py`(cairosvg 算圖 → **逆時針轉 90°**(面板原生
儲存方向,**不可略過**)→ 128 門檻二值化 → 寫出 `src/components/icons/<name>.h`,
陣列名 = `<Name>Icon`)。

```bash
# 從 repo 根目錄執行。需要 cairosvg + Pillow(pip install cairosvg pillow)。
PY=python3          # 或你的 venv,例如 PY=.venv/bin/python3
SRC=icons           # 這個目錄
W15=$(mktemp -d)

# 線寬 1.5 的暫時副本(只給 32px 用)
for f in notebook folder books send adjustments book-2 wifi access-point cover-crossmosa; do
  sed 's/stroke-width="2"/stroke-width="1.5"/' $SRC/$f.svg > $W15/$f.svg
done

# 32px(線寬 1.5)
$PY scripts/convert_icon.py $W15/notebook.svg        recent    32 32
$PY scripts/convert_icon.py $W15/folder.svg          folder    32 32
$PY scripts/convert_icon.py $W15/books.svg           library   32 32
$PY scripts/convert_icon.py $W15/send.svg            transfer  32 32
$PY scripts/convert_icon.py $W15/adjustments.svg     settings2 32 32
$PY scripts/convert_icon.py $W15/book-2.svg          book      32 32
$PY scripts/convert_icon.py $W15/wifi.svg            wifi      32 32
$PY scripts/convert_icon.py $W15/access-point.svg    hotspot   32 32
$PY scripts/convert_icon.py $W15/cover-crossmosa.svg cover     32 32

# 32px 實心(不改線寬)
$PY scripts/convert_icon.py $SRC/bookmark-filled.svg bookmark  32 32

# 24px(原生線寬 2)
$PY scripts/convert_icon.py $SRC/book-2.svg    book24   24 24
$PY scripts/convert_icon.py $SRC/file.svg      file24   24 24
$PY scripts/convert_icon.py $SRC/folder.svg    folder24 24 24
$PY scripts/convert_icon.py $SRC/photo.svg     image24  24 24
$PY scripts/convert_icon.py $SRC/file-text.svg text24   24 24
```

### ⚠️⚠️ 重跑 `bookmark.h` 之後一定要手動補回第二個陣列

`bookmark.h` 裡有**兩個**陣列,而轉換器只會寫第一個、**整檔覆蓋**:

1. `BookmarkIcon`(32×32)— 轉換器產生
2. `BookmarkStatusIcon`(16×16)— **手寫的**,狀態列的書籤標記,
   被 `src/components/themes/BaseTheme.cpp:38` 逐位元組索引

跑完 `convert_icon.py ... bookmark 32 32` 之後,`BookmarkStatusIcon` 會消失,
build 會炸在 BaseTheme.cpp。**必須把它接回檔尾**:

```c
// size: 16x16
static const uint8_t BookmarkStatusIcon[] = {0x0F, 0xF8, 0x0F, 0xF8, 0x0F, 0xF8, 0x0F, 0xF8, 0x0F, 0xF8, 0x0F,
                                             0xF8, 0x0F, 0xF8, 0x0F, 0xF8, 0x0F, 0xF8, 0x0F, 0xF8, 0x0F, 0xF8,
                                             0x0F, 0x78, 0x0E, 0x38, 0x0C, 0x18, 0x08, 0x08, 0x00, 0x00};
```

它跟這次換裝無關、內容未變動,只是被覆蓋掉而已。

---

## 轉換器的一個修正(v113 一併改掉)

`convert_icon.py` 的 **SVG 分支從來沒有把 alpha 壓平到白底**(PNG 分支一直有)。
cairosvg 對沒畫到的地方輸出的是**透明** `(0,0,0,0)`,而 `RGBA → L` 會**丟掉 alpha**
→ 整片背景變成 `0x00` = **墨**,結果是整顆圖示變成實心黑塊。

v113 把壓平那三行移到兩個分支**共用**的位置。舊圖示看起來沒事,是因為它們當初是
從 **PNG** 轉的(走的是有壓平的那條分支)。

**驗收方式**:產生後看 .h 的第一個位元組——應該是 `0xFF`(白底)。
如果是 `0x00` 就是這個 bug 又回來了。

---

## 驗證產出(強烈建議)

換完之後不要只看 build 過不過。位元組打包的方向錯了照樣編得過,只是刷上去
圖示是躺著的。把 .h 解回圖片(逆著存:順時針轉回 90°)排成一張放大圖,肉眼看
方向、有沒有斷筆、有沒有被切邊。

一個好用的對照法:**拿舊的 .h 用同一支腳本跑一次**——舊圖示如果也是正立的,
就證明你的解包方向跟裝置的儲存慣例一致,而不是你剛好把兩個錯誤湊成對。
