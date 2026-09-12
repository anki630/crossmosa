// 直排（縦書き）的排版路徑。
//
// 規格是 `tools/vertical-oracle/vlayout.py`（已在像素上驗過的 V0 桌面預言機）；
// 分欄演算法在 `VerticalColumns.h`（純中文已與 V0 逐字比對通過）；
// 碼位表與幾何常數在 `VerticalText.h`。決策與依據在帳本，搜「中文直排」。
//
// ⚠️ **為什麼是平行迴圈而不是在 extractLine 加分支**（帳本「V1 插入點測繪」）：
//    extractLine 已 421 行、三個互斥定位分支，而它的 DP 目標函式是 remainingSpace²
//    ——那是為【兩端對齊】設計的，而直排不做兩端對齊，那個最佳化目標在直排沒有意義。
//    參數化推進軸則要 touch 700–800 行，每一行都在既有書庫每一本都必走的路徑上，
//    而這個檔案正是 v61／v62／v139／v148-151 全部 crash 的所在地。
//
// ⚠️⚠️ **不要在本函式開頭加 `if (oom_) return;`**（ParsedText.h:28-30 的警告）：
//    v139 在橫排版本做過，跳過了尾端的「Remove consumed words」，
//    一次暫時性的記憶體拒絕變成正回饋迴圈，**每本書都打不開**。
//    本函式的尾段做同一件事，同樣不可被早退跳過。

#include <esp_heap_caps.h>

#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include "ParsedText.h"
#include "VerticalColumns.h"
#include "VerticalEm.h"
#include "VerticalText.h"

namespace {

// 一個 token 在直排裡的分類結果。**在排版階段決定，繪製端只查表** ——
// 因為 TextBlock::render 一頁跑 16 次（掃描 1 ＋ BW 1 ＋ 灰階 7 帶 × 2 平面 14）。
struct TokenPlan {
  // ⚠️ **切分判斷之前不得截斷。** 原本是 uint16：一個 >65,535 px 的 token
  //    （長 URL、無空白的 ASCII、壞掉的 EPUB）會迴繞成小值 → 被判定「放得下」
  //    → 整段畫到版心外 ＝ 掉字。截斷要發生在【切分之後】，那時每片都保證 ≤ 一欄。
  // ⚠️⚠️ **沿欄推進量必須是浮點。** 逐 token 取整看起來無害（em 45.8125 → 46），
  //    但格線是 `charsPerColumn × em` 算的：15 格 ＝ 687.19，而 15 個取整過的格子是
  //    15 × 46 = 690 > 687.19 → **每一欄都少放一個字**。誤差每格 +0.19px，
  //    累積到第 15 格就越過門檻（複查算出來的，22pt 實例）。
  //    ⚠️ 旋轉與詞間空白仍是【量出來的整數像素】，那本來就是整數，不受影響。
  float advance;      // 沿欄推進量
  int32_t crossPx;    // 跨軸（欄內）位移
  bool rotated;        // clreq §2.1.2 ②：整串順時針旋轉
  bool splitPerChar;   // clreq §2.1.2 ①：直立逐字排（單字母／數字／縮略詞）
  bool tateChuYoko;    // clreq §2.1.2 ③：兩到三位數併進一格
};

// 這一段整段是西文嗎？**逐碼位判斷，不是逐位元組。**
// ⚠️ 舊版叫 isAsciiToken、只看位元組 < 0x80 —— 於是 `Golěm` 這種詞（切段之後仍是一段）
//    會在這裡被判成「非西文」→ 落到全形那條 → 整串直立占格。
//    切段改成西文判準之後，這裡也必須跟著改，否則只搬動了缺陷的位置。
bool isWesternToken(const std::string& s) {
  if (s.empty()) return false;
  const char* p = s.c_str();
  const char* const end = p + s.size();
  while (p < end) {
    const uint32_t cp = vtext::nextCodepoint(p, end);
    if (cp == 0) break;
    if (!vtext::isWesternCodepoint(cp)) return false;
  }
  return true;
}

// 一個字串的【最後一個碼位】。往回跳過續接位元組即可，不必從頭掃。
uint32_t lastCodepointOf(const std::string& t) {
  if (t.empty()) return 0;
  size_t i = t.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(t[i]) & 0xC0) == 0x80) --i;
  const char* q = t.c_str() + i;
  return vtext::nextCodepoint(q, t.c_str() + t.size());
}

int countCodepoints(const std::string& s) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
  int n = 0;
  while (utf8NextCodepoint(&p)) ++n;
  return n;
}

// 一串【純 ASCII】文字的墨水左右界，相對於起筆點。回傳 false ＝ 一個字形都取不到。
//
// ⚠️ 起筆點必須用 `getTextAdvanceX` 量前綴，**不可以把單字寬度自己相加** ——
//    內建字型有字距對與連字，寬度不是逐字可加的（GfxRenderer.h:330 的但書）。
//    只有縦中横會呼叫它，token 長度 ≤ 4，成本可忽略。
bool asciiRunInkExtent(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style,
                       const char* s, const int len, int* outLeft, int* outRight) {
  bool any = false;
  int lo = 0;
  int hi = 0;
  std::string prefix;
  for (int i = 0; i < len; ++i) {
    const int pen = i == 0 ? 0 : renderer.getTextAdvanceX(fontId, prefix.c_str(), style);
    int gw = 0, gh = 0, gl = 0, gt = 0;
    if (renderer.getGlyphInkBox(fontId, static_cast<uint32_t>(static_cast<unsigned char>(s[i])), style, &gw, &gh,
                                &gl, &gt) &&
        gw > 0) {
      const int a = pen + gl;
      const int b = a + gw;
      if (!any) {
        lo = a;
        hi = b;
        any = true;
      } else {
        if (a < lo) lo = a;
        if (b > hi) hi = b;
      }
    }
    prefix.push_back(s[i]);
  }
  *outLeft = lo;
  *outRight = hi;
  return any;
}

// 直排的字形替換。**這是 `verticalForm()` 在韌體裡唯一的呼叫點** ——
// 在此之前那張 24 個碼位的表沒有任何呼叫者，`cross_check` 驗的是「表與 V0 一致」，
// 不是「表有被用到」（B-22 的另一種面貌：儀器／資料要先證明自己會被執行）。
// 沒有它，直排書裡的「」（）〔〕 全部維持橫排方向。
//
// ⚠️ **句讀不換形。** FE10–FE16 是右上角形（中國大陸／日本慣例），
//    台灣的教育部標準是【居正中】—— `isCenteredPunctuation` 就是那九處明文。
//    這個運算式與 `tools/vertical-oracle/cross_check.cpp` 逐字相同，所以桌面比對
//    驗到的就是這裡實際會做的事。
// ⚠️ **旋轉的判斷不可以做在 token 這一層。**（2026-09-08 實機：破折號顯示成「一一」）
//    原本要求「整個 token 只有一個碼位」才旋轉 —— 但 `─` 是行首禁則，切詞期會把它
//    **黏在前一個字後面**（追い出し），於是 token 是「事─」兩個碼位，永遠不符合條件，
//    直立畫出來就是一條橫線 ＝ 看起來像「一」。
//    → 改到【逐字元產出】那一層判斷，見下方 colWords 的迴圈。

