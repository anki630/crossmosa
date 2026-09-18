#include "TxtEngineLayout.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "Epub/ParsedText.h"
#include "Epub/css/CssStyle.h"
#include "GfxRenderer.h"
#include "Utf8.h"

namespace txtengine {
namespace {

// 與 ChapterHtmlSlimParser 的 MAX_WORD_SIZE 相同：一次 addWord 最多 200 位元組。
// 超過就切塊、後面的塊以 attachToPrevious 接上 —— 那正是 EPUB 解析器的作法，
// `addWord` 會把 CJK 的黏合轉成合法斷點，所以切在哪個碼位都不影響斷行。
constexpr size_t kMaxPieceBytes = 200;
// 每餵這麼多位元組就排一次（includeLastLine=false），排夠一頁再多一行當游標就停。
// 一頁中文約 900 位元組，所以一頁約兩到三次。這個數字同時是記憶體上限：
// 不論段落多長，`ParsedText` 手上最多只有約一次 flush 的詞。
constexpr size_t kFlushBytes = 512;

inline bool isCont(const unsigned char b) { return (b & 0xC0) == 0x80; }
inline bool isSpace(const char c) { return c == ' ' || c == '\t'; }

CssTextAlign toCssAlign(const uint8_t align) {
  // 與 TextSettingsPreview.cpp 同一個對映 —— 刻意重複：那邊是設定頁預覽、這邊是閱讀器，
  // 抽成共用函式會讓任一邊的改動悄悄影響另一邊。
  if (align == CrossPointSettings::BOOK_STYLE) return CssTextAlign::Justify;
  return static_cast<CssTextAlign>(align);
}

// 一次 addWord 的碼位起點 ↔ 位元組起點。`ParsedText` 交還的是碼位，要靠這張表對回位元組。
//
// ⭐ 「不跳字」的證明靠兩件事，缺一不可（v239 對抗式複查後改寫）：
//   1. **區間不重疊**：每塊佔用的碼位區間長度取 max(原文碼位數, NFC 後碼位數)。
//      `ParsedText` 對這塊交還的任何位移都落在 [cp, cp+長度) 內，所以 upper_bound
//      找到的一定是【產生那個 token 的塊】，不可能是後面的塊。
//   2. **只信任逐位元組相同的塊**：`exact` ＝ 合法 UTF-8 且 NFC 前後完全相同。只有這種塊，
//      `ParsedText` 的 countCodepoints 推進才與原文逐碼位一致，碼位走訪才精確。
//      其餘的塊一律退回塊起點 —— 那個位置 ≤ 真正起點，只可能重複、不可能跳過。
//   ⚠️ 舊版用 memcmp 驗證是錯的：它驗的是【內容相同】不是【位置相同】，重複的字會讓錯誤位置
//      通過驗證；而若 NFC 會增加碼位數，錯誤位置在真正起點之後 → 跳字（codex 抓到）。
//      memcmp 現在只當診斷（`verifyMiss`），不影響結果。
struct Anchor {
  uint32_t cp;
  uint32_t byte;
  uint32_t bytes;
  bool exact;
};

// 嚴格的 UTF-8（拒絕 overlong、surrogate、超過 U+10FFFF）。只有通過的塊才逐碼位走訪 ——
// 不猜解碼器（utf8NextCodepoint）對那些邊緣序列會吃掉幾個位元組。
bool isValidUtf8(const char* s, const size_t n) {
  size_t i = 0;
  while (i < n) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t need;
    unsigned char lo = 0x80;
    unsigned char hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
      need = 1;
    } else if (c == 0xE0) {
      need = 2;
      lo = 0xA0;
    } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) {
      need = 2;
    } else if (c == 0xED) {
      need = 2;
      hi = 0x9F;  // 排除 surrogate
    } else if (c == 0xF0) {
      need = 3;
      lo = 0x90;
    } else if (c >= 0xF1 && c <= 0xF3) {
      need = 3;
    } else if (c == 0xF4) {
      need = 3;
      hi = 0x8F;
    } else {
      return false;
    }
    if (n - i <= need) return false;
    const auto c1 = static_cast<unsigned char>(s[i + 1]);
    if (c1 < lo || c1 > hi) return false;
    for (size_t k = 2; k <= need; ++k) {
      if (!isCont(static_cast<unsigned char>(s[i + k]))) return false;
    }
    i += need + 1;
  }
  return true;
}

