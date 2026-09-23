# 螢幕停住了

刷完 1.x 之後畫面就不動了的，**看不到畫面也能直接刷成 2.0**，不必先刷回原廠韌體。
順的話大約五到六分鐘。**全程看不到畫面，照秒數操作就好——寧可多等，不要提早按。**

### 先把 SD 卡準備好

1. SD 卡插到電腦上
2. 卡裡的東西**全部刪掉**（不用格式化。想留書就先複製一份到電腦）
3. 下載 **`update.bin`**（2.0.0 以上，建議用[最新版](https://github.com/anki630/crossmosa/releases/latest)）
4. **原檔直接丟進卡的最外層**——不用解壓縮、不用改名
5. 卡裡只剩這一個 `update.bin`
6. 卡插回機器，**充飽電**

⚠️ 最重要的是第 2 步。卡上如果還留著上次刷機的舊 `.bin`，盲按會選到它，而你完全看不出來。

ℹ️ 寫到一半沒電**不會變磚**，只會開回原本的韌體，再來一次就好。

<img src="docs/img/button-map.png" width="340" alt="X3 按鍵編號">

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

If you flashed 1.x and the screen no longer updates, you can **blind-flash straight to 2.0** —
no need to go back to stock firmware first. About five to six minutes when it goes smoothly.
**You will see nothing the whole time — work by the clock, and err on the long side.**

### Get the SD card ready first

1. Put the SD card in a computer
2. **Delete everything on it** (no formatting needed; copy your books off first if you want them)
3. Download **`update.bin`** (2.0.0 or newer — use the [latest release](https://github.com/anki630/crossmosa/releases/latest))
4. **Drop it in the card's root exactly as downloaded** — do not unzip, do not rename
5. That one `update.bin` is the only thing left on the card
6. Put the card back in the device and **charge it fully**

⚠️ Step 2 is the one that matters. An old `.bin` left over from a previous update will be picked
instead, and you cannot tell.

ℹ️ Losing power mid-write **will not brick it** — it just boots the firmware you already had, so
you start over.

<img src="docs/img/button-map.png" width="340" alt="X3 button numbers">

| ① left edge | ② top-left | ③ top-right | ⑥ front, 2nd |
|---|---|---|---|
| Previous page | Reset | Power | Confirm |

**Only ① and ⑥ are used. Never press ⑤⑦⑧** (the other three on the front row).

1. Press **②**
2. Hold **① + ③** for 4 s → release **③ first, then ①**
3. Wait **2 s**
4. Press **①**, then **⑥**
5. Wait **90 s** → do step 4 again (**①** first, then **⑥**)
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

**Why "① then ⑥" can be repeated safely** — neither can make things worse on any screen:

| Screen | ① | ⑥ |
|---|---|---|
| File list | wraps selection to your `.bin` | selects it |
| "Update firmware?" | does nothing | confirms |
| Checking / writing | ignored | ignored |

So you never need to know which step you are on. That is what steps 5 and 7 are for.
**Press these one after the other, not together** — the only combinations you hold down at the
same time are **① + ③** (entering rescue) and **③ + ④** (screenshot).

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

**Stuck on 2.0** (no reports so far): same steps. Different mechanism — 2.0's prompt has two
options and starts on *Cancel*; ① moves it to *Confirm* and ⑥ selects.

**Another community write-up** reports different timings (~7 s combo, stock firmware first):
[CrossInk #479](https://github.com/uxjulia/CrossInk/discussions/479) ·
[issue #2](https://github.com/anki630/crossmosa/issues/2) (by @sk5s). Try that if the above fails.

**Lost track?** Press **③ + ④** (power + the right-edge button) for a screenshot — the panel is not updating, but the device still
knows what *should* be on screen. Read `screenshot-*.bmp` from the card. Static screens only;
nothing happens during the check or the write.

**Not guaranteed** — some users have followed these steps and still not recovered.

</details>

---

以上都救不回來，還有最後一條（拆機、直接燒 SPI flash）：
[Recovering a Bricked Xteink](fix-bricked-xteink.md)（英文）。
