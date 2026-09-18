#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Epub/blocks/TextBlock.h"

class GfxRenderer;

// 純文字 → EPUB 排版引擎（`ParsedText`）的餵入層。
//
// 為什麼走這裡而不是在 TxtReaderActivity 裡再寫一份直排：`ParsedText` 的公開入口
// 就是 `addWord()` ＋ `layoutAndExtractLines()`／`layoutAndExtractColumns()`，
// **不需要** HTML／CSS／spine／圖片／Section 快取。直排的懸掛、禁則、縦中横、字形替換、
// 欄距因此全部免費，而且只有一份排版程式要維護（帳本 2026-09-12 定案）。
//
// ⭐ 頁游標【仍是單一個位元組位移】，與舊的串流閱讀器相同 —— 往前翻的 ring、
//    `progress.bin`、預取、估計頁數都不必改。成立的理由（v239 讀碼確認）：
//    1. `layoutAndExtractLines(includeLastLine=false)` 會吃掉排好的行、【留下】最後一行的詞
//       （`ParsedText.cpp` 的 "Remove consumed words"）→ 可以邊餵邊排，排夠一頁就停，
//       段落再長每頁成本都固定。v238 那種「每頁把整段從頭重排」的 O(n²) 因此不存在。
//    2. 每一行交還 `lineVisibleOffset`（該行第一個 token 的位移）→ 「第一個沒被顯示的行」
//       的起點就是下一頁的起點。從那裡重排是【續排】，不縮排。
//
// ⚠️ `visibleTextOffset` 是【碼位】制，不是位元組（v238 塞位元組是錯的，只因為影子模式
//    沒上畫面才沒出事）：`addWord` 把 CJK 切成單字 token 時用 `countCodepoints` 往上加。
//    這一層自己記「每次 addWord 的碼位起點 ↔ 位元組起點」，交還的碼位再走 UTF-8 對回位元組。
//    只有「NFC 前後逐位元組相同、合法 UTF-8」的塊才逐碼位走訪；其餘退回塊起點（只會重複、
//    不會跳過），記入 `remapMiss`。證明寫在 TxtEngineLayout.cpp 的 `Anchor` 上。
//
// ⭐ 段落模型：一行＝一段（維護者 2026-09-12 拍板）。
//
// v239 的橫排刻意與舊引擎【同行為】，好讓實機可以逐頁對照：
//   - 不縮排（`textIndentDefined=true, textIndent=0`）；但來源行開頭的 ASCII 空白保留成
//     首行縮排 —— 舊引擎把那些空白原樣畫出來。
//   - 不斷字、不做專注閱讀（舊引擎都沒有；斷字還會讓行首的字片段對不回位元組）。
//   - 粗體內文由呼叫端關掉（`g_boldBodyText` 是全域，EPUB 閱讀器會留下它的值）。
//   ⚠️ 與舊引擎【不同】、刻意接受的（v239 複查後補齊清單）：
//      - 兩端對齊真的會對齊（舊的「當成靠左」）。
//      - 斷行是 greedy（v240 起；v239 是 DP）。greedy 具前綴穩定性：從任一行首重排，後面的斷點
//        與整段排相同 → 分頁唯一 → 往前翻頁可以精確。每 512 位元組 flush 一次不影響結果。
//      - 一行中間與行尾連續的 ASCII 空白收成一個／被丟掉（ParsedText 以空白切詞）。
//      - 行首 ASCII 空白改成首行縮排：tab 算 4 格、上限半行寬；置中／靠右時縮排不生效。
//      - 文字先做 NFC；UTF-8 BOM 不畫；RTL 由引擎處理（舊的只換對齊）。
//      - 往前翻頁（ring 空了）：v240 起一次排到目標為止、取最後 N 行（收集模式），不再往回試排。
namespace txtengine {

struct Params {
  int fontId = 0;
  // 橫排＝視窗寬度；直排＝欄長。
  uint16_t extent = 0;
  // 一頁裝得下幾個單位（橫排＝幾行，直排＝幾欄）。
  int maxUnits = 1;
  // v241 起接上直排：extent 傳欄長（視窗高）、maxUnits 傳每頁欄數。
  // ⚠️ 直排的分頁唯一性有一個已知例外：欄首落在一串西文片語中間時，ParsedTextVertical 的
  //    inWesternPhrase 在 i==0 恆為假 → 第一個字從「整串旋轉」變成「直立逐字」→ 往回翻可能不精確一欄
  //    （不跳字；TXTBACKCHK 會記 exact=0）。
  bool vertical = false;
  // `CrossPointSettings::paragraphAlignment` 的值。
  uint8_t alignment = 0;
  // v240 收集模式（往前翻頁用）：把 chunk 整段排到結尾，不留任何 TextBlock，
  // 只回報單位總數與最後 collectKeep 個單位的起點。maxUnits 被忽略。
  bool collectOnly = false;
  size_t collectKeep = 0;
};

struct Result {
  // 橫排是行、直排是欄，依序畫即可。
  // ⚠️ 元素可能是 `nullptr` ＝ 空白單位（來源檔的空行），只推進一格、不畫東西。
  //    `TextBlock` 的預設建構子是 private（只給 deserialize 用），沒有公開的空區塊。
  std::vector<std::shared_ptr<TextBlock>> units;
  // 下一頁的起點，**相對傳入的 chunk 起點**，單位是位元組。
  // 保證（v239 兩輪對抗式複查後的版本）：
  //   - 不跳字：≤ 第一個未顯示的字的真正起點。
  //   - 前進：> 0，唯一例外是 oom 而且一個單位都沒排出來（跳過就是跳字，所以寧可不動）。
  size_t nextOffset = 0;

  // ── 證人（每頁印一行，缺一個就沒辦法從 log 判斷新引擎有沒有出錯）──
  // 碼位 → 位元組改用保守位置（塊起點）的次數。非 0 ＝ 這一頁可能重複顯示最多一塊（≤200 位元組）。
  uint16_t remapMiss = 0;
  // 純診斷：行首的字與原文位元組不同（軟連字號被剝、RTL 視覺順序）。不影響游標。
  uint16_t verifyMiss = 0;
  // 呼叫排版出口的次數（每餵約 512 位元組一次）。
  uint16_t flushes = 0;
  // addWord 次數。CJK 以整段（≤200 位元組一塊）餵入，所以應該是個位數到十幾。
  uint16_t words = 0;
  uint16_t sourceLines = 0;
  // 排版期間 `ParsedText` 因低記憶體拒絕過。這一頁會在最後一行的起點截斷重來，不跳字。
  bool oom = false;
  // chunk 用完了一頁還沒排滿、也不是檔尾（實務上只有病態檔案會發生）。
  bool chunkCut = false;
  // 頁尾那一組起點相同的單位被整組移到下一頁（這一頁因此少幾行）。只在 NFC 會變的文字裡發生。
  uint16_t glueMoved = 0;
  // 整頁都是同一組、只能往前推一個碼位（下一頁會幾乎重複）。應該永遠是 0。
  uint16_t glueForced = 0;
  // 收集模式的輸出。
  size_t unitCount = 0;
  std::vector<size_t> lastStarts;
};

// `chunk` 是從當前頁起點讀進來的一塊原文（不必以行邊界結束）。
// `atEof`：chunk 的結尾就是檔尾。
// `startsMidParagraph`：這一頁是從段落中間開始的（前一個位元組不是 '\n'）→ 續排，不縮排。
Result layoutPage(const char* chunk, size_t chunkLen, bool atEof, bool startsMidParagraph,
                  const GfxRenderer& renderer, const Params& params);

}  // namespace txtengine
