# 螢幕停住了

這一頁適用 X3。

刷完 1.x 之後畫面就不動了的，**看不到畫面也能直接刷成 2.0**，不必先刷回原廠韌體。
順的話大約五到六分鐘。**全程看不到畫面，照秒數操作就好——寧可多等，不要提早按。**

### 先把 SD 卡準備好

1. SD 卡插到電腦上
2. 卡裡的東西**全部刪掉**（不用格式化。想留書就先複製一份到電腦）
3. 下載 **`update.bin`**（2.0.0 以上，建議用[最新版](https://github.com/anki630/crossmosa/releases/latest)）
4. **原檔直接丟進卡的最外層**——不用解壓縮、不用改名
5. 卡裡只剩這一個 `update.bin`
6. 卡插回機器，**充飽電**

**最重要的是第 2 步**。卡上如果還留著上次刷機的舊 `.bin`，盲按會選到它，而你完全看不出來。

寫到一半沒電**不會變磚**，只會開回原本的韌體，再來一次就好。

<img src="img/button-map.png" width="340" alt="X3 按鍵編號">

| ① 左側邊 | ② 上緣左 | ③ 上緣右 | ⑥ 正面左2 |
|---|---|---|---|
| 上一頁 | 重置 | 電源 | 確認 |

**只會用到 ① 和 ⑥。⑤⑦⑧（正面其餘三顆）一次都不要按。**

1. 按 **②**
2. 按住 **① ＋ ③** 四秒 → **先放 ③，再放 ①**
3. 等 **2 秒**
4. 按 **①**，再按 **⑥**
5. 等 **90 秒** → 再做一次第 4 步（先 **①**，再 **⑥**）
6. 等 **90 秒**
7. 按 **⑥**
8. **耐心等** —— 正在寫入，別碰機器、別拔卡。**成功的話它會自己開機**
9. 等很久還是沒動靜，就從第 1 步重來

**多試幾次。** 沒成功多半只是某一下沒按實——看不到畫面，按下去有沒有被機器收到，你完全感覺不出來。
重來個幾輪通常就過了。（開機了但畫面還是黑的，按 **②** 再長按 **③**。）

<details>
<summary>細節：為什麼這樣按、每一步在等什麼</summary>

**為什麼可以一直重複「先 ① 再 ⑥」**：因為這兩顆不管機器停在哪一頁，都不會把事情弄糟。

| 當下畫面 | ① | ⑥ |
|---|---|---|
| 檔案清單 | 跳到你的 `.bin` | 選它 |
| 「要更新韌體嗎？」 | 沒有作用 | 確認 |
| 檢查中／寫入中 | 忽略 | 忽略 |

所以不必知道自己在第幾步，不確定就再來一輪。第 5、7 步就是這個道理。
**這兩顆是【一顆一顆按】，不是同時按住**——本文裡只有 **① ＋ ③**（進救援）和
**③ ＋ ④**（截圖）這兩組才是要同時按住的。

**為什麼 `.bin` 只放一個**：救援畫面的清單只會列出 `.bin`，而且資料夾一定排在前面，
所以你那個 `.bin` 一定是最後一個。按 ① 會從第一個繞到最後一個，剛好就選到它。
卡上有好幾個 `.bin` 的話，選到的會是檔名排最後的那個。

**為什麼不能按 ⑤⑦⑧**：在「要更新韌體嗎？」那個畫面上 ⑤⑦ 都是**取消**。
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

**卡在 2.0 的話**（目前沒人遇到）步驟完全一樣，只是機器裡面走的路不同：
2.0 問你要不要更新那一頁有兩個選項、一開始停在「取消」上，① 會把它移到「確認」，⑥ 再按下去。

**其他版本的做法**：社群另有一版（組合鍵約 7 秒、先刷回原廠韌體）——
[CrossInk #479](https://github.com/uxjulia/CrossInk/discussions/479) ·
[本專案 issue #2](https://github.com/anki630/crossmosa/issues/2)（@sk5s 回報）。上面這套行不通時可以試。

**不知道自己走到哪了**：按 **③ ＋ ④**（電源 ＋ 右側邊那顆）拍一張截圖 —— 螢幕雖然沒更新，機器心裡還是知道
「現在該顯示什麼」。把卡拔到電腦上打開 `screenshot-*.bmp` 就看得到。
只有畫面停著不動的時候有效；第 6 步檢查中、第 8 步寫入中按了不會有反應。

**不保證每台都救得回來。** 已經有使用者照著做仍然沒救回。

</details>



---

# Screen stopped updating? Rescue (English)

This page applies to the X3.

If the screen stopped updating after you flashed 1.x, you can **blind-flash directly to 2.0**. You do not need to return to the stock firmware first.

The process takes about five to six minutes when everything goes smoothly. **You will not see the screen at any point. Follow the timings closely. It is safer to wait longer than to press early.**

### Prepare the SD card first

1. Put the SD card in a computer.
2. **Delete everything on the card.** You do not need to format it. If you want to keep your books, copy them to the computer first.
3. Download **`update.bin`**, version 2.0.0 or later. The [latest release](https://github.com/anki630/crossmosa/releases/latest) is recommended.
4. **Put the original file directly in the root of the card.** Do not extract or rename it.
5. Make sure this one `update.bin` is the only file left on the card.
6. Put the card back in the device and **fully charge the device**.

**Step 2 is the most important.** If an old `.bin` from a previous flash remains on the card, the blind sequence will select it and you will have no way to tell.

Losing power during writing **will not brick the device**. It will start with the firmware it already had, and you can try again.

<img src="img/button-map.png" width="340" alt="X3 button numbers">

| ① left edge | ② top-left edge | ③ top-right edge | ⑥ front, second from left |
|---|---|---|---|
| Previous page | Reset | Power | Confirm |

**You will only use ① and ⑥. Never press ⑤⑦⑧, the other three front keys.**

1. Press **②**.
2. Hold **① + ③** for four seconds → **release ③ first, then ①**.
3. Wait **2 seconds**.
4. Press **①**, then press **⑥**.
5. Wait **90 seconds** → repeat step 4 once more. Press **①** first, then **⑥**.
6. Wait **90 seconds**.
7. Press **⑥**.
8. **Wait patiently.** The device is writing the firmware. Do not touch it or remove the card. **If the update succeeds, the device will start by itself.**
9. If nothing happens after a long wait, start again from step 1.

**Try several times.** A failed attempt usually means that one press did not register. Because you cannot see the screen, you receive no feedback when this happens. Repeating the process a few times will usually work. If the device starts but the screen remains black, press **②**, then hold **③**.

<details>
<summary>Details: why this sequence works and what each wait is for</summary>

**Why you can safely repeat “①, then ⑥”:** These two keys will not cause a problem, regardless of which screen the device is on.

| Current screen | ① | ⑥ |
|---|---|---|
| File list | Moves to your `.bin` | Selects it |
| “Update firmware?” | No effect | Confirms |
| Checking or writing | Ignored | Ignored |

You do not need to know which step the device has reached. If you are unsure, repeat the sequence. This is why steps 5 and 7 work.

**Press these keys one at a time. Do not hold them together.** The only combinations in this page that you hold together are **① + ③** to enter rescue and **③ + ④** to take a screenshot.

**Why the card must contain only one `.bin`:** The rescue file list shows only `.bin` files, and folders always appear first. Your `.bin` is therefore the last item. Pressing ① wraps from the first item to the last and selects it. If the card contains several `.bin` files, the device selects the one whose filename sorts last.

**Why you must not press ⑤⑦⑧:** On the “Update firmware?” screen, both ⑤ and ⑦ mean **Cancel**. Key ⑦ is the easiest mistake to make. On the file-list screen, it is labeled **Up** and does the same thing as ①. On the confirmation screen, it becomes Cancel, **but the screen does not show that label**.

**What each step is waiting for**

- Step 1: The key combination is read **only at the moment the device starts**. If the device is already on with no visible screen, pressing the combination does nothing.
- Step 3: Releasing ① also counts as pressing it once. Wait two seconds for that input to pass.
- Step 5: This is a safeguard. If the presses in step 4 did not register, it repeats them. If step 4 already worked, the device ignores this round. Either result is safe.
- Step 6: The device checks the file for damage. It must read the entire file, about 6 MB. This takes 30–60 seconds in testing. The waits above include extra time. **It is safer to wait longer.**
- Step 8: The device writes the firmware. This takes 60–90 seconds in testing. It restarts automatically when writing finishes. That restart **does not restart the display controller**, so the screen may occasionally remain black. Press ② to perform a complete restart.

**If the device is stuck on 2.0**—which has not been reported—the steps are exactly the same, but the internal path is different. The 2.0 confirmation screen has two choices and starts on **Cancel**. Key ① moves to **Confirm**, then ⑥ selects it.

**Procedures for other versions:** The community has another procedure that holds the combination for about 7 seconds and returns to the stock firmware first: [CrossInk #479](https://github.com/uxjulia/CrossInk/discussions/479) · [this project’s issue #2](https://github.com/anki630/crossmosa/issues/2), reported by @sk5s. Try it if the procedure above does not work.

**If you do not know which screen the device has reached:** Press **③ + ④**, the power key and the key on the right edge, to take a screenshot. The display may not update, but the device still knows what it should be showing. Remove the card and open `screenshot-*.bmp` on a computer. This only works while the device is on a static screen. It does nothing during the check in step 6 or the write in step 8.

**Recovery is not guaranteed.** Some users followed these steps and still could not recover their devices.

</details>
