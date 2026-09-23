# 首次安裝

把 CrossMosa 刷進你的 X3。三步，順的話十分鐘。

> 已經在用 CrossMosa、只是要換新版？看 [日後更新](update.md)。
> 刷完畫面停住不動？看 [螢幕停住了](rescue.md)。

## ⚠️ 先讀這段

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

> 已經刷了 1.x、**畫面停住不動**的：[救援步驟在這裡](rescue.md)。順的話約五到六分鐘。

## ⚠️ 一定要做兩件事，少做一件，中文書就是滿頁方塊

刷韌體**只解決介面**。**書的內文字型不在韌體裡**，它在 SD 卡上。

韌體內建的閱讀備援字型**只有拉丁文**——沒有複製 SD 字型的話，
選單是正常中文，但**打開任何中文書，內文會整頁都是方塊(□□□□)**。這不是壞掉，是缺字型。

| 步驟 | 檔案 | 去哪 |
|---|---|---|
| **1. 刷韌體** | `crossmosa-2.0.1-firmware.zip` | 裝置的 flash |
| **2. 複製字型** | `crossmosa-2.0.1-sd-fonts.zip` | SD 卡的 `/.fonts/` |

兩個檔案都在同一個 [Release](../../releases) 頁面。

## 步驟 1:刷韌體（首次安裝）

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
> 上游完整原文:[`docs/UPSTREAM-README.md`](UPSTREAM-README.md) "USB-locked devices"。

## 步驟 2:複製 SD 卡字型

解開 `crossmosa-2.0.1-sd-fonts.zip`，把**整個字型資料夾**複製到 SD 卡的 `/.fonts/` 底下:

```
SD 卡根目錄
└── .fonts/
    ├── NotoSerifTC/          ← 明體(建議先裝這套)
    │   ├── NotoSerifTC_16.cpfont
    │   ├── NotoSerifTC_18.cpfont
    │   ├── NotoSerifTC_20.cpfont
    │   └── NotoSerifTC_22.cpfont
    ├── RoundTC/              ← 圓體
    ├── NotoSansTC/           ← 黑體
    ├── Iansui/               ← 硬筆楷書
    └── GuanKiapTsingKhai-90/ ← 楷書·直排用
```

五套字型的比較、大字版、以及「遇到方塊字」看 [字型](fonts.md)。**只裝一套也可以**——先裝 `NotoSerifTC`。


## 步驟 3:第一次開機

1. **開機就是繁體中文**（要英文介面:**設定 → 系統 → 語言 → English**;從原版升級、之前選過英文的，設定會保留，同一路徑可切）。
2. **選內文字型**:**設定 → 閱讀器 → 閱讀字型**，選剛剛複製的那套。
   （沒看到就代表 SD 卡路徑不對，檢查是 `/.fonts/字型名/` 而不是 `/.fonts/`。）
3. 選字級:**設定 → 閱讀器 → 閱讀字級**。清單上會出現你選的那套字型實際有的尺寸（標準包是 16/18/20/22，大字版是 24/26/28）。
4. 裝置會在 SD 卡建 `/.crossmosa/` 放進度、書籤、Wi-Fi 憑證與快取。**不要刪它。**
5. 版號顯示在**開機畫面**與**設定頁**，確認是 `2.0.1`。


---

裝好之後：[傳書進去](books.md) · [換待機壁紙](wallpaper.md) · [選字型](fonts.md)

刷機失敗了？看 [刷不進去怎麼辦](flash-troubleshooting.md)。

---

# Install (English)

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
> Already flashed 1.x and **the screen is frozen**? [Rescue steps below](rescue.md).
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
> [`docs/UPSTREAM-README.md`](UPSTREAM-README.md), "USB-locked devices".

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
2. **Copy the fonts** from `crossmosa-2.0.1-sd-fonts.zip` into `/.fonts/` on the SD card,
   keeping one folder per family (`/.fonts/NotoSerifTC/…`). One family is enough;
   **NotoSerifTC** is the recommended first choice. All five carry **27,950 Han characters**,
   including the whole CJK Extension A block, so rare characters no longer render as black
   boxes. 2.0.1 also added the symbols Chinese ebooks actually use: circled numbers,
   bopomofo, **vertical punctuation `︿ ﹀ ﹃ ﹄`**, box drawing.
   **If you downloaded the fonts before 2.0.1, download them again** — otherwise those
   characters are still boxes.
   `GuanKiapTsingKhai-90` is a pre-rotated brush face: on 2.0.x, pick it and turn the screen
   to landscape to read Chinese vertically. **Do not do this on 2.1** — 2.1 has real vertical
   layout (Settings -> Reader -> Text Settings -> Layout -> Text Direction), and a pre-rotated
   face there lays every character on its side. Pick `Iansui` for a brush face instead.
   Folder names must not contain spaces.

   **Can't read small text?** `crossmosa-2.0.1-sd-fonts-large.zip` has sans and serif at
   **24 / 26 / 28** — same install, no reflash. It stops at 28 on purpose: what makes reading
   comfortable is not how big the type is but how much text is left on a page. 22pt fits
   about 138 characters, 28pt about 85. Past that you spend the evening turning pages.

While you have the card out, also copy 《歡迎使用CrossMosa.epub》 from the firmware zip onto
it — a thirteen-chapter guided tour (in Traditional Chinese) that teaches the device by making
you press its keys. Read it first.

First boot: the UI defaults to **Traditional Chinese** (this fork's whole point). To switch to English: **設定 → 系統 → 語言 → English** (= Settings → System → Language). Then
**Settings → Reader → Reader Font Family** to pick the SD font. The device creates
`/.crossmosa/` on the card for progress, bookmarks and Wi-Fi credentials — don't delete it.

**Optional — sleep wallpapers.** `crossmosa-2.0.1-wallpapers.zip` holds **50 public-domain
masterpieces** from Wikimedia Commons, each individually checked and tuned for this panel's
4 grey levels. Copy the `.bmp` files to `/.sleep/` on the SD card (two or more to rotate),
then **Settings → Display → Sleep Screen → Custom**. The converter, the curation manifest and
the reasoning behind the selection are in [`wallpapers/`](../wallpapers/).
