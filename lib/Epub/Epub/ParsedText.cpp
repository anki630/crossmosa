#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstdarg>
#include <cstdio>

#include "ParsedText.h"

#include <BidiUtils.h>
#include <Breadcrumb.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// v272：**約物擠壓**（橫排）。clreq §3.1.6／JLReq：中文兩端對齊的正確順序是
//   「先壓縮全形標點（最多半格）→ 不夠才拉開字距 →（行尾）懸掛」。現況少了第一步，
//   所以禁則把標點推到下一行之後，那一行只能靠拉字距填滿 —— 使用者看到的「字距忽寬忽窄」。
//
// v274：集合擴到**括弧類**（JLReq 3.1「約物のアキ」／clreq §3.1.6 的標點擠壓表）。
//   實測我們五套字型 22pt（字框 45.8）：
//     句讀 。，、：； → 墨水**置中**，左右各留約 16px（台灣慣例，日文是靠角落 —— 我們跟台灣）
//     開括號 「（《   → 墨水靠右，**左邊留 26–31px**
//     閉括號 」）》   → 墨水靠左，**右邊留 26–31px**
//   → 括弧那半格空白就是 JLReq 說的「約物のアキ」，行要調整時它是**第一個該讓出來的**。
//   使用者的實例：「知識之聲」那一行因為兩個括號各佔了 0.6 格的空白而排不下一個字，只好拉字距。
// ⚠️ ！？ 不在集合裡：clreq 的擠壓表沒有它們（它們不帶那半格 aki，墨水本來就窄）。
inline bool isCompressibleOpenBracket(const uint32_t cp) {
  return cp == 0x300C     // 「
      || cp == 0x300E     // 『
      || cp == 0xFF08     // （
      || cp == 0x3014     // 〔
      || cp == 0xFF3B     // ［
      || cp == 0xFF5B     // ｛
      || cp == 0x3008     // 〈
      || cp == 0x300A     // 《
      || cp == 0x3010     // 【
      || cp == 0x3016;    // 〖
}
inline bool isCompressibleCloseBracket(const uint32_t cp) {
  return cp == 0x300D     // 」
      || cp == 0x300F     // 』
      || cp == 0xFF09     // ）
      || cp == 0x3015     // 〕
      || cp == 0xFF3D     // ］
      || cp == 0xFF5D     // ｝
      || cp == 0x3009     // 〉
      || cp == 0x300B     // 》
      || cp == 0x3011     // 】
      || cp == 0x3017;    // 〗
}
inline bool isCompressiblePunctuation(const uint32_t cp) {
  return isHangablePunctuation(cp)                    // 、，。
      || cp == 0xFF1A || cp == 0xFF1B                 // ：；（與句讀同形：墨水置中）
      || isCompressibleOpenBracket(cp) || isCompressibleCloseBracket(cp);
}

// v274：token **前面**那個開括號的位元組長度（0 ＝沒有，或整個 token 就是括號）。
//   尾端那條（下面）切的是「聲」」這種；這條切的是「「知」這種 —— 禁則不准在開括號後斷行，
//   所以它會跟後面的字黏成一個 token，不切就壓不到它。
inline size_t leadingCompressibleBracketBytes(const std::string& token) {
  const auto* p = reinterpret_cast<const unsigned char*>(token.c_str());
  const unsigned char* q = p;
  const uint32_t first = utf8NextCodepoint(&q);
  if (first == 0 || *q == '\0') return 0;  // 單獨一個碼位：不切（否則會無限迴圈）
  if (!isCompressibleOpenBracket(first)) return 0;
  return static_cast<size_t>(q - p);
}

// token 尾端那個可壓縮標點的位元組起點（npos ＝沒有，或整個 token 就是標點）。
inline size_t trailingCompressiblePunctOffset(const std::string& token) {
  const auto* p = reinterpret_cast<const unsigned char*>(token.c_str());
  size_t lastStart = std::string::npos;
  uint32_t last = 0;
  size_t n = 0;
  const unsigned char* q = p;
  while (*q) {
    const unsigned char* here = q;
    const uint32_t cp = utf8NextCodepoint(&q);
    if (cp == 0) break;
    last = cp;
    lastStart = static_cast<size_t>(here - p);
    ++n;
  }
  if (n < 2 || !isCompressiblePunctuation(last)) return std::string::npos;
  return lastStart;
}


// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
// Paragraph-level direction: scan the first N words to find base direction.
constexpr size_t RTL_PARAGRAPH_PROBE_WORDS = 3;
// Per-word: scan enough chars to see through leading neutrals (quotes, numbers)
// before giving up. 64 is a hedge for pathological cases like long numeric tokens.
constexpr int RTL_PER_WORD_PROBE_DEPTH = 64;
constexpr size_t MIN_JUSTIFY_GAPS = 1;

// Byte-level pre-check: Hebrew UTF-8 lead bytes 0xD6-0xD7, Arabic/Syriac 0xD8-0xDB.
bool mayContainRtlBytes(const char* str) {
  for (const auto* p = reinterpret_cast<const unsigned char*>(str); *p; ++p) {
    if (*p >= 0xD6 && *p <= 0xDB) return true;
  }
  return false;
}

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

// v118/v161：CJK 禁則四函式移至 lib/Utf8/Utf8.h 共用（txt 閱讀器與 wrappedText 同一份）。
// 移除前逐 case 比對過與本檔原複本完全相同 —— 排版結果不變。

uint32_t countCodepoints(const std::string_view text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.data());
  const auto* const end = ptr + text.size();
  uint32_t count = 0;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }
  return count;
}

std::vector<size_t> cjkCharacterBreakByteOffsets(const std::string& text) {
  struct CodepointBoundary {
    uint32_t cp;
    size_t endOffset;
  };

  std::vector<CodepointBoundary> codepoints;
  codepoints.reserve(text.size());
  bool hasCjkBreakable = false;

  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) break;
    if (utf8IsCjkBreakable(cp)) {
      hasCjkBreakable = true;
    }
    codepoints.push_back({cp, static_cast<size_t>(ptr - start)});
  }

  if (!hasCjkBreakable || codepoints.size() < 2) return {};

  std::vector<size_t> allowedOffsets;
  allowedOffsets.reserve(codepoints.size() - 1);
  for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
    const uint32_t current = codepoints[i].cp;
    const uint32_t next = codepoints[i + 1].cp;
    if (!hasCjkBreakOpportunityBetween(current, next, ParsedText::verticalKinsoku())) continue;
    allowedOffsets.push_back(codepoints[i].endOffset);
  }
  return allowedOffsets;
}

int computeJustifyExtra(const int spareSpace, const size_t gapCount) {
  if (gapCount < MIN_JUSTIFY_GAPS || spareSpace <= 0) return 0;
  // Distribute the spare space evenly across gaps. Do NOT bail out to 0 when the
  // per-gap stretch is large: a sparse line (few words on a wide page) legitimately
  // needs big gaps to reach the margin. Returning 0 there disables justification for
  // that line, leaving it right-aligned (RTL) / left-aligned (LTR) — the mismatched
  // alignment bug. Match the un-capped behavior of the old code.
  return spareSpace / static_cast<int>(gapCount);
}

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

// Checks if a UTF-8 codepoint should be counted as part of a word for Focus Reading
bool isWordCharacter(uint32_t cp) {
  // ASCII range (Catches 95%+ of characters immediately)
  if (cp < 128) {
    // Bitwise trick: (cp | 0x20) converts uppercase ASCII to lowercase.
    // This checks for A-Z and a-z mathematically, avoiding memory lookups and <cctype>
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '\'';
  }

  // General Punctuation Block, Currency, Math, Arrows, & Symbols (0x2000 - 0x2BFF)
  if (cp >= 0x2000 && cp <= 0x2BFF) {
    // Explicitly allow smart quotes, reject all other general punctuation (em-dashes, etc.)
    return cp == 0x2018 || cp == 0x2019;
  }

  // Latin-1 Punctuation Block (0x00A1 - 0x00BF)
  if (cp >= 0x00A1 && cp <= 0x00BF) {
    // Allow ordinal indicators and micro sign, reject the rest (¡, ¿, «, », etc.)
    return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
  }

  // Rejects Two-em dash, Three-em dash, Double oblique hyphen, etc.
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;

  // Rejects Modifier Minus (0x02D7), Small Hyphen (0xFE63), and Fullwidth Hyphen (0xFF0D)
  if (cp == 0x02D7 || cp == 0xFE63 || cp == 0xFF0D) return false;
  // Assume all other Unicode ranges (accented letters, Cyrillic, Greek, etc.) are valid

  return true;
}

}  // namespace

uint32_t ParsedText::visibleOffsetBaseAt(const size_t wordIndex) const {
  uint32_t base = visibleOffsetBase;
  for (const auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex > wordIndex) break;
    base = rebase.base;
  }
  return base;
}

uint32_t ParsedText::visibleOffsetAt(const size_t wordIndex) const {
  if (wordIndex >= wordVisibleOffsetDeltas.size()) return 0;
  return visibleOffsetBaseAt(wordIndex) + wordVisibleOffsetDeltas[wordIndex];
}