// 解出 [b, len) 開頭的碼位；非法序列回傳 0（呼叫端視為「這裡不是安全切點」）。
uint32_t decodeAt(const char* s, const size_t len, const size_t b) {
  if (b >= len) return 0;
  const auto c = static_cast<unsigned char>(s[b]);
  if (c < 0x80) return c;
  size_t need;
  uint32_t cp;
  if ((c >> 5) == 0x6) {
    need = 1;
    cp = c & 0x1F;
  } else if ((c >> 4) == 0xE) {
    need = 2;
    cp = c & 0x0F;
  } else if ((c >> 3) == 0x1E) {
    need = 3;
    cp = c & 0x07;
  } else {
    return 0;
  }
  if (len - b <= need) return 0;
  for (size_t k = 1; k <= need; ++k) {
    const auto cc = static_cast<unsigned char>(s[b + k]);
    if (!isCont(cc)) return 0;
    cp = (cp << 6) | (cc & 0x3F);
  }
  return cp;
}

class PageBuilder {
 public:
  PageBuilder(const char* chunk, const size_t len, const bool atEof, const GfxRenderer& renderer, const Params& params,
              Result& out)
      : chunk_(chunk),
        len_(len),
        atEof_(atEof),
        renderer_(renderer),
        params_(params),
        out_(out),
        wanted_(static_cast<size_t>(params.maxUnits)),
        collect_(params.collectOnly) {}

  void run(bool startsMidParagraph);

 private:
  const char* chunk_;
  const size_t len_;
  const bool atEof_;
  const GfxRenderer& renderer_;
  const Params& params_;
  Result& out_;
  const size_t wanted_;
  const bool collect_;
  std::vector<size_t> ring_;  // 收集模式：最後 collectKeep 個單位的起點（依序）

  std::vector<Anchor> anchors_;  // 只記當前段落；段落開始時清空、每次 flush 後剪掉用不到的前段
  size_t lastAnchorIdx_ = 0;     // 最近一次對應用到的塊；之後的行只會落在它或它之後
  bool done_ = false;            // 游標已經定了（或出錯），後面排出來的東西一律丟掉
  size_t placed_ = 0;            // 放進頁面或當成游標的單位數

  // 最近一個單位採用的起點，以及頁上每個單位採用的起點（與 out_.units 平行）。
  bool haveFloor_ = false;
  size_t floorByte_ = 0;
  std::vector<size_t> takenStarts_;

  // 段落首行的起點用段落真正的起點（含開頭的 ASCII 空白），不是第一個 token 的位置 ——
  // 否則頁尾剛好停在段落交界時，下一頁會從空白之後開始、被判成續排而弄丟那段縮排。
  bool paraFirstLinePending_ = false;
  size_t paraStartByte_ = 0;

  size_t nextCodepointByte(size_t b) const {
    if (b >= len_) return len_;
    ++b;
    while (b < len_ && isCont(static_cast<unsigned char>(chunk_[b]))) ++b;
    return b;
  }

  uint32_t countCodepoints(const size_t from, const size_t to) const {
    uint32_t n = 0;
    for (size_t i = from; i < to; ++i) {
      if (!isCont(static_cast<unsigned char>(chunk_[i]))) ++n;
    }
    return n;
  }

