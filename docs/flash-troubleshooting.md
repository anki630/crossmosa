# 刷不進去怎麼辦

按了組合鍵，出現「更新中」，半分鐘後退回、沒有更新——

代表更新器有啟動，是後半段沒過。依序檢查:

1. **檔案大小是否恰為 6,334,624 bytes、sha256 是否為 e1220b43f320b5b06192aa81d6f750da9423111fbddc0822677fa50c0af7c57a**（v2.0.1）——九成的問題在這:
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

---

回到 [首次安裝](install.md)。
（裝好之後的網路與傳檔問題看 [Troubleshooting](troubleshooting.md)。）
