# guide — 《歡迎使用 CrossMosa》導覽書

刷完機、複製完字型之後,第一本該讀的書。**十三章,每一章結尾都叫你按一顆鍵**——
翻頁、調閱讀字級、加書籤、選擇章節、開註腳、截圖、救援刷機。讀完一遍,這台機器就會用了。約 10 分鐘。

它同時是排版展示品:讀起來舒不舒服,本身就是這個分支想證明的事。

```
guide/
├── build-guide-epub.py       產生器(Python 3.8+ 與 Pillow,沒有其他相依)
├── chapters/*.xhtml          每章一個檔,只放 <body> 裡面的內容
├── style.css                 很薄的樣式
└── 歡迎使用CrossMosa.epub     建好的成品(進版控,Release 直接拿它)
```

## 怎麼建

```bash
python3 guide/build-guide-epub.py          # 建好之後跑驗證,寫進 guide/
python3 guide/build-guide-epub.py --out /tmp
python3 guide/build-guide-epub.py --no-verify
```

**逐位元組可重現。** 封存時間戳釘死在 `GUIDE_EPOCH`(1.0.0 release commit 的 `%ct`),
不像 `scripts/mk-release.sh` 那樣跟著 HEAD 跑——書的位元組應該是**內容**的函數,跟著
HEAD 走的話,一個無關的 commit 就會讓進版控的 `.epub` 變成過期檔。連建兩次比 sha256
可以自己驗。

改完內文記得**重建並把 `.epub` 一起 commit**,否則 Release 拿到的是舊書。

## 驗證在驗什麼

這本書講的每一件事都是裝置操作,而它的註腳那一章只有在標記跟 `lib/Epub` 真正解析的
形狀一致時才會動。所以建置的時候會拿**同一棵樹裡的韌體原始碼**去對:

| 檢查 | 依據 |
|---|---|
| 每一條 `設定 → …` 都是畫面上的原字 | `lib/I18n/translations/zh-hant.yaml`,逐段逐字比對 |
| 註腳 `<a href>` 解析得到目標 | `Epub::resolveHrefToSpineIndex`(比對檔名,不比對整條路徑) |
| 目標 `id` 真的會被記成 anchor | `ChapterHtmlSlimParser`——`<span>` 上的 `id` 一律不記 |
| 書名與每一條目錄文字都畫得出來 | 目錄與書名走**內建 UI 字型**(7,413 漢字),不是 SD 內文字型 |
| `FootnoteEntry` 的固定緩衝沒有溢位 | href 96 bytes、標籤 32 bytes |
| 兩張 PNG 在解碼器接受的範圍內 | `lib/PngToBmpConverter`:非交錯、色彩型別、尺寸、列長 |
| OPF / spine / manifest 對得起來,XHTML 都 well-formed | OCF 與 EPUB 3 基本結構 |

驗證本身做過變異測試:把 href 改成不存在的檔、把 `id` 移到 `<span>` 上、在目錄放一個
BIG5 以外的字(喆)、拿掉 EPUB 2 的 `<meta name="cover">`、把 PNG 標成交錯、把
「文字反鋸齒」寫成「文字抗鋸齒」——每一種都會變紅並指名。**這裡全綠不代表它在 X3 上
一定對**,只代表它不會因為已知的那幾種原因錯。裝置行為(註腳真的跳過去、書籤真的存下來)
只能在真機上驗。

## 幾個決定,還有理由

**檔名 `歡迎使用CrossMosa.epub`。** 韌體自己就會寫出中文檔名——`StringUtils::sanitizeFilename`
保留所有 ≥128 的碼位,OPDS 下載的檔名格式預設就是「作者-書名」。「不可以有空格」那條規則
講的是 `/.fonts/` 底下的**資料夾**名稱(上游 issue #2248),不是書檔;這個檔名裡本來也沒有
空格。開頭不是句點,所以不會被當成隱藏檔或受保護路徑。26 bytes,離 100 bytes 的上限很遠。

**用 `zipfile` 直接寫,不用 ebooklib。** 上游的 `scripts/generate_userguide_epub.py` 用
ebooklib,但那個套件不在 `scripts/requirements.txt` 裡;而且直接寫 zip 才控制得了 entry
順序與時間戳,也就是可重現的前提。EPUB 骨架照抄上游那支腳本驗過的形狀(EPUB 2 的
`<meta name="cover">` 加 EPUB 3 的 `properties="cover-image"`,nav 不放進 spine)。

**nav 與 NCX 兩份都放。** `Epub.cpp` 先找 EPUB 3 nav,失敗才退回 NCX。兩份加起來多一千
多個 bytes,換掉一個單點故障。

**註釋放在最後一個獨立檔案,不放在第 5 章末尾。** 註腳跳轉要看得出來跳了,目標就不能跟
連結落在同一頁——而第 5 章短,同頁的機率不低。放進 `90-notes.xhtml` 之後,跳轉一定跨檔,
短按返回鍵一定回得來。代價是讀完第 12 章再翻一頁會看到「註釋」,那是書末註的正常位置。

**封面的中文用 `fonts/ui-subset/UI-NotoSans-Bold.otf`。** 那個子集就在這棵樹裡,所以誰
建都長一樣,不用賭機器上裝了什麼 CJK 字型。

**第 6 章的圖是自己畫的灰階圖表,不是螢幕截圖。** 那一章的主張是「這塊螢幕只有四階灰」,
圖表能證明,截圖不能。

## 內容規則

- 每一章結尾恰好一句指示,用 `<p class="do"><strong>現在:…</strong></p>`。
- 每一條 `設定 → …` 路徑**一定要包在 `<strong>` 裡**,而且每一段都要跟
  `lib/I18n/translations/zh-hant.yaml` 的字串逐字相符——建置會擋。`<strong>` 既是排版
  慣例,也是驗證抓得到路徑的原因。改 UI 字串的時候,這本書要一起改。
- 引號裡的畫面文字(「續讀」「已加入書籤。」「忽略」…)沒有自動檢查,改動時請自己對過。
- 樣式不承載意義。讀者可以關掉內嵌樣式(設定 → 閱讀器 → 內嵌樣式),所以強調一律用
  `<strong>`,結構一律用 `<h1>` / `<p>` / `<li>`。
- 只用 `p`、`li`、`div`、`br`、`blockquote`、`h1`–`h6`、`b`/`strong`、`i`/`em`、
  `u`/`ins`、`img`——那是 `ChapterHtmlSlimParser` 認得的全部。
