#pragma once

// 直排的分欄演算法 —— **刻意寫成不依賴韌體型別的獨立單元**，好讓它能在桌面被測試。
//
// ⚠️ 為什麼要獨立：`--gc-sections` 已由 firmware.map 逐行證實會丟掉沒人呼叫的直排碼
// （Discarded 區塊行 10296），所以「編得過」不代表「跑得對」。而排錯的代價是一輪刷機
//  —— 只有一台機器、USB 是 eFuse 鎖死的。教訓 B-22：儀器要先證明自己會被執行。
//  → 這個檔的每一條規則都能用 `tools/vertical-oracle/cross_check` 在桌面跟 V0 預言機比對。
//
// 規格來源：`tools/vertical-oracle/vlayout.py`（已在像素上驗過的 V0 桌面預言機）。
// 決策與依據：`docs/specs/2026-08-25-feature-parity-ledger.md` 搜「中文直排」。

#include <cstdint>
#include <vector>

namespace vtext {

// 一個排版單元（token）在直排裡的分類。**在排版階段決定，繪製端只查表**——
// 因為 `TextBlock::render` 一頁跑 16 次（掃描 1 ＋ BW 1 ＋ 灰階 7 帶 × 2 平面 14）。
enum class UnitKind : uint8_t {
  Cell,     // 佔一個 em 格、直立：漢字、句讀、FE 直排形、單一字母／縮略詞的一個字母
  Rotated,  // 整串順時針旋轉 90°：西文的單詞、語句（clreq §2.1.2 ②）
  // 縦中横（clreq §2.1.2 ③）：**多個字元共用一個 em 格**，不旋轉、橫向並排。
  //
  // ⚠️⚠️ **它必須是自己一種 kind，不能歸在 Cell。**（v218 實機：「點的位置怪怪的」）
  //    產出端對 `Cell` 的處理是「逐碼位拆開、第 k 個放在 cellTop + k*em」——
  //    那對漢字是對的，對縦中横是災難：advance 只留了【一格】，
  //    但「1.」被拆成兩格 → **句點跑到下一格、蓋在下一個字上**
  //    （實機截圖：句點壓在「一」與「三」的筆畫上）。
  //    ⚠️ 這不是幾何沒調好，是**分類被吞掉**：`Cell` 的註解當時寫著「縦中横整組」，
  //      但產出端根本沒有那條分支 —— 說明寫了、程式沒寫（B-22 的變形）。
  TateChuYoko,
};

struct ColumnUnit {
  float advance;   // 沿欄方向的推進量（Cell 一律 = em；Rotated = 該串的橫向總寬）
  float crossOff;  // 跨軸（欄內）位移：Cell 若非全形要置中；Rotated 為整串置中的偏移
  UnitKind kind;
  bool gluedToPrev;  // 禁則造成的黏合（既有機制在【切詞期】就決定了，這裡只是讀結果）
  // ⭐ 行尾點號懸掛（ぶら下げ組）：這個 unit 結尾是可懸掛的句讀時，它的推進量。
  //    0 ＝ 不可懸掛。放不下時可以只算 advance − hangTail，讓最後那個句讀掛到版心外。
  float hangTail;
};

// 版心：把欄的可用長度對齊 em 格。
//
// ⭐ clreq「設計版心的順序」是【先決定一行的字數，再推版心】。我們原本反過來
//    （拿邊界減一減當欄長），於是每欄底多出不足一格的空隙 ——
//    那正是懸掛的標點看起來「掉出來」卻又沒真的出版心的原因。
struct ColumnGrid {
  int charsPerColumn;  // 一欄幾個 em 格
  float columnLength;  // = charsPerColumn × em（對齊格線後的欄長）
  // 格線量化之後剩下的餘量 ＝ 版心高 − 欄長。**這就是懸掛的字唯一能伸進去的地方。**
  // ⚠️ 它可能是 0（欄長剛好整除版心高時）—— 那種字級就掛不了，呼叫端要退回平均排列。
  float hangRoom;
};

inline ColumnGrid makeColumnGrid(const float availableLength, const float em) {
  ColumnGrid g{};
  g.charsPerColumn = (em > 0.0f) ? static_cast<int>(availableLength / em) : 0;
  if (g.charsPerColumn < 1) g.charsPerColumn = 1;
  g.columnLength = static_cast<float>(g.charsPerColumn) * em;
  g.hangRoom = availableLength - g.columnLength;
  return g;
}

// 分欄的結果：每一欄的 [起始 token, 結束 token) 與每個 token 沿欄的位移。
struct ColumnBreaks {
  std::vector<uint16_t> starts;  // 每欄的起始 token index；長度 = 欄數
  std::vector<float> alongOff;   // 每個 token 沿欄的位移（欄內座標）
  std::vector<uint8_t> shortByKinsoku;  // 每欄：是否因禁則而縮短（→ 要平均排列）
};

// 貪婪填欄。
//
// ⭐ **禁則不必在這裡實作** —— 既有機制在【切詞期】就決定了（`ParsedText.cpp:106/:415`
//    → `Utf8.h hasCjkBreakOpportunityBetween`）：禁則命中就不產生切點，標點與前一字黏成
//    同一 token，整組一起被推到下一欄 ＝ **追い出し 是免費的**。
//    這裡只要尊重 `gluedToPrev` 不在黏合處斷開即可。
//    ⚠️ 反過來，既有機制表達不出「懸掛」（黏合是二元的）—— V1 走追い出し所以沒事。
//
// ⚠️ **無進展守衛**：任何「換個地方再試」的迴圈都要有。V0 實測拔掉它會無限迴圈
//    ＝ 韌體上的看門狗重開機。而單靠守衛還不夠 —— 沒有跨欄切分的話 500 個字只有 26 個
//    留在畫面內（掉 474 個，不當機、不報錯、字就是不見了）。兩個都要，理由不同。
// ⚠️⚠️ **前置條件：呼叫端必須先把「比一整欄還長」的 token 切開。**
//
// 禁則是 **best-effort，不是絕對**。切詞期會把連續的收尾標點黏成單一 token
// （例如一串 `。` 之間沒有斷點），而那個 token 可能比一整欄還長。
// 這種情況下**必須違反禁則把它切開** —— 否則只剩兩條路，兩條都是錯的：
//   ① 強制放、允許超出 → 字被畫到版心外 ＝ **掉字**
//      （V0 實測同型：500 個字只有 26 個留在畫面內，不當機、不報錯、字就是不見了）
//   ② 不斷推遲 → 空轉／無限迴圈 ＝ 韌體上的看門狗重開機
// 2026-09-08 的跨實作比對就是靠「40 個連續句號」這個樣本抓到的。
//
// `unitsPerColumn()` 給呼叫端算切點用。
inline int unitsPerColumn(const ColumnGrid& grid, const float em) {
  return (em > 0.0f) ? static_cast<int>(grid.columnLength / em) : 1;
}

// `hangAllowed`：這個字級／版心組合放得下一個懸掛的句讀嗎？
// **由呼叫端用實際字形的墨水量決定**（見 ParsedTextVertical.cpp）——
// 不是猜一個比例，因為五套字型的句讀墨水高度不同。
inline ColumnBreaks fillColumns(const std::vector<ColumnUnit>& units, const ColumnGrid& grid,
                                const float firstIndent, const bool hangAllowed = false) {
  ColumnBreaks out;
  if (units.empty() || grid.columnLength <= 0.0f) return out;
  out.alongOff.assign(units.size(), 0.0f);

  const float limit = grid.columnLength + 0.01f;
  float y = firstIndent;
  size_t start = 0;
  out.starts.push_back(0);
  out.shortByKinsoku.push_back(0);

  size_t i = 0;
  while (i < units.size()) {
    const float adv = units[i].advance;

    // ⭐ **行尾點號懸掛（ぶら下げ組）** —— JLREQ 3.8.2：
    //    「句点類及び読点類に限り，版面に接して，指定の行長よりはみ出して配置する」，
    //    採用的理由是「**ベタ組の字間を空ける調整が避けられる**」。
    //    clreq 6.1.3 同意繁體【直排可做】（橫排不做，因為點號居中會顯得突兀），
    //    且 6.2.4 說縱橫對齊（多用於繁體）可配合懸掛。
    //    ⚠️ clreq：「通常，行尾只可懸掛一個標點符號」——
    //      掛完就把 y 推到 limit，下一個 unit 必然換欄，所以一欄最多掛一個。
    if (hangAllowed && y > 0.0f && units[i].hangTail > 0.0f && y + adv > limit &&
        y + (adv - units[i].hangTail) <= limit) {
      out.alongOff[i] = y;
      y = limit;  // 這一欄滿了；掛出去的部分不佔格
      ++i;
      continue;
    }

    if (y > 0.0f && y + adv > limit) {
      // 要在 i 之前斷欄，但黏合的 token 不可與前一個分開（那就是禁則）。
      size_t breakAt = i;
      while (breakAt > start && units[breakAt].gluedToPrev) --breakAt;

      // ⚠️⚠️ 無進展守衛（V0 的教訓，2026-09-08 的跨實作比對又抓到一次）：
      //   breakAt == start 表示【這一欄從頭到尾黏成一整組而且放不下】。
      //   若照樣斷在 start，就會產生一個【空欄】，而且下一輪還是同樣的狀況 →
      //   空轉或把所有字擠進一欄。實測 40 個連續句號（切詞期會黏成單一 token）
      //   就會踩到；本檔第一版在此對 size_t 下溢（breakAt − 1 當 breakAt == 0）。
      //   → 已經在欄頂還是放不下 ⇒ **強制放，允許超出**，然後從下一個 token 重新開欄。
      if (breakAt <= start) {
        // 整欄黏成一組而且放不下。前置條件（見檔頭）要求呼叫端已經把過長的 token 切開，
        // 所以走到這裡表示「這一欄只放得下這一個 token，而它本身不超過一欄」——
        // 直接在 i 斷欄（違反禁則，但那是 best-effort 的正確行為）。
        // ⚠️ 若呼叫端沒有遵守前置條件，這裡仍不會空轉也不會下溢：i > start 保證有進展。
        if (i > start) {
          out.shortByKinsoku.back() = 0;
          out.starts.push_back(static_cast<uint16_t>(i));
          out.shortByKinsoku.push_back(0);
          start = i;
          y = 0.0f;
          continue;  // 不 ++i：這個 token 改放到新欄的頂端
        }
        // i == start：單一 token 比一整欄還長且無法再切 → 強制放（會超出，但不會空轉）
        out.alongOff[i] = y;
        y = limit;
        ++i;
        continue;
      }

      // ⚠️ 原本寫 `(breakAt < i)`，而 `gluedToPrev` 在呼叫端恆為 false（禁則是在
      //    【切詞期】黏成同一個 token 表達的，不是靠這個旗標）→ breakAt 恆等於 i →
      //    這一格恆為 0 → `evenDistribute` **從來沒有執行過**。平均排列是死碼。
      //    正確的判準是「欄尾剩下整整一格以上」：一般換行最多剩不足一格，
      //    剩一格以上必然是被推出去的多格 token 造成的 ＝ 禁則。
      //
      // ⚠️⚠️ **要量【被關掉的那一欄】真正的結尾，不是 `y`。**（v230 引入、v231 修）
      //    v230 讓 `gluedToPrev` 第一次真的會動 → `breakAt` 第一次真的會往回走。
      //    而 `y` 是**退讓之前**的位置，也就是含了那些即將被搬到下一欄的 unit。
      //    進到這個分支必然 `columnLength − y < adv[i]`，所以只要觸發斷欄的那個 unit
      //    剛好是一格（單一漢字／縦中横一組／逐字排的一個字母 —— **正是 v230 會黏的那些**），
      //    `columnLength − y >= emCell` **恆為假** → 旗標永遠 0 → `evenDistribute` 不執行。
      //    淨效果：v229 會斷在「第|10|章」中間；v230 正確地整組推走、欄尾開了 2–3 格空洞，
      //    **然後不補平** —— 那正是 v212 使用者回報的「欄尾空一格」，被修禁則的那一版救回來。
      //    （V0 預言機 `vlayout.py` 是先推出去再評估，所以它一直是對的，是韌體這邊偏了。）
      const float emCell = (grid.charsPerColumn > 0) ? grid.columnLength / static_cast<float>(grid.charsPerColumn)
                                                     : grid.columnLength;
      const float usedEnd = (breakAt < i) ? out.alongOff[breakAt] : y;
      out.shortByKinsoku.back() = (grid.columnLength - usedEnd >= emCell - 0.01f) ? 1 : 0;
      out.starts.push_back(static_cast<uint16_t>(breakAt));
      out.shortByKinsoku.push_back(0);
      start = breakAt;
      y = 0.0f;
      i = breakAt;  // 從 breakAt 重排（不做 i-1 的技巧，那會對 size_t 下溢）
      continue;
    }

    out.alongOff[i] = y;
    y += adv;
    ++i;
  }
  return out;
}

// clreq「平均排列」：把【禁則造成】的欄尾空白均分進字距。
//
// > 平均排列　平均分配字距，使文字列兩端能夠對齊行首與行尾。…主要應用於：
// > 行首行尾禁則。…就會於行尾產生一到二字（甚至以上）的空白。**由於中文書籍各行行頭尾
// > 對齊是重要的排版規則**，此時就會利用平均排列將空白均分至該行各字字距。
//
// ⚠️ **只對被禁則縮短的欄做。** 全欄都做就變成【疏排】，而 clreq §6.3.1 明列疏排只用於
//    標題／圖表說明／詩詞／兒童書籍 —— 內文不在內。
// ⚠️ 段落最後一欄合理地短，**不做**（它不是禁則造成的）。
// ⭐ **夾的是「每個字距被撐開多少」，不是「總空白多少」。**（2026-09-09 改）
//    v207 用總量上限 2.5em，結果欄與欄的字距差到 46 vs 53 px，肉眼看得出來；
//    收到 1.0em 之後又反過來：被禁則推走一個【兩格】的詞（例如「眺，」）時
//    slack ＝ 2em > 上限 → 完全不補，欄尾就空一格（實機 v212 使用者發現）。
//    兩個症狀是同一個旋鈕的兩端，因為總量上限**沒有考慮攤給幾個字距**：
//    2em 攤到 16 個字距是每格 +12%（幾乎看不出來），攤到 4 個字距是每格 +50%（很醜）。
//    → 直接夾每格的伸展比例。這條規則自己會隨欄的長短調整，不必再猜一個總量。
inline void evenDistribute(const std::vector<ColumnUnit>& units, ColumnBreaks& br, const ColumnGrid& grid,
                           const float maxPerGapRatio) {
  for (size_t c = 0; c + 1 < br.starts.size(); ++c) {
    if (!br.shortByKinsoku[c]) continue;
    const size_t begin = br.starts[c];
    const size_t end = br.starts[c + 1];
    if (end <= begin + 1) continue;
    float used = 0.0f;
    for (size_t i = begin; i < end; ++i) used += units[i].advance;
    const float slack = grid.columnLength - (br.alongOff[begin] + used);
    if (slack <= 0.5f) continue;
    const float step = slack / static_cast<float>(end - begin - 1);
    // 每格撐開超過 em 的這個比例就不勻 —— 勻了反而比留白難看。
    const float em = (grid.charsPerColumn > 0) ? grid.columnLength / static_cast<float>(grid.charsPerColumn)
                                               : grid.columnLength;
    if (step > em * maxPerGapRatio) continue;
    float extra = 0.0f;
    for (size_t i = begin + 1; i < end; ++i) {
      extra += step;
      br.alongOff[i] += extra;
    }
  }
}

// 對齊：直排的 `text-align` 作用在【沿欄】方向 —— 直排的「行」就是欄。
//   兩端對齊／順流向 → 貼欄頭（不動）
//   置中             → 欄內置中
//   逆流向           → 貼欄尾
//
// ⚠️ 橫排在 `extractLine` 有三個互斥的定位分支（ParsedText.cpp），**直排先前一個都沒有**
//    —— 所以置中的標題、圖說在直排全部貼著欄頭。實機 v216 由使用者發現
//    （「這個標題的樣式有 show 出來嗎？看起來怪怪的」）。
//
// ⚠️ 懸掛過的欄 slack 為 0（y 已被推到 limit），所以不會被平移 —— 這是對的。
inline void alignColumns(const std::vector<ColumnUnit>& units, ColumnBreaks& br, const ColumnGrid& grid,
                         const bool toEnd) {
  for (size_t c = 0; c < br.starts.size(); ++c) {
    const size_t begin = br.starts[c];
    const size_t end = (c + 1 < br.starts.size()) ? br.starts[c + 1] : units.size();
    if (end <= begin) continue;
    float used = 0.0f;
    for (size_t i = begin; i < end; ++i) used += units[i].advance;
    const float slack = grid.columnLength - (br.alongOff[begin] + used);
    if (slack <= 0.5f) continue;
    const float shift = toEnd ? slack : slack * 0.5f;
    for (size_t i = begin; i < end; ++i) br.alongOff[i] += shift;
  }
}

}  // namespace vtext