void ParsedText::pushVisibleOffset(const uint32_t offset) {
  uint32_t base = visibleOffsetBase;
  if (wordVisibleOffsetDeltas.empty()) {
    visibleOffsetBase = offset;
    base = offset;
  } else if (!visibleOffsetRebases.empty()) {
    base = visibleOffsetRebases.back().base;
  }

  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    visibleOffsetRebases.push_back({wordVisibleOffsetDeltas.size(), offset});
    base = offset;
  }
  wordVisibleOffsetDeltas.push_back(static_cast<uint16_t>(offset - base));
}

void ParsedText::insertVisibleOffset(const size_t wordIndex, const uint32_t offset) {
  const uint32_t base = wordIndex > 0 ? visibleOffsetBaseAt(wordIndex - 1) : visibleOffsetBase;
  for (auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex >= wordIndex) rebase.wordIndex++;
  }

  uint32_t insertionBase = base;
  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    const auto rebaseIt = std::find_if(visibleOffsetRebases.begin(), visibleOffsetRebases.end(),
                                       [wordIndex](const auto& rebase) { return rebase.wordIndex > wordIndex; });
    visibleOffsetRebases.insert(rebaseIt, {wordIndex, offset});
    insertionBase = offset;
  }
  wordVisibleOffsetDeltas.insert(wordVisibleOffsetDeltas.begin() + wordIndex,
                                 static_cast<uint16_t>(offset - insertionBase));
}

void ParsedText::eraseVisibleOffsetPrefix(const size_t count) {
  if (count >= wordVisibleOffsetDeltas.size()) {
    wordVisibleOffsetDeltas.clear();
    visibleOffsetRebases.clear();
    visibleOffsetBase = 0;
    return;
  }

  const uint32_t newBase = visibleOffsetBaseAt(count);
  wordVisibleOffsetDeltas.erase(wordVisibleOffsetDeltas.begin(), wordVisibleOffsetDeltas.begin() + count);
  size_t writeIndex = 0;
  for (auto rebase : visibleOffsetRebases) {
    if (rebase.wordIndex <= count) continue;
    rebase.wordIndex -= count;
    visibleOffsetRebases[writeIndex++] = rebase;
  }
  visibleOffsetRebases.resize(writeIndex);
  visibleOffsetBase = newBase;
}

// v6/v149：被守護的成長之上留的安全邊際。2,048 是舊樹調校過的值（「第一版 v6 切得太狠」），
// 太大會誤拒「只是碎片化但其實夠用」的堆積。
static constexpr size_t LOW_HEAP_HEADROOM = 2 * 1024;

char ParsedText::lastRefusal[96] = {0};
ParsedText::RefusalHook ParsedText::refusalHook_ = nullptr;
void* ParsedText::refusalCtx_ = nullptr;
uint32_t ParsedText::buildGapMaxUs = 0;
uint8_t ParsedText::buildGapSite = 0;
uint32_t ParsedText::buildProbeCount = 0;
ParsedText::BuildProf ParsedText::buildProf;
bool ParsedText::buildProfInCd = false;
int64_t ParsedText::profNowUs() { return esp_timer_get_time(); }

namespace {
int64_t g_probeLastUs = 0;
// v190：只在 parseStep 進行中才計數。TextSettingsPreview 也會呼叫 layoutAndExtractLines，
// 而閱讀器切到設定頁時 onExit 不一定跑過（建置還活著）——沒有這道閘，預覽的第一個探針會把
// 「上一步到現在」這段秒級空檔算成一次盲區，gapmax 就是假的（本版唯一的產出，不能髒）。
bool g_probeArmed = false;
// v252：建置期間的按鍵輪詢（見 ParsedText.h）。
bool (*g_buildInputPollHook)() = nullptr;
bool g_buildInputPending = false;
int64_t g_buildInputLastPollUs = 0;
constexpr int64_t kBuildInputPollIntervalUs = 15000;
constexpr uint8_t kBuildProbeLine = 1;
constexpr uint8_t kBuildProbeWord = 2;
constexpr uint8_t kBuildProbeBreaks = 6;  // v190：斷行 DP 與字寬量測必須分得開，否則證不出哪個插點真的跑過
constexpr size_t kBuildProbeWordStride = 32;
}  // namespace

void (*ParsedText::vertDiagHook)(const char*) = nullptr;

namespace {
bool g_verticalKinsoku = false;
}

void ParsedText::setVerticalKinsoku(const bool enabled) { g_verticalKinsoku = enabled; }
bool ParsedText::verticalKinsoku() { return g_verticalKinsoku; }

void ParsedText::vertDiag(const char* fmt, ...) {
  if (!vertDiagHook) return;
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  vertDiagHook(buf);
}

void ParsedText::noteBuildProbe(const uint8_t site) {
  if (!g_probeArmed) return;
  const int64_t now = esp_timer_get_time();
  if (g_probeLastUs != 0) {
    const int64_t gap = now - g_probeLastUs;
    if (gap > static_cast<int64_t>(buildGapMaxUs)) {
      buildGapMaxUs = static_cast<uint32_t>(gap);
      buildGapSite = site;
    }
  }
  g_probeLastUs = now;
  buildProbeCount++;
  // v252：排版中的探針點平均幾 ms 一個（diag251 一章 3,662 次／29.6 秒、最長 148ms），
  // 在這裡讀按鍵才接得住 0.1 秒的短按 —— 步與步之間（平均 0.36 秒、最長 0.8 秒）只問一次會整個漏掉。
  // 接到之後【繼續】輪詢（codex 複查）：停下來的話放開要等這一步結束才被看到，短按會被讀成長按（長按跳章）。
  if (g_buildInputPollHook && now - g_buildInputLastPollUs >= kBuildInputPollIntervalUs) {
    g_buildInputLastPollUs = now;
    if (g_buildInputPollHook()) g_buildInputPending = true;
  }
}

void ParsedText::setBuildInputPollHook(bool (*fn)()) {
  g_buildInputPollHook = fn;
  g_buildInputPending = false;
  g_buildInputLastPollUs = 0;
}

bool ParsedText::buildInputPending() { return g_buildInputPending; }

void ParsedText::resetBuildProbeClock() {
  g_probeLastUs = esp_timer_get_time();  // v190：每個 parseStep 的計時起點，不含步與步之間的讓路
  g_probeArmed = true;
}

void ParsedText::stopBuildProbeClock() { g_probeArmed = false; }

// 先到先得：不覆寫還沒被讀走的紀錄（同 ImageBlock::noteFailure 的理由）。
static void noteRefusal(const char* what, size_t need, size_t defMax, size_t defFree) {
  if (breadcrumbPending(ParsedText::lastRefusal)) return;
  char line[sizeof(ParsedText::lastRefusal)];
  snprintf(line, sizeof(line), "%s need=%u defMax=%u defFree=%u", what, static_cast<unsigned>(need),
           static_cast<unsigned>(defMax), static_cast<unsigned>(defFree));
  breadcrumbPublish(ParsedText::lastRefusal, sizeof(ParsedText::lastRefusal), line);  // v249：跨 task 交接
  if (ParsedText::refusalHook_) ParsedText::refusalHook_(ParsedText::refusalCtx_);
}

namespace {
bool g_boldBodyText = false;
}  // namespace

