#include <esp_heap_caps.h>
#include <Arduino.h>
#include "SdCardFont.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>

#include "EpdFontFamily.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

namespace {

// FNV-1a hash for content-based font ID generation
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
// CPFONT_VERSION is defined as a #define in SdCardFont.h so it can be
// stringified into FONT_MANIFEST_URL.
constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;

// Helper to read little-endian values from byte buffer
inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

// Walks a null-terminated UTF-8 string and appends each unique codepoint to
// codepoints[0..cpCount-1] via O(n²) dedup.  Returns true if the buffer
// reached maxCount (cap hit), false if all codepoints fit.
bool collectUniqueCodepoints(const char* text, uint32_t* codepoints, uint32_t& cpCount, uint32_t maxCount) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (cpCount >= maxCount) return true;
      codepoints[cpCount++] = cp;
    }
  }
  return false;
}

const char* asCStr(const std::string& s) { return s.c_str(); }
const char* asCStr(const char* s) { return s; }

// resetStyleMiniData retention bounds (see the PerStyle comment in the header).
constexpr size_t MINI_RETAIN_MIN_FREE_HEAP = 40 * 1024;
// v188：留不留也要看【最大連續塊】（CLAUDE.md 硬限制 2：殺手是連續塊不是 free 總量）。
// diag187_2 的四次「記憶體不足」：總 free 48KB（>40KB → 留）而最大連續塊只剩 28KB。
// 但這個量是在 mini 還駐留時量的（自我參照）：門檻設 32KB 會在三分之一的頁觸發、變成每頁配丟的
// 碎片機（驗證者用 diag187_2 的 pmax 算過）。16KB（＝背景建置的地板）只在堆真的出事時觸發（~6% 頁）；
// 建置視窗本身由 releaseMiniData() 明確處理，這裡只是最後防線。每次 clearCache 只量一次。
constexpr size_t MINI_RETAIN_MIN_MAXBLOCK = 16 * 1024;
constexpr uint8_t MINI_UNDERUSE_RUNS_BEFORE_FREE = 3;

// Keep-if-fits buffer reuse: only reallocate when the needed size exceeds the
// current capacity. Freeing + reallocating slightly different sizes every page
// turn punches non-coalescing holes in the heap (the freed block rarely fits the
// next page's need), eroding the largest contiguous block all session. With
// reuse, capacities converge on the book's max page after a few turns and page
// turns stop touching the allocator. Only three small instantiations exist
// (interval/glyph/byte arrays), so template bloat is negligible.
// v142：把上游那個 keep-if-fits 補完 —— 它擋掉了「縮小重配」，但【沒擋掉放大重配】。
//
// 實機量到的殘留（v141，只讀書、無網路、讀約 20 頁）：
//    33800 build-start  p2 f=115,616 max=115,616
//   120552 build-start  p2 f= 71,244 max= 27,476     <- 87 秒內最大連續塊蝕掉 88,140
//   [92283] Failed to allocate mini bitmap (33422 bytes)
//   [95791] Failed to allocate mini bitmap (38071 bytes)
//   [99810] Failed to allocate mini bitmap (31569 bytes)
// 字圖要 31–38KB 連續，而 p2 最大只剩 27,476 -> 配不到 -> 字型降級 -> 最後 abort。
//
// 為什麼配【剛好夠】會這樣：每一頁的用字不同，所需大小逐頁小幅變動。配 exact size 時，
// 只要某一頁比歷史高水位大一點就重配一次；而剛釋放的舊塊【永遠差一點裝不下】新的，
// 於是配置器往前找新位置，舊塊變成洞。上游的註解自己描述了這個機制
// （"punches non-coalescing holes… eroding the largest contiguous block all session"），
// 只是 keep-if-fits 只解掉一半。
//
// 修法：成長時【向上取整到 8KB 級距】。容量因此在兩三次之內收斂到「這本書最大的一頁」，
// 之後翻頁完全不碰配置器。溢配上限是 8KB（有界），換掉的是無界的碎片化。
//   實測序列 30,000 -> 32,768 / 33,422 -> 40,960 / 38,071 命中 => 只重配兩次。
// 這與 v55/v138 的 advance table 同一個形狀（一次配到位、之後就地重用），那個已實機驗證。
//
// ⚠️ 級距用【位元組】換算成元素數，因為呼叫端的 needed 有的是位元組（miniBitmap）、
//    有的是元素數（miniIntervals / miniGlyphs / kern classes）。
// ⚠️ CapT 可能是 uint16_t（kern class 計數）—— 夾限，否則截斷後 capacity 會記成比實際
//    配置【小】的值，下一次判斷就錯，反而變成每頁都重配。
template <typename T, typename CapT>
bool ensureArrayCapacity(T*& buf, CapT& capacity, const uint32_t needed) {
  if (buf && capacity >= needed) return true;

  constexpr uint32_t kGrowGranularityBytes = 8 * 1024;
  constexpr uint32_t kStep =
      (kGrowGranularityBytes / sizeof(T)) > 0 ? static_cast<uint32_t>(kGrowGranularityBytes / sizeof(T)) : 1u;
  constexpr uint32_t kCapMax = static_cast<uint32_t>(static_cast<CapT>(-1));

  // ⚠️ 只有【配置本身 ≥ 8KB】才取整。小陣列（kern class 上限 256 個位元組）取整會變成
  //    為了 256 bytes 配 8KB，兩個就浪費 16KB —— 而小配置本來就不是碎片化的來源。
  //    這一條是桌面對拍抓到的：第一版無條件取整，kern 的容量直接跳到 8,192。
  uint32_t grown = needed;
  const bool worthRounding = static_cast<uint64_t>(needed) * sizeof(T) >= kGrowGranularityBytes;
  if (worthRounding && needed <= kCapMax - (kStep - 1)) {  // 取整不會溢位才做
    grown = ((needed + kStep - 1) / kStep) * kStep;
  }
  if (grown > kCapMax) grown = kCapMax;
  if (grown < needed) return false;               // needed 本身就超過 CapT 能表示：誠實失敗

  delete[] buf;
  buf = new (std::nothrow) T[grown > 0 ? grown : 1];
  capacity = buf ? static_cast<CapT>(grown) : 0;
  return buf != nullptr;
}

}  // namespace

char SdCardFont::lastAllocFail[96] = {0};
void (*SdCardFont::buildProbeHook_)(uint8_t) = nullptr;
uint32_t SdCardFont::advanceMissCount_ = 0;
uint32_t SdCardFont::advanceSdReadCount_ = 0;
uint32_t SdCardFont::advanceRejectCount_ = 0;
uint32_t SdCardFont::advanceEvictCount_ = 0;
uint8_t SdCardFont::advanceSdProbeDepth_ = 0;

void SdCardFont::resetAdvanceDiag() {
  advanceMissCount_ = 0;
  advanceSdReadCount_ = 0;
  advanceRejectCount_ = 0;
  advanceEvictCount_ = 0;
}

void SdCardFont::setAdvanceSdProbe(bool on) {
  // v192：巢狀深度，進 +1 出 -1；布林旗標會讓內層提早清成 false。
  if (on) {
    if (advanceSdProbeDepth_ < 255) ++advanceSdProbeDepth_;
  } else if (advanceSdProbeDepth_ > 0) {
    --advanceSdProbeDepth_;
  }
}

void SdCardFont::noteAllocFail(const char* what, unsigned bytes, unsigned defMax, unsigned defFree) {
  if (lastAllocFail[0] != '\0') return;  // 先到先得
  snprintf(lastAllocFail, sizeof(lastAllocFail), "%s bytes=%u defMax=%u defFree=%u", what, bytes, defMax, defFree);
}

