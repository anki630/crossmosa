# 刷不進去怎麼辦

按了組合鍵，畫面出現「更新中」，半分鐘後又退回去、沒有更新。這表示更新程式有啟動，是後半段沒過。依序檢查：

1. **檔案是不是完整的。** 九成的問題在這裡：沒下載完、瀏覽器存成 `update (1).bin`、Windows 隱藏副檔名讓它變成 `update.bin.bin`、把整個 zip 放進去沒有解壓縮，或卡上還留著一個舊的 `update.bin`。檔案要放在 SD 卡最外層，大小要跟你下載的那一頁上寫的一樣。
2. **檔案沒問題，還是失敗**：接上電腦，改用[首次安裝](install.md)裡「其他刷法」的網頁刷機。瀏覽器找不到機器的話，換一個 USB 孔、不要經過集線器、改用 Chrome 或 Edge。
3. **怎樣都找不到機器**：你的機器可能是出廠鎖住的批次（部分第三方通路），連 SD 卡刷機都只收原廠的檔案。這時要先用官方的 [Xteink Unlocker](https://crosspointreader.com/unlock) 裝上官方的 CrossPoint，再從它的「Settings → SD Card Firmware Update」選 CrossMosa 的 `update.bin`。裝上 CrossMosa 之後，X3 也能用 SD 救援模式刷回官方 CrossPoint。

遇到第 3 種情況，請[開一張 Issue](https://github.com/anki630/crossmosa/issues) 告訴我們你的原廠韌體版本號，幫後面的人整理「哪些批次會擋 SD 刷機」。

---

回到[首次安裝](install.md)。裝好之後的網路與傳檔問題，看 [Troubleshooting](troubleshooting.md)。

---

# Flash troubleshooting (English)

You pressed the key combination, the screen showed that an update was in progress, and about half a minute later the device returned without updating. This means the updater started, but a later stage failed. Check these items in order:

1. **Make sure the file is complete.** This causes nine out of ten problems. The download may be unfinished, the browser may have saved it as `update (1).bin`, Windows may have hidden the extension and produced `update.bin.bin`, you may have copied the entire zip without extracting it, or an older `update.bin` may still be on the card. Put the file in the root of the SD card and confirm that its size matches the size listed on the download page you used.
2. **If the file is correct but flashing still fails:** Connect the device to a computer and use the web flasher under “Other methods” in [Install](install.md#install-english). If the browser cannot find the device, try another USB port, connect without a hub, or use Chrome or Edge.
3. **If the computer still cannot find the device:** Your device may come from a factory-locked batch sold through some third-party channels. These devices may accept only official firmware even when flashing from an SD card. First use the official [Xteink Unlocker](https://crosspointreader.com/unlock) to install the official CrossPoint firmware. Then select the CrossMosa `update.bin` through Settings → System → SD Card Firmware Update. After CrossMosa is installed, X3 can also use SD rescue mode to return to the official CrossPoint firmware.

If step 3 applies to your device, [open an issue](https://github.com/anki630/crossmosa/issues) and include the version number of its factory firmware. This will help document which batches block SD card flashing.

---

Return to [Install](install.md#install-english). For network and file-transfer problems after installation, see [Troubleshooting](troubleshooting.md).