void ParsedText::setBoldBodyText(const bool enabled) { g_boldBodyText = enabled; }

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const uint32_t visibleTextOffset) {
  if (word.empty()) return;
  buildProf.words++;  // v252（只計數，不在每個字上計時）

  // The device fonts carry no combining-mark positioning, so EPUB text stored in NFD
  // (a base letter followed by separate combining accents -- common for Vietnamese,
  // and used for many EPUB <h1> chapter headings) renders with the marks detached or
  // misplaced. Compose to NFC here, the single funnel every word passes through, so a
  // precomposed glyph is used instead. This runs once per word at layout time (the
  // result is cached in the section file) and is a cheap no-op for mark-free text.
  word = utf8ComposeNfc(word);

  // 粗體閱讀：所有內文字升成粗體（已粗的標題不變）。放在 focus reading 判斷之前——內文全粗時
  // focus reading 自然無事可做（下面「已粗就整字粗」那條會接手）。
  EpdFontFamily::Style baseStyle =
      g_boldBodyText ? static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD) : fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }
  const bool wordStartsRtl = !hasRtlWord && mayContainRtlBytes(word.c_str()) &&
                             BidiUtils::startsWithRtl(word.c_str(), RTL_PER_WORD_PROBE_DEPTH);

  // Bulk-reserve the per-token parallel arrays before a burst of pushes so they
  // don't repeatedly double. Only the std::vector arrays are reserved: words and
  // rubyTexts are std::deque (chunked growth, no reserve()/capacity() and no large
  // contiguous reallocation to avoid). wordStyles' capacity gauges them all since
  // pushToken() keeps every array in lockstep.
  const auto ensureTokenCapacity = [&](const size_t additionalTokens) {
    if (oom_) return;
    if (additionalTokens == 0) return;
    const size_t requiredSize = words.size() + additionalTokens;
    if (wordStyles.capacity() >= requiredSize) return;

    size_t newCapacity = wordStyles.capacity() < 16 ? 16 : wordStyles.capacity();
    while (newCapacity < requiredSize) {
      newCapacity *= 2;
    }

    // v6/v149：這些 reserve 是【會丟例外的】—— -fno-exceptions 下 OOM 直接 abort 整台機器。
    // 實機（v148，讀一本長段落的書）：背景建置視窗內 p3 max=2,176、p2 被瞬時壓力壓碎，
    // addWord 的 vector 成長 abort（addr2line 落在 characterData→addWord→operator new）。
    // 最大的單一連續塊是 wordVisibleOffsetDeltas（2B/元素）；其餘（1B + 三個 bit-packed）
    // 落在 headroom 內 —— 與舊樹「words[] dominates」同一judgment 形狀。
    // 先試攤銷的加倍容量，配不下再試剛好的大小，再不行才拒絕並 latch ——
    // 讓「只是碎片化但其實夠用」的堆積照常排版。
    // codex 抓到的阻斷：只查「最大單塊」不夠 —— 五次 reserve 會從同一個洞逐一切走，
    // 前四次把洞切碎之後，最大的那次（deltas 2N）可能反而配不到。兩個對策：
    //  ① 預算查【兩側】：最大單塊 2N，總量 4N（≈3.4N 的新配置 + 舊緩衝在 reserve
    //     期間仍活著的暫時峰值）。
    //  ② 【最大的先配】：deltas(2N) 趁洞還完整時先拿，其餘（N + 3×N/8）跟在後面。
    // ⚠️ 誠實界線：throwing STL 下這仍是啟發式，不是證明 —— deque node、NFC、
    //    layout 的配置都還在守備範圍外。這一版買的是「把實測 crash 簽名的那條路徑
    //    從常見變罕見」，不是完全的 OOM 免疫（那需要自管容器，帳本記著）。
    // ⚠️ DEFAULT caps，不是 ESP.getMaxAllocHeap()（INTERNAL）—— 理由見 extractLine 的守衛。
    const auto budgetOk = [&](const size_t cap) {
      return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) >= cap * 2 + LOW_HEAP_HEADROOM &&
             heap_caps_get_free_size(MALLOC_CAP_DEFAULT) >= cap * 4 + 2 * LOW_HEAP_HEADROOM;
    };
    if (!budgetOk(newCapacity)) {
      newCapacity = requiredSize;
      if (!budgetOk(newCapacity)) {
        const size_t dm = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        const size_t df = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        LOG_ERR("PTX", "token reserve refused: %u words defMax=%u defFree=%u", static_cast<unsigned>(requiredSize),
                static_cast<unsigned>(dm), static_cast<unsigned>(df));
        noteRefusal("token-reserve", requiredSize * 2, dm, df);
        oom_ = true;
        return;
      }
    }

    wordVisibleOffsetDeltas.reserve(newCapacity);  // 最大的先配（見上）
    wordStyles.reserve(newCapacity);
    wordContinues.reserve(newCapacity);
    wordNoSpaceBefore.reserve(newCapacity);
    wordIsFocusSuffix.reserve(newCapacity);
  };

  const auto pushToken = [&](std::string token, const bool continues, const bool noSpaceBefore,
                             const bool isFocusSuffix, const uint32_t tokenOffset) {
    // v6/v149：每一條 append 都走這個漏斗，一道守衛涵蓋所有路徑（CJK 逐字、單 token、
    // focus split）。拒絕時丟掉這個 token 並保持 oom_ —— 解析器會看到 hasOom() 而中止本章，
    // 已排好的部分由 Section 保留成 partial。五個 vector 一起 reserve，過了閘的 token
    // 不可能在 push 中途重配而彼此失步。
    ensureTokenCapacity(1);
    if (oom_) return;
    words.push_back(std::move(token));
    wordStyles.push_back(baseStyle);
    wordContinues.push_back(continues);
    wordNoSpaceBefore.push_back(noSpaceBefore);
    wordIsFocusSuffix.push_back(isFocusSuffix);
    pushVisibleOffset(tokenOffset);
    if (!rubyTexts.empty()) {
      rubyTexts.push_back("");
    }
  };

  bool effectiveAttachToPrevious = attachToPrevious;
  bool effectiveNoSpaceBefore = false;
  // Only a glued token (attachToPrevious == true, i.e. no whitespace separated it from the
  // previous one in the source) may be turned into a gap-less break opportunity. When real
  // whitespace separated the two words, that space is content and must be rendered: Korean
  // is a space-delimited script written in Hangul, which utf8IsCjkBreakable() covers.
  if (attachToPrevious && !words.empty() &&
      hasCjkBreakOpportunityBetween(lastCodepoint(words.back()), firstCodepoint(word),
                                    ParsedText::verticalKinsoku())) {
    effectiveAttachToPrevious = false;
    effectiveNoSpaceBefore = true;
  }


  if (auto breakOffsets = cjkCharacterBreakByteOffsets(word); !breakOffsets.empty()) {
    // CJK-heavy paragraphs can push hundreds of tiny tokens quickly when CSS toggles
    // inline styles. Reserve once up front to avoid repeated vector growth reallocations.
    ensureTokenCapacity(breakOffsets.size() + 1);
    bool firstToken = true;
    size_t tokenStart = 0;
    uint32_t tokenVisibleOffset = visibleTextOffset;
    // v272：**行尾的句讀自己一個 token**。切詞期禁則讓「英，」黏成一個 token，而黏著的標點
    //   沒有自己的座標 —— 既不能單獨壓縮（約物擠壓），行尾懸掛也只能靠「扣寬度」間接表達。
    //   切開之後用 `continues=true` 接回去：那個旗標的語意就是「不可以在這裡斷行、而且沒有空隙」，
    //   所以禁則一個位元都沒鬆（DP 與貪婪斷行都看它）。
    //   ⚠️ **只有橫排切**：直排的 `hangTailOf` 要求 token ≥2 碼位，切開會讓直排的懸掛失效
    //      （它自己有一套，別動）。
    const bool splitTrailingPunct = !ParsedText::verticalKinsoku();
    const auto pushMaybeSplit = [&](std::string token, const bool continues, const bool noSpaceBefore,
                                    const uint32_t tokenOffset) {
      if (splitTrailingPunct) {
        // 連續標點（例如「英，。」）要**每一個都切開**，否則前面那個還是埋在混合 token 裡、壓不到（codex）。
        // v274：兩頭都切 —— 前面的開括號（「知）與尾端的標點（聲」、英，。）。
        //   切出來的順序必須與原文一致：[開括號…][本體][尾端標點…]。
        std::string body = std::move(token);
        std::vector<std::string> pieces;
        while (true) {
          const size_t lead = leadingCompressibleBracketBytes(body);
          if (lead == 0) break;
          pieces.push_back(body.substr(0, lead));
          body.erase(0, lead);
        }
        std::vector<std::string> tails;
        while (true) {
          const size_t tailStart = trailingCompressiblePunctOffset(body);
          if (tailStart == std::string::npos) break;
          tails.push_back(body.substr(tailStart));
          body.erase(tailStart);
        }
        if (!pieces.empty() || !tails.empty()) {
          if (!body.empty()) pieces.push_back(std::move(body));
          for (auto it = tails.rbegin(); it != tails.rend(); ++it) pieces.push_back(std::move(*it));
          uint32_t offset = tokenOffset;
          for (size_t k = 0; k < pieces.size(); ++k) {
            const uint32_t cps = countCodepoints(pieces[k]);
            // 第一片繼承原 token 的旗標；其餘一律 continues＝「不可以在這裡斷行、而且沒有空隙」。
            //   noSpaceBefore 也給 true（原文本來就沒有空白），兩個旗標要自洽，別讓將來的消費者
            //   看到「有空白」而插出一個空隙（codex）。
            pushToken(std::move(pieces[k]), k == 0 ? continues : true, k == 0 ? noSpaceBefore : true, false, offset);
            offset += cps;
          }
          return;
        }
        token = std::move(body);
      }
      pushToken(std::move(token), continues, noSpaceBefore, false, tokenOffset);
    };
    for (const size_t breakOffset : breakOffsets) {
      if (breakOffset <= tokenStart || breakOffset > word.size()) continue;
      const std::string_view token(word.data() + tokenStart, breakOffset - tokenStart);
      pushMaybeSplit(std::string(token), firstToken ? effectiveAttachToPrevious : false,
                     firstToken ? effectiveNoSpaceBefore : true, tokenVisibleOffset);
      tokenVisibleOffset += countCodepoints(token);
      firstToken = false;
      tokenStart = breakOffset;
    }
    if (tokenStart < word.size()) {
      pushMaybeSplit(word.substr(tokenStart), firstToken ? effectiveAttachToPrevious : false,
                     firstToken ? effectiveNoSpaceBefore : true, tokenVisibleOffset);
    }
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  if (containsCjkBreakableCodepoint(word)) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, false, visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // Already-bold text should stay fully bold; focus splitting would make its suffix regular later.
  if (!this->focusReadingEnabled || (baseStyle & EpdFontFamily::BOLD) != 0) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, false, visibleTextOffset);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // --- FOCUS READING LOGIC BELOW ---

  // Worst case: a segment boundary on each byte (highly punctuated UTF-8 text).
  ensureTokenCapacity(word.length());
  // v149（codex 抓到的阻斷）：這條 Focus Reading 路徑【不走 pushToken】，下面的
  // processSegment 是裸的 emplace_back —— 守衛拒絕之後不 return 的話，裸 push 照樣
  // 撞 throwing 成長。capacity 已由上面的 bulk ensure 保證（word.length() >= token 數），
  // 所以過了這一關之後的裸 push 不會觸發 vector 成長。
  if (oom_) return;

  // Lambda helper to process and push individual sub-segments of the string
  // Use std::string_view to avoid heap allocations when slicing
  auto processSegment = [&](std::string_view segment, bool isWord, bool attach, bool noSpaceBefore) {
    const unsigned char* wordBegin = reinterpret_cast<const unsigned char*>(word.data());
    const unsigned char* segmentBegin = reinterpret_cast<const unsigned char*>(segment.data());
    uint32_t segmentOffset = visibleTextOffset;
    const unsigned char* offsetPtr = wordBegin;
    while (offsetPtr < segmentBegin) {
      utf8NextCodepoint(&offsetPtr);
      segmentOffset++;
    }
    if (!isWord) {
      // Punctuation and Numbers stay regular
      words.emplace_back(segment);
      wordStyles.push_back(baseStyle);
      wordContinues.push_back(attach);
      wordNoSpaceBefore.push_back(noSpaceBefore);
      wordIsFocusSuffix.push_back(false);
      pushVisibleOffset(segmentOffset);
    } else {
      size_t charCount = 0;
      const unsigned char* countPtr = reinterpret_cast<const unsigned char*>(segment.data());
      const unsigned char* countEnd = countPtr + segment.length();

      while (countPtr < countEnd) {
        utf8NextCodepoint(&countPtr);
        charCount++;
      }

      // Target 45% for 1-bold at 4 chars and 3-bold at 7 chars with floor truncation
      constexpr size_t FOCUS_READING_PERCENT = 45;
      size_t targetBoldChars = (charCount * FOCUS_READING_PERCENT) / 100;
      targetBoldChars = std::clamp<size_t>(targetBoldChars, 1, 9);

      if (targetBoldChars >= charCount) {
        // Whole segment is bold - no suffix split needed
        words.emplace_back(segment);
        wordStyles.push_back(static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD));
        wordContinues.push_back(attach);
        wordNoSpaceBefore.push_back(noSpaceBefore);
        wordIsFocusSuffix.push_back(false);
        pushVisibleOffset(segmentOffset);
      } else {
        countPtr = reinterpret_cast<const unsigned char*>(segment.data());
        for (size_t i = 0; i < targetBoldChars; ++i) {
          utf8NextCodepoint(&countPtr);
        }
        size_t splitByteOffset = countPtr - reinterpret_cast<const unsigned char*>(segment.data());

        // Bold prefix
        words.emplace_back(segment.substr(0, splitByteOffset));
        wordStyles.push_back(static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD));
        wordContinues.push_back(attach);
        wordNoSpaceBefore.push_back(noSpaceBefore);
        wordIsFocusSuffix.push_back(false);
        pushVisibleOffset(segmentOffset);

        // Regular suffix - marked so extractLine can merge it back into single TextBlock entry
        words.emplace_back(segment.substr(splitByteOffset));
        wordStyles.push_back(baseStyle);
        wordContinues.push_back(true);
        wordNoSpaceBefore.push_back(false);
        wordIsFocusSuffix.push_back(true);
        pushVisibleOffset(segmentOffset + static_cast<uint32_t>(targetBoldChars));
      }
    }
  };

  // Tokenize the string by alternating states (Word vs. Non-Word)
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* end = ptr + word.length();

  const unsigned char* segmentStart = ptr;
  uint32_t firstCp = utf8NextCodepoint(&ptr);  // Consume the first char to determine initial state
  bool inWordSegment = isWordCharacter(firstCp);

  bool isFirstSegment = true;

  while (ptr < end) {
    const unsigned char* currentCpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    bool isWordChar = isWordCharacter(cp);

    // Whenever the character type flips, slice off the segment we just completed and process it
    if (isWordChar != inWordSegment) {
      size_t segmentLen = currentCpStart - segmentStart;
      std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);

      // Only the very first segment inherits the original attachToPrevious flag.
      // Every subsequent segment MUST attach=true so it glues seamlessly to the prefix.
      processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                     isFirstSegment ? effectiveNoSpaceBefore : false);

      // Setup for the next segment
      segmentStart = currentCpStart;
      inWordSegment = isWordChar;
      isFirstSegment = false;
    }
  }

  // Process the final remaining segment
  size_t segmentLen = end - segmentStart;
  std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);
  processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                 isFirstSegment ? effectiveNoSpaceBefore : false);
  if (wordStartsRtl) {
    hasRtlWord = true;
  }
}