void SdCardFont::setBuildProbeHook(void (*fn)(uint8_t)) { buildProbeHook_ = fn; }

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  delete[] s.miniDropped;
  s.miniDropped = nullptr;
  s.miniDroppedCount = 0;
  s.miniDroppedCapacity = 0;
  delete[] s.miniIntervals;
  s.miniIntervals = nullptr;
  delete[] s.miniGlyphs;
  s.miniGlyphs = nullptr;
  delete[] s.miniBitmap;
  s.miniBitmap = nullptr;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniIntervalCapacity = 0;
  s.miniGlyphCapacity = 0;
  s.miniBitmapCapacity = 0;
  s.miniBitmapUsed = 0;
  s.miniUnderuseRuns = 0;
  freeStyleMiniKern(s);
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::resetStyleMiniData(PerStyle& s, const bool heapTight) {
  // Retention is a bet that the next scope needs similar data. Don't hold it
  // when the heap is tight: the arenas are rebuildable for one page's worth of
  // allocations, and this floor keeps retained fonts out of the way of section
  // builds and the render path's own floors.
  if (heapTight) {
    freeStyleMiniData(s);
    return;
  }
  // Underuse hysteresis, on the bitmap arena (the dominant allocation): an
  // outlier page (e.g. three styles cramped together) would otherwise pin its
  // high-water arena for the rest of the book. Keep while the page used at
  // least 3/4 of capacity; release only after several consecutive rebuilds
  // below that, so alternating dense/sparse pages never thrash. Evaluated at
  // most once per rebuild (a scope both constructs and destructs through here,
  // and subset hits load nothing new to judge).
  if (s.miniHysteresisPending && s.miniBitmapCapacity > 0 && s.miniBitmapUsed > 0) {
    s.miniHysteresisPending = false;
    if (s.miniBitmapUsed < s.miniBitmapCapacity - s.miniBitmapCapacity / 4) {
      if (++s.miniUnderuseRuns >= MINI_UNDERUSE_RUNS_BEFORE_FREE) {
        LOG_DBG("SDCF", "mini release (underuse): used=%u cap=%u", s.miniBitmapUsed, s.miniBitmapCapacity);
        freeStyleMiniData(s);
        return;
      }
    } else {
      s.miniUnderuseRuns = 0;
    }
  }
  // Data (intervals/glyphs/bitmaps/kern) deliberately survives the scope: the
  // next prewarm subset-checks against it, which is what lets the idle prewarm
  // of page N+1 serve the actual page turn with zero SD reads.
}

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  delete[] s.kernLeftClasses;
  s.kernLeftClasses = nullptr;
  delete[] s.kernRightClasses;
  s.kernRightClasses = nullptr;
  delete[] s.ligaturePairs;
  s.ligaturePairs = nullptr;
  s.kernLigLoaded = false;
}

void SdCardFont::freeStyleMiniKern(PerStyle& s) {
  delete[] s.miniKernLeftClasses;
  s.miniKernLeftClasses = nullptr;
  delete[] s.miniKernRightClasses;
  s.miniKernRightClasses = nullptr;
  delete[] s.miniKernMatrix;
  s.miniKernMatrix = nullptr;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  s.miniKernLeftCapacity = 0;
  s.miniKernRightCapacity = 0;
  s.miniKernMatrixCapacity = 0;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  delete[] s.fullIntervals;
  s.fullIntervals = nullptr;
  delete[] s.bmpIntervals;
  s.bmpIntervals = nullptr;
  s.intervalsAreBmp16 = false;
  freeStyleKernLigatureData(s);
  s.present = false;
}

// --- Global free/cleanup ---

bool SdCardFont::ensureFileOpen() {
  if (sharedFile_) return true;
  if (!filePath_[0]) return false;
  if (!Storage.openFileForRead("SDCF", filePath_, sharedFile_)) {
    LOG_ERR("SDCF", "Failed to open shared .cpfont handle: %s", filePath_);
    return false;
  }
  return true;
}

