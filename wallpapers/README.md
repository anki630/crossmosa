# wallpapers — X3 待機壁紙:50 張世界名畫 + 轉檔工具

X3 闔上之後不是黑畫面,是一幅畫。

![X3 待機顯示葛飾北齋《神奈川沖浪裏》](../docs/promo/mock_wave_white.png)

**大部分人不需要跑這裡的任何東西**——算好的 50 張 BMP 直接放在 Release 的
`crossmosa-<版本>-wallpapers.zip` 裡,解開複製到 SD 卡就好(見下方「安裝」)。
這個資料夾是給想**換成自己的圖**、或想知道**這 50 張是怎麼挑的**的人。

---

## 安裝

1. 把 `.bmp` 複製到 SD 卡的 **`/.sleep/`**(**放兩張以上才會輪播**)。
2. 裝置上:**設定 → 顯示 → 待機畫面 → 自訂**(或**封面+自訂**)。

> ⚠️ SD 根目錄**不要**放單獨一個 `/sleep.bmp` —— 它會優先、固定顯示、不輪播。

橫式構圖的那幾張是**整張轉 90 度**存的(待機畫面固定跑 528×792 直向,韌體不會自動轉正)。
看到橫的畫時,把裝置**往順時針方向轉 90 度**。

---

## 策展:為什麼是這 50 張

50 張全部取自 **Wikimedia Commons 的公共領域作品**(畫作本身已進入公共領域;二維平面作品的
忠實攝影翻拍在美國不產生新的著作權)。清單、出處檔名與每一筆的取捨理由寫在
[`artworks.py`](artworks.py) 的註解裡。

挑選標準只有一個:**在 4 階灰階、528×792、沒有背光的螢幕上,這張畫還是不是那張畫。**
這跟「這張畫有沒有名」是兩回事,好幾張名畫因此被換掉:

- **透納《被拖去解體的戰艦無畏號》→ 換掉。** 透納那種柔霧氛圍靠的是低幅度的局部對比,
  全解析度 dither 模擬之後就證實 4 階灰階救不回來。
  換成溫斯洛·荷馬《灣流》——同樣的海難主題,但明暗對比強烈。
- **莫內《印象·日出》→ 換掉。** 整張畫靠「橘色太陽 vs 藍灰霧霾」的**色相**對比撐著,
  而兩者的**亮度幾乎相同**——轉成灰階之後對比幾乎歸零,dither 也生不出原本就不存在的對比。
  換成葛飾北齋《凱風快晴》(赤富士),跟《神奈川沖浪裏》同系列的浮世繪版畫。
- **雷諾瓦《煎餅磨坊》→ 實測失敗。** 樹蔭間的斑斕光點在 4 階下變成雜訊。
  換成卡耶博特《巴黎街道:雨天》(灰調、明暗結構清楚)。
- **秀拉《大碗島的星期日下午》→ 實測失敗。** 點描法配上粉色調,灰階化之後糊成一片。
  換成沙金《X 夫人》——黑禮服對上蒼白膚色,對比極強。

另一半的工夫花在**確認拿到的是畫本身**。Commons 上同一幅畫常有好幾個檔案,而檔名不保證內容:

- 有一份被當成正選的 `…MoMA.jpg`,下載核對後發現是**遊客在展場對著畫拍的照片**
  (授權標成 CC-BY 通常就是這個訊號——公共領域的畫作翻拍不會有新的授權主張)。
- 有一個梵谷的檔名,內容其實是**鋼筆素描草稿**,不是大家熟悉的那張油畫。
- 有兩張是**博物館展框翻拍**(泛黃、打光不均、連畫框一起入鏡)與**書本圖版翻拍**
  (網點濁、底部還有印刷說明文字),都換成乾淨的平面掃描。

還有幾張是構圖問題而不是畫質問題:《最後的晚餐》13 個人橫跨整幅、《創世紀》上帝與亞當分踞兩端,
置中裁切會直接切掉主角,所以這幾張標了 `fit=True` 走完整呈現(留邊)而不是裁滿版;
《倒牛奶的女僕》這類主體偏一側的,用 `focus=(x, y)` 把裁切焦點移過去。

---

## 韌體格式要求(踩過的雷)

| 項目 | 要求 | 備註 |
|---|---|---|
| 壓縮 | **未壓縮 BI_RGB(compression=0)** | ⚠️ 最常錯:匯出時選到 **RLE 壓縮**會被裝置直接跳過 |
| 位元深度 | 1/2/4/8/24/32 bpp | 名畫管線輸出 **2-bit**,通用轉檔輸出 **8-bit 灰階** |
| 尺寸 | ≤ 2048×3072 | 輸出 **528×792**(直向,1:1 對上待機,裝置不縮放) |
| 副檔名 | `.bmp` | 檔名開頭是 `.` 或放在子資料夾會被裝置忽略 |

---

## 用法 A:把任何一張圖轉成壁紙(`make-wallpaper.py`)