  // 行尾被 chunk 邊界截斷時，找一個不會切到字的結尾：尾巴 200 位元組內有空白就退到那裡
  // （截斷的是西文單字），否則退到最後一個【完整】碼位之後。只在「不是檔尾而且沒有換行」時用。
  // ⚠️ 空白只能在尾巴附近找。退到【整行】最後一個空白是錯的：整章一行的中文檔只要開頭有一個
  //    ASCII 空白（「第一章 標題」），就只會餵到那裡，一頁剩幾個字（自審抓到）。
  size_t safeTruncatedEnd(const size_t start, const size_t end) const {
    const size_t nearEnd = end - start > kMaxPieceBytes ? end - kMaxPieceBytes : start;
    for (size_t s = end; s > nearEnd; --s) {
      if (isSpace(chunk_[s - 1])) return s - 1;
    }
    size_t k = end;
    while (k > start && isCont(static_cast<unsigned char>(chunk_[k - 1]))) --k;
    if (k == start) return start;
    const auto lead = static_cast<unsigned char>(chunk_[k - 1]);
    const size_t need = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
    return (end - (k - 1)) < need ? k - 1 : end;
  }

  // ⭐ v240：切塊點只選「兩側都是 CJK 字、而且中間本來就能斷行」的位置。
  //    分頁要唯一，從任一行首重排產生的 token 就必須與整段排【完全相同】。切塊點依餵入起點而變
  //    （每 200 位元組一刀），切在標點黏合處時，addWord 會把兩塊之間處理成「續接＋kerning」而不是
  //    一個合併的 token，字寬可能差一點點 → 臨界的行斷點不同。切在本來就是斷點的地方，兩種餵法的
  //    token 逐一相同（cjkCharacterBreakByteOffsets 也會在那裡切、兩側都是 noSpaceBefore）。
  //    找不到這種位置（例如 200 位元組的西文長字）才退回任一碼位邊界 —— 那種字本身比一行還寬，
  //    行首本來就可能落在字中間，唯一性只能盡量。
  size_t pieceCut(const size_t k, const size_t limit) const {
    for (size_t b = limit; b > k; --b) {
      if (isCont(static_cast<unsigned char>(chunk_[b]))) continue;
      size_t a = b - 1;
      while (a > k && isCont(static_cast<unsigned char>(chunk_[a]))) --a;
      const uint32_t left = decodeAt(chunk_, len_, a);
      const uint32_t right = decodeAt(chunk_, len_, b);
      // ⚠️ 直排的禁則表與橫排不同（v241）：addWord 在排版期間讀 ParsedText::verticalKinsoku()，
      //    切塊點也必須用同一張表，否則直排的 token 仍然會因切點而不同。
      if (left != 0 && right != 0 && utf8IsCjkBreakable(left) && utf8IsCjkBreakable(right) &&
          hasCjkBreakOpportunityBetween(left, right, params_.vertical)) {
        return b;
      }
    }
    size_t cut = limit;
    while (cut > k && isCont(static_cast<unsigned char>(chunk_[cut]))) --cut;
    return cut == k ? limit : cut;  // 連前導位元組都找不到（壞掉的 UTF-8）：硬切
  }

  size_t mapToByte(uint32_t cp, const TextBlock* block);
  void place(std::shared_ptr<TextBlock> unit, size_t startByte);
  void onLine(std::shared_ptr<TextBlock> unit, uint32_t cp);
  void flush(ParsedText& parsed, bool includeLast);
  void streamParagraph(size_t start, size_t end, bool complete, bool continuation, uint32_t cpAtStart);
};