void SdCardFont::freeAll() {
  delete[] cpScratch_;
  cpScratch_ = nullptr;
  sharedFile_ = HalFile{};  // 關閉共用檔柄（移動指派讓舊的解構→close）
  clearOverflow();
  clearPersistentCache();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    delete[] overflow_[i].bitmap;
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const {
  // Kern data uses the per-page mini tables (renumbered class IDs). The full
  // kern matrix is never resident — see PerStyle::miniKernMatrix comment.
  data.kernLeftClasses = s.miniKernLeftClasses;
  data.kernRightClasses = s.miniKernRightClasses;
  data.kernMatrix = s.miniKernMatrix;
  data.kernLeftEntryCount = s.miniKernLeftEntryCount;
  data.kernRightEntryCount = s.miniKernRightEntryCount;
  data.kernLeftClassCount = s.miniKernLeftClassCount;
  data.kernRightClassCount = s.miniKernRightClassCount;
  // Ligatures are small (typically < 1KB) so they stay resident.
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.header.ligaturePairCount;
}

bool SdCardFont::loadStyleKernLigatureData(PerStyle& s) {
  if (s.kernLigLoaded) return true;
  bool hasKern = s.header.kernLeftEntryCount > 0;
  bool hasLig = s.header.ligaturePairCount > 0;
  if (!hasKern && !hasLig) {
    s.kernLigLoaded = true;
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (hasKern) {
    // Load only the small class-lookup tables (~3KB each). The full matrix
    // (~36KB contiguous for Literata) is built per-page from SD in
    // buildMiniKernMatrix().
    s.kernLeftClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernLeftEntryCount];
    s.kernRightClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernRightEntryCount];

    if (!s.kernLeftClasses || !s.kernRightClasses) {
      LOG_ERR("SDCF", "Failed to allocate kern classes (%u+%u bytes)", s.header.kernLeftEntryCount * 3u,
              s.header.kernRightEntryCount * 3u);
      freeStyleKernLigatureData(s);
      return false;
    }

    if (!file.seekSet(s.kernLeftFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to kern data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (file.read(reinterpret_cast<uint8_t*>(s.kernLeftClasses), leftSz) != static_cast<int>(leftSz) ||
        file.read(reinterpret_cast<uint8_t*>(s.kernRightClasses), rightSz) != static_cast<int>(rightSz)) {
      LOG_ERR("SDCF", "Failed to read kern classes");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  if (hasLig) {
    s.ligaturePairs = new (std::nothrow) EpdLigaturePair[s.header.ligaturePairCount];
    if (!s.ligaturePairs) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
    if (!file.seekSet(s.ligatureFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to ligature data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(s.ligaturePairs), sz) != static_cast<int>(sz)) {
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  s.kernLigLoaded = true;

  // Make ligatures visible to the stub (used when no mini data built yet).
  // Kern stays nullptr on the stub — it is only wired in miniData via
  // applyKernLigaturePointers() after buildMiniKernMatrix() runs.
  s.stubData.ligaturePairs = s.ligaturePairs;
  s.stubData.ligaturePairCount = s.header.ligaturePairCount;

  LOG_DBG("SDCF", "Kern classes + lig loaded: kernL=%u, kernR=%u, ligs=%u", s.header.kernLeftEntryCount,
          s.header.kernRightEntryCount, s.header.ligaturePairCount);
  return true;
}

// --- Per-page mini kern matrix ---

// Local copy of EpdFont.cpp's lookupKernClass (that one is file-static there).
// Returns the 1-based class ID for `cp`, or 0 if the codepoint has no kerning class.
static uint8_t miniLookupKernClass(const EpdKernClassEntry* entries, uint16_t count, uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) return 0;
  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;
  const auto it =
      std::lower_bound(entries, end, target, [](const EpdKernClassEntry& e, uint16_t v) { return e.codepoint < v; });
  return (it != end && it->codepoint == target) ? it->classId : 0;
}

// Build a small per-page kern matrix containing ONLY the (leftClass, rightClass)
// pairs reachable from codepoints in the current text. Class IDs are renumbered
// to a dense 1..N range so the resulting matrix is usedLeft × usedRight (typical
// Latin page: ~25×25 bytes) instead of the font's full ~180×200 (~36KB).
//
// Correctness: EpdFont::getKerning only touches `kernLeftClasses` /
// `kernRightClasses` / `kernMatrix` / the count fields — we swap all of them to
// the mini versions together in applyKernLigaturePointers, so a codepoint not
// on this page simply returns class 0 (no kerning), which was the pre-existing
// behavior for any codepoint outside the kern classes.
bool SdCardFont::buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount) {
  // No freeStyleMiniKern here: it zeroed the capacities, which forced the
  // ensureArrayCapacity calls below to reallocate every page and defeated the
  // buffer reuse. prewarmStyle is the only caller and the success path
  // overwrites the contents and all four counts, so keeping the buffers is
  // safe. The early returns zero the counts (buffers kept) so a page with no
  // applicable kern pairs kerns as none instead of through the previous
  // page's tables.
  const auto resetMiniKernCounts = [&s]() {
    s.miniKernLeftEntryCount = 0;
    s.miniKernRightEntryCount = 0;
    s.miniKernLeftClassCount = 0;
    s.miniKernRightClassCount = 0;
  };
  if (!s.kernLeftClasses || !s.kernRightClasses || s.header.kernLeftEntryCount == 0 ||
      s.header.kernRightEntryCount == 0) {
    resetMiniKernCounts();
    return true;  // font has no kern classes — nothing to build
  }

  // Step 1: mark used left/right classes via a 256-wide bitmap (class IDs are uint8_t).
  bool usedLeft[256] = {};
  bool usedRight[256] = {};
  for (uint32_t i = 0; i < cpCount; i++) {
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]);
    if (lc) usedLeft[lc] = true;
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]);
    if (rc) usedRight[rc] = true;
  }

  // Step 2: build renumber maps (oldClassId -> newClassId, 1-based) and
  // reverse maps (newClassId -> oldClassId) for the SD read step.
  uint8_t leftRenumber[256] = {};
  uint8_t rightRenumber[256] = {};
  uint8_t newToOldLeft[256] = {};
  uint8_t newToOldRight[256] = {};
  uint8_t numLeft = 0, numRight = 0;
  for (int i = 1; i < 256; i++) {
    if (usedLeft[i]) {
      numLeft++;
      leftRenumber[i] = numLeft;
      newToOldLeft[numLeft] = static_cast<uint8_t>(i);
    }
    if (usedRight[i]) {
      numRight++;
      rightRenumber[i] = numRight;
      newToOldRight[numRight] = static_cast<uint8_t>(i);
    }
  }
  if (numLeft == 0 || numRight == 0) {
    resetMiniKernCounts();
    return true;  // no kern pairs applicable on this page
  }

  // Step 3: count how many codepoint→classId entries the mini class tables need.
  // Each resident class table has one entry per kerned codepoint in the page.
  uint16_t miniLeftCount = 0;
  uint16_t miniRightCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]) != 0) miniLeftCount++;
    if (miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]) != 0) miniRightCount++;
  }

  // Step 4: size the three mini buffers (reused across pages when they fit; the
  // per-page sizes vary by a few entries, which as free+realloc churn was punching
  // non-coalescing holes in the heap every page turn).
  const uint32_t matrixBytes = static_cast<uint32_t>(numLeft) * numRight;
  if (!ensureArrayCapacity(s.miniKernLeftClasses, s.miniKernLeftCapacity, miniLeftCount) ||
      !ensureArrayCapacity(s.miniKernRightClasses, s.miniKernRightCapacity, miniRightCount) ||
      !ensureArrayCapacity(s.miniKernMatrix, s.miniKernMatrixCapacity, matrixBytes)) {
    LOG_ERR("SDCF", "Failed to allocate mini kern (%u+%u+%u bytes)", miniLeftCount * 3u, miniRightCount * 3u,
            matrixBytes);
    freeStyleMiniKern(s);
    return false;
  }

  // Step 5: populate mini class tables. `codepoints` is already sorted (see
  // prewarm()) so the output is sorted by codepoint — required for binary
  // search in lookupKernClass during render.
  uint16_t lIdx = 0, rIdx = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    uint32_t cp = codepoints[i];
    if (cp > 0xFFFF) continue;  // kern class entries are uint16_t
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, cp);
    if (lc) {
      s.miniKernLeftClasses[lIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernLeftClasses[lIdx].classId = leftRenumber[lc];
      lIdx++;
    }
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, cp);
    if (rc) {
      s.miniKernRightClasses[rIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernRightClasses[rIdx].classId = rightRenumber[rc];
      rIdx++;
    }
  }

  // Step 6: read the full matrix's rows for each used left class, keep only
  // columns for used right classes. One SD seek + one read per used left class;
  // a row is kernRightClassCount bytes (~200 for Literata).
  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for mini kern: %s", filePath_);
    freeStyleMiniKern(s);
    return false;
  }

  std::unique_ptr<int8_t[]> rowBuf(new (std::nothrow) int8_t[s.header.kernRightClassCount]);
  if (!rowBuf) {
    LOG_ERR("SDCF", "Failed to allocate row buffer (%u bytes)", s.header.kernRightClassCount);
    freeStyleMiniKern(s);
    return false;
  }

  for (uint8_t newL = 1; newL <= numLeft; newL++) {
    const uint8_t oldL = newToOldLeft[newL];
    const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * s.header.kernRightClassCount;
    if (!file.seekSet(rowFileOff)) {
      LOG_ERR("SDCF", "Failed to seek to kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    if (file.read(reinterpret_cast<uint8_t*>(rowBuf.get()), s.header.kernRightClassCount) !=
        static_cast<int>(s.header.kernRightClassCount)) {
      LOG_ERR("SDCF", "Failed to read kern row %u", oldL);
      freeStyleMiniKern(s);
      return false;
    }
    int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * numRight;
    for (uint8_t newR = 1; newR <= numRight; newR++) {
      miniRow[newR - 1] = rowBuf[newToOldRight[newR] - 1u];
    }
  }

  s.miniKernLeftEntryCount = lIdx;
  s.miniKernRightEntryCount = rIdx;
  s.miniKernLeftClassCount = numLeft;
  s.miniKernRightClassCount = numRight;

  LOG_DBG("SDCF", "Built mini kern: %u×%u matrix (%u bytes, full was %u×%u = %u bytes)", numLeft, numRight, matrixBytes,
          s.header.kernLeftClassCount, s.header.kernRightClassCount,
          static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount);
  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.stubData.coverageHandler = &SdCardFont::onCoverageQuery;
}

bool SdCardFont::onCoverageQuery(void* ctx, const uint32_t codepoint) {
  const auto* octx = static_cast<OverflowContext*>(ctx);
  const PerStyle& s = octx->self->styles_[octx->styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return false;  // coverage index freed/never loaded
  return octx->self->findGlobalGlyphIndex(s, codepoint) >= 0;
}

// --- Compute per-style file offsets from a base data offset ---

void SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset) {
  s.intervalsFileOffset = baseOffset;
  s.glyphsFileOffset = s.intervalsFileOffset + s.header.intervalCount * sizeof(EpdUnicodeInterval);
  s.kernLeftFileOffset = s.glyphsFileOffset + s.header.glyphCount * sizeof(EpdGlyph);
  s.kernRightFileOffset = s.kernLeftFileOffset + s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  s.kernMatrixFileOffset = s.kernRightFileOffset + s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
  s.ligatureFileOffset =
      s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
  s.bitmapFileOffset = s.ligatureFileOffset + s.header.ligaturePairCount * sizeof(EpdLigaturePair);
}

// --- Load ---

