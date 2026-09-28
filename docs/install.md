# 首次安裝

把 CrossMosa 裝進你的 X3 或 X4。三步，順的話十分鐘。

> 已經在用 CrossMosa，只是要換新版？看[更新到新版](update.md)。
> X3 刷完畫面停住不動？看[螢幕停住了](rescue.md)。

## 開始之前

1. **備份 SD 卡上的 `.crossmosa` 資料夾。** 設定、Wi-Fi 密碼、閱讀進度和書籤都在裡面，重新刷機救不回來。
2. **SD 卡最外層只放一個 `.bin` 檔。** 刷機或救援時畫面可能沒有反應，卡上有好幾個 `.bin`，你看不出來選到的是哪一個。
3. **刷機有風險。** 已經有機器變磚，照著救援步驟也沒救回來。請先假設這台機器可能就這樣沒了，你仍然願意，再開始。

## 步驟 1：刷機

1. 下載 `update.bin`。建議用 [Beta 版](https://github.com/anki630/crossmosa/releases/tag/v2.1.0-beta.5)，X3、X4 都能用；X3 也可以用[正式版](https://github.com/anki630/crossmosa/releases/latest)。
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
- [救援步驟](rescue.md)目前只適用 X3。

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

Install CrossMosa on your X3 or X4. It takes three steps and about ten minutes when everything goes smoothly.

> Already using CrossMosa and only need a newer version? See [Updating](update.md#updating-english).
> If your X3 screen stopped updating after flashing, see [Screen stopped updating? Rescue](rescue.md#screen-stopped-updating-rescue-english).

## Before you start

1. **Back up the `.crossmosa` folder on your SD card.** It contains your settings, Wi-Fi passwords, reading progress, and bookmarks. Reflashing cannot recover them.
2. **Keep only one `.bin` file in the root of the SD card.** The screen may not respond during flashing or rescue. If the card contains several `.bin` files, you cannot tell which one you selected.
3. **Flashing has risks.** Some devices have been bricked and could not be recovered even by following the rescue steps. Before you begin, assume you might lose the device and proceed only if you accept that risk.

## Step 1: Flash the firmware

1. Download `update.bin`. We recommend the [beta](https://github.com/anki630/crossmosa/releases/tag/v2.1.0-beta.5), which works on both the X3 and X4; on an X3, you can also use the [stable release](https://github.com/anki630/crossmosa/releases/latest).
2. Put `update.bin` in the root of the SD card. Do not rename it. A browser may save a repeated download as `update (1).bin`, which will not work.
3. Put the SD card back in the device. Turn the device off, then hold the following two keys while turning it on. Release them when the loading screen appears:
   - **X3:** the previous-page key on the left edge + power button
   - **X4:** the upper key on the right edge + power button. If nothing happens, connect USB power and try again.
4. Wait for flashing to finish and for the device to restart by itself. This takes about five minutes. Afterward, delete `update.bin` from the card so you do not accidentally install the old version later.

You only need this key combination for the first installation. For future releases, update from Settings. See [Updating](update.md#updating-english).

If flashing fails partway through, hold the power button for 5 to 10 seconds to force a restart. Download the file again and retry. An incomplete download is the most common cause.

If it still does not work, see [Flash troubleshooting](flash-troubleshooting.md#flash-troubleshooting-english).

This method does not require connecting the device to a computer. It also works with most devices that have USB flashing locked. A few batches also block SD card flashing; see [Flash troubleshooting](flash-troubleshooting.md#flash-troubleshooting-english).

**If you use an X4**

- X4 hardware does not support the clock or tilt page turning.
- The [rescue procedure](rescue.md#screen-stopped-updating-rescue-english) currently applies only to the X3.

<details>
<summary>Other methods: if the device can connect to a computer</summary>

**Web flasher**

1. Connect the device to your computer with a USB-C cable, then wake the device.
2. Open https://crosspointreader.com/#flash-tools, select **X3** or **X4**, select **Custom .bin**, and upload `update.bin`.
3. If the browser cannot find the device, try another USB port, connect without a hub, or use Chrome or Edge. If it still does not work, use the SD card method above.

**Command line** (advanced)

```bash
pip install esptool
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
           write_flash 0x10000 update.bin
```

The firmware zip also includes `bootloader.bin` and `partitions.bin`. You only need them for a complete reflash from 0x0. A normal update only needs `update.bin`.

**Devices with USB flashing locked**

Some devices sold through third-party channels, such as AliExpress, leave the factory with USB flashing locked. Devices bought directly from xteink.com are not locked.

The SD card method above usually still works. **Do not use Xteink Unlocker to flash CrossMosa.** That tool officially supports only CrossPoint and CrossInk. Flashing other firmware with it can brick the device.

Rescue also has limitations. It is known to fail in at least two situations:

- If the firmware is stuck in a boot loop, SD rescue mode cannot start. It only starts when the device wakes through the power button. You must first let the battery drain completely for a chance to interrupt the loop.
- Some devices have a charge-only USB connection with no data transfer. Neither the web flasher nor the command-line method will work on them.

No method can guarantee recovery. See “USB-locked devices” in [`UPSTREAM-README.md`](UPSTREAM-README.md) for the original upstream explanation.

</details>

## Step 2: Add fonts

The firmware does not include Chinese fonts for book text. Without this step, the menus appear in Chinese, but an opened book shows a full page of boxes.

1. Download and extract the [font pack](https://github.com/anki630/crossmosa/releases/tag/fonts-2026-09).
2. Create a `fonts` folder in the root of the SD card. Copy the complete font folders into it:

```
SD card
└── fonts/
    ├── NotoSerifTC/   ← serif (recommended first)
    ├── RoundTC/       ← rounded
    ├── NotoSansTC/    ← sans serif
    └── Iansui/        ← handwritten
```

You can install only one family. Start with `NotoSerifTC`. You do not need to move fonts already stored in `.fonts`; CrossMosa reads both locations.

To compare fonts or install large-print or zhuyin fonts, see [Fonts](fonts.md#fonts-english).

## Step 3: Start the device

1. The device starts in Traditional Chinese. To use the English interface, open 設定 → 系統 → 語言 → English.
2. Choose a font at Settings → Reader → Reader Font Family. Select the family you copied. If it is not listed, check that its folder is at `fonts/font-name/`.
3. Choose a text size at Settings → Reader → Reader Font Size.
4. On the firmware download page you used earlier, download the `CrossMosa.epub` user guide (in Traditional Chinese). Put it on the SD card and open it. Read along and try each control.
5. The device creates a `.crossmosa` folder on the SD card for progress, bookmarks, and settings. Do not delete it.

The startup screen and Settings page show the version number. Confirm that it matches the version you installed.

---

After installation: [Add books](books.md#adding-books-english) · [Change the sleep screen](wallpaper.md#changing-the-sleep-screen-english) · [Choose fonts](fonts.md#fonts-english)