// clreq §2.1.2 的三種配置，在這裡一次決定完。
// `inWesternPhrase` ＝ 這個 token 的左右鄰居裡有西文詞（中間補了詞間空白）。
// ⚠️ **clreq ①（直立逐字）講的是「夾在中文裡的」單一字母／縮略詞**，不是西文句子裡的短字。
//    實機：`（Praha a okolí）` 這種括號裡的西文串，**`a`** 站了起來，
//    左右的字全躺著 —— 因為單獨看它就是「單一西文字母」。
//    同一條也適用於縮略詞與數字：`the FBI agent`／`Volume 10 of` 在句子裡要跟著躺。
//    → 在西文句子裡一律走 ②（整串旋轉），基線才會一致。
TokenPlan planToken(const GfxRenderer& renderer, const int fontId, const std::string& tok,
                    const EpdFontFamily::Style style, const float em, const bool inWesternPhrase,
                    const int rotatedCrossPx) {
  TokenPlan p{};
  const int emPx = static_cast<int>(em + 0.5f);

  if (isWesternToken(tok)) {
    const int len = static_cast<int>(tok.size());
    // ⚠️ **縦中横要排在直立逐字【之前】判斷。** clreq 2.1.2 把兩到三位數的
    //    縦中横（③）與單字母／縮略詞的直立逐字（①）分開，而數字兩者都符合，
    //    先判到哪個就是哪個。列點的「1.」與「第10章」的「10」都該走 ③。
    const auto cls =
        inWesternPhrase ? vtext::WesternRunPlan::Rotated : vtext::classifyWesternRun(tok.c_str(), len);
    if (cls == vtext::WesternRunPlan::TateChuYoko) {
      // ③ 縦中横：整組塞進一個 em 格、跨軸置中。
      //
      // ⚠️ **要用【墨水】不是【前進量】。** 兩件事都靠它：
      //    (a) 置中 —— 句點的墨水只占它自己前進量的左半，照前進量置中會整組偏左；
      //    (b) 放不放得下 —— 兩位數的**前進量**約 1.0–1.2 em（看起來超格），
      //        但**墨水**只有 0.93–1.17 em；用前進量判會把該併的判成不該併。
      //    這與句讀居中用同一個機制（`getGlyphInkBox`），理由也同一個：
      //    「正中央在哪」是字形自己的外框決定的，不是可以猜的常數。
      int inkL = 0;
      int inkR = 0;
      const bool haveInk = asciiRunInkExtent(renderer, fontId, style, tok.c_str(), len, &inkL, &inkR);
      const int advPx = renderer.getTextAdvanceX(fontId, tok.c_str(), style);
      const int inkW = haveInk ? (inkR - inkL) : advPx;
      const bool fits = static_cast<float>(inkW) <= em * vtext::TATE_CHU_YOKO_MAX_INK_EM;
      if (ParsedText::vertTcyLogged < 4) {
        ++ParsedText::vertTcyLogged;
        ParsedText::vertDiag("VERTTCY \"%s\" em=%d adv=%d ink=%d..%d fits=%d", tok.c_str(), emPx, advPx, inkL,
                             inkR, fits ? 1 : 0);
      }
      if (fits) {
        p.tateChuYoko = true;
        p.advance = em;
        if (haveInk) {
          // crossOff 最後存進 uint16 的 colCross，所以夾在 0 以上；
          // 墨水比一格寬時只往右溢（＝退回舊行為），不會迴繞成巨值。
          const int centred = (emPx - inkW) / 2 - inkL;
          p.crossPx = centred > 0 ? centred : 0;
        } else {
          p.crossPx = advPx < emPx ? (emPx - advPx) / 2 : 0;
        }
        return p;
      }
      // 放不下 → 退回 ① 直立逐字（clreq 的另一條規範路徑），不是硬塞。
      // ⚠️ **不能只是「往下掉」**：`isUprightLatinRun("123")` 是 false（沒有字母），
      //    往下掉會落到 ② 整串旋轉 ＝ 一串數字躺著，比塞不下還糟。
      p.splitPerChar = true;
      p.advance = em * static_cast<float>(countCodepoints(tok));
      return p;
    }
    if (cls == vtext::WesternRunPlan::UprightPerChar) {
      // ① 直立逐字排：每個字母佔一個 em 格。crossOff 在切開後逐字算。
      // ⚠️ 格數是【碼位】數不是位元組數 —— 單一的 é 是兩個位元組、一格。
      p.splitPerChar = true;
      p.advance = em * static_cast<float>(countCodepoints(tok));
      return p;
    }
    // ② 整串順時針旋轉：沿欄推進量 ＝ 該串的橫向總寬。
    p.rotated = true;
    p.advance = static_cast<float>(renderer.getTextAdvanceX(fontId, tok.c_str(), style));
    // ⭐⭐ **跨軸位移是【固定的】，與這個詞有哪些字母無關。**（維護者 2026-09-11 回報
    //     「有些字往左靠一點點，有些字往右靠一點點」）
    //
    //     先前是拿【這個詞自己的墨水外框】去置中。那讓每個詞的「墨水中心」一致，
    //     但**基線各自不同** —— 因為外框取決於那個詞剛好有沒有上伸部／下伸部。
    //     用真的 .cpfont 算（NotoSerifTC 22pt、em 46）：
    //         大寫＋長音 2 / 有上伸部 4 / 有上伸部 4 / 大寫 5 / 大寫 5 / 大寫 6 /
    //         小寫 7 / 有下伸部 10 / 單字母 11 / 有下伸部 16（同一段裡十個相鄰的詞）
    //     → 基線散布 2..16，**最大差 14 px ＝ em 的 30%**。使用者看到的就是這個。
    //
    //     ⚠️ 註解原本寫「同一串的字母必須共用一條基線」—— 那個意圖只做到了
    //     **run 內部**（切段時各段共用 plan.crossPx），run 與 run 之間從來沒有。
    //     一句沒有被機制保證的註解，就只是一句註解（本專案的老毛病）。
    //
    //     新規則：基線固定在字格的 `1 − CELL_ASCENT_FACTOR` 處，一次算好、所有詞共用。
    //     ⚠️ 不要再寫「JLREQ 規定置中」—— 複查核對過原文，**JLREQ 與 clreq 都沒有這句**。
    //        依據是 CSS 的 central baseline（＝字身框中點）與本檔量出來的 CELL_ASCENT_FACTOR。
    p.crossPx = rotatedCrossPx;
    return p;
  }

  // 全形（漢字／句讀／FE 直排形）：一律一個 em 格。
  // ⚠️ 推進量【不是】量出來的 advance —— 那是橫排的量。實測 advanceY 在五套字型是
  //    42/47/60/60/83 而 em 恆為 41.688，差到 ±100%（帳本更正①）。
  // ⚠️ 用【浮點 em】不用整數 emPx：分欄拿 advance 判斷、繪製拿 em 定位，
  //    兩把尺不同就會累積誤差（46 vs 45.8125，一欄 15 格差 2.8 px）。
  const int chars = countCodepoints(tok);
  p.advance = em * static_cast<float>(chars);
  return p;
}

}  // namespace