bool SdCardFont::load(const char* path) {
  freeAll();
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  HalFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u)", fileVersion, CPFONT_VERSION);
    return false;
  }

  // Begin content hash: accumulate global header
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", styleCount);
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      file.close();
      freeAll();
      return false;
    }

    auto& s = styles_[styleId];
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating.
    // Kern class counts are uint8 (bounded by type). Entry counts are uint16
    // but in practice a sane font has far fewer than 4096 per-side kern entries.
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    static constexpr uint32_t MAX_KERN_ENTRIES = 4096;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS ||
        s.header.kernLeftEntryCount > MAX_KERN_ENTRIES || s.header.kernRightEntryCount > MAX_KERN_ENTRIES) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (iv=%u, gl=%u, kL=%u, kR=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount, s.header.kernLeftEntryCount, s.header.kernRightEntryCount);
      file.close();
      freeAll();
      return false;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    computeStyleFileOffsets(s, dataOffset);
  }

  styleCount_ = styleCount;
  contentHash_ = hash;

  // Load full intervals into RAM for each present style. BMP-only fonts with
  // fewer than 65536 glyphs use a compact 6-byte interval table instead of the
  // on-disk 12-byte table; large sparse CJK subsets otherwise keep tens of KB
  // of always-resident heap just for lookup metadata.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to intervals for style %u", i);
      freeAll();
      return false;
    }

    // Validate interval contents before any later code (findGlobalGlyphIndex,
    // glyph reads) trusts them. A malformed file could otherwise drive
    // out-of-range glyph indices into bogus on-disk reads.
    bool canUseBmp16 = s.header.glyphCount <= UINT16_MAX;
    uint32_t expectedOffset = 0;
    uint32_t prevLast = 0;
    EpdUnicodeInterval iv{};
    for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
      if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
        LOG_ERR("SDCF", "Failed to read interval %u for style %u", j, i);
        freeAll();
        return false;
      }
      if (iv.first > iv.last) {
        LOG_ERR("SDCF", "Style %u: invalid interval %u (first 0x%lX > last 0x%lX)", i, j,
                static_cast<unsigned long>(iv.first), static_cast<unsigned long>(iv.last));
        file.close();
        freeAll();
        return false;
      }
      const uint32_t span = iv.last - iv.first + 1;
      const bool overlapsPrev = (j > 0 && iv.first <= prevLast);
      const bool spanTooBig = (span > s.header.glyphCount);
      const bool offsetMismatch = (iv.offset != expectedOffset);
      const bool offsetOverruns = (iv.offset > s.header.glyphCount - span);
      if (overlapsPrev || spanTooBig || offsetMismatch || offsetOverruns) {
        LOG_ERR("SDCF", "Style %u: invalid interval layout at %u (overlap=%d span=%u offMis=%d offOver=%d)", i, j,
                overlapsPrev, span, offsetMismatch, offsetOverruns);
        file.close();
        freeAll();
        return false;
      }
      if (iv.first > UINT16_MAX || iv.last > UINT16_MAX || iv.offset > UINT16_MAX) {
        canUseBmp16 = false;
      }
      expectedOffset += span;
      prevLast = iv.last;
    }

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek back to intervals for style %u", i);
      freeAll();
      return false;
    }

    if (canUseBmp16) {
      s.bmpIntervals = new (std::nothrow) PerStyle::BmpInterval16[s.header.intervalCount];
      if (!s.bmpIntervals) {
        LOG_ERR("SDCF", "Failed to allocate compact intervals for style %u", i);
        freeAll();
        return false;
      }
      for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
        if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
          LOG_ERR("SDCF", "Failed to read compact interval %u for style %u", j, i);
          freeAll();
          return false;
        }
        s.bmpIntervals[j] = {static_cast<uint16_t>(iv.first), static_cast<uint16_t>(iv.last),
                             static_cast<uint16_t>(iv.offset)};
      }
      s.intervalsAreBmp16 = true;
    } else {
      s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, i);
        freeAll();
        return false;
      }
      size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
        LOG_ERR("SDCF", "Failed to read intervals for style %u", i);
        freeAll();
        return false;
      }
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, CPFONT_VERSION, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, kernL=%u, kernR=%u, ligs=%u", i,
            h.intervalCount, h.glyphCount, h.advanceY, h.ascender, h.descender, h.kernLeftEntryCount,
            h.kernRightEntryCount, h.ligaturePairCount);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const uint32_t first = s.intervalsAreBmp16 ? s.bmpIntervals[mid].first : s.fullIntervals[mid].first;
    const uint32_t last = s.intervalsAreBmp16 ? s.bmpIntervals[mid].last : s.fullIntervals[mid].last;
    if (codepoint < first) {
      right = mid - 1;
    } else if (codepoint > last) {
      left = mid + 1;
    } else {
      const uint32_t offset = s.intervalsAreBmp16 ? s.bmpIntervals[mid].offset : s.fullIntervals[mid].offset;
      return static_cast<int32_t>(offset + (codepoint - first));
    }
  }
  return -1;
}