size_t PageBuilder::mapToByte(const uint32_t cp, const TextBlock* block) {
  const auto it = std::upper_bound(anchors_.begin(), anchors_.end(), cp,
                                   [](const uint32_t v, const Anchor& a) { return v < a.cp; });
  if (it == anchors_.begin()) {
    // 不該發生（每個 token 的位移 ≥ 它那塊的起點）。退回段落起點：保守、只會重複。
    ++out_.remapMiss;
    return paraStartByte_;
  }
  const size_t idx = static_cast<size_t>(it - anchors_.begin()) - 1;
  lastAnchorIdx_ = idx;
  const Anchor& a = anchors_[idx];
  if (!a.exact) {
    ++out_.remapMiss;
    return a.byte;
  }
  const size_t pieceEnd = static_cast<size_t>(a.byte) + a.bytes;
  size_t b = a.byte;
  uint32_t n = cp - a.cp;
  while (n > 0 && b < pieceEnd) {
    b = nextCodepointByte(b);
    --n;
  }
  if (n != 0 || b >= pieceEnd) {
    ++out_.remapMiss;
    return a.byte;
  }
  // 純診斷：行首的字與原文不同。合法原因有兩個 —— extractLine 會剝掉軟連字號、
  // RTL 行的 TextBlock 是視覺順序。都不影響上面走訪的正確性。
  if (block && block->wordCount() > 0) {
    const char* w = block->wordText(0);
    const size_t wl = std::strlen(w);
    if (wl == 0 || b + wl > len_ || std::memcmp(chunk_ + b, w, wl) != 0) ++out_.verifyMiss;
  }
  return b;
}

// ⭐ 起點相同的單位是【一組】，分頁邊界不准切在組中間（v239 第二輪複查後改寫）。
//
// 舊版遇到「起點 ≤ 前一個」就往前推一個碼位來保證嚴格遞增 —— codex 構造出反例：
// 若兩個單位的第一個 token 真的來自同一個原文碼位（NFC 把一個字拆成兩個），往前推的位置
// 就在真正起點之後 → 跳字。只有位元組位移的游標，無法同時讓這兩個單位「不偏晚」又「嚴格遞增」。
// 所以改成：採用 max(對應值, 前一個採用值)（仍 ≤ 真正起點，因為前一個的真正起點 ≤ 這一個的），
// 相等就黏在一起；放不下的那一個如果屬於頁尾那一組，整組移到下一頁。
//   → 不跳字不再依賴任何「對應會嚴格遞增」的假設。
//   → 附帶好處：落在 NFC 會變的那一小塊裡的行，現在不會重複，只是那頁少一兩行。
void PageBuilder::place(std::shared_ptr<TextBlock> unit, size_t startByte) {
  if (haveFloor_ && startByte < floorByte_) startByte = floorByte_;
  const bool gluedToPrev = haveFloor_ && startByte == floorByte_;
  if (collect_) {
    // 收集模式（往前翻頁用）：不留任何 TextBlock，只記起點。整段排到結尾，不會有「放不下」。
    (void)unit;
    haveFloor_ = true;
    floorByte_ = startByte;
    ++placed_;
    ++out_.unitCount;
    ring_.push_back(startByte);
    if (ring_.size() > params_.collectKeep) ring_.erase(ring_.begin());
    return;
  }
  haveFloor_ = true;
  floorByte_ = startByte;
  ++placed_;
  if (out_.units.size() < wanted_) {
    out_.units.push_back(std::move(unit));
    takenStarts_.push_back(startByte);
    return;
  }
  // 第一個放不下的單位：它的起點（或它那一組的起點）就是下一頁的起點。
  done_ = true;
  out_.nextOffset = startByte;
  if (!gluedToPrev) return;

  size_t keep = out_.units.size();
  while (keep > 0 && takenStarts_[keep - 1] == startByte) --keep;
  if (keep > 0) {
    ++out_.glueMoved;
    out_.units.resize(keep);
    takenStarts_.resize(keep);
    return;
  }
  // 整頁都是同一組（一頁只有一兩行、又整頁落在一塊 NFC 會變的文字裡）：沒辦法不切組。
  // 往前推一個碼位保證前進。不會跳字：放不下的單位真正起點 > 本頁第一個單位的真正起點 ≥ startByte，
  // 所以 ≥ startByte ＋ 一個碼位。代價是下一頁與這一頁幾乎相同。
  ++out_.glueForced;
  out_.nextOffset = nextCodepointByte(startByte);
}

