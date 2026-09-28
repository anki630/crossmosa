# 首次安裝

把 CrossMosa 裝進你的 X3 或 X4。三步，順的話十分鐘。

> 已經在用 CrossMosa，只是要換新版？看[更新到新版](update.md)。
> X3 刷完畫面停住不動？看[螢幕停住了](rescue.md)。

## 開始之前

1. **備份 SD 卡上的 `.crossmosa` 資料夾。** 設定、Wi-Fi 密碼、閱讀進度和書籤都在裡面，重新刷機救不回來。
2. **SD 卡最外層只放一個 `.bin` 檔。** 刷機或救援時畫面可能沒有反應，卡上有好幾個 `.bin`，你看不出來選到的是哪一個。
3. **刷機有風險。** 已經有機器變磚，照著救援步驟也沒救回來。請先假設這台機器可能就這樣沒了，你仍然願意，再開始。

## 步驟 1：刷機

1. 下載 `update.bin`：X3 用[正式版](https://github.com/anki630/crossmosa/releases/latest)，X4 用[測試版](https://github.com/anki630/crossmosa/releases/tag/v2.1.0-beta.5)。
2. 把 `update.bin` 放進 SD 卡最外層，檔名不要改。瀏覽器重複下載時會存成 `update (1).bin`，那樣不行。
3. SD 卡插回機器。關機後，按住下面兩顆鍵開機，看到載入畫面就放手：
   - **X3**：左側的上一頁鍵＋電源鍵
   - **X4**：右側上面那顆鍵＋電源鍵。沒有反應的話，接上 USB 電源再試一次。
4. 等它刷完、自己重新開機，約五分鐘。刷完之後，把 `update.bin` 從卡上刪掉，免得以後誤刷舊版。

這組按鍵只有第一次需要。以後出新版，在設定裡就能更新，見[更新到新版](update.md)。

刷到一半失敗了，長按電源鍵 5 到 10 秒強制重開，重新下載檔案再試一次，多半是檔案沒下載完整。
還是不行，看[刷不進去怎麼辦](flash-troubleshooting.md)。

這個方法機器不用接電腦，多數 USB 被鎖住的機器也能用；少數批次連 SD 卡刷機也鎖住，見[刷不進去怎麼辦](flash-troubleshooting.md)。

**用 X4 的話**

- X4 的硬體不支援時鐘和傾斜翻頁。
- [救援步驟](rescue.md)目前只適用 X3，X4 的還在整理。

<details>
<summary>其他刷法：機器接得上電腦的話</summary>

**網頁刷機**

1. 用 USB-C 線接上電腦，喚醒機器。
2. 打開 https://crosspointreader.com/#flash-tools ，選 **X3**（用 X4 就選 **X4**），點 **Custom .bin**，上傳 `update.bin`。
3. 瀏覽器找不到機器的話：換一個 USB 孔、不要經過集線器、改用 Chrome 或 Edge。還是不行，就用上面的 SD 卡刷法。

**命令列**（進階）

```bash
pip install esptool
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
           write_flash 0x10000 update.bin
```

韌體 zip 裡另附的 `bootloader.bin` 與 `partitions.bin`，只有從 0x0 完整重刷時才需要；一般更新只要 `update.bin`。

**USB 被鎖住的機器**

部分第三方通路（例如 AliExpress）的機器出廠就鎖住 USB 刷機，直接向 xteink.com 買的沒有鎖。
上面的 SD 卡刷法多半不受影響。**不要用 Xteink Unlocker 來刷 CrossMosa**：那個工具官方只支援 CrossPoint 與 CrossInk，刷其他韌體有變磚風險。

救援也有限制，已知至少兩種情況會失效：

- 韌體卡在開機迴圈時，SD 救援模式進不去（它要用電源鍵喚醒才會啟動），要先讓電池完全放光，才有機會打斷迴圈。
- 部分機器的 USB 只能充電、不能傳資料，網頁刷機和命令列都用不了。

沒有任何一條路能保證把機器救回來。上游原文見 [`UPSTREAM-README.md`](UPSTREAM-README.md) 的 “USB-locked devices”。

</details>

## 步驟 2：放字型

韌體裡沒有書用的中文字型。少了這一步，選單是中文，但打開書會整頁都是方塊。

1. 下載[字型包](https://github.com/anki630/crossmosa/releases/tag/fonts-2026-09)，解壓縮。
2. 在 SD 卡最外層建一個 `fonts` 資料夾，把字型資料夾整個放進去：

```
SD 卡
└── fonts/
    ├── NotoSerifTC/   ← 明體（建議先裝這套）
    ├── RoundTC/       ← 圓體
    ├── NotoSansTC/    ← 黑體
    └── Iansui/        ← 硬筆楷書
```

只裝一套也可以，先裝 `NotoSerifTC`。已經放在 `.fonts` 資料夾裡的字型不用搬，兩個位置都讀得到。
想挑字型、裝大字版或注音字型，看[字型](fonts.md)。

## 步驟 3：開機

1. 開機就是繁體中文。想換英文介面：設定 → 系統 → 語言 → English。
2. 選字型：設定 → 閱讀器 → 閱讀字型，選剛剛放進去的那套。清單上沒看到的話，檢查字型資料夾是不是放在 `fonts/字型名稱/`。
3. 選字級：設定 → 閱讀器 → 閱讀字級。
4. 在剛剛下載韌體的那一頁，下載使用手冊 `CrossMosa.epub`，放進 SD 卡，打開它，一邊讀一邊按。
5. 機器會在 SD 卡上建一個 `.crossmosa` 資料夾，放進度、書籤和設定。不要刪它。

開機畫面和設定頁會顯示版本號，確認是你剛刷的那一版。

---

裝好之後：[把書放進去](books.md) · [換待機名畫](wallpaper.md) · [挑字型](fonts.md)

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
   hold the **left side button + power** (X3) or the **upper right side button + power** (X4)
   until the loader screen appears; it flashes and reboots in ~5 minutes. Or use the web
   flasher at https://crosspointreader.com/#flash-tools (X3 or X4 → Custom .bin), or
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