// --- Prewarm ---

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints from UTF-8 text (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  // Heap-allocated: MAX_PAGE_GLYPHS * 4 = 2048 bytes, too large for stack (limit < 256 bytes)
  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[MAX_PAGE_GLYPHS]);
  if (!codepoints) {
    LOG_ERR("SDCF", "Failed to allocate codepoint buffer (%u bytes)", MAX_PAGE_GLYPHS * 4);
    return -1;
  }
  uint32_t cpCount = 0;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < MAX_PAGE_GLYPHS) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      codepoints[cpCount++] = cp;
    }
  }

  // Always include the replacement character
  {
    bool hasReplacement = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == REPLACEMENT_GLYPH) {
        hasReplacement = true;
        break;
      }
    }
    if (!hasReplacement && cpCount < MAX_PAGE_GLYPHS) {
      codepoints[cpCount++] = REPLACEMENT_GLYPH;
    }
  }

  // Add ligature output codepoints from all styles being prewarmed.
  // Skip during metadata-only prewarm (layout measurement) to avoid loading
  // kern/lig data for all styles upfront (~22KB per style). Kern/lig is
  // loaded per-style in prewarmStyle() during the full render prewarm instead.
  if (!metadataOnly) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
      auto& s = styles_[si];

      loadStyleKernLigatureData(s);
      if (s.ligaturePairs && s.header.ligaturePairCount > 0) {
        for (uint8_t li = 0; li < s.header.ligaturePairCount && cpCount < MAX_PAGE_GLYPHS; li++) {
          uint32_t leftCp = s.ligaturePairs[li].pair >> 16;
          uint32_t rightCp = s.ligaturePairs[li].pair & 0xFFFF;
          uint32_t outCp = s.ligaturePairs[li].ligatureCp;

          bool hasLeft = false, hasRight = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == leftCp) hasLeft = true;
            if (codepoints[i] == rightCp) hasRight = true;
            if (hasLeft && hasRight) break;
          }
          if (!hasLeft || !hasRight) continue;

          bool hasOut = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == outCp) {
              hasOut = true;
              break;
            }
          }
          if (!hasOut) {
            codepoints[cpCount++] = outCp;
          }
        }
      }
    }
  }

  // Sort codepoints for ordered interval building
  std::sort(codepoints.get(), codepoints.get() + cpCount);

  // Prewarm each requested style
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    totalMissed += prewarmStyle(si, codepoints.get(), cpCount, metadataOnly);
  }

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly) {
  auto& s = styles_[styleIdx];

  // Idle-prewarm hit: mini data persists across PrewarmScopes (resetStyleMiniData
  // keeps it), so when the previous scope -- typically the idle prewarm of this
  // exact page -- already loaded every requested codepoint the font covers, this
  // page needs zero SD reads. A mini built metadata-only cannot serve a full
  // request (no bitmaps). Any uncovered codepoint falls through to the rebuild.
  if (s.miniGlyphCount > 0 && !(s.miniMetadataOnly && !metadataOnly)) {
    bool covered = true;
    int missedInMini = 0;
    for (uint32_t i = 0; i < cpCount && covered; i++) {
      const uint32_t cp = codepoints[i];
      bool inMini = false;
      for (uint32_t iv = 0; iv < s.miniIntervalCount; iv++) {
        if (cp < s.miniIntervals[iv].first) break;  // intervals sorted ascending
        if (cp <= s.miniIntervals[iv].last) {
          inMini = true;
          break;
        }
      }
      if (inMini) continue;
      // v154（codex P1-1）：被降級丟棄的碼位視為 covered —— 它已由 miss ring 承接，
      // 重建整套 mini 也只會在同樣的堆積條件下再丟一次。二分搜尋（miniDropped 已排序）。
      if (s.miniDroppedCount > 0) {
        const uint32_t* lo = s.miniDropped;
        const uint32_t* hi = s.miniDropped + s.miniDroppedCount;
        const uint32_t* it = std::lower_bound(lo, hi, cp);
        if (it != hi && *it == cp) continue;
      }
      if (findGlobalGlyphIndex(s, cp) < 0) {
        missedInMini++;  // not in font coverage: the rebuild couldn't load it either
      } else {
        covered = false;
      }
    }
    if (covered) {
      return missedInMini;
    }
  }

  // Map codepoints to global glyph indices for this style
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };
  CpGlyphMapping* mappings = new (std::nothrow) CpGlyphMapping[cpCount];
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    int32_t idx = findGlobalGlyphIndex(s, codepoints[i]);
    if (idx >= 0) {
      mappings[validCount].codepoint = codepoints[i];
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  int missed = static_cast<int>(cpCount - validCount);

  if (validCount == 0) {
    freeStyleMiniData(s);
    delete[] mappings;
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Build mini intervals from sorted codepoints. Reset counts and fall back to the
  // stub until the rebuild completes, but KEEP the existing buffers (keep-if-fits
  // reuse) — the free-and-realloc-per-page pattern here was a primary fragmenter.
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;

  if (!ensureArrayCapacity(s.miniIntervals, s.miniIntervalCapacity, validCount)) {
    LOG_ERR("SDCF", "Failed to allocate mini intervals for style %u", styleIdx);
    delete[] mappings;
    return static_cast<int>(cpCount);
  }

  s.miniIntervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      s.miniIntervals[s.miniIntervalCount].first = mappings[rangeStart].codepoint;
      s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
      s.miniIntervals[s.miniIntervalCount].offset = rangeStart;
      s.miniIntervalCount++;
      rangeStart = i;
    }
  }

  // Mini glyph array (reused across pages when it fits)
  if (!ensureArrayCapacity(s.miniGlyphs, s.miniGlyphCapacity, validCount)) {
    LOG_ERR("SDCF", "Failed to allocate mini glyphs for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  s.miniGlyphCount = validCount;

  // Build sorted read order for sequential I/O
  uint32_t* readOrder = new (std::nothrow) uint32_t[validCount];
  if (!readOrder) {
    LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
  std::sort(readOrder, readOrder + validCount,
            [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });

  // v154：共用檔柄 —— 每頁 prewarm 省一次 12–18ms 的開檔。
  if (!ensureFileOpen()) {
    delete[] readOrder;
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  HalFile& file = sharedFile_;

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;

  // Read glyph metadata. lastReadIndex tracks sequential reads to skip redundant
  // seeks; INT32_MIN guarantees the first iteration always seeks to the correct
  // offset (otherwise when gIdx == 0, the "gIdx != lastReadIndex + 1" check would
  // be false and we'd read from the file's current position — the header — which
  // decodes to a garbage EpdGlyph with a massive advanceX, inflating any word
  // containing that codepoint beyond page width).
  int32_t lastReadIndex = INT32_MIN;
  for (uint32_t i = 0; i < validCount; i++) {
    uint32_t mapIdx = readOrder[i];
    int32_t gIdx = mappings[mapIdx].globalIndex;

    uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
    if (gIdx != lastReadIndex + 1) {
      if (!file.seekSet(fileOff)) {
        LOG_ERR("SDCF", "Prewarm: failed to seek to glyph %d (style %u)", gIdx, styleIdx);
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      seekCount++;
    }
    if (file.read(reinterpret_cast<uint8_t*>(&s.miniGlyphs[mapIdx]), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
      LOG_ERR("SDCF", "Prewarm: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
      delete[] readOrder;
      delete[] mappings;
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }
    lastReadIndex = gIdx;
  }

  uint32_t totalBitmapSize = 0;

  if (!metadataOnly) {
    s.miniDroppedCount = 0;  // v154：每次重建重新決定 dropped 集合（ladder 會再填）
    // Compute total bitmap size
    for (uint32_t i = 0; i < validCount; i++) {
      totalBitmapSize += s.miniGlyphs[i].dataLength;
    }

    if (!ensureArrayCapacity(s.miniBitmap, s.miniBitmapCapacity, totalBitmapSize)) {
      // v154（P1 效能止血①）：miniBitmap 降級階梯 —— 搬回舊樹（v55 系）的作法。
      //
      // 原本這裡整段放棄（freeStyleMiniData + return cpCount）＝整頁的字全走
      // glyph-miss ring（8 格）→ 每個字每一趟灰階都從 SD 讀。實機 v153 量到的帳單：
      //   SEG tiled lsb=12,630 msb=11,872 total=26,787  （27 秒一頁）
      //   SDCFFAIL mini-bitmap bytes=33,190 defMax=14,324
      // 而 v130（舊樹）同一本書順暢 —— 差的就是這個階梯。
      //
      // 作法：丟掉「最占空間」的字（丟越少個越好），剩下的字仍走快路徑。
      // 排版不受影響 —— advanceX 走獨立的 advanceTable_，這裡只關 render。
      // 被丟的字由 glyph-miss ring 承接（OVERFLOW_CAPACITY 本版一併 8→32，
      // 帳本記過兩者是配套：ring 太小時被丟的字每一趟都重新 miss）。
      const unsigned dm0 = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
      stats_.bitmapAllocFailures++;
      noteAllocFail("mini-bitmap", totalBitmapSize, dm0,
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)));

      // 依 bitmap 大小遞減排序（readOrder 此刻的 glyph-index 排序已用完，可以重用；
      // 下面讀 bitmap 前反正會再按 dataOffset 重排）。
      std::sort(readOrder, readOrder + validCount,
                [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataLength > s.miniGlyphs[b].dataLength; });

      // dropped 標記借用 mappings[].globalIndex（metadata 已讀完，它不再被使用）。
      static constexpr uint32_t BITMAP_BUDGET_MARGIN = 4096;  // TLSF 取整律：貼上限配必失敗
      // codex P2：ensureArrayCapacity 對 >=8KB 的配置會取整到 8KB 級距 —— 預算必須比較
      // 【量化後】的實配大小，否則「看似符合、實配仍失敗」-> 白白多砍 25%、多丟字，
      // 正好把 dropped 數推過 miss ring 的容量臨界點。
      const auto quantized = [](uint32_t n) -> uint32_t {
        return n >= 8 * 1024 ? ((n + 8 * 1024 - 1) / (8 * 1024)) * (8 * 1024) : n;
      };
      uint32_t dropCursor = 0;
      uint32_t remaining = totalBitmapSize;
      bool bitmapOk = false;
      for (uint8_t attempt = 0; attempt < 4 && !bitmapOk; attempt++) {
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        while (dropCursor < validCount &&
               (largest <= BITMAP_BUDGET_MARGIN ||
                quantized(remaining) + BITMAP_BUDGET_MARGIN > static_cast<uint32_t>(largest))) {
          const uint32_t idx = readOrder[dropCursor++];
          // dataLength==0 的字（空格等）不佔 bitmap，丟了只是白佔 ring —— 跳過。
          if (s.miniGlyphs[idx].dataLength == 0) continue;
          remaining -= s.miniGlyphs[idx].dataLength;
          mappings[idx].globalIndex = -1;  // 丟棄標記
        }
        if (dropCursor >= validCount && quantized(remaining) + BITMAP_BUDGET_MARGIN > largest) break;
        bitmapOk = ensureArrayCapacity(s.miniBitmap, s.miniBitmapCapacity, remaining > 0 ? remaining : 1);
      }

      if (!bitmapOk) {
        LOG_ERR("SDCF", "mini bitmap ladder exhausted (%u bytes) style %u", totalBitmapSize, styleIdx);
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }

      // 就地壓縮 mappings/miniGlyphs（保持碼位遞增 —— mappings 本來就按碼位序），
      // 再重建 intervals：被丟的字從 mini 消失 → render 時 interval 查不到 → 走 miss ring。
      // 壓縮會就地搬移，先把「哪些碼位被丟」快照下來（壓縮後就讀不到了）。
      const uint32_t validCountOrig = validCount;
      // 借 readOrder 前段當快照緩衝不行（它還要重建）—— 用臨時 VLA 風格 new。
      uint32_t* origCp = new (std::nothrow) uint32_t[validCount];
      uint8_t* origDropFlag = new (std::nothrow) uint8_t[validCount];
      if (origCp && origDropFlag) {
        for (uint32_t i = 0; i < validCount; i++) {
          origCp[i] = mappings[i].codepoint;
          origDropFlag[i] = mappings[i].globalIndex < 0 ? 1 : 0;
        }
      }
      uint32_t kept = 0;
      for (uint32_t i = 0; i < validCount; i++) {
        if (mappings[i].globalIndex < 0) continue;
        if (kept != i) {
          mappings[kept] = mappings[i];
          s.miniGlyphs[kept] = s.miniGlyphs[i];
        }
        kept++;
      }
      const uint32_t dropped = validCount - kept;
      // codex P1-1：把被丟的碼位記成排序集合（mappings 原本就按碼位序，被丟的挑出來
      // 仍是遞增），coverage 檢查據此不再重建。codex P3：**不**把 dropped 算進 missed ——
      // 它們仍會由 miss ring 畫出來，不是「字型沒有」。
      if (origCp && origDropFlag &&
          ensureArrayCapacity(s.miniDropped, s.miniDroppedCapacity, dropped > 0 ? dropped : 1)) {
        uint16_t dc = 0;
        for (uint32_t i = 0; i < validCountOrig; i++) {
          if (origDropFlag[i]) s.miniDropped[dc++] = origCp[i];
        }
        s.miniDroppedCount = dc;
        stats_.bitmapGlyphsDropped += dropped;
      } else {
        stats_.bitmapGlyphsDropped += dropped;
        s.miniDroppedCount = 0;  // 記不下就退回「每次重建」的舊行為，不是錯誤
      }
      validCount = kept;
      s.miniGlyphCount = validCount;

      delete[] origCp;
      delete[] origDropFlag;

      s.miniIntervalCount = 0;
      uint32_t rs = 0;
      for (uint32_t i = 1; i <= validCount; i++) {
        if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
          s.miniIntervals[s.miniIntervalCount].first = mappings[rs].codepoint;
          s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
          s.miniIntervals[s.miniIntervalCount].offset = rs;
          s.miniIntervalCount++;
          rs = i;
        }
      }
      totalBitmapSize = remaining;
      // ⚠️ 壓縮讓 readOrder 裡的舊索引全部失效（陣列已就地搬移）——下面的 bitmap 讀取
      // 迴圈會拿它按 dataOffset 重排。必須重建成 0..validCount-1，否則讀進錯的格子
      // = 字形錯置（畫錯字、不當機、不留 log —— 最陰險的那種）。
      for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
      LOG_ERR("SDCF", "mini bitmap degraded: kept=%u dropped=%u bytes=%u style %u", kept, dropped, remaining, styleIdx);
    }
    s.miniBitmapUsed = totalBitmapSize;  // underuse-hysteresis signal for resetStyleMiniData

    // Read bitmap data sorted by file offset
    std::sort(readOrder, readOrder + validCount,
              [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset; });

    uint32_t miniBitmapOffset = 0;
    uint32_t lastBitmapEnd = UINT32_MAX;
    for (uint32_t i = 0; i < validCount; i++) {
      uint32_t mapIdx = readOrder[i];
      EpdGlyph& glyph = s.miniGlyphs[mapIdx];

      if (glyph.dataLength == 0) {
        glyph.dataOffset = miniBitmapOffset;
        continue;
      }

      uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
      if (fileOff != lastBitmapEnd) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "Prewarm: failed to seek to bitmap (style %u)", styleIdx);
          delete[] readOrder;
          delete[] mappings;
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
        seekCount++;
      }
      if (file.read(s.miniBitmap + miniBitmapOffset, glyph.dataLength) != static_cast<int>(glyph.dataLength)) {
        LOG_ERR("SDCF", "Prewarm: short bitmap read (style %u)", styleIdx);
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      lastBitmapEnd = fileOff + glyph.dataLength;

      glyph.dataOffset = miniBitmapOffset;
      miniBitmapOffset += glyph.dataLength;
    }
  }

  uint32_t sdTime = millis() - sdStart;
  delete[] readOrder;
  delete[] mappings;

  // Full render prewarm: load the persistent kern classes + ligatures (one-time
  // per style, small — the big matrix is NOT loaded here) and then build the
  // per-page mini kern matrix restricted to class pairs reachable from this
  // page's codepoints. Skip during metadata-only prewarm — layout only needs
  // advanceX and the mini kern would be thrown away before rendering.
  bool kernLigOk = false;
  if (!metadataOnly) {
    if (loadStyleKernLigatureData(s)) {
      kernLigOk = buildMiniKernMatrix(s, codepoints, cpCount);
    }
  }

  // Populate miniData and swap
  s.miniMetadataOnly = metadataOnly;
  // codex P2：降級的 mini（capacity 是 8K 取整、used 是砍過的量）會被 underuse 判定
  // 誤殺 -> 反覆釋放重建。降級這輪跳過 hysteresis。
  s.miniHysteresisPending = !metadataOnly && s.miniDroppedCount == 0;
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.miniData.bitmap = s.miniBitmap;
  s.miniData.glyph = s.miniGlyphs;
  s.miniData.intervals = s.miniIntervals;
  s.miniData.intervalCount = s.miniIntervalCount;
  s.miniData.advanceY = s.header.advanceY;
  s.miniData.ascender = s.header.ascender;
  s.miniData.descender = s.header.descender;
  s.miniData.is2Bit = s.header.is2Bit;
  if (kernLigOk) {
    applyKernLigaturePointers(s, s.miniData);
  }
  s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.miniData.coverageHandler = &SdCardFont::onCoverageQuery;

  s.epdFont.data = &s.miniData;

  // Accumulate stats
  stats_.sdReadTimeMs += sdTime;
  stats_.seekCount += seekCount;
  stats_.uniqueGlyphs += validCount;
  stats_.bitmapBytes += totalBitmapSize;

  return missed;
}

// --- Cache management ---

void SdCardFont::clearCache() {
  clearOverflow();
  // Note: advance table is intentionally preserved here. It persists across
  // layout passes so repeated section indexing amortizes SD reads. Use
  // clearPersistentCache() to wipe it.
  // 一次量完（heap walk 持有 heap 鎖，別每個字面各走一趟）。
  const bool heapTight = ESP.getFreeHeap() < MINI_RETAIN_MIN_FREE_HEAP ||
                         heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < MINI_RETAIN_MIN_MAXBLOCK;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    resetStyleMiniData(styles_[i], heapTight);
    applyGlyphMissCallback(i);
  }
}

size_t SdCardFont::releaseMiniData() {
  clearOverflow();
  size_t bytes = 0;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    PerStyle& s = styles_[i];
    if (!s.present) continue;
    bytes += static_cast<size_t>(s.miniBitmapCapacity) + static_cast<size_t>(s.miniGlyphCapacity) * sizeof(s.miniGlyphs[0]) +
             static_cast<size_t>(s.miniIntervalCapacity) * sizeof(s.miniIntervals[0]);
    freeStyleMiniData(s);
    applyGlyphMissCallback(i);
  }
  return bytes;
}