void PageBuilder::onLine(std::shared_ptr<TextBlock> unit, const uint32_t cp) {
  if (done_) return;  // 出口無法中止；游標定了之後排出來的行直接丟
  size_t startByte;
  if (paraFirstLinePending_) {
    startByte = paraStartByte_;
    paraFirstLinePending_ = false;
  } else {
    startByte = mapToByte(cp, unit.get());
  }
  place(std::move(unit), startByte);
}

void PageBuilder::flush(ParsedText& parsed, const bool includeLast) {
  if (done_ || parsed.size() == 0) return;
  ++out_.flushes;
  if (params_.vertical) {
    parsed.layoutAndExtractColumns(
        renderer_, params_.fontId, params_.extent,
        [this](std::shared_ptr<TextBlock> col, const uint32_t cp, int) { onLine(std::move(col), cp); }, includeLast);
  } else {
    parsed.layoutAndExtractLines(
        renderer_, params_.fontId, params_.extent,
        [this](std::shared_ptr<TextBlock> line, const uint32_t cp) { onLine(std::move(line), cp); }, includeLast);
  }
  // 剪掉已經用不到的塊：之後排出來的行，位移都 ≥ 最近一行 → 塊索引都 ≥ lastAnchorIdx_。
  // 不剪的話，全是單字母詞的長段落會讓這張表隨頁長線性成長（codex 指出）。
  if (lastAnchorIdx_ > 0) {
    anchors_.erase(anchors_.begin(), anchors_.begin() + static_cast<std::ptrdiff_t>(lastAnchorIdx_));
    lastAnchorIdx_ = 0;
  }
  if (!parsed.hasOom() || done_) return;

  // ⚠️ 低記憶體拒絕：`extractLine` 一旦拒絕就 latch，之後不再產生任何行 → 已經放進頁面的
  //    單位是完整前綴，被拒的行在最後一個單位【之後】。從最後一個單位的起點重來保證不跳字。
  out_.oom = true;
  done_ = true;
  if (out_.units.empty()) {
    out_.nextOffset = 0;  // 什麼都沒排出來：不前進（跳過就是跳字）。呼叫端看得到 eoom=1。
    return;
  }
  // 拿掉頁尾那一【組】，從組的起點重來（組的定義見 place）。
  const size_t groupStart = takenStarts_.back();
  size_t keep = out_.units.size();
  while (keep > 0 && takenStarts_[keep - 1] == groupStart) --keep;
  if (keep > 0) {
    out_.units.resize(keep);
    takenStarts_.resize(keep);
    out_.nextOffset = groupStart;
  } else {
    // 整頁一組：保留、往前推一個碼位。被拒的行真正起點 > 最後一個單位的真正起點 ≥ groupStart。
    out_.nextOffset = nextCodepointByte(groupStart);
  }
}

