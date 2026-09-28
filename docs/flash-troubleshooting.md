# 刷不進去怎麼辦

按了組合鍵，畫面出現「更新中」，半分鐘後又退回去、沒有更新。這表示更新程式有啟動，是後半段沒過。依序檢查：

1. **檔案是不是完整的。** 九成的問題在這裡：沒下載完、瀏覽器存成 `update (1).bin`、Windows 隱藏副檔名讓它變成 `update.bin.bin`、把整個 zip 放進去沒有解壓縮，或卡上還留著一個舊的 `update.bin`。檔案要放在 SD 卡最外層，大小要跟下載頁上寫的一樣（X3 看[正式版的下載頁](https://github.com/anki630/crossmosa/releases/latest)，X4 看[測試版的下載頁](https://github.com/anki630/crossmosa/releases/tag/v2.1.0-beta.5)）。
2. **檔案沒問題，還是失敗**：接上電腦，改用[首次安裝](install.md)裡「其他刷法」的網頁刷機。瀏覽器找不到機器的話，換一個 USB 孔、不要經過集線器、改用 Chrome 或 Edge。
3. **怎樣都找不到機器**：你的機器可能是出廠鎖住的批次（部分第三方通路），連 SD 卡刷機都只收原廠的檔案。這時要先用官方的 [Xteink Unlocker](https://crosspointreader.com/unlock) 裝上官方的 CrossPoint，再從它的「Settings → SD Card Firmware Update」選 CrossMosa 的 `update.bin`。裝上 CrossMosa 之後，X3 也能用 SD 救援模式刷回官方 CrossPoint。

遇到第 3 種情況，請[開一張 Issue](../../issues) 告訴我們你的原廠韌體版本號，幫後面的人整理「哪些批次會擋 SD 刷機」。

---

回到[首次安裝](install.md)。裝好之後的網路與傳檔問題，看 [Troubleshooting](troubleshooting.md)。