size_t SdCardFont::retainedMiniBitmapCapacity() const {
  size_t best = 0;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    const PerStyle& s = styles_[i];
    if (!s.present || !s.miniBitmap) continue;
    if (s.miniBitmapCapacity > best) best = s.miniBitmapCapacity;
  }
  return best;
}

// --- Advance table ---

void SdCardFont::clearPersistentCache() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    delete[] advanceTable_[i];
    advanceTable_[i] = nullptr;
    advanceTableSize_[i] = 0;
  }
}

uint32_t SdCardFont::resetAdvanceTables() {
  // v193：一次配滿 768 格之後就地重用；只把 size 歸零，下一章直接往同一塊寫。
  uint32_t used = 0;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    used += advanceTableSize_[i];
    advanceTableSize_[i] = 0;
  }
  return used;
}

bool SdCardFont::advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const {
  const AdvanceEntry* table = advanceTable_[styleIdx];
  const uint32_t size = advanceTableSize_[styleIdx];
  if (!table || size == 0) return false;
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    if (outAdvance) *outAdvance = table[lo].advanceX;
    return true;
  }
  return false;
}

void SdCardFont::mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount) {
  if (newCount == 0) return;
  uint32_t oldSize = advanceTableSize_[styleIdx];
  if (oldSize >= ADVANCE_CACHE_LIMIT) {
    // v192：表滿拒收整批新項（newCount>0 才走到這裡）；不是擠掉舊項。
    ++advanceRejectCount_;
    return;
  }

  // v55：一次配足終局大小，之後【原地】反向合併 —— 不再「配新的 → 複製 → 刪舊的」。
  //
  // 為什麼：舊寫法每次成長都要一塊更大的連續空間，新配置永遠不可能重用剛釋放的舊洞，
  // 於是一路往堆積前方走。閱讀穩態下主池最大塊只有約 3.7KB，所以表長到 3.7KB 之後每次
  // 成長都落進【備援池 p2】，長到 768 筆上限後就永遠不再重配也不釋放 —— 一塊約 9KB 的
  // 長壽殘骸卡在 p2 中段，把最大連續塊【從 115,616 砍到 42,312】（v54 實機實測，
  // 整個 session 數值一字不變；v58/v59/v60/v126 四份 POOL 傾印佐證修後為「兩顆約 6,400B
  // 的表靠在池頭、無楔子」）。而每頁字圖需要 45,350 → 必然失敗 → 整頁字型降級 → 25 秒翻頁。
  //
  // 一次配足 6,144B 峰值不變（那本來就是終局值，只是提前到位），但配置只發生一次、
  // 位置固定，不再製造遞增的洞。
  //
  // ⚠️⚠️ 【這兩半必須同時存在】只搬「原地反向合併」而不搬「一次配滿 ADVANCE_CACHE_LIMIT」，
  //    就會在 mergedCap > oldSize 時寫出配置範圍外 —— 堆積越界寫，與殺掉第一台機器的
  //    v131 同型（那次也是「配置大小」與「寫入大小」的隱含前提脫鉤）。
  //
  // 2026-08-25 實機 crash 佐證（v137，中文 EPUB 讀約 95 秒後）：
  //   [SDCF] buildAdvanceTable: failed to allocate codepoint buffer (16384 bytes)  ×6
  //   接著 std::__new_allocator<FootnoteEntry>::allocate -> operator new -> terminate -> abort()
  // 最大連續塊已掉到 16KB 以下。上游用的正是被 v55 否決掉的成長搬遷寫法。
  if (!advanceTable_[styleIdx]) {
    advanceTable_[styleIdx] = new (std::nothrow) AdvanceEntry[ADVANCE_CACHE_LIMIT];
    if (!advanceTable_[styleIdx]) {
      LOG_ERR("SDCF", "advanceTable: alloc failed (%u entries) style %u", ADVANCE_CACHE_LIMIT, styleIdx);
      return;
    }
    advanceTableSize_[styleIdx] = 0;
    oldSize = 0;
  }

  AdvanceEntry* const tbl = advanceTable_[styleIdx];
  uint32_t mergedCap = oldSize + newCount;
  if (mergedCap > ADVANCE_CACHE_LIMIT) mergedCap = ADVANCE_CACHE_LIMIT;

  // 反向合併：兩個來源都已排序，從尾端往前寫進同一塊緩衝。寫入位置 k-1 恆 >= 讀取位置 i-1
  // （k >= i 在整個迴圈都成立），所以不會覆寫還沒讀到的舊資料 —— 原地合併成立的關鍵不變量。
  //
  // 超過上限時【丟棄尾端】，與上游的前向實作逐位元組等價（桌面對拍 7,000 組含重複碼位全同）：
  // 前向填充從最小的開始、填滿即停 => 保留最小的 mergedCap 個；
  // 反向填充天然保留「最大的 mergedCap 個」，故要先空轉跳過最大的 drop 個。
  uint32_t i = oldSize;
  uint32_t j = newCount;
  uint32_t k = mergedCap;
  uint32_t drop = (oldSize + newCount > mergedCap) ? (oldSize + newCount - mergedCap) : 0;
  while (drop > 0 && (i > 0 || j > 0)) {
    if (i > 0 && (j == 0 || tbl[i - 1].codepoint > sortedNew[j - 1].codepoint)) {
      --i;
      ++advanceEvictCount_;  // v192：超量合併丟掉既有高碼位項；與表滿拒收（areject）不是同一條路
    } else {
      --j;
      ++advanceRejectCount_;  // v192：超量合併丟掉新項目，同樣是新字進不去
    }
    --drop;
  }
  while (k > 0 && (i > 0 || j > 0)) {
    if (i > 0 && (j == 0 || tbl[i - 1].codepoint > sortedNew[j - 1].codepoint)) {
      tbl[--k] = tbl[--i];
    } else {
      tbl[--k] = sortedNew[--j];
    }
  }
  // 來源總數 < mergedCap 時 k>0，前 k 格是未使用的殘留，把有效資料往前搬齊。
  if (k > 0) {
    for (uint32_t m = k; m < mergedCap; m++) tbl[m - k] = tbl[m];
    mergedCap -= k;
  }
  advanceTableSize_[styleIdx] = mergedCap;
}