void PageBuilder::streamParagraph(const size_t start, const size_t end, const bool complete, const bool continuation,
                                  const uint32_t cpAtStart) {
  BlockStyle style;
  style.alignment = toCssAlign(params_.alignment);
  style.textAlignDefined = true;  // 使用者選了就照辦；RTL 由文字內容自動偵測
  style.textIndentDefined = true;
  style.textIndent = 0;

  // 來源行開頭的 ASCII 空白 → 首行縮排（舊引擎把空白原樣畫出來，這裡保持相同的外觀）。
  // ParsedText 以空白切詞、會吞掉它們，所以要換成縮排量。續排的頁不縮排。
  size_t i = start;
  int spaces = 0;
  while (i < end && isSpace(chunk_[i])) {
    spaces += chunk_[i] == '\t' ? 4 : 1;
    ++i;
  }
  if (!continuation && spaces > 0) {
    const int px = spaces * renderer_.getSpaceWidth(params_.fontId, EpdFontFamily::REGULAR);
    style.textIndent = static_cast<int16_t>(std::min(px, static_cast<int>(params_.extent) / 2));
  } else if (!continuation && params_.vertical) {
    // ⭐ v241 直排的段落首欄：放行引擎自己的規則 —— U+3000 開頭不加、否則 2em（維護者 2026-09-12 拍板
    //    的段落模型「一行＝一段」正是為了接這條）。續排的頁仍是 textIndentDefined=true／0。
    //    跨 flush 不會重複縮排：直排用 ParsedText 的成員 verticalIndentApplied 記住，不看 breakIndex。
    style.textIndentDefined = false;
  }
  bool indentActive = style.textIndent != 0;

  // extraParagraphSpacing＝false：只有這樣 `resolveFirstLineIndent` 才會採用 textIndent。
  // hyphenation／focusReading＝false：舊引擎都沒有；斷字還會讓行首的字片段對不回位元組。
  ParsedText parsed(false, false, false, style);
  // v240：橫排改 greedy（不斷字）。理由寫在 ParsedText::setGreedyLineBreaks —— 分頁要唯一，
  // 往前翻頁才對得準。直排走 layoutAndExtractColumns，不看這個旗標。
  parsed.setGreedyLineBreaks(true);
  anchors_.clear();
  lastAnchorIdx_ = 0;
  paraStartByte_ = start;
  paraFirstLinePending_ = true;

  uint32_t cp = cpAtStart + static_cast<uint32_t>(i - start);  // 開頭的空白是 ASCII，一位元組一碼位
  size_t fed = 0;

  const auto maybeFlush = [&]() {
    if (fed < kFlushBytes) return;
    const size_t before = placed_;
    flush(parsed, false);
    fed = 0;
    // ⚠️ `extractLine` 用 `breakIndex == 0` 判斷首行 —— 每一次 flush 的第一行都會被當成首行。
    //    首行一旦送出，後續 flush 就不能再縮排，否則段落中間會冒出縮排。
    if (indentActive && placed_ > before) {
      parsed.getBlockStyle().textIndent = 0;
      indentActive = false;
    }
  };

  while (i < end && !done_) {
    if (isSpace(chunk_[i])) {
      ++cp;
      ++i;
      ++fed;  // 空白也算進 flush 門檻：全是單字母詞的段落，否則要到 512 個詞才排一次
      continue;
    }
    size_t j = i;
    while (j < end && !isSpace(chunk_[j])) ++j;

    bool attach = false;
    for (size_t k = i; k < j && !done_;) {
      size_t cut = j;
      if (j - k > kMaxPieceBytes) {
        cut = pieceCut(k, k + kMaxPieceBytes);
      }
      std::string piece(chunk_ + k, cut - k);
      std::string composed = utf8ComposeNfc(piece);
      const uint32_t srcCps = countCodepoints(k, cut);
      uint32_t nfcCps = 0;
      for (const char ch : composed) {
        if (!isCont(static_cast<unsigned char>(ch))) ++nfcCps;
      }
      const bool exact = composed == piece && isValidUtf8(piece.data(), piece.size());
      anchors_.push_back({cp, static_cast<uint32_t>(k), static_cast<uint32_t>(cut - k), exact});
      // 已經是 NFC 了；`addWord` 會再做一次，那是冪等的。
      parsed.addWord(std::move(composed), EpdFontFamily::REGULAR, false, attach, cp);
      ++out_.words;
      cp += std::max(srcCps, nfcCps);  // 區間長度取較大者 → 各塊區間不重疊
      fed += cut - k;
      attach = true;
      k = cut;
      maybeFlush();
    }
    i = j;
  }
  if (done_) return;

  flush(parsed, true);  // 段落結束（或 chunk 用完）：最後一行也排出來
  if (!complete && !done_) {
    // chunk 用完了一頁還沒排滿、也不是檔尾（實務上只有病態檔案會發生）：以餵到的位置當游標。
    out_.chunkCut = true;
    done_ = true;
    out_.nextOffset = end;  // 餵到的文字全部已放上頁面（上面 flush 了 includeLast），end 在它們之後
  }
}