void ParsedText::setRubyForWordAt(size_t index, const std::string& ruby) {
  if (index >= words.size()) return;
  if (rubyTexts.size() <= index) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[index] = ruby;
}

void ParsedText::setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby) {
  if (startIndex >= words.size()) return;
  if (rubyTexts.size() <= startIndex) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[startIndex] = ruby;
  for (size_t i = 1; i < count; i++) {
    size_t idx = startIndex + i;
    if (idx >= words.size()) break;
    if (rubyTexts.size() <= idx) {
      rubyTexts.resize(words.size());
    }
    rubyTexts[idx] = "";
    wordStyles[idx] =
        static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(wordStyles[idx]) | EpdFontFamily::RUBY_CONTINUE);
    wordContinues[idx] = true;  // Prevent page breaker from splitting the Group Ruby!
  }
}

void ParsedText::ensureRubyCapacity() {
  // No-op: rubyTexts is a std::deque (chunked growth, no capacity to pre-reserve
  // and no large contiguous reallocation to avoid). Kept for call-site stability.
}

// 「自然對齊」＝ 兩端對齊，或順著文字流方向靠齊。置中／逆流向靠齊的區塊不縮排。
// ⚠️ 橫排與直排**都要呼叫**，而且要在讀 isNaturalAlign 之前。
void ParsedText::updateNaturalAlign() {
  isNaturalAlign =
      blockStyle.alignment == CssTextAlign::Justify ||
      (blockStyle.isRtl ? blockStyle.alignment == CssTextAlign::Right : blockStyle.alignment == CssTextAlign::Left);
}

int ParsedText::resolveFirstLineIndent(const bool isFirstLine, const GfxRenderer& renderer, const int fontId) const {
  if (!isFirstLine || !isNaturalAlign) {
    return 0;
  }
  if (blockStyle.textIndentDefined) {
    if (blockStyle.textIndent < 0) {
      // v263：懸掛縮排（負值）最多只能用掉區塊自己那一側的（正的）邊距，不再往外凸。
      // BlockStyle::fromCssStyle 把左右邊距夾在 MAX_HORIZONTAL_INSET_EM，
      // text-indent 卻原樣保留 —— 「margin-left:4.5em; text-indent:-2.5em」的清單
      // 夾完變成 2em 邊距配 -2.5em 縮排，首行從版心左緣再往外 0.5em，編號被切掉一半。
      // 凸出的那一側：LTR 是左（xpos 從縮排起算），RTL 是右（有效行寬加長、靠右對齊）；
      // 三個呼叫點（兩種斷行＋定位）都用同一個「行寬 − 縮排」，所以斷行與位置一致。
      // ⚠️ 邊距本身是負的（CSS 合法）時縮排歸零，但區塊自己已經在版心外，這裡救不回來。
      // 取「邊距全部用完」而不是「照邊距被夾的比例縮小縮排」：前者留給編號的懸掛空間最大，
      // 等比縮小（-2.5em × 2/4.5 ≈ -1.1em）會讓兩位數編號壓到續行文字底下。
      const int inset = blockStyle.isRtl ? blockStyle.rightInset() : blockStyle.leftInset();
      const int floor = inset > 0 ? -inset : 0;
      return blockStyle.textIndent < floor ? floor : blockStyle.textIndent;
    }
    if (!extraParagraphSpacing) {
      return blockStyle.textIndent;
    }
    return 0;
  }
  if (!extraParagraphSpacing) {
    return renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR) * 3;
  }
  return 0;
}
// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }
  // v252 BUILDPROF：整段排版（RAII：所有出口都計）。
  struct LayProf {
    int64_t t0 = ParsedText::profNowUs();
    ~LayProf() {
      const uint64_t d = static_cast<uint64_t>(ParsedText::profNowUs() - t0);
      ParsedText::buildProf.layUs += d;
      if (ParsedText::buildProfInCd) ParsedText::buildProf.layCdUs += d;
      ParsedText::buildProf.layCalls++;
    }
  } layProf;

  // Per-paragraph RTL auto-detection: only when CSS/HTML didn't explicitly set direction.
  // Explicit dir="ltr" must be respected and not overridden by content heuristic.
  if (!blockStyle.directionDefined && hasRtlWord) {
    // Check the first few words for RTL letter codepoints (no heap allocation).
    const size_t wordsToScan = std::min(words.size(), RTL_PARAGRAPH_PROBE_WORDS);
    for (size_t i = 0; i < wordsToScan; ++i) {
      if (BidiUtils::startsWithRtl(words[i].c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
        blockStyle.isRtl = true;
        break;
      }
    }
  }

  updateNaturalAlign();

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For SD card fonts this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  if (renderer.isSdCardFont(fontId)) {
    // v254：逐字字重 —— 每個字重只準備自己那些字的字寬（原本段落裡有一個粗體字，整段每個字都會去讀粗體字寬）。
    const int64_t advT0 = profNowUs();
    renderer.ensureSdCardFontReady(fontId, words, wordStyles, hyphenationEnabled);
    buildProf.advUs += static_cast<uint64_t>(profNowUs() - advT0);
  }

  const int pageWidth = viewportWidth;
  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled || greedyLineBreaks_) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    // v240：greedyLineBreaks_（txt 專用）也走這裡，但下面的斷字只在 hyphenationEnabled 時才做。
    lineBreakIndices =
        computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, lineBreakIndices, processLine, renderer,
                fontId);
    noteBuildProbe(kBuildProbeLine);  // v190：每行一次，量「行級讓路」後盲區會剩多少
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
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