void ParsedText::layoutAndExtractColumns(
    const GfxRenderer& renderer, const int fontId, const uint16_t columnLength,
    const std::function<void(std::shared_ptr<TextBlock>, uint32_t, int)>& processColumn, const bool includeLastColumn) {
  if (words.empty()) return;

  // ⚠️⚠️ **任何早退都不可以只是 `return`。**（ParsedText.h:28-30 / 本檔檔頭的 v139 教訓）
  //    呼叫端（ChapterHtmlSlimParser）看到「沒有產出任何欄」就會再排一次同一段 →
  //    words 沒被消耗 ＝ 正回饋迴圈 ＝ 每本書都打不開。橫排版本因此付過一次代價。
  //    直排的早退條件（字型量不到 em、版心比一個字還短）雖然罕見，但形狀完全相同。
  //    → 走不下去就【把整段吃掉】並記錄。掉一段文字很糟，但比整本書打不開好，
  //      而且會在 diag.log 留下可歸因的一行。
  const auto consumeAllAndBail = [this](const char* why) {
    ParsedText::vertDiag("VERTBAIL %s words=%u", why, static_cast<unsigned>(words.size()));
    const size_t n = words.size();
    words.clear();
    wordStyles.clear();
    wordContinues.clear();
    wordNoSpaceBefore.clear();
    wordIsFocusSuffix.clear();
    eraseVisibleOffsetPrefix(n);
    rubyTexts.clear();
  };

  // ⚠️ 必須自己算 —— 先前這個成員只在橫排的 layoutAndExtractLines 裡賦值，
  //    直排讀到的永遠是建構子的 false（複查抓到，v215 未上線前）。
  updateNaturalAlign();

  // SD 字型的 advance 預熱（只載 advance 不載點陣）——與橫排同，這一段是軸無關的。
  renderer.ensureSdCardFontReady(fontId, words, false);

  // em 的探測有備援鏈：U+3000 表意空格 → U+4E00「一」（任何中文字型都有）。
  // ⚠️ 原本只探 U+3000 就放棄 —— 而字型【可以】沒有它（SD 字型是我們自己產的，
  //    字集由 --additional-intervals 決定，表意空格不在常用字表裡是完全可能的）。
  const int32_t emFP = vtext::probeEmFP(renderer, fontId);
  const float em = static_cast<float>(emFP) / 16.0f;
  if (em <= 1.0f) {
    consumeAllAndBail("no-em");
    return;
  }
  const int emPx = static_cast<int>(em + 0.5f);

  // ⚠️⚠️ **`GfxRenderer::drawText` 的 y 是【行頂】不是基線** —— 它內部自己加
  //    `getFontAscenderSize()`（GfxRenderer.cpp:610）。而 `drawTextVerticalCW`
  //    是 `int lastBaseY = y;`，**直接當原點、不加**（GfxRenderer.cpp:2406）。
  //    同一個 render 迴圈裡兩套慣例。
  //    v211 之前我對兩者都送「已經加過 cellAscent 的值」→ 直立的字被多推一個
  //    ascender、旋轉的字沒有 → 整欄下移壓到狀態列，而破折號留在原位橫穿旁邊的字。
  //    **一個 bug 兩個症狀**，而且 ascender 隨字級放大 → 26pt 特別嚴重（實機回報）。
  //    → 直立的字在排版階段先把 ascender 扣掉，讓 drawText 加回來剛好抵銷。
  const float ascender = static_cast<float>(renderer.getFontAscenderSize(fontId));

  // ⭐⭐ **旋轉西文的跨軸位移：整個區塊一個常數，＝ 基線在字格裡的位置。**
  //
  //     `cross = em × (1 − CELL_ASCENT_FACTOR)`。
  //
  //     旋轉之後「基線上方」對到 **+x**（VerticalText.h 的 verticalCwScreenX：
  //     `screenX = cursorX + top − glyphY`，字頂朝右），所以把西文的 em 盒
  //     [−0.12em, +0.88em] 擺進字格 [0, em] 的解就是 cross = 0.12 em。
  //
  //     ⚠️⚠️ **錨點必須是【字身框】不是【行框】。**（2026-09-11 三路對抗複查一致駁回
  //     我原本的 `(em − (ascender + descender))/2`。）Noto TC 兩套的 asc−desc 是
  //     **1.44–1.48 em**（NotoSansTC 22pt：54 −(−14) = 68 px，而 em 只有 45.81）——
  //     那是 CJK 的行高，跟西文墨水無關。把它置中會讓**每一個西文詞相對漢字左偏**
  //     4–10 px（大字版 28pt 最嚴重，−8.6／−9.8）。
  //     ⭐ `CELL_ASCENT_FACTOR` 是**同一個物理量在沿欄軸的答案**，而且是量出來的
  //     （103 個漢字、σ 1.5%，見 VerticalText.h）。同一個量在兩軸用兩個來源，
  //     正是這個 bug 的形狀 —— 所以兩軸共用它。
  //     交叉驗證（複查跑的）：這個錨點讓**拉丁大寫的墨水中心**落在格心 ±1.4 px 內
  //     （原方案是 −1.8…−2.9）。兩個互不相干的判準（漢字墨水置中、拉丁大寫置中）
  //     指向同一個 0.88。
  //
  //     ⚠️ 小寫字身仍會比漢字中心偏左約 0.1 em。**那不是缺陷是常態**——
  //     橫排也一樣（基線對齊時小寫本來就「低」）。要再加光學位移的話請當成
  //     具名常數、寫明是品味決定，**而且要在同一版決定**：cross 會烤進 SD 快取，
  //     每改一次錨點就是整個書庫重排一次。
  //
  //     ℹ️ 規範查證（複查核對原文）：clreq §2.1.2 ② 與 JLREQ 都**只規定旋轉與前後空白，
  //     沒有規定跨軸位置**。唯一的形式定義是 CSS：直排的 dominant baseline ＝ central，
  //     而 central ＝ ideographic under／over 的中點 ＝ **字身框**。
  const int rotatedCrossPx = [&] {
    const int centred = static_cast<int>(em * (1.0f - vtext::CELL_ASCENT_FACTOR) + 0.5f);
    return centred > 0 ? centred : 0;
  }();

  const auto grid = vtext::makeColumnGrid(static_cast<float>(columnLength), em);
  // ⭐ **懸掛的讓格必須在這裡決定，不能等到分欄之前。**
  //    先前 `maxCells` 用讓格【前】的 grid（18 格）算，而欄實際用 grid2（17 格）→
  //    一個 18 格的 token 被判定「放得下」，實際超出正好一格。
  //    實機 v216 的 `VERTCLIP col=115 k=17 top=709 gridLen=708` 逐位元組吻合這個算術。
  //
  // JLREQ 3.8.2：「**版面に接して**，指定の行長よりはみ出して配置する」——
  // 懸掛的字是【貼著版心邊】，所以需要的餘量只是那個字的**墨水高度**，
  // 而且是用 getGlyphInkBox 從**實際字形**量的（五套字型的句讀墨水高度不同）。
  float hangNeed = 1.0f;
  for (const uint32_t hc : {0x3001u, 0xFF0Cu, 0x3002u}) {
    int gw = 0, gh = 0, gl = 0, gt = 0;
    if (renderer.getGlyphInkBox(fontId, hc, EpdFontFamily::REGULAR, &gw, &gh, &gl, &gt)) {
      if (static_cast<float>(gh) > hangNeed) hangNeed = static_cast<float>(gh);
    }
  }
  // 餘量不夠就讓出一格來換懸掛（使用者明確選了懸掛，「只有某些字級有效」更難用）。
  auto grid2 = grid;
  if (grid2.hangRoom < hangNeed && grid2.charsPerColumn > 2) {
    grid2.charsPerColumn -= 1;
    grid2.columnLength = static_cast<float>(grid2.charsPerColumn) * em;
    grid2.hangRoom = static_cast<float>(columnLength) - grid2.columnLength;
  }
  const bool hangAllowed = grid2.hangRoom >= hangNeed;
  if (!vertHangLogged) {
    vertHangLogged = true;
    ParsedText::vertDiag("VERTHANG allowed=%d room=%d need=%d cells=%d->%d", hangAllowed ? 1 : 0,
                         static_cast<int>(grid2.hangRoom + 0.5f), static_cast<int>(hangNeed + 0.5f),
                         grid.charsPerColumn, grid2.charsPerColumn);
  }

  // ⚠️ **用 grid2** —— 切 token 的上限必須與欄實際的容量一致。
  const int maxCells = vtext::unitsPerColumn(grid2, em);
  // 幾何證人：每次建置印一次。**不要從畫面反推這些數字** —— v206 就是因為只能從截圖
  // 量而卡在「欄長對不對」的猜測上。em ×10 是為了避開浮點格式。
  const int geoKey = fontId * 1000 + emPx;
  if (vertGeoLogged < 8 && vertGeoKey != geoKey) {
    ++vertGeoLogged;
    vertGeoKey = geoKey;
    // ⚠️ 必須用 DiagLog：這台沒有序列埠，LOG_ERR 等於丟掉。
    //    v207 就是這樣白裝的 —— VERTGEO 一行都沒進 diag.log。
    ParsedText::vertDiag("VERTGEO colLen=%u em10=%d cells=%d gridLen=%d asc=%d rotcross=%d",
                         static_cast<unsigned>(columnLength), static_cast<int>(em * 10.0f + 0.5f),
                         grid.charsPerColumn, static_cast<int>(grid.columnLength), static_cast<int>(ascender),
                         rotatedCrossPx);
  }
  if (maxCells < 1) {
    consumeAllAndBail("column-too-short");
    return;
  }

  // ── 分批（bounded scratch）────────────────────────────────────────────
  // ⚠️ **一次只物化一批 token 的 units，不是整段。**
  //    units 這組平行陣列每個單元約 22 bytes；閱讀時唯一能供應大塊的池上限約 53 KB
  //    → 約 2,400 個單元就見底，而堆積耗盡在 `-fno-exceptions` 下是 abort、不是失敗。
  //    自己的書從沒觸發過（VERTBIG 一次都沒印），但 RC 要給別人的書用，
  //    「量不到就不知道」不能當成護欄。
  //  ⭐ 為什麼分批是安全的：每一批都**只產出完整的欄**（includeLast=false），
  //     最後那個沒排完的欄留著不消耗 —— 這正是橫排 `layoutAndExtractLines` 的既有契約，
  //     所以欄不會在批次邊界被切斷，也不會多出一個欄界。
  constexpr size_t CHUNK_WORDS = 800;            // 粗篩
  constexpr size_t MAX_UNITS_PER_CHUNK = 1000;   // 真正的上限：約 26 KB
  bool firstChunk = true;
  while (!words.empty()) {
    const bool lastChunk = words.size() <= CHUNK_WORDS;
    const size_t wordBudget = lastChunk ? words.size() : CHUNK_WORDS;
    bool includeLast = lastChunk && includeLastColumn;
    const size_t wordsBefore = words.size();
    // 證人：分批只在超過 CHUNK_WORDS 的段落發生。一般段落是單批，行為與分批前逐位元組相同 ——
    // 所以這一行【沒有印】就代表新路徑沒被走到，也就代表這個改動對你的書零影響。
    if (!lastChunk || !firstChunk) {
      ParsedText::vertDiag("VERTCHUNK words=%u budget=%u first=%d last=%d",
                           static_cast<unsigned>(wordsBefore), static_cast<unsigned>(wordBudget),
                           firstChunk ? 1 : 0, lastChunk ? 1 : 0);
    }

  // ⚠️ **縮排必須在建 units【之前】算好** —— 切片上限要扣掉它佔的格數，
  //    否則第一欄的第一個 token 會被強制放到版心外（掉字）。
  float indent = 0.0f;
  if (firstChunk && !isNaturalAlign && vertIndGateLogged < 3) {
    // 證人：閘門擋下了一個非自然對齊的區塊（置中的標題／圖說／詩句）。
    // 它【有印】才證明閘門會分辨；恆不印 ＝ isNaturalAlign 又變成死值了。
    ++vertIndGateLogged;
    ParsedText::vertDiag("VERTINDGATE align=%d words=%u", static_cast<int>(blockStyle.alignment),
                         static_cast<unsigned>(words.size()));
  }
  if (firstChunk && !verticalIndentApplied && isNaturalAlign) {
    if (blockStyle.textIndentDefined) {
      indent = static_cast<float>(blockStyle.textIndent);
    } else {
      const auto* p0 = reinterpret_cast<const unsigned char*>(words[0].c_str());
      if (!vtext::startsWithEmbeddedIndent(utf8NextCodepoint(&p0))) {
        indent = vtext::FIRST_LINE_INDENT_EM * em;
      }
    }
    // ⚠️ 進了這個分支就算「這個區塊的首行縮排已經決定過了」——
    //    包含「刻意不縮」（書本來就打了全形空格）那條路，否則下一趟會補上縮排。
    verticalIndentApplied = true;
  }
  // ⚠️ **上下都要夾。** `textIndent` 是 BlockStyle 裡唯一沒有夾限的長度
  //    （BlockStyle.h 同一函式的 marginLeft/paddingLeft 都夾了 MAX_HORIZONTAL_INSET）。
  //    出版社寫 `text-indent: 10em` 就是 10 × 字級 —— 沒有上限的話第一個 token 會被
  //    推到版心外靜默掉字。這條在複查當下是死碼（閘門恆假），修好閘門之後就活了。
  //    上限取半欄：縮排超過半欄在任何排版規範裡都不是「縮排」。
  if (indent < 0.0f) indent = 0.0f;  // 懸掛縮排（負值）直排不做
  const float maxIndent = static_cast<float>(columnLength) * 0.5f;
  if (indent > maxIndent) indent = maxIndent;

  // ── 建 units ────────────────────────────────────────────────────────────
  // units 與 token 盡量 1:1（省記憶體：這台機器 p2 的可用上限約 53 KB，
  // 而一段可能有數千個 token）。只有兩種情況會拆：
  //   ① clreq ① 的直立逐字排（縮略詞拆成單字母）
  //   ② 比一整欄還長的 token —— **必須拆，否則掉字或空轉**（VerticalColumns.h 檔頭）
  // ⚠️ 記憶體：units 這組平行陣列每個單元約 22 bytes（ColumnUnit 12 ＋ 三個 uint16 ＋
  //    alongOff 4）。閱讀時唯一能供應大塊的池上限約 53 KB → 約 2,400 個單元就吃光。
  //    一「段」有 2,400 字很罕見，但**我們量不到就不知道有沒有發生**
  //    （教訓：量不到的風險，儀器要跟改動同一版出貨）。所以這裡先估、超標就記一行。
  //    真正的解是「一次只物化一頁的 bounded scratch」，那是 V1b 的重構，不是這一版。
  if (words.size() > CHUNK_WORDS) {
    ParsedText::vertDiag("VERTBIG words=%u est=%uB maxblk=%uB", static_cast<unsigned>(words.size()),
            static_cast<unsigned>(words.size() * 22),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
  }
  std::vector<vtext::ColumnUnit> units;
  std::vector<uint16_t> unitSrcWord;   // 這個 unit 來自哪個 token
  std::vector<uint16_t> unitByteBegin;  // token 內的起始 byte
  std::vector<uint16_t> unitByteLen;
  // ⚠️ **必須是 wordBudget 不是 words.size()** —— `reserve(n)` 精確配置 n 個元素，
  //    用 words.size() 的話第一批的峰值配置與分批前一模一樣，護欄買到 0（複查抓到）。
  units.reserve(wordBudget);
  unitSrcWord.reserve(wordBudget);
  unitByteBegin.reserve(wordBudget);
  unitByteLen.reserve(wordBudget);

  // 這個 token 的結尾可不可以懸掛？
  // clreq 6.1.3：「適合行尾懸掛的標點符號有頓號、逗號及句號」、
  //             「通常，行尾只可懸掛一個標點符號」；
  // 編輯草案更明確：「連續多個標點符號的情況下，不作行尾點號懸掛的配置」。
  // → 條件：多於一個碼位、最後一個可懸掛、而且**倒數第二個不是句讀**。
  const auto hangTailOf = [&](const std::string& tok) -> float {
    const auto* q = reinterpret_cast<const unsigned char*>(tok.c_str());
    uint32_t prev = 0, last = 0;
    int n = 0;
    while (const uint32_t c = utf8NextCodepoint(&q)) {
      prev = last;
      last = c;
      ++n;
    }
    if (n < 2 || !isHangablePunctuation(last)) return 0.0f;
    if (isHangablePunctuation(prev)) return 0.0f;  // 連續標點不掛
    return em;
  };

  // ⚠️ `glued` ＝ 這個 unit 不可與前一個分開（禁則）。預設 false，因為禁則本來是靠
  //    「切詞期不產生切點、黏成同一個 token」表達的 —— 但**我們自己又把 token 拆開了**：
  //    混合 token 依 ASCII 邊界切段、縮略詞逐字拆。拆完不黏回去，`fillColumns` 就可以
  //    斷在「第|10|章」中間，等於把切詞期建立的禁則毀掉（複查抓到）。
  //    ⚠️ **長度切片那兩條路【不能】黏** —— 它們本來就是為了跨欄而切的。
  const auto pushUnit = [&](const size_t w, const size_t b0, const size_t blen, const float adv, const int cross,
                            const vtext::UnitKind kind, const float hangTail = 0.0f, const bool glued = false) {
    vtext::ColumnUnit u{};
    u.advance = adv;
    u.crossOff = static_cast<float>(cross);
    u.kind = kind;
    u.hangTail = hangTail;
    u.gluedToPrev = glued;
    units.push_back(u);
    unitSrcWord.push_back(static_cast<uint16_t>(w));
    unitByteBegin.push_back(static_cast<uint16_t>(b0));
    unitByteLen.push_back(static_cast<uint16_t>(blen));
  };

  // ⚠️ **真正的上限掛在 unit 數，token 數只是粗篩** —— 一個 token 可以吐出多個 unit
  //    （直立逐字排把縮略詞拆成單字母、過長的 token 被切片），所以 800 個 token
  //    可能變成數千個 unit。以 unit 計，每個約 26 bytes（ColumnUnit 16 ＋ 三個 uint16
  //    ＋ alongOff 4），1,000 個 ≈ 26 KB，離 53 KB 池底還有餘裕。
  // words[i-1] 與 words[i] 之間要不要補【西文詞間空白】。
  // ⭐ 抽成一個判準的理由：它同時回答兩個問題 ——「這裡要不要補空白」與
  //    「這個 token 在不在一串西文裡」（見 planToken 的 inWesternPhrase）。
  //    寫兩份就會分岔，而分岔的症狀是「空白補了但字沒跟著躺」這種只有實機看得到的東西。
  const auto westernSpaceBefore = [&](const size_t i) {
    if (i == 0 || i >= wordBudget) return false;
    if (i >= wordNoSpaceBefore.size() || i >= wordContinues.size()) return false;
    if (wordNoSpaceBefore[i] || wordContinues[i]) return false;
    if (words[i - 1].empty() || words[i].empty()) return false;
    const uint32_t prevLast = lastCodepointOf(words[i - 1]);
    const char* fp = words[i].c_str();
    const uint32_t curFirst = vtext::nextCodepoint(fp, words[i].c_str() + words[i].size());
    // ⚠️ 判準與切段同一張表（`isWesternCodepoint`），不是「位元組 < 0x80」——
    //    否則 `José Silva` 的空白會因為前一個字以非 ASCII 字母結尾而被丟掉。
    return prevLast != 0 && curFirst != 0 && vtext::isWesternCodepoint(prevLast) &&
           vtext::isWesternCodepoint(curFirst);
  };

  // 把一段（可能是整個 token，也可能是混合 token 切出來的一段）轉成 units。
  // ⚠️ 抽成函式是為了讓混合 token 的每一段走**完全相同**的三分支邏輯 ——
  //    複製一份會立刻分岔（本專案「修缺陷類別不是修實例」）。
  //    b0/blen 是相對母 token 的位元組範圍，pushUnit 存的也是那個座標。
  const auto emitRun = [&](const size_t w, const size_t b0, const size_t blen, const bool segmentIsFirst,
                           const bool inWesternPhrase) {
    const std::string& parent = words[w];
    const std::string run = parent.substr(b0, blen);
    if (run.empty()) return;
    const auto style = w < wordStyles.size() ? wordStyles[w] : EpdFontFamily::REGULAR;
    const TokenPlan plan = planToken(renderer, fontId, run, style, em, inWesternPhrase, rotatedCrossPx);

    if (plan.splitPerChar) {
      // ① 直立逐字：每個 ASCII 字母一格，欄內置中。
      // ⚠️ 置中用【墨水】不是【前進量】——與縦中横同一個病：字母的墨水在自己的
      //    前進量裡不置中（右側有側承），照前進量置中整排會偏左。
      //    實機量到 J/a/s/o/n 的墨水中心落在 389.5–396.5，而欄心是 403（偏左 7–14 px）。
      // ⚠️ 逐【碼位】不是逐位元組：單一的 é／ě 也走這條（clreq ① 的「單一西文字母」）。
      const char* rp = run.c_str();
      const char* const rend = rp + run.size();
      bool firstCp = true;
      while (rp < rend) {
        const char* const cpStart = rp;
        const uint32_t cp = vtext::nextCodepoint(rp, rend);
        if (cp == 0) break;
        const size_t off = static_cast<size_t>(cpStart - run.c_str());
        const size_t clen = static_cast<size_t>(rp - cpStart);
        int gw = 0, gh = 0, gl = 0, gt = 0;
        int cross = 0;
        if (renderer.getGlyphInkBox(fontId, cp, style, &gw, &gh, &gl, &gt) && gw > 0) {
          const int c = (emPx - gw) / 2 - gl;
          cross = c > 0 ? c : 0;
        } else {
          const std::string one = run.substr(off, clen);
          const int w1 = renderer.getTextAdvanceX(fontId, one.c_str(), style);
          cross = w1 < emPx ? (emPx - w1) / 2 : 0;
        }
        pushUnit(w, b0 + off, clen, em, cross, vtext::UnitKind::Cell, 0.0f, !firstCp || !segmentIsFirst);
        firstCp = false;
      }
      return;
    }

    if (plan.rotated) {
      // ② 旋轉：比一整欄還長就切（優先在空白，否則硬切）——不加連字號。
      if (ParsedText::vertWordRotLogged < 3) {
        ++ParsedText::vertWordRotLogged;
        // 實機證人：西文單詞【整串旋轉】這條路真的被走到了嗎？
        // v218 起 `（Jason` 這種混合 token 才會切出 ASCII 段；桌面比對已證明分類正確，
        // 但「分類對」不等於「畫出來對」——這一行是為了下一份 log 能直接回答。
        ParsedText::vertDiag("VERTWORDROT \"%s\" adv=%d", run.c_str(), static_cast<int>(plan.advance + 0.5f));
      }
      // ⚠️ 第一欄的可用長度比 columnLength 少一個縮排（與下面全形那條同一個理由）。
      //    不扣的話，一個剛好塞得下整欄的旋轉串放在縮排後面就會被強制放到版心外。
      const float rotLimit = (units.empty() && indent > 0.0f && indent < grid2.columnLength)
                                 ? grid2.columnLength - indent
                                 : grid2.columnLength;
      if (plan.advance <= rotLimit) {
        pushUnit(w, b0, run.size(), plan.advance, plan.crossPx, vtext::UnitKind::Rotated, 0.0f, !segmentIsFirst);
      } else {
        size_t c0 = 0;
        while (c0 < run.size()) {
          size_t take = run.size() - c0;
          while (take > 1) {
            const std::string piece = run.substr(c0, take);
            if (renderer.getTextAdvanceX(fontId, piece.c_str(), style) <= rotLimit) break;
            const size_t sp = piece.find_last_of(' ');
            size_t next = (sp != std::string::npos && sp > 0) ? sp : take - 1;
            // ⚠️ 退到【碼位邊界】：西文段現在可能含多位元組字母（ě），
            //    切在中間會產出壞掉的 UTF-8 —— 畫出來就是一個替代字元。
            while (next > 1 && (static_cast<unsigned char>(run[c0 + next]) & 0xC0) == 0x80) --next;
            take = next;
          }
          const std::string piece = run.substr(c0, take);
          pushUnit(w, b0 + c0, take, static_cast<float>(renderer.getTextAdvanceX(fontId, piece.c_str(), style)), plan.crossPx,
                   vtext::UnitKind::Rotated);
          c0 += take;
          while (c0 < run.size() && run[c0] == ' ') ++c0;  // 切點的空白不帶到下一欄
        }
      }
      return;
    }

    if (plan.tateChuYoko) {
      // ③ 縦中横：整組一格。
      pushUnit(w, b0, run.size(), plan.advance, plan.crossPx, vtext::UnitKind::TateChuYoko, 0.0f, !segmentIsFirst);
      return;
    }

    // 全形：比一整欄還長就按格數切（禁則是 best-effort，見 VerticalColumns.h 檔頭）。
    //
    // ⚠️ **第一欄的容量比 maxCells 少，因為它前面有首行縮排。** 切片上限若不扣掉，
    //    一個剛好 maxCells 格的 token 放在縮排後面就超出版心 —— 而它是該欄的第一個
    //    unit，`fillColumns` 的無進展守衛會「強制放、允許超出」→ **畫到版心外 ＝ 掉字**。
    //    只有【本區塊的第一個 unit】受影響，之後的欄沒有縮排。
    const int indentCells = units.empty() && indent > 0.0f
                                ? static_cast<int>(indent / em + 0.999f)
                                : 0;
    const int cellLimit = (maxCells > indentCells + 1) ? (maxCells - indentCells) : maxCells;
    const int chars = countCodepoints(run);
    if (chars <= cellLimit) {
      pushUnit(w, b0, run.size(), em * static_cast<float>(chars), 0, vtext::UnitKind::Cell, hangTailOf(run),
               !segmentIsFirst);
      return;
    }
    size_t c0 = 0;
    int done = 0;
    while (done < chars) {
      const int take = (chars - done > cellLimit) ? cellLimit : (chars - done);
      const auto* q = reinterpret_cast<const unsigned char*>(run.c_str() + c0);
      const auto* st = q;
      for (int k = 0; k < take; ++k) utf8NextCodepoint(&q);
      const size_t bl = static_cast<size_t>(q - st);
      pushUnit(w, b0 + c0, bl, em * static_cast<float>(take), 0, vtext::UnitKind::Cell);
      c0 += bl;
      done += take;
    }
  };

  bool unitsTruncated = false;
  for (size_t w = 0; w < wordBudget; ++w) {
    if (units.size() >= MAX_UNITS_PER_CHUNK) {
      unitsTruncated = true;
      break;
    }
    const std::string& tok = words[w];
    if (tok.empty()) continue;

    // ⭐ **詞間空白**：直排原本【整個丟掉】。
    //    中文看不出來（漢字之間本來就沒有空白，切詞期也把它們標成 noSpaceBefore），
    //    但西文姓名會黏成一塊 —— 使用者 2026-09-09 回報「Jason Kaufman 中間的空格
    //    被忽略了？我看到的是連在一起的字」。**是的，被忽略了。**
    //    根因：橫排的空白是 `extractLine` 逐字加上去的（ParsedText.cpp:1071），
    //    而直排是另一條平行路徑，那段沒有對應物 —— 空白從來不是一個 token。
    //
    //    判準與橫排**同一條**：`!noSpaceBefore && !continues`。
    //    ⚠️ 只在【兩邊都是 ASCII】時補：
    //      ① 漢字之間沒有空白可補；
    //      ② 補在漢字側會把後續的字推離 em 格線。
    //      西文與漢字之間的「和欧文間隔」（clreq 的四分空）是另一件事，兩軸都還沒做。
    //    ⚠️ 空白不在任何 token 裡（它是分隔符），所以這個 unit 指向前一個 word 的
    //      尾端、長度 0 → `substr(size, 0)` 是空字串，`drawTextVerticalCW` 直接 return。
    //      它只貢獻推進量。
    if (westernSpaceBefore(w)) {
      const uint32_t prevLast = lastCodepointOf(words[w - 1]);
      const char* fp = tok.c_str();
      const uint32_t curFirst = vtext::nextCodepoint(fp, tok.c_str() + tok.size());
      const auto prevStyle = (w - 1) < wordStyles.size() ? wordStyles[w - 1] : EpdFontFamily::REGULAR;
      const int sp = renderer.getSpaceAdvance(fontId, prevLast, curFirst, prevStyle);
      if (sp > 0) pushUnit(w - 1, words[w - 1].size(), 0, static_cast<float>(sp), 0, vtext::UnitKind::Rotated);
    }

    // ⭐ **混合 token 依「漢字段／西文段」拆開。**
    //    中文裡的數字幾乎都貼著漢字（「第10章」是一個 token），而 isWesternToken 對它
    //    為 false → 走全形那條 → 逐碼位一格 → 「1」「0」上下疊起來（實機回報）。
    //    ⇒ 縦中横對真實內容從來沒生效過。切出西文段之後它才走得到。
    // 這個 token 在一串西文裡嗎（左右任一側有補西文詞間空白）。見 planToken 的註解。
    const bool inWesternPhrase = westernSpaceBefore(w) || westernSpaceBefore(w + 1);
    bool firstSegment = true;
    vtext::forEachWesternSegment(tok.c_str(), tok.size(), [&](const size_t b0, const size_t blen) {
      emitRun(w, b0, blen, firstSegment, inWesternPhrase);
      if (blen > 0) firstSegment = false;
    });
  }

  // ⚠️ unit 被截斷 ＝ 這一批不是最後一批，不論 words 剩多少。
  //    否則 includeLast=true 會把「沒排到的 words」也一起消耗掉 → **掉字**。
  if (unitsTruncated) includeLast = false;

  if (units.empty()) {
    consumeAllAndBail("no-units");
    return;
  }

  // ── 分欄 ────────────────────────────────────────────────────────────────
  // 段首縮排：clreq §6.2.1.1「以兩個漢字的空間為標準」。
  // ⚠️ 但**段落已以全形空格開頭時不再加** —— 日本規範用實體 U+3000 做縮排，
  //    中文書也一樣而且比例更高（實測中文直排書：約三分之二的段落以全形空格開頭，
  //    其中不少書是全部段落都這樣）。兩者相加就變成三格。
  // ── 段首縮排：出版社優先，其次才是中文的預設 ────────────────────────
  //
  // 順序（維護者 2026-09-09 拍板「尊重出版社」）：
  //   ① 非自然對齊（置中／逆流向靠齊）→ **不縮排**
  //      橫排在 `resolveFirstLineIndent`（ParsedText.cpp:639）第一行就是這個閘門，
  //      直排先前【沒有】→ 置中的標題、詩句都被縮了 2 em。
  //   ② CSS 有 `text-indent` → **照出版社說的做**。
  //      值已經是像素（`toPixelsInt16(emSize, vw)`，BlockStyle.h:136），同字級直接可用。
  //      ⚠️ 橫排在 extraParagraphSpacing 開啟時會把【正值】丟掉（改走西式不縮排）；
  //         直排刻意不這樣做 —— 「尊重出版社」的意思就是出版社寫了就照做。
  //   ③ 段落本身以全形空格開頭 → 不再加（否則變三格）。
  //      實測中文直排書約三分之二如此；台灣數位出版聯盟的直排範本
  //      37/37 段都是兩個 U+3000。
  //   ④ 以上皆非 → clreq 6.2.1.1「中文出版品上，段首縮排以**兩個漢字**的空間為標準」。
  //      （日文是一字：JLREQ 3.5.1「全角アキが原則」—— 這是中日確實不同、我們選中文的地方。）
  //
  // ⚠️ 縮排只在段落的第一批算（分批處理見上面 CHUNK_WORDS）。

  // ⭐ 這個字級／版心組合掛不掛得下？**用實際字形的墨水量出來**，不猜比例 ——
  //    五套字型的句讀墨水高度並不相同（。13px、、11px、，13px @22pt）。
  //    居中之後，墨水底距離格頂 ＝ em/2 + 墨水高/2；懸掛的字整格在版心外，
  //    所以需要的餘量就是那個值。餘量只有格線量化剩下的 `hangRoom`，可能是 0。
  auto breaks = vtext::fillColumns(units, grid2, indent, hangAllowed);
    // 平均排列（clreq：「中文書籍各行行頭尾對齊是重要的排版規則」）。
  // 參數是【每個字距允許被撐開的比例】，不是總空白上限 —— 理由見 VerticalColumns.h。
  // 0.15 ＝ 每格最多鬆 15%：一個兩格的詞被推走、攤到十幾個字距上約 +12%，會補平；
  // 而短欄（沒幾個字距可攤）就維持留白，不會出現 v207 那種「這欄明顯比較鬆」。
  vtext::evenDistribute(units, breaks, grid2, 0.15f);

  // 對齊（置中／貼欄尾）。⚠️ 只對【非自然對齊】的區塊做 —— 兩端對齊與順流向維持貼欄頭。
  //    這也是縮排閘門用的同一個判準，兩者共用 `isNaturalAlign` 不會分岔。
  if (!isNaturalAlign) {
    const bool toEnd = blockStyle.isRtl ? (blockStyle.alignment == CssTextAlign::Left)
                                        : (blockStyle.alignment == CssTextAlign::Right);
    vtext::alignColumns(units, breaks, grid2, toEnd);
  }

  size_t columnCount = includeLast ? breaks.starts.size()
                                   : (breaks.starts.size() > 1 ? breaks.starts.size() - 1 : 0);

  // ⭐⭐ **欄界必須落在 token 邊界上。**（複查抓到：soft flush 時文字會重複一段）
  //
  // 產出是以 **unit** 為單位，而保留（下面的 Remove consumed words）是以 **word** 為單位
  // —— 而一個 word 可以吐出多個 unit（縮略詞逐字、混合 token 切段、過長 token 切片、
  // 詞間空白）。欄界若落在某個 word 的 unit 中間：
  //   ① 那個 word 沒被消耗（正確，它還沒排完）；
  //   ② 但它前面幾個 unit **已經畫出去了**；
  //   ③ 下一輪它整個重排 → 那幾個 unit **再畫一次** ＝ 段落中間重複一段文字。
  // 橫排沒有這個問題，因為橫排一個 word 恰好一個單位。
  //
  // → 往回退到「邊界 unit 是它那個 word 的第一個 unit」為止。
  // ⚠️ 例外（過長的 token 被切成多片，本來就跨欄）：退到 0 會產出零欄 →
  //    呼叫端看到沒有產出就再排一次同樣的輸入 ＝ **正回饋迴圈，每本書都打不開**（v139）。
  //    那種情況維持原樣並留下證人：**寧可重複，也不可掉字或空轉**。
  if (!includeLast && columnCount > 0) {
    size_t adjusted = columnCount;
    const auto midWord = [&](const size_t cc) {
      if (cc >= breaks.starts.size()) return false;
      const size_t u = breaks.starts[cc];
      return u > 0 && u < units.size() && unitSrcWord[u] == unitSrcWord[u - 1];
    };
    while (adjusted > 0 && midWord(adjusted)) --adjusted;
    if (adjusted == 0) {
      ParsedText::vertDiag("VERTSPLITWORD cols=%u", static_cast<unsigned>(columnCount));
    } else {
      columnCount = adjusted;
    }
  }

  // ── 產出 TextBlock ──────────────────────────────────────────────────────
  // ⭐ **轉置編碼：arena 格式一個位元組都不改。**
  //     xpos[i]          → 沿欄的位移
  //     focusSuffixX[i]  → 跨軸（欄內）位移（focusBoundary 全 0，所以 focus 邏輯不會跑）
  //     styles[i] bit 7  → 這個 token 要旋轉（Style 用到 bit 0-6，bit 7 是空的）
  for (size_t c = 0; c < columnCount; ++c) {
    const size_t begin = breaks.starts[c];
    const size_t end = (c + 1 < breaks.starts.size()) ? breaks.starts[c + 1] : units.size();
    if (end <= begin) continue;

    std::vector<std::string> colWords;
    std::vector<int16_t> colAlong;
    std::vector<EpdFontFamily::Style> colStyles;
    std::vector<uint8_t> colBoundary;
    std::vector<uint16_t> colCross;
    // ⚠️ 逐字元拆開之後，實際筆數 ≥ unit 數 —— colBoundary 必須在迴圈【之後】才配。
    const size_t n = end - begin;
    colWords.reserve(n);
    colAlong.reserve(n);
    colStyles.reserve(n);
    colCross.reserve(n);

    for (size_t i = begin; i < end; ++i) {
      const size_t w = unitSrcWord[i];
      const std::string raw = words[w].substr(unitByteBegin[i], unitByteLen[i]);
      auto st = w < wordStyles.size() ? wordStyles[w] : EpdFontFamily::REGULAR;

      if (units[i].kind == vtext::UnitKind::Rotated) {
        // 旋轉：整串一次畫（drawTextVerticalCW 自己沿欄推進）。不換字形。
        colWords.push_back(raw);
        colAlong.push_back(static_cast<int16_t>(breaks.alongOff[i] + 0.5f));
        colStyles.push_back(
            static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(st) | vtext::STYLE_BIT_ROTATED));
        colCross.push_back(static_cast<uint16_t>(units[i].crossOff + 0.5f));
        continue;
      }

      if (units[i].kind == vtext::UnitKind::TateChuYoko) {
        // ③ 縦中横：**整組一筆、一個 em 格**，不旋轉也不拆。
        //
        // ⚠️ 這條分支是 v219 補的。v218 沒有它 → 縦中横掉進下面的 Cell 路徑被
        //    逐碼位拆開，而 advance 只留了一格 → 「1.」的句點被放到 `cellTop + em`，
        //    **蓋在下一個字上**（實機截圖：句點壓在「一」的橫畫與「三」的第三畫上）。
        //    使用者回報「數字跟點有改進，但是點的位置怪怪的」——
        //    「怪」的不是格內位置，是它整個掉到下一格去了。
        // drawText 內部會 +ascender，所以這裡先扣掉；淨效果是基線落在 cellAscent。
        const float tcyAlong = breaks.alongOff[i] + em * vtext::CELL_ASCENT_FACTOR - ascender;
        colWords.push_back(raw);
        colAlong.push_back(static_cast<int16_t>(tcyAlong + 0.5f));
        colStyles.push_back(st);
        colCross.push_back(static_cast<uint16_t>(units[i].crossOff + 0.5f));
        continue;
      }

      // ⭐⭐ **Cell 一定要逐字元拆開，一個字一筆。**
      //    繪製端用的是 `drawText` —— 那是【橫排】的繪製函式，沿 +X 推進。
      //    把多字元的字串整個交給它，第二個字之後就會被畫到【右邊那一欄】去
      //    （欄由右往左，右邊是已經畫過的地方），而沿欄這邊卻保留了 N 格 →
      //    畫面同時出現「標點跑到隔壁欄」與「本欄空一格」。
      //    而多字元的 Cell 是常態不是例外：禁則就是靠「不產生切點、黏成同一個 token」
      //    表達的（追い出し），所以每一個行首禁則都會踩到。
      //    ⚠️ 拆在【產出階段】而不是建 units 時：分欄必須看到黏合後的整體才會把它
      //      整組推到下一欄；拆早了禁則就沒了。
      const float cellAscent = em * vtext::CELL_ASCENT_FACTOR;
      const auto* cp = reinterpret_cast<const unsigned char*>(raw.c_str());
      int k = 0;
      while (const uint32_t c = utf8NextCodepoint(&cp)) {
        // 直排字形替換：這是唯一決定「畫出什麼字」的地方。
        // ⚠️ 句讀不換形（台灣居正中），FE10–FE16 是右上角形。
        const bool centered = vtext::isCenteredPunctuation(c);
        const bool rotateChar = vtext::needsRotationInVertical(c);
        // B-22：新表要證明自己會被執行。v218 以前只有四個碼位，箭頭一個都轉不到。
        if (rotateChar && ParsedText::vertRotLogged < 6) {
          ++ParsedText::vertRotLogged;
          ParsedText::vertDiag("VERTROT cp=U+%04X", static_cast<unsigned>(c));
        }
        const uint32_t vf = vtext::verticalForm(c);
        const uint32_t drawCp = (centered || rotateChar) ? c : (vf ? vf : c);
        std::string one;
        utf8AppendCodepoint(drawCp, one);
        colWords.push_back(std::move(one));

        const float cellTop = breaks.alongOff[i] + static_cast<float>(k) * em;
        // 這一格是不是「掛出去的那一個」？（ぶら下げ 的定義就是超出欄長，不是異常，
        // 所以 VERTCLIP 不能對它誤報，而它的位置也要另外算 —— 貼著版心邊。）
        const bool hungCell = units[i].hangTail > 0.0f && cellTop + em > grid2.columnLength + 1.0f &&
                              cellTop <= grid2.columnLength + 1.0f;
        // drawText 內部會 +ascender，所以這裡先扣掉；淨效果就是「基線落在 cellAscent」。
        float along = cellTop + cellAscent - ascender;
        float cross = units[i].crossOff;
        auto styleBits = st;

        // ⭐ ～ ─ － ― 這四個沒有直排字形，必須**整個轉 90°**。
        //    轉了之後：字形的寬對到沿欄、高對到跨軸（見 VerticalText.h 的座標映射）。
        //      沿欄置中： along = 格頂 + (em − width)/2 − left
        //      跨軸置中： cross = em/2 + height/2 − top
        //    ⚠️ 兩個 ─ 相接要無縫：width ≈ em 時 along = 格頂 − left，
        //      而相鄰格相距正好 em → 接得起來。
        if (rotateChar) {
          int gw = 0, gh = 0, gl = 0, gt = 0;
          if (renderer.getGlyphInkBox(fontId, drawCp, st, &gw, &gh, &gl, &gt)) {
            styleBits = static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(st) | vtext::STYLE_BIT_ROTATED);
            along = cellTop + (em - static_cast<float>(gw)) * 0.5f - static_cast<float>(gl);
            cross = em * 0.5f + static_cast<float>(gh) * 0.5f - static_cast<float>(gt);
            if (cross < 0.0f) cross = 0.0f;  // colCross 是 uint16
          }
        }

        // ⭐ **句讀居正中**（教育部《重訂標點符號手冊》九處明文；台灣與中國大陸／日本
        //    的差別就在這裡 —— 對岸與日本放右上角，我們放正中央）。
        //    字型給的是【橫排】的位置：、。，在 em 框的左下角。所以要按這個字形自己的
        //    墨水外框把它挪到格子中心 —— **不是一個可以猜的常數**，五套字型並不相同。
        //    直立字的墨水左上角 ＝ (x + left, y − top)（GfxRenderer.cpp:432-434），代入
        //    「墨水中心 == 格心」解出來就是下面兩行。
        if (centered) {
          int gw = 0, gh = 0, gl = 0, gt = 0;
          if (renderer.getGlyphInkBox(fontId, drawCp, st, &gw, &gh, &gl, &gt)) {
            along = cellTop + em * 0.5f + static_cast<float>(gt) - static_cast<float>(gh) * 0.5f - ascender;
            // ⭐ 懸掛的那一個：**貼著版心邊**（JLREQ「版面に接して」），
            //    墨水頂落在欄長上，而不是在版心外擺一個完整的格再置中。
            //
            // ⚠️⚠️ **只准往【上】拉，不准往下推。**（v219 實機回報：「懸掛的標點符號
            //    好像移動太下方了」，22 級字）
            //    v214–v219 是**無條件**指定 `columnLength`，而那是【格線】的盡頭，
            //    不是【這一欄的文字】的盡頭。欄尾若還空著一格（禁則把下一組推走了），
            //    這個標點就被推到文字下面整整一個 em ——
            //    實機量到：最後一個字墨水底 y=701，懸掛的「，」墨水頂 y=747，
            //    中間 46 px ＝ 一整格空白；而欄內正常的「，」只離前一字 19 px。
            //    ⇒ 取兩者的較小值：本來就在版心內就別動它，真的要溢出才貼邊。
            //    （文字剛好填滿格線時，natural 會大於 hung，仍然貼邊 ＝ v214 的行為不變。）
            if (hungCell) {
              const float hungAlong = grid2.columnLength + static_cast<float>(gt) - ascender;
              if (ParsedText::vertHungLogged < 4) {
                ++ParsedText::vertHungLogged;
                ParsedText::vertDiag("VERTHUNG cp=U+%04X top=%d gridLen=%d nat=%d hung=%d", static_cast<unsigned>(c),
                                     static_cast<int>(cellTop + 0.5f), static_cast<int>(grid2.columnLength),
                                     static_cast<int>(along + 0.5f), static_cast<int>(hungAlong + 0.5f));
              }
              if (hungAlong < along) along = hungAlong;
            }
            cross = em * 0.5f - static_cast<float>(gl) - static_cast<float>(gw) * 0.5f;
            if (cross < 0.0f) cross = 0.0f;  // colCross 是 uint16，負值會迴繞
          }
        }

        // 溢出證人。**照畫不誤** —— 不畫就是掉字，比壓到狀態列更糟。
        // 但要留下數字：v207 實機看到滿欄的字壓到狀態列上，而桌面重現不出來
        // （evenDistribute 的不變量在桌面是成立的，最壞超出 0.00 px）。
        // 缺的就是「裝置上 colLen / em / cellTop 到底是多少」。
        if (!hungCell && cellTop + em > grid2.columnLength + 1.0f) {
          // ⚠️ 這裡的 `c` 是【碼位】不是欄索引（內層迴圈遮蔽了外層的欄索引）——
        //    欄位名一度寫成 col=，於是 log 印出 col=12290 這種「不存在的欄」。
        ParsedText::vertDiag("VERTCLIP cp=U+%04X k=%d top=%d em10=%d gridLen=%d", static_cast<unsigned>(c), k,
                        static_cast<int>(cellTop + 0.5f), static_cast<int>(em * 10.0f + 0.5f),
                        static_cast<int>(grid2.columnLength));
        }
        colAlong.push_back(static_cast<int16_t>(along + 0.5f));
        colStyles.push_back(styleBits);
        colCross.push_back(static_cast<uint16_t>(cross + 0.5f));
        ++k;
      }
    }

    colBoundary.assign(colWords.size(), 0);

    const uint32_t visibleOffset = visibleOffsetAt(unitSrcWord[begin]);
    // ⚠️ ruby 在直排【刻意不做】，而且必須顯式不傳 —— 只要 rubyTexts 非空，
    //    TextBlock::render 就會用橫排座標亂畫三處（不是靜默不畫）。實測未見真正的注音 ruby。
    auto block = std::make_shared<TextBlock>(colWords, colAlong, colStyles, colBoundary, colCross, blockStyle,
                                             std::vector<std::string>{});
    if (!block->valid()) {
      LOG_ERR("PTV", "Dropping column: TextBlock arena allocation failed");
      continue;
    }
    // 這一欄用掉幾個 token：unit 是 token 的細分，只數「某個 token 的第一個 unit」。
    int tokensInColumn = 0;
    for (size_t i = begin; i < end; ++i) {
      if (i == 0 || unitSrcWord[i] != unitSrcWord[i - 1]) ++tokensInColumn;
    }
    processColumn(std::move(block), visibleOffset, tokensInColumn);
  }

  // ── 尾段：Remove consumed words ─────────────────────────────────────────
  // ⚠️⚠️ **這一段不可被任何早退跳過**（ParsedText.h:28-30）：v139 在橫排版本的開頭加了
  //    `if (oom_) return;`，跳過這裡，一次暫時性的記憶體拒絕就變成正回饋迴圈，
  //    每本書都打不開。
  if (columnCount > 0) {
    const size_t lastUnit = (columnCount < breaks.starts.size()) ? breaks.starts[columnCount] : units.size();
    // 只有整個 token 都被排完才算消耗（token 可能被拆成多個 unit）。
    size_t consumed = (lastUnit < units.size()) ? unitSrcWord[lastUnit] : words.size();
    if (consumed > words.size()) consumed = words.size();
    if (consumed > 0) {
      words.erase(words.begin(), words.begin() + consumed);
      wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
      wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
      wordNoSpaceBefore.erase(wordNoSpaceBefore.begin(), wordNoSpaceBefore.begin() + consumed);
      wordIsFocusSuffix.erase(wordIsFocusSuffix.begin(), wordIsFocusSuffix.begin() + consumed);
      eraseVisibleOffsetPrefix(consumed);
      if (!rubyTexts.empty()) {
        const size_t rtConsumed = std::min(consumed, rubyTexts.size());
        rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + rtConsumed);
      }
    }
  }

    // ⚠️⚠️ **無進展守衛**（本專案每一個「換個地方再試」的迴圈都要有）：
    //    這一批一個 token 都沒消耗掉就跳出，否則是無限迴圈 ＝ 看門狗重開機。
    //    正常情況只在「最後一欄還沒排滿、留給下次呼叫」時發生，那時本來就該結束。
    if (words.size() == wordsBefore) break;
    firstChunk = false;
    if (lastChunk && !unitsTruncated) break;
  }
}