void PageBuilder::run(const bool startsMidParagraph) {
  size_t pos = 0;
  bool continuation = startsMidParagraph;
  uint32_t cpAt = 0;

  while (pos < len_ && !done_) {
    const void* nl = std::memchr(chunk_ + pos, '\n', len_ - pos);
    const size_t eol = nl ? static_cast<size_t>(static_cast<const char*>(nl) - chunk_) : len_;
    const bool complete = (nl != nullptr) || atEof_;
    const size_t after = nl ? eol + 1 : eol;
    size_t lineEnd = eol;
    if (lineEnd > pos && chunk_[lineEnd - 1] == '\r') --lineEnd;
    ++out_.sourceLines;

    bool blank = true;
    for (size_t q = pos; q < lineEnd; ++q) {
      if (!isSpace(chunk_[q])) {
        blank = false;
        break;
      }
    }
    if (blank) {
      // ⚠️ 只有空白（ASCII 空白／tab）的行也是空行。不這樣判的話它會走進 streamParagraph、
      //    一個詞都餵不進去、什麼都不產生 → 整行消失（舊引擎會畫出一個空行；codex 抓到）。
      if (!complete) {
        // 殘缺的空白尾巴。頁上已經有東西就留給下一頁讀完整；頁上什麼都沒有（整塊 8KB 只有空白、
        // 沒有換行）就直接跳過 —— 空白裡沒有字，不跳的話下一頁從同一個位置讀、永遠卡住（codex 反例 A）。
        if (out_.units.empty()) {
          done_ = true;
          out_.nextOffset = eol;
        }
        break;
      }
      // 空來源行 ＝ 一個空白單位（章節之間的留白要留著，與舊引擎相同）。
      place(nullptr, pos);
      if (done_) break;
      cpAt += countCodepoints(pos, after);
      pos = after;
      continuation = false;
      continue;
    }

    const size_t end = complete ? lineEnd : safeTruncatedEnd(pos, lineEnd);
    if (end > pos) streamParagraph(pos, end, complete, continuation, cpAt);
    if (done_) break;
    if (!complete) {
      // 殘缺的尾巴裡沒有任何可以安全送出的字。
      out_.chunkCut = true;
      done_ = true;
      out_.nextOffset = pos;
      break;
    }
    cpAt += countCodepoints(pos, after);
    pos = after;
    continuation = false;
  }

  if (!done_) out_.nextOffset = pos;  // 排到 chunk 尾（檔尾）還沒填滿

  // 最後一道：沒有 OOM、頁上什麼都沒有、也沒有前進 ＝ 這一塊是無法送出的殘渣（例如整塊都是
  // 孤立的延續位元組）。往前推一個碼位；裡面沒有能畫的字，所以不算跳字。
  if (!collect_ && !out_.oom && out_.units.empty() && out_.nextOffset == 0 && len_ > 0) {
    out_.nextOffset = nextCodepointByte(0);
  }
  if (collect_) out_.lastStarts = std::move(ring_);
}

}  // namespace

Result layoutPage(const char* chunk, const size_t chunkLen, const bool atEof, const bool startsMidParagraph,
                  const GfxRenderer& renderer, const Params& params) {
  Result out;
  if (!chunk || chunkLen == 0 || params.maxUnits < 1) return out;
  // ⚠️ 排版期間兩個全域軸向旗標都要跟著這一頁（v241）：
  //    - VerticalScope：直排的字寬量測與旋轉判斷看 renderer.isVerticalLayout()（EPUB 在 Section 建置時也包著它）。
  //    - VerticalKinsokuScope：addWord 的跨塊斷點判斷讀 ParsedText::verticalKinsoku()，不開就是橫排禁則。
  //    橫排也明確設 false —— 不賭上一個閱讀器留下的值。兩者都是 RAII，離開就還原。
  const GfxRenderer::VerticalScope axis(renderer, params.vertical);
  const ParsedText::VerticalKinsokuScope kinsoku(params.vertical);
  PageBuilder builder(chunk, chunkLen, atEof, renderer, params, out);
  builder.run(startsMidParagraph);
  return out;
}

}  // namespace txtengine