static inline bool isCjkIdeograph(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x3FFFF);
}

// The first word of a line may have its ruby characters wider than the word (the base text). In that case, we need to
// move the base text to the right a bit so that ruby text doesn't overflow the left border, and it is still centered
// over the base text. This function calculates how much we need to move the base text to the right.
int ParsedText::calculateRubyExtraStartOffset(const size_t wordIdx, const size_t maxWordIdx,
                                              const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || wordIdx >= rubyTexts.size() || rubyTexts[wordIdx].empty() ||
      (wordStyles[wordIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  size_t groupWordCount = 1;
  while (wordIdx + groupWordCount < maxWordIdx &&
         (wordStyles[wordIdx + groupWordCount] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    groupWordCount++;
  }
  int groupActualWidth = 0;
  for (size_t k = 0; k < groupWordCount; ++k) {
    groupActualWidth += measureWordWidth(renderer, fontId, words[wordIdx + k], wordStyles[wordIdx + k]);
  }
  const int rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[wordIdx].c_str(), EpdFontFamily::SUP);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  const int leftOverlap = (rubyWidth - groupActualWidth) / 2;

  // This function is only ever called for the first word of a line.
  // words[wordIdx - 1], if it exists, is always the last word of the *prior* line
  // and cannot absorb any left overhang on the current line.
  // The full leftOverlap must therefore be reserved as a visual indent so the
  // ruby text does not overflow the left margin.
  return leftOverlap;
}

// The last ruby group on a line may have its ruby characters wider than the group's base text.
// The right half of that overhang protrudes past the last base character. This function returns
// the amount of right-margin space that must be reserved so the ruby does not overflow the right
// border. It mirrors calculateRubyExtraStartOffset: words[lineBreak] is on the *next* line and
// cannot absorb any of the right overhang on the current line, so the full rightOverlap is returned.
int ParsedText::calculateRubyExtraEndOffset(const size_t lineStartIdx, const size_t lineBreakIdx,
                                            const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || lineBreakIdx == 0 || lineStartIdx >= lineBreakIdx) {
    return 0;
  }

  // Walk backwards from the last word to find the leader of the last ruby group on the line.
  size_t leaderIdx = lineBreakIdx - 1;
  while (leaderIdx > lineStartIdx && (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    leaderIdx--;
  }

  // leaderIdx must be a ruby group leader (non-empty ruby, no RUBY_CONTINUE flag).
  if (leaderIdx >= rubyTexts.size() || rubyTexts[leaderIdx].empty() ||
      (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  // Measure the group.
  int groupActualWidth = 0;
  for (size_t k = leaderIdx; k < lineBreakIdx; ++k) {
    groupActualWidth += measureWordWidth(renderer, fontId, words[k], wordStyles[k]);
  }
  const int rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[leaderIdx].c_str(), EpdFontFamily::SUP);
  if (rubyWidth <= groupActualWidth) {
    return 0;
  }

  return (rubyWidth - groupActualWidth) / 2;
}

// v272：約物擠壓的量。**v272 起句讀是自己一個 token**（見 addWord 的切分），
//   所以判斷看的是 token 本身，而不是在一個混合 token 裡面找尾巴。
//
// ⚠️ **v273 起橫排【不做】行尾懸掛**（使用者判斷：「橫排不適合使用懸吊的方式，看起來蠻怪的」）。
//    中文橫排的美感基礎是方格：漢字與全形標點各佔一格，版心右緣是一條直線；懸掛會把那條線打破。
//    日本的 JLReq 也把 ぶら下げ 列為【直排】的慣例，横組み 的標準作法是把句讀收進行內（約物半角／二分）
//    ——也就是下面這個擠壓。直排維持懸掛（ParsedTextVertical 自己那套），這裡只管橫排。
//    v271 的 `hangWidthAt`／`hangMarginPx` 已整個移除，不要復活；要復活先讀這段與帳本。
//
// 擠壓：這個 token 若是單獨一個可壓縮標點，最多可以讓出「它自己的空白」，上限半格（clreq 的二分）。
//
// v274：**讓出的量按左右空白的比例扣**，而不是一律左移半個壓縮量。
//   空白在哪一邊，就從哪一邊收 ——
//     句讀（墨水置中，左右各 16px）：對半收 → 與 v272/v273 的行為**逐像素相同**。
//     開括號 「（左邊 30px、右邊 0.8px）：幾乎全部從左邊收 → 墨水往左靠，右邊的細邊距保住。
//     閉括號 」（左邊 1px、右邊 30px）：幾乎全部從右邊收 → 字形**不動**，只是格子變窄。
//   這個比例規則自動滿足「不會撞到鄰字」：收掉的左側 cL ≤ 左空白、右側 cR ≤ 右空白，
//   所以墨水一定還在（變窄後的）格子裡 —— v272 那條 `min(2·gl, 2·(advance−gl−gw))` 的特例
//   就是這條的對稱情形，不必再另外寫（也不必再假設墨水置中）。
// ⚠️ 墨水可能超出字框（斜體、某些字型）→ 空白算成負的，夾到 0 再算，不要讓它變成「可以多讓一點」。
// ⚠️ 樣板：`ParsedText::words` 是 deque（分塊成長、避免大塊重配），而 extractLine 手上的是 vector。
//    兩邊要用**同一份**判斷，否則斷行與定位會各說各話。
struct CompressMetrics {
  int maxAmount = 0;  // 最多讓得出來的寬度
  int left = 0;       // 字框左側空白（夾過 0）
  int right = 0;      // 字框右側空白（夾過 0）
};

template <typename Words, typename Styles>
static CompressMetrics compressMetricsAt(const GfxRenderer& renderer, const int fontId, const Words& words,
                                         const Styles& styles, const size_t idx) {
  CompressMetrics m;
  if (idx >= words.size()) return m;
  const auto* p = reinterpret_cast<const unsigned char*>(words[idx].c_str());
  const uint32_t cp = utf8NextCodepoint(&p);
  if (cp == 0 || *p != '\0' || !isCompressiblePunctuation(cp)) return m;
  const int advance = measureWordWidth(renderer, fontId, words[idx], styles[idx]);
  int gw = 0, gh = 0, gl = 0, gt = 0;
  if (!renderer.getGlyphInkBox(fontId, cp, styles[idx], &gw, &gh, &gl, &gt)) return m;
  m.left = gl > 0 ? gl : 0;
  const int trailing = advance - (gl + gw);
  m.right = trailing > 0 ? trailing : 0;
  const int room = m.left + m.right;
  const int half = advance / 2;  // clreq §3.1.6：全形標點最多壓到二分
  int limit = room < half ? room : half;
  m.maxAmount = limit > 0 ? limit : 0;
  return m;
}

template <typename Words, typename Styles>
static uint16_t compressAmountAt(const GfxRenderer& renderer, const int fontId, const Words& words,
                                 const Styles& styles, const size_t idx) {
  return static_cast<uint16_t>(compressMetricsAt(renderer, fontId, words, styles, idx).maxAmount);
}

// 讓出 `amount` 時，其中有多少是從**左側**收的 ＝ 字形要往左挪多少。
inline int compressLeftShare(const CompressMetrics& m, const int amount) {
  const int room = m.left + m.right;
  if (amount <= 0 || room <= 0) return 0;
  const int share = (amount * m.left) / room;
  return share > m.left ? m.left : share;
}

// v271（codex）／v273：**斷行與定位要用同一個判準**，不然斷行認為放得下、定位卻照原寬擺 → 出血或壓縮錯。
//   ① 自然對齊才做（置中／逆向靠齊不做：壓縮只有在要填滿一行時才有意義）
//   ② 整段不含右到左文字 —— BiDi 重排之後「第 k 個 token」不一定還在原來的位置
//   ③ 專注閱讀模式不做 —— 那個模式會在詞後面另外畫東西，token 的視覺寬度不是它自己的字寬
bool ParsedText::punctFittingAllowed() const {
  return isNaturalAlign && !blockStyle.isRtl && !hasRtlWord && !focusReadingEnabled;
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i]));
    if ((i % kBuildProbeWordStride) == 0) {
      noteBuildProbe(kBuildProbeWord);  // v190：每 N 詞一次（SD 字寬量測）
    }
  }

  // Adjust widths for ruby groups to comply with JLReq standards
  if (!rubyTexts.empty()) {
    struct RubyGroupInfo {
      size_t start;
      size_t count;
      int baseWidth;
      int rubyWidth;
      int leftOverlap;
      int rightOverlap;
    };

    std::vector<RubyGroupInfo> groups;
    for (size_t i = 0; i < words.size(); ++i) {
      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyles[i] & EpdFontFamily::RUBY_CONTINUE) == 0) {
        RubyGroupInfo g;
        g.start = i;
        g.baseWidth = wordWidths[i];
        g.count = 1;
        while (i + g.count < words.size() && (wordStyles[i + g.count] & EpdFontFamily::RUBY_CONTINUE) != 0) {
          g.baseWidth += wordWidths[i + g.count];
          g.count++;
        }
        g.rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP);
        g.leftOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        g.rightOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        groups.push_back(g);
        i += g.count - 1;
      }
    }

    // Adjust widths based on adjacent characters and group-to-group spacing
    for (size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
      const auto& g = groups[gIdx];

      // 1. Preceding character (left overhang)
      if (g.start > 0) {
        const uint32_t cpPrev = lastCodepoint(words[g.start - 1]);
        if (isCjkIdeograph(cpPrev)) {
          wordWidths[g.start - 1] += g.leftOverlap;
        } else {
          const int maxLeftOverhang = wordWidths[g.start - 1] / 2;
          wordWidths[g.start - 1] += std::max(0, g.leftOverlap - maxLeftOverhang);
        }
      }

      // 2. Succeeding character (right overhang / group collision)
      const size_t nextIdx = g.start + g.count;
      if (nextIdx < words.size()) {
        if (gIdx + 1 < groups.size() && groups[gIdx + 1].start == nextIdx) {
          // Adjacent ruby groups: compute collision
          const auto& nextG = groups[gIdx + 1];
          const int collision = g.rightOverlap + nextG.leftOverlap;
          if (collision > 0) {
            wordWidths[g.start + g.count - 1] += collision;
          }
        } else {
          // Regular character following: check if it's Kanji
          const uint32_t cpNext = firstCodepoint(words[nextIdx]);
          if (isCjkIdeograph(cpNext)) {
            wordWidths[g.start + g.count - 1] += g.rightOverlap;
          } else {
            const int maxRightOverhang = wordWidths[nextIdx] / 2;
            wordWidths[g.start + g.count - 1] += std::max(0, g.rightOverlap - maxRightOverhang);
          }

          // Check if there is another ruby group further ahead separated only by non-ideographs
          if (gIdx + 1 < groups.size()) {
            const auto& nextG = groups[gIdx + 1];
            bool onlyNonIdeographsInBetween = true;
            int gapWidth = 0;
            for (size_t k = nextIdx; k < nextG.start; ++k) {
              const uint32_t cp = firstCodepoint(words[k]);
              if (isCjkIdeograph(cp)) {
                onlyNonIdeographsInBetween = false;
                break;
              }
              gapWidth += wordWidths[k];
            }
            if (onlyNonIdeographsInBetween) {
              const int maxRightOverhang = wordWidths[g.start + g.count - 1] / 2;
              const int maxLeftOverhang = wordWidths[nextG.start - 1] / 2;
              const int allowedRight = std::min(g.rightOverlap, maxRightOverhang);
              const int allowedLeft = std::min(nextG.leftOverlap, maxLeftOverhang);
              const int touchOverlap = allowedRight + allowedLeft - gapWidth;
              if (touchOverlap > 0) {
                wordWidths[g.start + g.count - 1] += touchOverlap;
              }
            }
          }
        }
      }
    }
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec) {
  if (words.empty()) {
    return {};
  }

  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // v272：可壓縮量先算成一張表 —— 下面的 DP 是 O(n²)，內迴圈裡逐次量會讓長段落變慢。
  //   只有自然對齊才需要（其餘對齊不壓縮），所以不是自然對齊時連配都不配。
  //   ⚠️ 必須在上面的 hyphenateWordAtIndex 迴圈【之後】建：那個迴圈會就地拆詞，words 會變。
  std::vector<uint16_t> compWidths;
  if (punctFittingAllowed()) {
    compWidths.reserve(totalWordCount);
    for (size_t i = 0; i < totalWordCount; ++i) {
      compWidths.push_back(compressAmountAt(renderer, fontId, words, wordStyles, i));
    }
  }
  const auto compAt = [&compWidths](const size_t idx) -> int {
    return idx < compWidths.size() ? compWidths[idx] : 0;
  };

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    if ((static_cast<size_t>(i) % kBuildProbeWordStride) == 0) {
      noteBuildProbe(kBuildProbeBreaks);  // v190：斷行 DP 外層，長段落的 O(n²) 才是盲區
    }
    int currlen = 0;
    int compRun = 0;  // v272：這一行到目前為止、可以讓出的標點寬度（行內的才算，行尾那個走懸掛）
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i) && noSpaceBeforeVec[j]) {
        gap = 0;
      } else if (j > static_cast<size_t>(i) && !continuesVec[j]) {
        gap =
            renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      } else if (j > static_cast<size_t>(i) && continuesVec[j]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        gap = renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      }

      // Calculate extraStartOffset for the first word on the line (i) (protect left margin)
      const int extraStartOffset = (j == i) ? calculateRubyExtraStartOffset(i, totalWordCount, renderer, fontId) : 0;

      currlen += wordWidths[j] + gap + (j == i ? extraStartOffset : 0);
      compRun += compAt(j);

      // v272：行內的標點可以各讓出半格（約物擠壓）；v273 起**行尾那一個也照壓**
      //   （懸掛已移除，不再有「兩者擇一」）—— 那正是日文横組み的行末約物半角。
      //   只在自然對齊時做：置中／逆向靠齊的行本來就不需要填滿。
      const int fitLen = currlen - compRun;

      if (fitLen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      const int extraEndOffset = calculateRubyExtraEndOffset(i, j + 1, renderer, fontId);
      // v271（codex）：行尾有注音（ruby）突出時**整行不壓縮** —— 那個突出是相對【未扣除】的字寬
      //   定位的，壓縮之後注音的墨水會跨出版心。有 ruby 就照原寬算（extractLine 用同一個條件）。
      const int lineComp = extraEndOffset > 0 ? 0 : compRun;
      const int fitLenHere = currlen - lineComp;

      if (fitLenHere + extraEndOffset > effectivePageWidth) {
        continue;  // Cannot split here as it would overflow the right margin
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        // v272（codex）：**壞度要用「實際會畫成多長」算，不是用「最多能壓多少」算** ——
        //   壓縮是【需要多少讓多少】（extractLine 那邊如此），用最大壓縮量當基準會讓
        //   標點多的候選被系統性高估成「排得很滿」，選出比較差的斷點。
        const int renderedLen = currlen > effectivePageWidth
                                    ? (fitLenHere > effectivePageWidth ? fitLenHere : effectivePageWidth)
                                    : currlen;
        const int spaceLeft = effectivePageWidth - renderedLen - extraEndOffset;
        const int remainingSpace = spaceLeft > 0 ? spaceLeft : 0;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      // Favor longer lines when line-breaking costs are equal, to avoid unnecessary short lines in Chinese and Japanese
      // text.
      if (cost <= dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec,
                                                            std::vector<bool>& noSpaceBeforeVec) {
  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  const bool compOk = punctFittingAllowed() && rubyTexts.empty();
  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    // v272（codex）：這條路（斷字開啟時的貪婪斷行）原本沒有做約物擠壓 ——
    //   那等於「開了斷字就沒有這個功能」。這裡跟著累計這一行可讓出的標點寬度。
    int lineComp = 0;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      int spacing = 0;
      if (!isFirstWord && noSpaceBeforeVec[currentIndex]) {
        spacing = 0;
      } else if (!isFirstWord && !continuesVec[currentIndex]) {
        spacing = renderer.getSpaceAdvance(fontId, lastCodepoint(words[currentIndex - 1]),
                                           firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      } else if (!isFirstWord && continuesVec[currentIndex]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        spacing = renderer.getKerning(fontId, lastCodepoint(words[currentIndex - 1]),
                                      firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      }
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // v272：約物擠壓 —— 這個 token 若是單獨一個句讀，它可以讓出最多半格。
      // ⚠️ 這條（斷字開啟時的貪婪斷行）**沒有**逐行的注音突出計算，而注音是相對未扣除的字寬定位的 ——
      //    所以只要這一段有注音就整段不壓（codex 第二輪）。DP 那條有逐行判斷，不受這個保守規則影響。
      const int compW = compOk ? compressAmountAt(renderer, fontId, words, wordStyles, currentIndex) : 0;
      const int lineCompIfEnds = lineComp + compW;

      // Word fits on current line
      if (lineWidth + candidateWidth - lineCompIfEnds <= effectivePageWidth) {
        lineWidth += candidateWidth;
        lineComp += compW;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (hyphenationEnabled && availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  uint32_t remainderOffset = visibleOffsetAt(wordIndex);
  const unsigned char* offsetPtr = reinterpret_cast<const unsigned char*>(word.data());
  const unsigned char* splitPtr = offsetPtr + chosenOffset;
  while (offsetPtr < splitPtr) {
    utf8NextCodepoint(&offsetPtr);
    remainderOffset++;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  insertVisibleOffset(wordIndex + 1, remainderOffset);
  // The hyphen remainder is not a focus suffix - it starts fresh on the next line.
  wordIsFocusSuffix.insert(wordIsFocusSuffix.begin() + wordIndex + 1, false);
  if (wordIndex + 1 <= rubyTexts.size()) {
    rubyTexts.insert(rubyTexts.begin() + wordIndex + 1, "");
  }

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);
  wordNoSpaceBefore.insert(wordNoSpaceBefore.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  // v150：本函式為每一行配三個容器（lineRubyTexts/lineWords/lineWordStyles，throwing）。
  // 實機 v149 的 abort 正是這裡的 vector<string>::reserve —— codex 上一輪的預言
  // （「修掉 addWord 後壓力移到下一個 throwing allocation」）。
  //
  // ⚠️ 門檻是【8KB 硬地板】，不是按行大小的預算 —— v139 的教訓：算錯的預算會讓
  //    正常的書全軍覆沒。一行的實際需求 ~1-2KB；8KB 只在堆積瀕死時成立
  //    （歷次實測的穩態 maxAlloc 全部 >23K），誤觸發在量測上不可能。
  // ⚠️ 拒絕後【不做】v139 那種繼續解析 —— oom_ latch，剩餘的 extractLine 呼叫直接
  //    return，尾端的「Remove consumed words」照常執行（被拒的行等於被丟棄），
  //    解析器在 flushPartWordBuffer 的 hasOom 檢查立刻 latch 整章中止、保留 partial。
  //    被丟的行無所謂：這一章不會被提交。
  if (oom_) return;
  if (breakIndex >= lineBreakIndices.size()) {
    // 不該發生；發生了就 latch —— 靜默早退會讓呼叫端照樣消耗 words、提交缺行章節（codex）。
    LOG_ERR("PTX", "extractLine: breakIndex %u out of range %u", static_cast<unsigned>(breakIndex),
            static_cast<unsigned>(lineBreakIndices.size()));
    oom_ = true;
    return;
  }
  // ⚠️ 用 MALLOC_CAP_DEFAULT，不是 ESP.getMaxAllocHeap()（那是 INTERNAL）——
  // plain new 走的是 DEFAULT 配置策略；用 INTERNAL 量會把 new 根本用不到的池
  // 也算進「還有空間」（codex 指出，而且這可能正是 v139 與三次「有空間卻配不到」
  // 異常的統一解）。量一次、快取、失敗時附總量。
  // v151：地板 8K -> 4K。一行實需約 1–2KB；DEFAULT 語意下穩態的 p3-only 最大塊
  // 實測落在 5–6K —— 8K 會在那種狀態【長期誤拒】，這可能正是 v150 那本書在固定頁
  // 出現確定性「索引失敗」的原因（清快取無效 = 守衛在同一記憶體剖面重複拒絕）。
  // 4K 仍遠高於單行需求，只在真正瀕死時成立。
  const size_t defaultMaxAlloc = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  if (defaultMaxAlloc < 4 * 1024) {
    const size_t df = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    LOG_ERR("PTX", "extractLine refused: defaultMax=%u defaultFree=%u", static_cast<unsigned>(defaultMaxAlloc),
            static_cast<unsigned>(df));
    noteRefusal("extract-floor", 4 * 1024, defaultMaxAlloc, df);
    oom_ = true;
    return;
  }
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;
  const uint32_t lineVisibleOffset = visibleOffsetAt(lastBreakAt);

  const int firstLineIndent = resolveFirstLineIndent(breakIndex == 0, renderer, fontId);

  std::vector<std::string> lineRubyTexts(lineWordCount);
  if (!rubyTexts.empty() && lastBreakAt < rubyTexts.size()) {
    const size_t copyCount = std::min(lineBreak, rubyTexts.size()) - lastBreakAt;
    std::copy(rubyTexts.begin() + lastBreakAt, rubyTexts.begin() + lastBreakAt + copyCount, lineRubyTexts.begin());
  }

  const int extraStartOffset = calculateRubyExtraStartOffset(lastBreakAt, lineBreak, renderer, fontId);
  const int extraEndOffset = calculateRubyExtraEndOffset(lastBreakAt, lineBreak, renderer, fontId);

  std::vector<std::string> lineWords;
  lineWords.reserve(lineWordCount);
  std::vector<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; ++i) {
    std::string word = std::move(words[lastBreakAt + i]);
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
    lineWords.push_back(std::move(word));
    lineWordStyles.push_back(wordStyles[lastBreakAt + i]);
  }

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation
    if (wordIdx > 0 && noSpaceBeforeVec[lastBreakAt + wordIdx]) {
      // Unicode break opportunity with no inserted Latin-style space. It is still
      // a stretchable gap for justified CJK/Korean text.
      actualGapCount++;
    } else if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
      totalNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                                   firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    } else if (wordIdx > 0 && continuesVec[lastBreakAt + wordIdx]) {
      // Non-breaking space tokens (" " with continues=true) are visible, stretchable spaces —
      // count them as justifiable gaps so justifyExtra is distributed to them too.
      if (lineWords[wordIdx] == " ") {
        actualGapCount++;
      }
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                              firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For RTL, implicit/default Left alignment becomes Right alignment.
  // Explicit text-align:left must remain left for CSS correctness.
  const CssTextAlign effectiveAlignment =
      (blockStyle.isRtl && !blockStyle.textAlignDefined && blockStyle.alignment == CssTextAlign::Left)
          ? CssTextAlign::Right
          : blockStyle.alignment;

  // v272：**約物擠壓**。這一行的句讀各可讓出半格；只讓出「這一行真的需要」的量，而且與斷行時
  //   算的是同一份（`compressAmountAt`），否則斷行認為放得下、定位卻擺不下 → 出血。
  //   分配方式：從行首往後逐個讓，讓滿需要的量就停（clreq 沒有規定分配法；逐個讓比平均讓更穩定，
  //   而且同一行重排兩次結果一定相同）。
  //   ⚠️ 只在自然對齊、整段沒有右到左文字、且行尾沒有注音突出時才做（與兩條斷行路徑同一個判準）。
  //   ⚠️ v273：行尾那個標點不再有「懸掛」這個選項，所以它跟行內的一樣可以壓（compEnd = 全行）。
  const bool compressHere = punctFittingAllowed() && lineWordCount > 0 && extraEndOffset == 0;
  std::vector<uint16_t> lineCompress;
  if (compressHere) {
    const size_t compEnd = lineWordCount;
    int available = 0;
    for (size_t k = 0; k < compEnd; ++k) {
      available += compressAmountAt(renderer, fontId, lineWords, lineWordStyles, k);
    }
    if (available > 0) {
      const int contentWidth = lineWordWidthSum + totalNaturalGaps + extraStartOffset + extraEndOffset;
      int needed = contentWidth - effectivePageWidth;
      if (needed > 0) {
        if (needed > available) needed = available;
        lineCompress.assign(lineWordCount, 0);
        for (size_t k = 0; k < compEnd && needed > 0; ++k) {
          const int c = compressAmountAt(renderer, fontId, lineWords, lineWordStyles, k);
          if (c <= 0) continue;
          const int take = c < needed ? c : needed;
          lineCompress[k] = static_cast<uint16_t>(take);
          needed -= take;
        }
      }
    }
  }
  int lineCompressTotal = 0;
  for (const uint16_t c : lineCompress) lineCompressTotal += c;

  // For justified text, compute per-gap extra to distribute remaining space evenly.
  // extraEndOffset reserves space for any ruby group at the right edge of the line.
  const int spareSpace = effectivePageWidth - extraStartOffset - extraEndOffset -
                         (lineWordWidthSum - lineCompressTotal) - totalNaturalGaps;
  const int justifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                               ? computeJustifyExtra(spareSpace, actualGapCount)
                               : 0;

  // BiDi processing: reorder words with UAX#9 in full-line context.
  visualOrderScratch.clear();
  visualOrderScratch.reserve(lineWordCount);
  // Skip expensive visual-order resolution for pure LTR paragraphs that have no RTL words.
  const bool shouldResolveVisualOrder = blockStyle.isRtl || hasRtlWord;
  const bool willReorder =
      shouldResolveVisualOrder && BidiUtils::computeVisualWordOrder(lineWords, blockStyle.isRtl, visualOrderScratch);

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  if (willReorder) {
    reorderedWordsScratch.clear();
    reorderedStylesScratch.clear();
    reorderedWidthsScratch.clear();
    reorderedContinuesScratch.clear();
    reorderedNoSpaceBeforeScratch.clear();
    reorderedFocusSuffixScratch.clear();
    reorderedWordsScratch.reserve(visualOrderScratch.size());
    reorderedStylesScratch.reserve(visualOrderScratch.size());
    reorderedWidthsScratch.reserve(visualOrderScratch.size());
    reorderedContinuesScratch.reserve(visualOrderScratch.size());
    reorderedNoSpaceBeforeScratch.reserve(visualOrderScratch.size());
    reorderedFocusSuffixScratch.reserve(visualOrderScratch.size());

    for (size_t i = 0; i < visualOrderScratch.size(); ++i) {
      const uint16_t src = visualOrderScratch[i];
      reorderedWordsScratch.push_back(std::move(lineWords[src]));
      reorderedStylesScratch.push_back(lineWordStyles[src]);
      reorderedWidthsScratch.push_back(wordWidths[lastBreakAt + src]);
      reorderedFocusSuffixScratch.push_back(wordIsFocusSuffix[lastBreakAt + src]);

      // Continuation means "no break/gap between two adjacent logical tokens".
      // After visual reordering (common in RTL), an adjacent logical pair can appear
      // as either (prev -> curr) or (curr -> prev) in visual order; preserve both.
      bool continues = false;
      if (i > 0) {
        const size_t prevSrc = visualOrderScratch[i - 1];
        const size_t currSrc = src;
        const bool forwardAdjacent = currSrc == prevSrc + 1;
        const bool reverseAdjacent = prevSrc == currSrc + 1;

        if (forwardAdjacent && continuesVec[lastBreakAt + currSrc]) {
          continues = true;
        } else if (reverseAdjacent && continuesVec[lastBreakAt + prevSrc]) {
          continues = true;
        }
      }
      reorderedContinuesScratch.push_back(continues);
      reorderedNoSpaceBeforeScratch.push_back(!continues && noSpaceBeforeVec[lastBreakAt + src]);
    }

    int reorderedWordWidthSum = 0;
    size_t reorderedGapCount = 0;
    int reorderedNaturalGaps = 0;
    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      reorderedWordWidthSum += reorderedWidthsScratch[wordIdx];
      if (wordIdx > 0 && reorderedNoSpaceBeforeScratch[wordIdx]) {
        // Unicode break opportunity with no inserted Latin-style space. It is still
        // a stretchable gap for justified CJK/Korean text.
        reorderedGapCount++;
      } else if (wordIdx > 0 && !reorderedContinuesScratch[wordIdx]) {
        reorderedGapCount++;
        reorderedNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                                         firstCodepoint(reorderedWordsScratch[wordIdx]),
                                                         reorderedStylesScratch[wordIdx - 1]);
      } else if (wordIdx > 0 && reorderedContinuesScratch[wordIdx]) {
        if (reorderedWordsScratch[wordIdx] == " ") {
          reorderedGapCount++;
        }
        reorderedNaturalGaps +=
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                firstCodepoint(reorderedWordsScratch[wordIdx]), reorderedStylesScratch[wordIdx - 1]);
      }
    }

    const int reorderedSpare =
        effectivePageWidth - extraStartOffset - extraEndOffset - reorderedWordWidthSum - reorderedNaturalGaps;
    const int reorderedJustifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                          ? computeJustifyExtra(reorderedSpare, reorderedGapCount)
                                          : 0;

    const int justifyContribution = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                        ? reorderedJustifyExtra * static_cast<int>(reorderedGapCount)
                                        : 0;
    const int contentWidth = reorderedWordWidthSum + reorderedNaturalGaps + justifyContribution;

    int xpos = 0;
    if (blockStyle.isRtl) {
      if (effectiveAlignment == CssTextAlign::Right || effectiveAlignment == CssTextAlign::Justify) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    } else {
      xpos = firstLineIndent;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    }

    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      lineXPos.push_back(static_cast<int16_t>(xpos));
      xpos += reorderedWidthsScratch[wordIdx];

      const bool nextIsContinuation =
          wordIdx + 1 < reorderedWidthsScratch.size() && reorderedContinuesScratch[wordIdx + 1];
      if (nextIsContinuation) {
        int advance =
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                firstCodepoint(reorderedWordsScratch[wordIdx + 1]), reorderedStylesScratch[wordIdx]);
        // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
        // no-break space must not receive justifyExtra, or the line over-stretches by one
        // gap and the last word is pushed past the right margin (issue #2185).
        if (wordIdx > 0 && reorderedWordsScratch[wordIdx] == " " && reorderedContinuesScratch[wordIdx] &&
            effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          advance += reorderedJustifyExtra;
        }
        xpos += advance;
      } else if (wordIdx + 1 < reorderedWidthsScratch.size()) {
        const bool nextNoSpace = reorderedNoSpaceBeforeScratch[wordIdx + 1];
        int gap = nextNoSpace ? 0
                              : renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                                         firstCodepoint(reorderedWordsScratch[wordIdx + 1]),
                                                         reorderedStylesScratch[wordIdx]);
        if (effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          gap += reorderedJustifyExtra;
        }
        xpos += gap;
      }
    }

    lineWords.swap(reorderedWordsScratch);
    lineWordStyles.swap(reorderedStylesScratch);
  } else {
    // Standard LTR/RTL positioning loop when no visual reordering is needed
    if (blockStyle.isRtl) {
      // RTL: position words from right to left
      int xpos = effectivePageWidth;
      if (effectiveAlignment == CssTextAlign::Left) {
        // Explicit left alignment in RTL context
        xpos = lineWordWidthSum + totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth + lineWordWidthSum + totalNaturalGaps) / 2;
      }
      // For Right and Justify, start from right edge (xpos = effectivePageWidth)

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        xpos -= wordWidths[lastBreakAt + wordIdx];
        lineXPos.push_back(static_cast<int16_t>(xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          // Cross-boundary kerning for continuation words
          int advance = renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                            firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          // wordIdx > 0: see the LTR branch — a leading no-break space is not a justifiable gap.
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos -= advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace
                      ? 0
                      : renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                 firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos -= gap;
        }
      }
    } else {
      // LTR: position words from left to right
      int xpos = firstLineIndent + extraStartOffset;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
      }

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        // v272／v274：被擠壓的標點 —— 格子變窄，字形往左挪「從左側收掉的那一份」，
        //   墨水才會留在自己（變窄後）的格子裡。句讀是對半收 ＝ 挪半個，與 v272 相同；
        //   括弧類的空白只在一邊，挪的量就自動變成幾乎全部或幾乎零。
        const int compHere = wordIdx < lineCompress.size() ? lineCompress[wordIdx] : 0;
        const int shiftHere =
            compHere > 0
                ? compressLeftShare(compressMetricsAt(renderer, fontId, lineWords, lineWordStyles, wordIdx), compHere)
                : 0;
        lineXPos.push_back(static_cast<int16_t>(xpos - shiftHere));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          int advance = wordWidths[lastBreakAt + wordIdx] - compHere;
          advance += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                         firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
          // no-break space must not receive justifyExtra, or the line over-stretches by one
          // gap and the last word is pushed past the right margin (issue #2185).
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos += advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace
                      ? 0
                      : renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                 firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos += wordWidths[lastBreakAt + wordIdx] - compHere + gap;  // v272：擠壓過的標點格子變窄
        }
      }
    }
  }

  const auto isFocusSuffixAt = [&](const size_t idx) {
    return willReorder ? reorderedFocusSuffixScratch[idx] : wordIsFocusSuffix[lastBreakAt + idx];
  };

  // Fast path: when no word on this line was split for focus reading, skip the merge work
  // entirely and pass empty boundary/suffixX vectors. TextBlock pays zero per-word RAM cost
  // for these annotations when the vectors are empty.
  bool lineHasFocusSplit = false;
  for (size_t i = 0; i < lineWordCount; i++) {
    if (isFocusSuffixAt(i)) {
      lineHasFocusSplit = true;
      break;
    }
  }

  if (!lineHasFocusSplit) {
    // TextBlock flattens the vectors into its arena; they stay owned here and die at return.
    auto block = std::make_shared<TextBlock>(lineWords, lineXPos, lineWordStyles, std::vector<uint8_t>{},
                                             std::vector<uint16_t>{}, blockStyle, std::move(lineRubyTexts));
    if (!block->valid()) {
      LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
      return;
    }
    processLine(std::move(block), lineVisibleOffset);
    return;
  }

  // Slow path: merge focus suffix tokens back into their preceding word entry so each
  // original word occupies one TextBlock slot. Splits are recorded as per-word annotations
  // applied at render time, cutting the token count significantly when the feature is active.
  std::vector<std::string> outWords;
  std::vector<int16_t> outXPos;
  std::vector<EpdFontFamily::Style> outStyles;
  std::vector<uint8_t> outBoundaries;
  std::vector<uint16_t> outSuffixX;
  std::vector<std::string> outRubyTexts;
  outWords.reserve(lineWordCount);
  outXPos.reserve(lineWordCount);
  outStyles.reserve(lineWordCount);
  outBoundaries.reserve(lineWordCount);
  outSuffixX.reserve(lineWordCount);
  outRubyTexts.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; i++) {
    if (isFocusSuffixAt(i) && !outWords.empty()) {
      // Focus suffix: merge string into the preceding bold-prefix entry.
      outWords.back() += lineWords[i];
    } else {
      // Normal word: check for a following focus suffix to record the byte boundary.
      uint8_t boundary = 0;
      uint16_t suffixX = 0;
      if (i + 1 < lineWordCount && isFocusSuffixAt(i + 1)) {
        boundary = static_cast<uint8_t>(std::min(lineWords[i].size(), size_t{255}));
        // Suffix x offset = layout-time advance of the bold prefix, already known from xpos table.
        const int suffixDelta = static_cast<int>(lineXPos[i + 1]) - static_cast<int>(lineXPos[i]);
        suffixX = static_cast<uint16_t>(suffixDelta > 0 ? suffixDelta : 0);
      }
      outWords.push_back(std::move(lineWords[i]));
      outXPos.push_back(lineXPos[i]);
      // For focus entries with a suffix, strip BOLD from the stored style.
      // Render re-applies it to the prefix portion only, via the boundary field.
      const EpdFontFamily::Style storedStyle =
          boundary > 0 ? static_cast<EpdFontFamily::Style>(lineWordStyles[i] & ~EpdFontFamily::BOLD)
                       : lineWordStyles[i];
      outStyles.push_back(storedStyle);
      outBoundaries.push_back(boundary);
      outSuffixX.push_back(suffixX);
      outRubyTexts.push_back(i < lineRubyTexts.size() ? std::move(lineRubyTexts[i]) : std::string());
    }
  }

  auto block = std::make_shared<TextBlock>(outWords, outXPos, outStyles, outBoundaries, outSuffixX, blockStyle,
                                           std::move(outRubyTexts));
  if (!block->valid()) {
    LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
    return;
  }
  processLine(std::move(block), lineVisibleOffset);
}