bool SdCardFont::hasAdvanceTable() const {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (advanceTable_[i]) return true;
  }
  return false;
}

uint16_t SdCardFont::getAdvance(uint32_t codepoint, uint8_t style) const {
  style &= (MAX_STYLES - 1);
  if (!advanceTable_[style]) {
    ++advanceMissCount_;  // v192：量測路徑沒命中才可能去打 SD
    return 0;
  }
  const AdvanceEntry* table = advanceTable_[style];
  const uint32_t size = advanceTableSize_[style];
  // Binary search sorted by codepoint
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    return table[lo].advanceX;
  }
  ++advanceMissCount_;
  return 0;
}

// Given a sorted array of unique codepoints, resolve glyph indices per style,
// batch-read advanceX from SD, and merge into the persistent advance table.
// Caller owns the codepoints buffer.
int SdCardFont::fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask) {
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    const auto& s = styles_[si];

    // Stop fetching once the cache is full — further inserts would be dropped
    // by the merge anyway. The renderer fast path tolerates missing entries
    // (returns 0); the slow path is still correct for those codepoints.
    if (advanceTableSize_[si] >= ADVANCE_CACHE_LIMIT) {
      for (uint32_t i = 0; i < cpCount; i++) {
        if (advanceTableLookup(si, codepoints[i], nullptr)) continue;
        // v192：確定有新碼位要插、表滿插不進才算；整批早已在表裡不准假陽性。
        ++advanceRejectCount_;
        break;
      }
      continue;
    }

    // For each codepoint in `codepoints`, skip those already cached, then
    // resolve to a glyph index. Build a parallel array sorted by glyph index
    // for sequential SD reads.
    struct CpIdx {
      uint32_t codepoint;
      int32_t glyphIndex;
    };
    std::unique_ptr<CpIdx[]> mappings(new (std::nothrow) CpIdx[cpCount]);
    if (!mappings) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate mappings for style %u", si);
      totalMissed += cpCount;
      continue;
    }

    uint32_t needCount = 0;
    uint32_t missedThisStyle = 0;
    const int32_t replacementIdx = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH);
    for (uint32_t i = 0; i < cpCount; i++) {
      const uint32_t cp = codepoints[i];
      if (advanceTableLookup(si, cp, nullptr)) continue;  // already cached
      int32_t idx = findGlobalGlyphIndex(s, cp);
      if (idx < 0) {
        if (replacementIdx < 0) {
          missedThisStyle++;
          continue;
        }
        idx = replacementIdx;
      }
      mappings[needCount].codepoint = cp;
      mappings[needCount].glyphIndex = idx;
      needCount++;
    }
    totalMissed += static_cast<int>(missedThisStyle);

    if (needCount == 0) continue;

    // Sort by glyph index so SD reads are mostly sequential.
    std::sort(mappings.get(), mappings.get() + needCount,
              [](const CpIdx& a, const CpIdx& b) { return a.glyphIndex < b.glyphIndex; });

    // Open file once and read advanceX for each needed glyph.
    HalFile file;
    if (!Storage.openFileForRead("SDCF", filePath_, file)) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to open .cpfont for style %u", si);
      continue;
    }

    std::unique_ptr<AdvanceEntry[]> staged(new (std::nothrow) AdvanceEntry[needCount]);
    if (!staged) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate staging for style %u", si);
      file.close();
      continue;
    }

    uint32_t fetched = 0;
    EpdGlyph tempGlyph;
    int32_t lastReadIndex = INT32_MIN;
    for (uint32_t i = 0; i < needCount; i++) {
      int32_t gIdx = mappings[i].glyphIndex;
      uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
      if (gIdx != lastReadIndex + 1) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "buildAdvanceTable: failed to seek to glyph %d (style %u)", gIdx, si);
          break;
        }
      }
      if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
        LOG_ERR("SDCF", "buildAdvanceTable: short glyph read (style %u, glyph %d)", si, gIdx);
        break;
      }
      lastReadIndex = gIdx;
      staged[fetched].codepoint = mappings[i].codepoint;
      staged[fetched].advanceX = tempGlyph.advanceX;
      fetched++;
      // v191：打點必須在 seek+read 【之後】——打在迴圈開頭時，單一 glyph 的那次讀取（以及每批最後一個）
      // 沒有後續 site 7 收尾，空窗會被下一個字寬探針收走而繼續誤報成 gapsite=2，儀器目的落空（複查抓到）。
      if (buildProbeHook_) buildProbeHook_(7);
    }
    file.close();

    if (fetched > 0) {
      // Sort staged by codepoint, then merge into the persistent table.
      std::sort(staged.get(), staged.get() + fetched,
                [](const AdvanceEntry& a, const AdvanceEntry& b) { return a.codepoint < b.codepoint; });
      mergeIntoAdvanceTable(si, staged.get(), fetched);
    }

    LOG_DBG("SDCF", "Advance table style %u: +%u from SD, total=%u/%u", si, fetched, advanceTableSize_[si],
            ADVANCE_CACHE_LIMIT);
  }

  return totalMissed;
}

