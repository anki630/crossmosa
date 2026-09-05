![CrossMosa — 台灣黑熊與月牙](docs/promo/logo.png)

# CrossMosa

**終於，你的 X3 能好好讀中文了。**

給 Xteink X3 的繁體中文系統——免費、開源，刷一次機就有。

**快速前往:**
[**這一版更新了什麼**](CHANGELOG.md) ·
[安裝](#安裝) ·
[主要特色](#主要特色相對原版) ·
[字集限制](#ui-字型的字集限制請先讀這段) ·
[螢幕停住了？](#螢幕停住了救援步驟) ·
[與原版的差異](#與原版-crosspoint-的關係) ·
[下載 2.0](https://github.com/anki630/crossmosa/releases/latest) ·
[English](#crossmosa-english)

![CrossMosa 實機照:全繁中主畫面、明體內文、名畫待機](docs/promo/hero-photo.jpg)

你的 X3，書名還是一排 □□□ 嗎?

CrossMosa 只做一件事:**讓 X3 把繁體中文書讀好。**

- **書名、選單、目錄，說中文**——內建 7,413 個漢字（BIG5 一級全收+二級常用 2,000 字），連「憨」「璐」這類冷僻字都有。
- **內文想用什麼字體就用什麼**——明體、黑體、硬筆楷書三套中文字型，各四個字級。
- **翻頁更快了**——下一頁的字趁你還在讀時先備好;備妥的頁面快 0.3 秒，沒備妥也不會更慢。
- **讀到最精彩的一章，不會突然重開機**——排版、圖片、字型快取全都有記憶體不足時的退路。
- **闔上機器，它是一幅畫**——50 張世界名畫待機壁紙，每一張都為這塊 4 階灰階螢幕挑過、裁過、調過;今天梵谷，明天北齋。

安裝就三步:刷韌體 → 複製字型 → 開機切中文。今晚就能開始讀。
刷壞了也有退路:SD 卡救援模式隨時能換回任何韌體;首刷機器不用接電腦（SD 卡就能刷，見安裝章方法 A）。

這是個人專案，免費開源，不是產品——但上面每一項，都在真機上量測過。

---

## 這些痛，你遇過幾個?

用電子書機讀中文，大概都撞過這幾件事:

- 書名在檔案清單裡是一排方塊，要一本本點開用猜的。
- 某些書就是打不開;或讀到一半，整台機器突然重開。
- 連上 Wi-Fi 想抓書，機器直接當掉。
- 長篇小說讀到後半，翻頁越來越卡。
- 選單是英文或簡體;內文沒有台灣讀者習慣的明體。

這五條，CrossMosa 各有一個具體的修法，而且都在真機上驗證過:內建 7,413 個漢字（方塊）、
排版與記憶體護欄（打不開/突然重開）、連線前自動騰出記憶體（Wi-Fi 當機）、
背景預先排版（後段卡頓）、繁中介面+三套中文字型（語言與字型）。
細節在下面的特色表;每一項的來龍去脈都在 [CHANGELOG](CHANGELOG.md)。

另外幾樣，是我們所知目前其他 X3 系統上沒有的:
**翻頁字型預取**（備妥的頁面快 0.3 秒）、**50 張名畫待機策展**、**逐位元組可重現的建置**
（任何人都能驗證發佈的韌體真的出自這份源碼），以及一個**連官方框架都還沒修的
Wi-Fi 連線當機**，這裡先修掉了（細節見 CHANGELOG 的 1.0.0 節）。

---

## 這是什麼

版本:[`2.0.0`](https://github.com/anki630/crossmosa/releases/latest)

**我該刷哪一版？**

| | |
|---|---|
| **第一次刷，或不知道該選哪一版** | **刷 2.0.0。** 它會自己認出你的螢幕是哪一種，新舊機器都認得。刷舊版才要碰運氣——比較新的 X3 刷上去，畫面就不會再更新 |
| **已經在用 1.0／1.1／1.2** | 建議升：換一章從十秒左右變成一兩秒，翻頁更順，書裡的圖也更少出不來，冷僻字不再變黑框 |

> **2.0 做了什麼**：新一批次 X3 換了面板控制器 **UC8279**，舊韌體刷上去畫面不會更新——
> 這一版會在開機自動辨識控制器並驅動它，新舊批次都認得。基底對齊 **CrossPoint 1.5.0**。
> 另外換章從十秒級降到一兩秒、翻頁更順、清除快取可以保留閱讀進度。
> 完整說明見 [CHANGELOG](CHANGELOG.md)。

CrossMosa 是原版 [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader)（本分支的來源專案，開發圈慣稱 upstream）的繁體中文分支。
原版是通用的開源電子書系統，支援兩種機型、二十幾種介面語言、多種檔案格式。
CrossMosa 把範圍收窄，專心做三件事:

1. **介面與內文都是繁體中文**——選單、檔名、書名、OPDS 書庫、書的內文。
2. **閱讀優先**。這台機器只有 320 KB RAM，任何功能都在跟閱讀搶記憶體。
   凡是會讓翻頁掉字、讓長章節排不出來的東西，一律讓路（見「與原版的差異」）。
3. **只針對 X3 調校**。原版同時支援 X3 與 X4;本分支的顯示波形時間、記憶體預算、
   字型尺寸全部照 X3 的 792×528 面板與這顆 ESP32-C3 實測而定。
   X3 有兩種螢幕驅動晶片：**UC8253**（較早的機器）與 **UC8279**（較新的機器）。
   本版開機時會自己認出是哪一種，**兩種都已經在實機上刷過**。

這是個人專案，不是產品。**沒有任何隸屬於 Xteink 或原版 CrossPoint 專案的關係。**

---

## 主要特色（相對原版）

| | 內容 |
|---|---|
| **全繁中介面** | 446 個 UI 字串完整翻譯，台灣用語;經過一輪逐字串對照實際畫面的情境複查 |
| **內建 UI 字型含漢字** | BIG5 一級全部 + BIG5 二級常用 2,000 字 = **7,413 個漢字**，檔名 / 書名 / 選單不再是方塊 |
| **SD 卡中文內文字型** | 黑體 (Noto Sans TC) / 明體 (Noto Serif TC) / 硬筆楷書 （芫荽 Iansui），各四個字級 16/18/20/22 |
| **字型預取** | 下一頁要用的字趁你還在讀這一頁的空檔先從 SD 讀好，翻頁中位數 1,278 ms → 952 ms（省約 330 ms） |
| **CJK 排版** | 逐字斷行、中文行距校正、重組文字時不插多餘空格 |
| **記憶體護欄群** | 章節排版、斷字、圖片解碼、字型快取、TLS 握手全部有記憶體不足時的降級路徑，不再直接重開機 |
| **OPDS 中文書庫** | 已造訪頁存 SD 的上/下一頁堆疊、返回保留游標位置、長按翻頁一次一動作、下載完成直接開書 |
| **圖片與灰階加速** | 有圖的書頁整頁 5,849 ms → 2,422 ms;抗鋸齒的兩個灰階平面合併成單趟走訪 |
| **網頁傳檔** | 瀏覽器上傳書 / 字型 / 管理 SD 卡，不必拔卡(`http://crossmosa.local`) |
| **介面重整** | 可選主題：**Formosa**、**Formosa Extended**、**Formosa Pro**（Pro 為這一版新增）。圓角外框 + 左側豎條的統一選取語言、真頁籤形分頁列、e-ink 上不塗滿背景 |

![OPDS 中文書庫](docs/promo/photo-opds.jpg)

其他:QR 書摘（修正過容量計算，掃得出來）、書籤與註腳、每頁停留時間量測、
可用哨兵檔開關的裝置端診斷紀錄（這台機器沒有序列埠）。

---

## ☕ 覺得好用的話

CrossMosa 是下班後的個人專案。如果它讓你的 X3 變好用了，幾種讓我開心的方式:

- 到 [Discussions](../../discussions) 留句話，說說你拿它讀了什麼書——**這是我最想看的**。
- 推薦給也有 X3 的朋友。
- 請我喝杯咖啡（連結籌備中）——不影響任何功能，純粹讓下一個版本寫得更有勁。

回報缺字或問題，一樣歡迎開 Issue。

---

## 安裝

以下把這套系統（技術上叫「韌體」）刷進機器。

<a id="flash-warning"></a>

### ⚠️ 先讀這段

**新一批的 X3 換了螢幕驅動晶片。** 出廠時面板控制器從 **UC8253** 換成 **UC8279**。1.x 不認得它——
刷完之後畫面就不再更新，可能停在「更新已完成」一個像素都不換，但空白 SD 卡放進去仍會出現資料目錄：
機器還在跑，只是舊韌體不會驅動新控制器。上游在
[#2707](https://github.com/crosspoint-reader/crosspoint-reader/pull/2707) 加入偵測、收在 1.5.0，本版包含它。

**如果你想試試，很歡迎。** 換章從十秒級降到一兩秒、翻頁更順、圖片顯示更穩、清除快取可以保留閱讀進度——
這一版累積的改進不少。它是預發布版，所以請先讀完這一段再開始；除此之外，想嚐鮮就試吧。

**但仍然不保證每一台都成功。** 面板控制器是已知的原因之一，不是唯一的。
**e-ink 會保留殘影，所以「畫面上有東西」不代表機器還活著**；反過來，韌體也可能正常執行，
只是面板收不到命令。要判斷機器是死是活，**看 SD 卡上檔案的時間戳，不要看螢幕**。
上游還有一筆同類的未解回報：[#2183](https://github.com/crosspoint-reader/crosspoint-reader/issues/2183)。

**開始之前，先做這三件事**

1. **把 SD 卡上的 `/.crossmosa/` 整個資料夾備份到電腦。** 設定、Wi-Fi 憑證、OPDS 設定，以及
   每一本書的閱讀進度都在裡面——重刷韌體救不回來。（下面的救援步驟**不需要**格式化 SD 卡，
   但社群另一版的做法會要求格式化，備份起來比較安心。）
2. **SD 卡根目錄只留一個 `.bin` 檔。** 救援時螢幕可能完全沒有畫面，你會看不到自己選了什麼。
3. **最壞的情況是機器救不回來。** 已經有使用者的機器變成磚，**照著救援程序也沒救回來**。
   救援是一條可能有用的路，不是保險。刷之前先假設這台機器可能就這樣沒了，你仍然願意，再開始。

> 已經刷了 1.x、**畫面停住不動**的：[救援步驟在這裡](#螢幕停住了救援步驟)。順的話約五到六分鐘。

### ⚠️ 一定要做兩件事，少做一件，中文書就是滿頁方塊

刷韌體**只解決介面**。**書的內文字型不在韌體裡**，它在 SD 卡上。

韌體內建的閱讀備援字型**只有拉丁文**——沒有複製 SD 字型的話，
選單是正常中文，但**打開任何中文書，內文會整頁都是方塊(□□□□)**。這不是壞掉，是缺字型。

| 步驟 | 檔案 | 去哪 |
|---|---|---|
| **1. 刷韌體** | `crossmosa-2.0.0-firmware.zip` | 裝置的 flash |
| **2. 複製字型** | `crossmosa-2.0.0-sd-fonts.zip` | SD 卡的 `/.fonts/` |

兩個檔案都在同一個 [Release](../../releases) 頁面。

### 步驟 1:刷韌體（首次安裝）

**方法 A — SD 卡首刷（推薦:機器不用接電腦）**

原廠韌體自帶 SD 更新模式。你只需要有辦法把一個檔案放進 SD 卡
（電腦+讀卡機、或手機+轉接頭都行），機器本身從頭到尾不用接任何東西:

1. 把 zip 裡的 `update.bin` 複製到 SD 卡**根目錄**(檔名已預先改好——
   這顆就是其他教學裡說要改名的 firmware.bin;注意瀏覽器重複下載會變
   `update (1).bin`，那樣不行)。
2. 關機 → **按住左側「上一頁」鍵 + 電源鍵**，看到載入畫面就放手。
3. 等它刷完自己開機，約五分鐘。**刷完建議把 `update.bin` 從卡上刪掉**
   （避免日後誤刷舊版）。失敗的話長按電源 5–10 秒強制重開，
   重新下載檔案再試（多半是檔案沒抓完整）。

這條路是 **X3 限定**（X4 原廠韌體沒有這個組合鍵），而且**不需要電腦偵測得到機器**——
線材、Hub、驅動有問題、甚至 USB 被鎖，都不影響。社群文件記載它**連 USB-locked
的機器也適用**。維護者自己的第一次就是這樣刷的（Mac 的 Hub 一直偵測不到機器）。

**方法 B — 網頁 flasher（USB 偵測得到的話）**

1. USB-C 接電腦，喚醒裝置。
2. 開 https://crosspointreader.com/#flash-tools,選 **X3**，點 **Custom .bin**,
   上傳 zip 裡的 `update.bin`。
3. 瀏覽器的序列裝置選單看不到機器?換一個 USB 埠、不要經過 Hub、換一個支援
   WebSerial 的瀏覽器(Chrome/Edge)。還是不行就回方法 A，不用糾結。

**方法 C — 命令列**（進階）

```bash
pip install esptool
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
           write_flash 0x10000 update.bin
```

> zip 裡另附 `bootloader.bin` 與 `partitions.bin`，只有在做完整重刷（0x0 起）時才需要;
> 一般更新只要 `update.bin`。

> **關於 USB-locked 機器**:部分第三方通路（例如 AliExpress）的機器出廠鎖住 USB 燒錄，
> 直接向 xteink.com 買的沒有鎖。**方法 A 不受鎖定影響**。上游的警告仍然算數:
> **不要用 Xteink Unlocker 來刷 CrossMosa**(該工具官方只支援 CrossPoint 與 CrossInk，
> 刷其他韌體有變磚風險)。退路:CrossMosa 保留完整的 SD 救援模式（見「日後更新」），
> 但別把它當成保證:已有使用者照著救援程序，機器仍然沒回來。
> 已知會讓救援失效的情況至少有兩種，我 2026-08 兩種都踩到了:
> ①救援模式需要**以電源鍵喚醒**才會觸發，所以**韌體一旦卡在開機迴圈就進不去**
> （重置迴圈的喚醒原因不是電源鍵）;那種情況要先讓**電池完全放光**打斷迴圈才有機會。
> ②**部分 X3 的 USB 只有充電、沒有資料傳輸**，那種機器上方法 B 與方法 C 完全不可用。
> 除此之外還有目前無法解釋的失敗案例。**沒有任何一條路能保證把機器救回來。**
> 上游完整原文:[`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md) "USB-locked devices"。

### 步驟 2:複製 SD 卡字型

解開 `crossmosa-2.0.0-sd-fonts.zip`，把**整個字型資料夾**複製到 SD 卡的 `/.fonts/` 底下:

```
SD 卡根目錄
└── .fonts/
    ├── NotoSerifTC/          ← 明體(建議先裝這套)
    │   ├── NotoSerifTC_16.cpfont
    │   ├── NotoSerifTC_18.cpfont
    │   ├── NotoSerifTC_20.cpfont
    │   └── NotoSerifTC_22.cpfont
    ├── NotoSansTC/           ← 黑體
    ├── Iansui/               ← 硬筆楷書
    └── GuanKiapTsingKhai-90/ ← 楷書·直排用
```

| 字型 | 風格 | 漢字涵蓋 | 大小 | 說明 |
|---|---|---|---|---|
| **NotoSerifTC** | 明體 | 20,992（完整） | 65 MB | 建議先裝這套 |
| **NotoSansTC** | 黑體 | 20,976（完整） | 64 MB | 有粗體，1-bit 下筆畫最穩 |
| **Iansui** 芫荽 | 硬筆楷書 | 20,992（完整） | 31 MB | 只有 Regular（粗體會退回一般字） |
| **GuanKiapTsingKhai-90** 原俠正楷 | 楷書·**偽直排** | 20,992（完整） | 30 MB | 字形預先轉了 90 度，見下 |
| IBMPlexSansTC | 黑體 | 20,976（完整） | 62 MB | 保留的舊選項 |

**2026-08-31 起五套都是完整漢字涵蓋**——台語文、古文、人名的冷僻字以前會變黑框，現在都有字。
（做法是替每套掛一個同風格的備援字型來補洞，不是把字裁掉。）

**只裝一套也可以**——空間有限就先裝 `NotoSerifTC`。五套全裝約 252 MB。
不影響 RAM:字型是按需從 SD 讀的，不會整份載進記憶體。

> **想直排讀中文**:選 `GuanKiapTsingKhai-90`，再把螢幕轉成橫向，
> 中文就會由上而下、由右而左排列。它的字形是**預先轉了 90 度**的，
> 所以正常橫排時選它會整頁躺著——只在要直排時用。

**SD 卡要求**:FAT32 或 exFAT。**字型資料夾名稱不可以有空格**（原版已知會 crash，用底線）。

**順手做的步驟 2.5**:firmware zip 裡有一本《歡迎使用 CrossMosa》，把它一起複製進 SD 卡。
十三章、約十分鐘，每一章結尾都叫你按一顆鍵，讀完這台機器就會用了（含電源鍵的五種本事與救援刷機）——刷完之後第一本就讀它。

![X3 正在讀《歡迎使用 CrossMosa》](docs/promo/photo-guide.jpg)

### 疑難排解:按了組合鍵，出現「更新中」，半分鐘後退回、沒有更新

代表更新器有啟動，是後半段沒過。依序檢查:

1. **檔案大小是否恰為 6,145,952 bytes、sha256 是否為 ef17c9dfab7d04914fa3c21b9b351c41bb134e5062171367af31f45efe678399**（v2.0.0）——九成的問題在這:
   下載不完整、瀏覽器存成 `update (1).bin`、Windows 隱藏副檔名變成
   `update.bin.bin`、誤放整個 zip 沒解壓，**或 SD 卡上殘留著一顆舊的
   `update.bin`**（更新器抓到的是舊檔——社群實例，換上正確的檔就成功了）。
   檔案要放在 SD 卡**最外層**;**刷完建議把它刪掉**，免得日後誤刷舊版。
2. 檔案正確仍失敗 → 接電腦走**網頁 flasher**（方法 B）。瀏覽器的序列裝置
   選單看不到機器，先換 USB 埠、不要經 Hub、換 Chrome/Edge。
3. 怎樣都看不到裝置 → 你的機器可能是**出廠鎖定批次**（部分第三方通路），
   連 SD 更新器都只收原廠簽章的映像。正規解法:用官方的
   [Xteink Unlocker](https://crosspointreader.com/unlock) 先裝上**官方 CrossPoint**,
   再用它的「Settings → SD Card Firmware Update」選本專案的 `update.bin` 換裝——
   CrossMosa 裝上後自帶 SD 救援模式，隨時能刷回官方 CrossPoint，退路完整。

遇到第 3 種情況，請順手回報你的原廠韌體版本號（開一張 issue 即可）——
我們在收集「哪些批次會擋 SD 首刷」的對照資料，幫到後面的人。

### 日後更新（已刷過 CrossMosa 之後）

從第二次起連讀卡機都可以免了。[Release](../../releases) 頁有**單獨一顆
`update.bin`**（跟 zip 裡同一顆，免解壓）——手機直接下載，開瀏覽器連上
機器的網頁傳輸上傳進 SD 卡（或照舊用讀卡機）→
**設定 → 系統 → SD 卡韌體更新** → 選檔案。韌體會先完整驗證映像檔才寫入，
比 USB 直刷更保險，USB 被鎖的機器也能用。
萬一哪天機器開不了機:關機 → 按住左側「上一頁」鍵 → 按電源，直接進同一個
SD 韌體選擇畫面（救援模式）——這條路只要機器上還是 CrossMosa 就永遠在。

### 步驟 3:第一次開機

1. **開機就是繁體中文**（要英文介面:**設定 → 系統 → 語言 → English**;從原版升級、之前選過英文的，設定會保留，同一路徑可切）。
2. **選內文字型**:**設定 → 閱讀器 → 閱讀字型**，選剛剛複製的那套。
   （沒看到就代表 SD 卡路徑不對，檢查是 `/.fonts/字型名/` 而不是 `/.fonts/`。）
3. 選字級:**設定 → 閱讀器 → 閱讀字級**（對應 SD 字型的 16/18/20/22）。
4. 裝置會在 SD 卡建 `/.crossmosa/` 放進度、書籤、Wi-Fi 憑證與快取。**不要刪它。**
5. 版號顯示在**開機畫面**與**設定頁**，確認是 `2.0.0`。

### 傳書進去

- **拔 SD 卡**直接複製（最穩，大檔尤其）。
- **網頁上傳**:主畫面 → 檔案傳輸 → 加入網路或開熱點 → 電腦瀏覽器開 `http://crossmosa.local`。
- **OPDS**:設定好書庫伺服器後從裝置上瀏覽下載。
- **Calibre 無線連線**:原版的流程原封保留。

> 書的來源:請使用正版取得的 EPUB。本專案不提供、也不代找書籍內容。

### 待機壁紙（選配）

[Release](../../releases) 另附壁紙包：**50 張世界名畫**，
全部取自 Wikimedia Commons 的公共領域作品，每一張都為 X3 這塊 4 階灰階面板挑過、裁過、調過。

把 `.bmp` 複製到 SD 卡的 `/.sleep/`（**放兩張以上才會輪播**），
然後 **設定 → 顯示 → 待機畫面 → 自訂**。

> ⚠️ SD 根目錄不要放單獨一個 `/sleep.bmp` —— 它會優先、固定顯示、不輪播。

轉檔工具、策展清單與「為什麼是這 50 張」都在 [`wallpapers/`](wallpapers/)，
可以自己換成任何圖片。

---

## UI 字型的字集限制（請先讀這段）

這是本分支最需要事先講清楚的取捨。

**內建 UI 字型涵蓋 7,413 個漢字**:BIG5 一級全部，加上 BIG5 二級裡最常用的 2,000 字。
**不是全部的中文字。** 完整的 BIG5 有 13,060 字，Unicode 的中日韓統一表意文字更多。

沒被涵蓋到的字，會在**選單、檔名、書名、OPDS 書目、章節目錄**顯示成方塊 □。

### 兩種會踩到的情況

1. **BIG5 二級的罕用字**——落在 2,000 名額之外的部分。
2. **BIG5 範圍外的字**。這是**已知的缺口**:很多台灣人名用字根本不在 BIG5 裡。
   最常見的三個（**喆、堃、彣**）已在 build 115 補上;其餘 BIG5 外的字仍缺——
   系統性補齊需要換一套候選字源（現在的候選池與字頻表都以 BIG5 為界），1.0 沒做。

### 書的內文不受影響

UI 字型與內文字型是**完全獨立的兩套**。書的內文走 SD 卡字型，
而 **2026-08-31 起五套 SD 字型都是完整漢字涵蓋**（約 21,000 字）——
台語文、古文、BIG5 外的人名用字，內文都有字。

也就是說:**書名在檔案清單上是方塊、打開之後內文正常**，是預期中的行為，不是 bug。
缺口只在 UI（檔名／選單／書名），內文沒有。

### 遇到方塊怎麼辦

**回報**:開一個 [Issue](../../issues)，標題寫「缺字」，內容貼上**那個字本身**
（直接打在 issue 裡就好）以及它出現的地方（選單 / 檔名 / 書名 / 內文）。
字集是可重現的資料檔，補字是例行維護。

**自己重產**:UI 字型的字集與選字工具都在這個 repo 裡，可完整重現。

| 東西 | 路徑 |
|---|---|
| 目前的 UI 字集（含完整出處與選字規則，寫在檔頭） | `fonts/charsets/charset-ui-v5.txt` |
| UI 字型重產腳本 | `fonts/regen-ui-fonts.sh` |
| 二級字選字程式（候選池 + 字頻排序） | `fonts/pick-big5-l2-chars.py` |
| SD 卡字型產生器（原版的，未修改） | `lib/EpdFont/scripts/fontconvert_sdcard.py` |

流程是:把字加進字集檔 → 跑重產腳本 → 重新編譯韌體 → 重刷。
**UI 字型無法用 SD 卡替換，只能重編韌體。**

> 字集檔的檔頭**必須全部是 ASCII**——整個檔案會餵給 `pyftsubset --text-file`，
> 檔頭裡的任何一個中文字都會悄悄進到字型裡。重產腳本有 cmap 斷言擋這件事。

### 為什麼不乾脆全部收進去

全 BIG5 加進 UI 字型大約要多 2 MB，而 app 分割區只有 6.5 MB，目前已經用掉 90.9%
（5,955,789 bytes，剩約 584 KB）。要放得下就得先重新分割 flash——有變磚風險。
現在這 7,413 字，就是塞得進去的最大值。

---

<a id="screen-halted"></a>

## 螢幕停住了？救援步驟

刷完 1.x 之後畫面就不動了的，**看不到畫面也能直接刷成 2.0**，不必先刷回原廠韌體。
順的話大約五到六分鐘。**全程看不到畫面，照秒數操作就好——寧可多等，不要提早按。**

**準備**：把韌體的 `.bin` 放進 SD 卡最外層，**只放一個**。資料夾不用管。

<img src="docs/img/button-map.png" width="340" alt="X3 按鍵編號">

| ① 左側邊 | ② 上緣左 | ③ 上緣右 | ④ 右側邊 | ⑥ 正面左2 |
|---|---|---|---|---|
| 上一頁 | 重置 | 電源 | 下一頁 | 確認 |

**只會用到 ① 和 ⑥。⑤⑦⑧（正面其餘三顆）一次都不要按。**

1. 按 **②**
2. 按住 **① ＋ ③** 四秒 → **先放 ③，再放 ①**
3. 等 **2 秒**
4. 按 **①**，再按 **⑥**
5. 等 **90 秒** → 再按一次 **① ⑥**
6. 等 **90 秒**
7. 按 **⑥**
8. **耐心等** —— 正在寫入，別碰機器、別拔卡。**成功的話它會自己開機**
9. 等很久還是沒動靜，就從第 1 步重來

**多試幾次。** 沒成功多半只是某一下沒按實——看不到畫面，按下去有沒有被機器收到，你完全感覺不出來。
重來個幾輪通常就過了。（開機了但畫面還是黑的，按 **②** 再長按 **③**。）

<details>
<summary>細節：為什麼這樣按、每一步在等什麼</summary>

**為什麼可以一直重複按 ① ⑥**：因為這兩顆不管機器停在哪一頁，都不會把事情弄糟。

| 當下畫面 | ① | ⑥ |
|---|---|---|
| 檔案清單 | 跳到你的 `.bin` | 選它 |
| 「要更新韌體嗎？」 | 沒有作用 | 確認 |
| 檢查中／寫入中 | 忽略 | 忽略 |

所以不必知道自己在第幾步，不確定就再按一輪。第 5、7 步就是這個道理。

**為什麼 `.bin` 只放一個**：救援畫面的清單只會列出 `.bin`，而且資料夾一定排在前面，
所以你那個 `.bin` 一定是最後一個。按 ① 會從第一個繞到最後一個，剛好就選到它。
卡上有好幾個 `.bin` 的話，選到的會是檔名排最後的那個。

**⛔ 為什麼不能按 ⑤⑦⑧**：在「要更新韌體嗎？」那個畫面上 ⑤⑦ 都是**取消**。
⑦ 最容易中招——它在清單那一頁螢幕上標的是「上」，做的事跟 ① 一模一樣；
可是到了問你要不要更新那一頁，它就變成取消，**而且螢幕上不會寫出來**。

**每一步在等什麼**
- 第 1 步：那組按鍵**只有在開機那一瞬間**才會被讀到。機器如果現在是「開著但沒畫面」，直接按沒有用。
- 第 3 步：你剛剛放開的 ①，機器也會算成按了一下。等兩秒讓它過去。
- 第 5 步：純粹是保險。第 4 步沒按實的話由它補上；已經成功的話這一輪會被忽略。兩種都沒差。
- 第 6 步：機器在檢查這個檔案有沒有壞掉，整個約 6 MB 都要讀一遍（實測 30–60 秒）。
  上面寫的等待時間都留了餘裕，**寧可多等**。
- 第 8 步：正在寫入（實測 60–90 秒），寫完會自己重開機。
  但那種重開**不會把螢幕晶片一起重來**，所以偶爾畫面還是黑的——按 ② 才會真的從頭開始。

**不知道自己走到哪了**：按 **③ ＋ ④** 拍一張截圖 —— 螢幕雖然沒更新，機器心裡還是知道
「現在該顯示什麼」。把卡拔到電腦上打開 `screenshot-*.bmp` 就看得到。
只有畫面停著不動的時候有效；第 6 步檢查中、第 8 步寫入中按了不會有反應。

**卡在 2.0 的話**（目前沒人遇到）步驟完全一樣，只是機器裡面走的路不同：
2.0 問你要不要更新那一頁有兩個選項、一開始停在「取消」上，① 會把它移到「確認」，⑥ 再按下去。

**其他版本的做法**：社群另有一版（組合鍵約 7 秒、先刷回原廠韌體）——
[CrossInk #479](https://github.com/uxjulia/CrossInk/discussions/479) ·
[本專案 issue #2](https://github.com/anki630/crossmosa/issues/2)（@sk5s 回報）。上面這套行不通時可以試。

**不保證每台都救得回來。** 已經有使用者照著做仍然沒救回。

</details>

## 自行建置

```bash
git submodule update --init --recursive --depth 1   # freeink-sdk 是 submodule,缺了會 link 失敗
pip install platformio
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)  # 見下方「可重現建置」
pio run -e gh_release                                # 產物在 .pio/build/gh_release/firmware.bin
```

### 可重現建置

**發佈的映像檔（`update.bin`，即建置產物 `firmware.bin` 改名）是逐位元組可重現的**——同一個 commit、同一組釘住版本的相依套件，
任何人都能建出 sha256 完全相同的檔案。條件只有一個:**必須設 `SOURCE_DATE_EPOCH`**。

不設的話，`__DATE__` / `__TIME__` 會把建置當下的時刻編進 binary(其中一處還在 Arduino
core 裡，不是本專案能改的)，兩次建置就會差幾十個位元組。設了之後 GCC 會用這個值取代那兩個
巨集，同時本專案的網頁資產壓縮也會用它當 gzip 的 mtime。

**每個 Release 都會公佈當次使用的 `SOURCE_DATE_EPOCH` 與 firmware 的 sha256。**
打包腳本 [`scripts/mk-release.sh`](scripts/mk-release.sh) 預設直接取 release commit 自己的
時間戳(`git log -1 --format=%ct`)，所以只要 checkout 同一個 tag 就會自動得到同一個值。
機制與判讀方式寫在 [`docs/reproducible-builds.md`](docs/reproducible-builds.md)。

---

## 與原版 CrossPoint 的關係

**CrossMosa 的一切都建立在 [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) 上面。**
閱讀引擎、EPUB 解析、排版、活動框架、網頁介面、OPDS、Calibre 流程——這些都是原版寫的，
本分支只是在上面做中文化與 X3 特化。

- 原版作者:**Dave Allie** 與 CrossPoint 貢獻者們。授權 MIT,`LICENSE` 原封保留。
- 原版的錯誤回報請發到[原版 repo](https://github.com/crosspoint-reader/crosspoint-reader/issues)，
  不要發到這裡;本 repo 只處理本分支自己改壞的東西。
- **想要完整功能的人應該用原版**，不是用這個分支。

### 與原版的差異

**已移除**（不是關閉，是程式碼層面拔掉入口讓連結器回收，換 flash 空間給中文字型）:

| 移除 | 原因 |
|---|---|
| English / 繁體中文以外的 **29 種 UI 語言** | 約 258 KB，換中文字型 |
| **KOReader 進度同步** | 沒有伺服器可同步 |
| **字典查詢**(StarDict) | 未使用 |
| **OTA 線上更新** | 會指向原版的 release 把本分支蓋掉;**SD 卡韌體更新保留** |
| **Classic / RoundedRaff 主題** | 字級與語系支援跟不上中文;留 Formosa、Formosa Extended 與 Formosa Pro |
| **非英文的斷字表**（9 種語言） | 中文不斷字，約 323 KB |
| **內建斜體字面** | 自動退回正體，約 544 KB |
| 內建閱讀字型縮成**單一 14px 備援** | 只在沒有 SD 字型時用得到，約 373 KB |
| **SMB2 伺服器**（iOS「檔案」App 直接管理 SD 卡） | 已移除。先前公開版預設就不編進發佈韌體；這一版連原始碼一併拿掉，無法再開編譯開關編回來。請改用網頁傳檔、Calibre、OPDS 或拔卡複製 |
| **BLE 翻頁遙控器** | 已移除。發佈韌體本來就沒有；這一版原始碼也不再保留，無法自編加回 |

**保留**:Calibre 無線推書（相容原版外掛生態）、網頁設定與傳檔、WebDAV、OPDS、
傾斜翻頁、螢幕截圖、按鍵重配、待機畫面。

---

## 免責聲明

- **本專案不提供任何書籍內容，也不內建任何書源。** 韌體與 Release 裡沒有書。
  請從正版管道取得電子書(無 DRM 的正版 EPUB:出版社或獨立書店直售、公共領域書庫、
  你自己的文件)，放進 SD 卡或自架書庫使用。請支持正版，尊重創作者。
- **刷機有風險，自負。** 刷第三方韌體可能讓裝置無法開機。**已經有實際變磚的案例，
  而且救援程序對部分機器無效——請假設有可能救不回來。**
  開始之前請先讀安裝章開頭的「刷機無法保證成功」與 USB-locked 注意事項:
  SD 救援模式在韌體卡住開機迴圈時進不去，部分機器的 USB 也沒有資料傳輸。
- **與 Xteink 無關，與原版 CrossPoint 專案也無隸屬關係。** 兩者都不為這個分支負責。
- **驗證主力是一台 UC8279 新批次 X3**，舊批次（UC8253）由使用者回報刷機成功。
  沒有 X4，沒有自動化的硬體測試。
  很多改動的驗證方式就是「用了幾天沒出事」。
- **沒有遙測。** 本韌體不會回報使用狀況給任何人。Wi-Fi 憑證、閱讀進度、書籤只存在你自己的
  SD 卡上(`/.crossmosa/`)。裝置只有在你主動要求時才連外:連 Wi-Fi 後對時(NTP)、
  你設定的 OPDS 伺服器、Calibre 無線連線。原版的 OTA 更新檢查已經移除，
  所以本韌體不會主動連任何本專案或原版的伺服器。
- 「AS IS」，無任何擔保，見 `LICENSE`。

---

## 授權

- 原版 CrossPoint:MIT,Copyright (c) 2025 Dave Allie（`LICENSE`，原封保留）。
- CrossMosa 的修改:MIT,Copyright (c) 2026 CrossMosa contributors。
- 內含的第三方程式庫與字型各有授權，**完整清單見 [`NOTICE.md`](NOTICE.md)**。
  ⚠️ 其中有 GPLv2 與 LGPL-2.1 的元件會連結進發佈的韌體 binary，
  請先讀 NOTICE 的「發佈義務」一節。

維護:**CrossMosa contributors**。

---
---

![CrossMosa — a Formosan black bear and a crescent moon](docs/promo/logo.png)

# CrossMosa (English)

**Jump to:**
[**What's new**](CHANGELOG.md) ·
[Install](#install--you-must-do-both-steps) ·
[Highlights](#highlights-vs-upstream) ·
[Character-set limits](#ui-character-set-limits-please-read) ·
[Screen frozen?](#screen-stopped-updating-rescue) ·
[Vs upstream](#relationship-to-upstream) ·
[Download](https://github.com/anki630/crossmosa/releases/latest)

**Finally, your X3 can read Traditional Chinese properly.**

**Traditional-Chinese-focused firmware for the Xteink X3 e-reader**, based on
[CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) 1.5.0.

![CrossMosa on real hardware: Traditional Chinese home menu, serif body text, masterpiece sleep screen](docs/promo/hero-photo.jpg)

Version: `2.0.0`

## What it is

A narrow fork of CrossPoint with one goal: read Traditional Chinese books well on the X3.
It trades away breadth (other languages, other formats, the X4) for Chinese typography,
a reading-first memory policy, and X3-specific display tuning. Personal project, not a product.
Not affiliated with Xteink or with the upstream CrossPoint project.

## Sound familiar?

- Book titles are a row of boxes in the file list, so you open them one by one to guess.
- Some books simply won't open — or the device reboots mid-chapter.
- Joining Wi-Fi to fetch a book hangs the device.
- Page turns get slower the deeper you are into a long novel.

CrossMosa has a specific, device-verified fix for each: 7,413 built-in Han characters,
out-of-memory guards through layout and rendering, freeing memory before the radio comes up,
and background pre-pagination. Four things here are, as far as we know, not in any other X3
system: **glyph prefetch** (~0.3 s off prefetched page turns, never slower), the **curated 50-masterpiece sleep
wallpapers**, **byte-for-byte reproducible builds**, and a fix for a **Wi-Fi connection crash
that upstream arduino-esp32 still carries**. See the [CHANGELOG](CHANGELOG.md) for each.

## Highlights vs upstream

- Fully translated Traditional Chinese UI (446 strings, Taiwan usage).
- Built-in UI font carries **7,413 Han characters** (all of BIG5 Level 1 + the 2,000 most
  common Level 2 characters), so filenames, book titles and menus render.
- Chinese reading fonts for the SD card: Serif / Sans / handwriting-style, four sizes each.
- **Glyph prefetch**: the next page's SD reads happen while you are still reading the current page —
  median page turn 1,278 ms → 952 ms.
- CJK line breaking, corrected CJK line spacing, no spurious spaces when re-joining text.
- Out-of-memory guards throughout layout, hyphenation, image decoding, font cache and TLS —
  low memory degrades instead of rebooting.
- OPDS improvements for large Chinese libraries; image/greyscale rendering roughly 2.4× faster;
  browser-based file transfer; UI themes **Formosa**, **Formosa Extended**, and **Formosa Pro**
  (Pro is new in this release). Newer X3 batches use a **UC8279** panel controller; this build
  identifies it at boot. If your screen already updates normally, you do not need to flash.

## Install — you must do BOTH steps

> ### ⚠️ Read this first
>
> **Newer X3 batches ship a UC8279 display controller** (older batches use UC8253). Firmware 1.x
> does not drive UC8279: after flashing, the screen stops updating — it may freeze on the
> "update complete" page while the device is still running (a blank SD card still gets a data
> directory written to it). Upstream added detection in
> [#2707](https://github.com/crosspoint-reader/crosspoint-reader/pull/2707), shipped in 1.5.0;
> this build includes it.
>
> **If you want to try it, please do.** Chapter switches drop from ~10 s to a second or two,
> page turns are smoother, images are more reliable, and clearing the cache can now keep your
> reading positions. It is a pre-release, so read this section first — beyond that, go ahead.

>
> **It is still not guaranteed to work on every device.** The panel controller is one known
> cause, not the only one. **E-ink retains its last image, so "something is on screen" does not
> mean the device is alive** — and conversely the firmware may still be running fine while the
> panel never hears a command. Judge by **timestamps of files on the SD card, not by the
> screen.** Upstream has a related unresolved report:
> [#2183](https://github.com/crosspoint-reader/crosspoint-reader/issues/2183).
>
> **Before you start:** back up the whole `/.crossmosa/` folder from your SD card (settings,
> Wi-Fi credentials, OPDS config and every book's reading position live there and cannot be
> recovered by reflashing); keep **only one** `.bin` in the SD card root (rescue may run with
> no display at all, so you cannot see what you are selecting); and be sure you can live with
> the worst case — **which is losing the device.** Users have bricked units, and **the rescue
> procedure has failed for some of them too.** Rescue is a path that *may* work, not insurance.
> Assume the device might not come back, and only proceed if you still accept that.
>
> Already flashed 1.x and **the screen is frozen**? [Rescue steps below](#screen-stopped-updating-rescue).
> About five to six minutes when it goes smoothly.

Flashing the firmware only fixes the **menus**. **Book text needs fonts on the SD card.**
The built-in fallback reader font is Latin-only, so **without the SD fonts every Chinese book
renders as boxes (□□□□)**.

> ### If the web flasher can't see your device — you don't need it
>
> The stock firmware has its own SD update mode. **Method A below never connects the device
> to a computer** (you only need to copy one file onto the SD card),
> and community documentation confirms it works even on USB-locked units. Do not use the
> Xteink Unlocker to flash this firmware (that tool officially supports only CrossPoint and
> CrossInk). Escape hatch on locked units: CrossMosa keeps the full SD rescue mode — but
> **do not treat it as a guarantee** — some users have followed the rescue procedure and still
> did not get their device back. At least two known conditions defeat it (both hit by this
> project in 2026-08), and there are further failures with no explanation yet:
> (1) rescue mode only triggers on a **power-button wake**, so it is **unreachable once the
> firmware is stuck in a boot loop** (a reset loop does not wake via the power button); you
> must let the **battery drain completely** to break the loop first. (2) **on some X3 units the
> USB port is charge-only with no data lines**, which makes Methods B and C unusable entirely. Full upstream text:
> [`docs/UPSTREAM-README.md`](docs/UPSTREAM-README.md), "USB-locked devices".

1. **Flash** (first install) — **Method A, recommended — the device never touches a computer**:
   copy the zip's **`update.bin`** to the **root** of the SD
   card (any card reader or phone adapter works), power off, then
   hold the **left side button + power** until the loader screen appears; it flashes and
   reboots in ~5 minutes (X3 only — the X4 stock firmware lacks this combo). Or use the web
   flasher at https://crosspointreader.com/#flash-tools (X3 → Custom .bin), or
   `esptool.py --chip esp32c3 write_flash 0x10000 update.bin`.
   Later updates never need a computer: grab the standalone `update.bin` from Releases
   (no unzip), upload it via the device's web transfer page, then
   **Settings → System → SD Card Firmware Update** — or
   the rescue combo (power off, hold the left side button, press power) straight into the
   SD firmware picker.
2. **Copy the fonts** from `crossmosa-2.0.0-sd-fonts.zip` into `/.fonts/` on the SD card,
   keeping one folder per family (`/.fonts/NotoSerifTC/…`). One family is enough;
   **NotoSerifTC** is the recommended first choice. As of 2026-08-31 all five families
   cover the full Han block (~21,000 characters), so rare characters no longer render as
   black boxes. `GuanKiapTsingKhai-90` is a pre-rotated brush face: pick it and turn the
   screen to landscape to read Chinese vertically.
   Folder names must not contain spaces.

While you have the card out, also copy 《歡迎使用CrossMosa.epub》 from the firmware zip onto
it — a thirteen-chapter guided tour (in Traditional Chinese) that teaches the device by making
you press its keys. Read it first.

First boot: the UI defaults to **Traditional Chinese** (this fork's whole point). To switch to English: **設定 → 系統 → 語言 → English** (= Settings → System → Language). Then
**Settings → Reader → Reader Font Family** to pick the SD font. The device creates
`/.crossmosa/` on the card for progress, bookmarks and Wi-Fi credentials — don't delete it.

**Optional — sleep wallpapers.** `crossmosa-2.0.0-wallpapers.zip` holds **50 public-domain
masterpieces** from Wikimedia Commons, each individually checked and tuned for this panel's
4 grey levels. Copy the `.bmp` files to `/.sleep/` on the SD card (two or more to rotate),
then **Settings → Display → Sleep Screen → Custom**. The converter, the curation manifest and
the reasoning behind the selection are in [`wallpapers/`](wallpapers/).

## UI character-set limits (please read)

The built-in UI font covers **7,413 Han characters**, not all of Chinese. Characters outside
that set render as boxes **in menus, filenames and titles only** — book *text* uses the SD
font and is unaffected (fully so with NotoSerifTC; NotoSansTC and Iansui have smaller sets).

Two known gaps: rare BIG5 Level 2 characters outside the 2,000 chosen, and characters
**outside BIG5 entirely**. The three most common Taiwanese given-name characters
（喆， 堃， 彣） were added in build 115; the rest still need a different character source
and are not in 1.0.

Report missing characters as a GitHub issue (paste the character itself and say where it
appeared). To regenerate the UI fonts yourself: edit `fonts/charsets/charset-ui-v5.txt`
(its ASCII header documents the exact sources and selection rule), run
`fonts/regen-ui-fonts.sh`, rebuild and reflash. UI fonts cannot be replaced from the SD card.

Why not include everything: full BIG5 would add ~2 MB to a 6.5 MB app partition that is
already 90.9% full.

<a id="screen-halted-en"></a>

## Screen stopped updating? Rescue

If you flashed 1.x and the screen no longer updates, you can **blind-flash straight to 2.0** —
no need to go back to stock firmware first. About five to six minutes when it goes smoothly.
**You will see nothing the whole time — work by the clock, and err on the long side.**

**Prepare:** put the firmware `.bin` in the SD card root — **just one**. Folders do not matter.

<img src="docs/img/button-map.png" width="340" alt="X3 button numbers">

| ① left edge | ② top-left | ③ top-right | ④ right edge | ⑥ front, 2nd |
|---|---|---|---|---|
| Previous page | Reset | Power | Next page | Confirm |

**Only ① and ⑥ are used. Never press ⑤⑦⑧** (the other three on the front row).

1. Press **②**
2. Hold **① + ③** for 4 s → release **③ first, then ①**
3. Wait **2 s**
4. Press **①**, then **⑥**
5. Wait **90 s** → press **① ⑥** again
6. Wait **90 s**
7. Press **⑥**
8. **Be patient** — writing; do not touch the device or remove the card. **On success it reboots
   by itself**
9. If nothing has happened after a good while, start again from step 1

**Try a few times.** Failures are usually just a press that did not register — you cannot see the
screen, so there is no feedback either way. (If it boots but the screen stays blank, press **②**
then hold **③**.)

<details>
<summary>Details: why these buttons, and what each wait is for</summary>

**Why ① ⑥ can be repeated safely** — neither can make things worse on any screen:

| Screen | ① | ⑥ |
|---|---|---|
| File list | wraps selection to your `.bin` | selects it |
| "Update firmware?" | does nothing | confirms |
| Checking / writing | ignored | ignored |

So you never need to know which step you are on. That is what steps 5 and 7 are for.

**Why only one `.bin`** — the rescue list shows only `.bin` files and always sorts folders first,
so your `.bin` is necessarily the last entry; ① wraps the selection from the first round to the
last. With several, you get the one that sorts last by name.

**⛔ Why not ⑤⑦⑧** — on the "Update firmware?" prompt, ⑤ and ⑦ both mean *cancel*. ⑦ is the
trap: in the list the screen labels it "up" and it behaves exactly like ①, but on the prompt it
cancels — **and the screen does not say so**.

**What each wait is for** — step 1: the rescue combo is only read at the instant of boot, so if
the device is already on with a dead screen the combo does nothing. Step 3: releasing the ① you
were holding also counts as a press. Step 5: insurance — it covers step 4 not registering, and is
ignored if step 4 already worked. Step 6: reading all ~6 MB to verify (measured 30–60 s).
Step 8: writing (measured 60–90 s), then it restarts by itself — but that restart does **not**
reset the screen chip, so occasionally the display stays blank until you press ②. All the waits
above have margin built in; erring on the long side costs nothing.

**Lost track?** Press **③ + ④** for a screenshot — the panel is not updating, but the device still
knows what *should* be on screen. Read `screenshot-*.bmp` from the card. Static screens only;
nothing happens during the check or the write.

**Stuck on 2.0** (no reports so far): same steps. Different mechanism — 2.0's prompt has two
options and starts on *Cancel*; ① moves it to *Confirm* and ⑥ selects.

**Another community write-up** reports different timings (~7 s combo, stock firmware first):
[CrossInk #479](https://github.com/uxjulia/CrossInk/discussions/479) ·
[issue #2](https://github.com/anki630/crossmosa/issues/2) (by @sk5s). Try that if the above fails.

**Not guaranteed** — some users have followed these steps and still not recovered.

</details>

## Building

```bash
git submodule update --init --recursive --depth 1
export SOURCE_DATE_EPOCH=$(git log -1 --format=%ct)
pio run -e gh_release
```

**Builds are byte-for-byte reproducible** — but only if `SOURCE_DATE_EPOCH` is set, because
`__DATE__`/`__TIME__` otherwise bake the wall clock into the image (one of the two sites is in
the Arduino core, not ours to patch). Every release publishes the epoch it used together with
the firmware sha256; [`scripts/mk-release.sh`](scripts/mk-release.sh) defaults to the release
commit's own timestamp, so checking out the tag reproduces the value automatically. See
[`docs/reproducible-builds.md`](docs/reproducible-builds.md).

## Relationship to upstream

Everything here stands on **CrossPoint** by **Dave Allie** and its contributors (MIT;
`LICENSE` kept as-is). Report upstream bugs upstream. If you want the full feature set,
use upstream rather than this fork.

**Removed** (to reclaim flash for Chinese fonts): 29 UI languages beyond English and
Traditional Chinese, KOReader progress sync, StarDict dictionary,
the OTA updater (SD-card firmware update is kept), the Classic and RoundedRaff themes
(kept: **Formosa**, **Formosa Extended**, **Formosa Pro**),
non-English hyphenation tables, built-in italic faces, and all but one built-in reader font size.

**Also removed from the tree** (not merely disabled; they cannot be compiled back in):
the **SMB2 server** for the iOS Files app, and the **BLE page-turner remote**.
Use browser file transfer, Calibre, OPDS, or copy files onto the SD card instead.

## ☕ If it made your X3 better

Say hi in [Discussions](../../discussions) and tell me what you've been reading with it, tell a
friend with an X3, or buy me a coffee (link coming — it changes nothing about the firmware).
Issues for missing characters or bugs are welcome too.

## Disclaimer

This project ships **no book content and no book sources** — bring your own legally obtained, DRM-free EPUBs (publisher or indie-store direct sales, public-domain libraries, your own documents). Support the authors. Flash at your own risk; third-party firmware can leave a device unbootable. Not affiliated
with Xteink or upstream. Verified primarily on **one newer-batch UC8279 X3**; an
older-batch (UC8253) unit was **flashed successfully by a user**. No X4 — much of the verification is "used it for
a few days and nothing broke". **No telemetry**: credentials, progress and bookmarks stay on
your SD card, and the device only reaches the network when you ask it to (NTP after joining
Wi-Fi, your own OPDS server, Calibre). The upstream OTA update check is removed, so this
firmware never contacts a project server on its own. Provided AS IS, see `LICENSE`.

## Licensing

Upstream CrossPoint: MIT © 2025 Dave Allie. CrossMosa modifications: MIT © 2026 CrossMosa
contributors. Bundled third-party code and fonts carry their own licences — see
[`NOTICE.md`](NOTICE.md). ⚠️ Some components linked into the released firmware binary are
GPLv2 and LGPL-2.1; read the "Distribution obligations" section of NOTICE first.

Maintained by **CrossMosa contributors**.