```bash
python3 wallpapers/make-wallpaper.py             # 轉 <repo>/sleep/ 裡的每一張圖(就地)
python3 wallpapers/make-wallpaper.py a.jpg b.png # 只轉指定檔案(輸出放原檔旁)
python3 wallpapers/make-wallpaper.py --dir 路徑  # 換一個資料夾
python3 wallpapers/make-wallpaper.py --mode fit  # 完整放入、四周留邊(預設 cover=裁切填滿)
python3 wallpapers/make-wallpaper.py --pad white # fit 模式的留邊改白色(預設黑)
python3 wallpapers/make-wallpaper.py --size 792x528  # 換目標尺寸(橫向的話)
python3 wallpapers/make-wallpaper.py --tidy      # 把用過的非 bmp 原檔移到 sleep/_src/ 保留
```

- **cover(預設)**:縮放後裁切,填滿整個 528×792(不留邊,但邊緣可能被切掉)。
- **fit**:完整放入、比例不變,不足處用 `--pad` 的顏色補滿。

轉完會**直接解析輸出 BMP 的標頭 bytes**(寬高 / bpp / compression),用與韌體
`lib/GfxRenderer/Bitmap.cpp` 相同的規則判定 ✅/❌——不是「應該可以」,是實際比對過。

## 用法 B:重跑整套名畫壁紙(三個腳本)

```bash
pip install pillow numpy
python3 wallpapers/fetch_sources.py        # 依 artworks.py 從 Wikimedia Commons 下載原圖
python3 wallpapers/make-art-wallpapers.py  # 全部 50 張 → <repo>/sleep/<slug>.bmp
python3 wallpapers/make-art-wallpapers.py mona_lisa great_wave   # 或只做指定的 slug
```

- [`artworks.py`](artworks.py):清單資料(slug / 標題 / 藝術家 / 直向或橫向 /
  Wikimedia Commons 的精確檔名 / 裁切策略),**以及每一筆為什麼是這一筆**。
- [`fetch_sources.py`](fetch_sources.py):下載原圖到 `<repo>/sleep/_src/`。
  內建 rate-limit 重試——Commons 對連續請求會回 429。
- [`make-art-wallpapers.py`](make-art-wallpapers.py):讀 `_src/` 的原圖,輸出 2-bit BMP。

**新增畫作**:在 `artworks.py` 加一筆(`orientation` 依畫作實際長寬比,不要猜),
重跑 `fetch_sources.py` + `make-art-wallpapers.py`。
⚠️ 少數幾張的 `_src/*.jpg` 是**已經手動裁好的**(三聯畫取中幅、裁掉展框或書本白邊),
**不要**用 `fetch_sources.py --force` 覆蓋掉它們;`artworks.py` 的註解有逐張標示。

---

## 正式管線(經實機驗證,每一條都踩過坑,別亂改)

```
原圖 → 灰階 → autocontrast(cutoff=1) → Lanczos 縮 528×792(最終顯示解析度)
     → Floyd-Steinberg dither(僅畫作區,4 階) → 疊乾淨標籤(就近取整,不 dither)
     → 橫式最後無損旋轉 90° → 寫 2-bit 原生 BMP(色盤 0/85/170/255)
```

- **2-bit 而非 8-bit**:8-bit 灰階會被韌體用它自己的 **Atkinson 重新 dither**,
  電腦端的演算法全部白做。2-bit(bpp ≤ 2)→ 韌體判定 `nativePalette=true` → **原樣顯示、不碰**,
  演算法的決定權才在電腦這端。檔案還小 4 倍(419 KB → 105 KB)。
- **Floyd-Steinberg**:誤差擴散(自適應)的細節勝過 blue noise;暗部比 Jarvis 乾淨
  (核小、誤差不亂散);色調比韌體的 Atkinson 平滑(不丟誤差)。多輪實機 A/B 後定案。
- **autocontrast**:對比與可見度這一軸跟 dither 演算法無關,靠它補;不加會顯得淡、費眼。
- **先算圖再打字**:整張一起 dither 會把文字的反鋸齒邊緣打散成雜點 → 字糊。標籤區只做就近取整。
- **dither 放在最終 528×792、當最後一步**:裝置 1:1 不縮放顯示,dither 的點才精準落在像素上;
  任何 dither 之後的重新取樣都會毀掉點陣(所以旋轉用整數無損 90°、文字最後才疊)。

**驗證**:`make-art-wallpapers.py` 會用**與韌體相同的邏輯**把產出的 2-bit BMP 完整解碼回來,
逐 byte 比對是否等於輸入的四階 index,每張都印 `roundtrip=True/False`。
⚠️ 手刻 2-bit BMP 時 palette 是 **BGRA 順序**(byte 54=B、55=G、56=R、57=reserved);
韌體的亮度公式是 `lum = (77*R + 150*G + 29*B) >> 8`。
這裡曾經把解碼的 byte offset 寫錯 off-by-one,結果是誤判。

---

## 需求

Python 3 + [Pillow](https://pypi.org/project/pillow/) + NumPy(`pip install pillow numpy`)。
標籤用的是系統的 DejaVu Serif(`/usr/share/fonts/truetype/dejavu`)。

## 授權

- 這幾個腳本:MIT,與本專案其餘部分相同(見 [`../LICENSE`](../LICENSE))。
- 產出的壁紙:畫作本身是**公共領域**,透過 Wikimedia Commons 取得;
  二維平面作品的忠實攝影翻拍不產生新的著作權。沒有額外的授權義務。
  逐張的出處檔名記在 [`artworks.py`](artworks.py),另見 [`../NOTICE.md`](../NOTICE.md)。