template <typename Iter>
int SdCardFont::buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask,
                                       const char* extraText) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // +2 reserved slots for space and hyphen injected after the main scan.
  static constexpr uint32_t MAX_UNIQUE_CODEPOINTS = 4096;
  // v164：常駐化（v55「一次配滿、就地重用」手法）。原本每次呼叫都 new/delete 16KB —— 
  // 實機 diag（v161/162 段 12 次 SDCFFAIL codepoint-buf）證明建置視窗裡這顆 16KB 連續塊
  // 常常賭輸（最緊時 defMax 只剩 3,060），輸了整頁 advance 表就退回逐字 SD 慢路徑。
  // 改成第一次成功配置後就留著（freeAll 釋放）：配置時機在開書早期、堆積寬鬆，
  // 之後建置視窗裡不再有這顆反覆出現的峰值。UI 備援字型不做版面量測，永遠不配。
  if (!cpScratch_) {
    cpScratch_ = new (std::nothrow) uint32_t[MAX_UNIQUE_CODEPOINTS + 2];
  }
  uint32_t* codepoints = cpScratch_;
  if (!codepoints) {
    // v150：附上【失敗當下】的堆積數字。三次實機（v141/v148/v149）都出現「量測點顯示
    // p2 有 26–80KB，274–450ms 後這個 16KB 配置卻失敗」—— 只有失敗現場的數字能分辨
    // 「瞬時被吃光」與「這個配置根本用不到 p2」兩個假說。
    {
      const unsigned dm = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
      const unsigned df = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate codepoint buffer (%u bytes) maxAlloc=%u free=%u",
              MAX_UNIQUE_CODEPOINTS * 4, dm, df);
      noteAllocFail("codepoint-buf", MAX_UNIQUE_CODEPOINTS * 4, dm, df);
    }
    return -1;
  }
  uint32_t cpCount = 0;
  bool hitCap = false;

  for (auto it = begin; it != end && !hitCap; ++it) {
    hitCap = collectUniqueCodepoints(asCStr(*it), codepoints, cpCount, MAX_UNIQUE_CODEPOINTS);
  }
  if (extraText && !hitCap) {
    hitCap = collectUniqueCodepoints(extraText, codepoints, cpCount, MAX_UNIQUE_CODEPOINTS);
  }

  if (includeSpace && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == ' '; }))
    codepoints[cpCount++] = ' ';
  if (includeHyphen && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == '-'; }))
    codepoints[cpCount++] = '-';

  if (hitCap) {
    LOG_ERR("SDCF", "buildAdvanceTable: unique codepoint cap (%u) hit, layout may be approximate",
            MAX_UNIQUE_CODEPOINTS);
  }
  std::sort(codepoints, codepoints + cpCount);
  int totalMissed = fetchAdvancesForCodepoints(codepoints, cpCount, styleMask);
  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::buildAdvanceTable(const char* utf8Text, uint8_t styleMask, const char* extraText) {
  return buildAdvanceTableRange(&utf8Text, &utf8Text + 1, false, false, styleMask, extraText);
}

int SdCardFont::buildAdvanceTable(const std::deque<std::string>& words, bool includeHyphen, uint8_t styleMask,
                                  const char* extraText) {
  return buildAdvanceTableRange(words.begin(), words.end(), words.size() > 1, includeHyphen, styleMask, extraText);
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  style &= (MAX_STYLES - 1);
  if (!styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return styles_[style & (MAX_STYLES - 1)].present; }

uint8_t SdCardFont::resolveStyle(uint8_t style) const {
  static const uint8_t kFallbacks[MAX_STYLES][MAX_STYLES] = {
      // REGULAR: REGULAR -> BOLD -> ITALIC -> BOLD_ITALIC
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC},
      // BOLD: BOLD -> REGULAR -> BOLD_ITALIC -> ITALIC
      {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::ITALIC},
      // ITALIC: ITALIC -> REGULAR -> BOLD_ITALIC -> BOLD
      {EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD},
      // BOLD_ITALIC: BOLD_ITALIC -> BOLD -> ITALIC -> REGULAR
      {EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR},
  };

  const uint8_t styleBits = style & (MAX_STYLES - 1);
  for (uint8_t candidate : kFallbacks[styleBits]) {
    if (styles_[candidate].present) return candidate;
  }
  return EpdFontFamily::REGULAR;
}

uint8_t SdCardFont::resolveStyleMask(uint8_t styleMask) const {
  uint8_t resolvedMask = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (styleMask & (1 << si)) {
      resolvedMask |= static_cast<uint8_t>(1u << resolveStyle(si));
    }
  }
  return resolvedMask;
}

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  const auto& s = self->styles_[styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return nullptr;

  // Check overflow cache first (matching both codepoint and style)
  for (uint32_t i = 0; i < self->overflowCount_; i++) {
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx) {
      return &self->overflow_[i].glyph;
    }
  }

  // Look up global glyph index via full intervals
  int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails. Bookkeeping (count/next)
  // is deferred until after all I/O succeeds to avoid inconsistent state.
  uint32_t slot = self->overflowNext_;
  bool wasAtCapacity = (self->overflowCount_ == OVERFLOW_CAPACITY);

  // Read glyph metadata into temporary
  // v154：共用檔柄 —— miss 路徑原本每個字開一次檔（12–18ms），
  // 降級階梯丟出來的字全走這裡，這是 27 秒頁的第二半。
  if (advanceSdProbeDepth_ > 0) {
    ++advanceSdReadCount_;  // v192：計嘗試次數，讀失敗也算；繪製 miss／overflow 命中不算
  }

  if (!self->ensureFileOpen()) {
    LOG_ERR("SDCF", "Overflow: failed to open .cpfont");
    return nullptr;
  }

  EpdGlyph tempGlyph = {};
  uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  if (!self->sharedFile_.seekSet(glyphFileOff)) {
    LOG_ERR("SDCF", "Overflow: failed to seek to glyph for U+%04X style %u", codepoint, styleIdx);
    
    return nullptr;
  }
  if (self->sharedFile_.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    return nullptr;
  }

  // Read bitmap data into temporary (if any)
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength > 0) {
    // v167（crash_report166 定案）：事前檢查，不能只靠 nothrow —— libstdc++ 的 nothrow
    // 版是「呼叫丟例外版再 catch」，堆積低到連 130B 的例外物件都配不出來時，
    // __cxa_allocate_exception 直接 terminate（v93 實測教訓）。miss ring 在渲染最缺
    // 記憶體的時刻每個字都要進來一次，這裡是全機最高頻的輪盤。512B 餘裕給例外機制。
    if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) <
        static_cast<size_t>(tempGlyph.dataLength) + 512) {
      LOG_ERR("SDCF", "Overflow: heap floor, skip U+%04X (%u bytes)", codepoint, tempGlyph.dataLength);
      return nullptr;
    }
    tempBitmap = new (std::nothrow) uint8_t[tempGlyph.dataLength];
    if (!tempBitmap) {
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      return nullptr;
    }
    if (!self->sharedFile_.seekSet(s.bitmapFileOffset + tempGlyph.dataOffset)) {
      LOG_ERR("SDCF", "Overflow: failed to seek to bitmap for U+%04X", codepoint);
      delete[] tempBitmap;
      
      return nullptr;
    }
    if (self->sharedFile_.read(tempBitmap, tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      delete[] tempBitmap;
      return nullptr;
    }
  }

  // All reads succeeded — commit to slot and advance ring buffer
  if (wasAtCapacity) {
    delete[] self->overflow_[slot].bitmap;
  } else {
    self->overflowCount_++;
  }
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = tempBitmap;
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;

  LOG_DBG("SDCF", "Overflow: loaded U+%04X style %u on demand (slot %u/%u)", codepoint, styleIdx, slot,
          OVERFLOW_CAPACITY);

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) {
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }
