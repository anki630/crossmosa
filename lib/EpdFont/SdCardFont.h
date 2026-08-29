#pragma once

#include <HalStorage.h>  // v154: HalFile sharedFile_
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "EpdFont.h"
#include "EpdFontData.h"

// On-disk binary format version for .cpfont files. Defined as a preprocessor
// macro (rather than a constexpr) so it can be stringified into the SD-fonts
// release URL — see FONT_MANIFEST_URL in FontDownloadActivity.h. No integer
// suffix because stringification would include it (e.g. `4U` → `"4U"`).
//
// The canonical version for the build tooling lives in
// lib/EpdFont/scripts/cpfont_version.py. This firmware-side copy must be
// bumped manually when the firmware is updated to support a new format.
// Reader enforcement: SdCardFont::load().
#define CPFONT_VERSION 4

class SdCardFont {
 public:
  // v153：最後一次字型系統配置失敗的描述（靜態、先到先得、由 src 端讀走進 diag.log）。
  // v151 的兩行 mini bitmap 失敗只在 LOG_ERR（沒序列埠＝丟掉），慢頁的頭號嫌犯因此隱形。
  static char lastAllocFail[96];
  static void noteAllocFail(const char* what, unsigned bytes, unsigned defMax, unsigned defFree);
  // v191：建置探針 hook。lib/EpdFont 對 lib/Epub 零依賴，閱讀器把 site 7 轉去 noteBuildProbe。
  static void setBuildProbeHook(void (*fn)(uint8_t));
  // v192：字寬表診斷（靜態、無配置）。lib/EpdFont 對 lib/Epub 零依賴，由 app 在 BUILD 視窗讀走。
  static uint32_t advanceMissCount_;
  static uint32_t advanceSdReadCount_;
  static uint32_t advanceRejectCount_;
  static uint32_t advanceEvictCount_;
  static void resetAdvanceDiag();
  // v192：asd 計 fallback 讀 glyph 的嘗試次數；巢狀深度避免內層提早歸零。
  static void setAdvanceSdProbe(bool on);
 private:
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_STYLES = 4;

  SdCardFont() = default;
  ~SdCardFont();
  // Owns raw buffers freed in dtor — no shallow-copy semantics. Make any
  // accidental pass-by-value or move a compile-time error.
  SdCardFont(const SdCardFont&) = delete;
  SdCardFont& operator=(const SdCardFont&) = delete;
  SdCardFont(SdCardFont&&) = delete;
  SdCardFont& operator=(SdCardFont&&) = delete;

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path);

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false);

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from words (no MAX_PAGE_GLYPHS cap),
  // batch-reads advanceX from SD, stores in a sorted per-style table.
  // extraText: optional additional codepoints to warm in the same SD pass
  // (e.g. shaped Arabic presentation forms the measurement path will look up).
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F, const char* extraText = nullptr);
  int buildAdvanceTable(const std::deque<std::string>& words, bool includeHyphen, uint8_t styleMask = 0x0F,
                        const char* extraText = nullptr);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;

  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;

  // Free mini data for all styles and restore stub EpdFontData.
  // Preserves the persistent advance cache so repeated layout passes can reuse
  // previously fetched metrics.
  void clearCache();
  // v188：真正釋放所有字面的 mini 資料（bitmap／glyph／interval 緩衝），回傳釋放的位元組數。
  // clearCache() 為了防 p2 碎片化【保留容量】，但同步章節重排是記憶體峰值，那 25–43KB 留著就是
  // 重排配不到 4KB 的原因（diag187_2：4 次「記憶體不足」全是這形狀）。只在建置視窗前呼叫。
  size_t releaseMiniData();
  // v189：目前保留中的 mini bitmap 容量（各字面取最大，bytes）。預取的堆積地板要把它算進去——
  // clearCache() 保留這塊容量正是為了讓下一頁就地重用（ensureArrayCapacity keep-if-fits），
  // 在它還駐留時量「最大連續塊」再拿去否決預取，等於拿預取自己會重用的緩衝否決預取自己
  // （diag-prev188：pmax=28 的 89 頁裡 75 頁被 pg=7 擋掉，而 mini 容量 40–44KB 就在旁邊）。
  size_t retainedMiniBitmapCapacity() const;

  // Drop the persistent advance cache. Call when unloading the SD font or
  // when font/size/family/glyph-table state changes.
  void clearPersistentCache();
  // v193：換章時清空各字面 advance 表的「用量」但【保留已配置的 768 格】。
  // 表是一次配滿 ADVANCE_CACHE_LIMIT、之後原地合併（見 mergeIntoAdvanceTable），
  // 釋放再重配會在 p2 挖洞。回傳清之前各字面 size 加總，給呼叫端印 ADVRESET。
  uint32_t resetAdvanceTables();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // Resolve requested style bits to the closest present style.
  uint8_t resolveStyle(uint8_t style) const;

  // Resolve every requested style bit through fallback and return the actual
  // styles that need cache/advance preparation.
  uint8_t resolveStyleMask(uint8_t styleMask) const;

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
    // v161（TXTPAGE 回退判準；v121 折算預取統計用）：
    uint32_t bitmapAllocFailures = 0;  // 撞到連續區塊天花板的次數（觸發降級階梯）
    uint32_t bitmapGlyphsDropped = 0;  // 階梯實際丟掉的字圖數（仍由 miss ring 畫出）
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig, prewarm cache, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Full intervals loaded from file (kept in RAM for codepoint lookup)
    EpdUnicodeInterval* fullIntervals = nullptr;
    EPD_PACKED_BEGIN
    struct BmpInterval16 {
      uint16_t first;
      uint16_t last;
      uint16_t offset;
    } EPD_PACKED_ATTR;
    EPD_PACKED_END
    static_assert(sizeof(BmpInterval16) == 6, "BmpInterval16 must remain compact");
    BmpInterval16* bmpIntervals = nullptr;
    bool intervalsAreBmp16 = false;

    // Persistent kern-class + ligature tables (lazy-loaded on first prewarm).
    // The full kern MATRIX is NOT resident — on Literata-class fonts a single
    // style's matrix is ~36-42KB contiguous, and 4 styles' worth won't fit
    // alongside bitmaps + framebuffer on a 380KB device. Only kernLeftClasses
    // and kernRightClasses (small codepoint→classId tables, ~3KB each) stay
    // resident; the matrix is reconstructed per-page as miniKernMatrix.
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    bool kernLigLoaded = false;

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm. Buffers are kept-if-fits across pages
    // (capacities below track allocated sizes): freeing and reallocating slightly
    // different sizes on every page turn was a primary heap fragmenter — each page's
    // freed hole rarely fit the next page's need, so maxAlloc eroded all session.
    // The per-render PrewarmScope calls clearCache() -> resetStyleMiniData(), which
    // keeps both the allocations AND the loaded data. Buffers: reuse means
    // ensureArrayCapacity early-returns once capacities converge on the book's
    // max, so page turns stop touching the allocator (the free/realloc-per-page
    // pattern was a primary heap fragmenter). Data: the next prewarm
    // subset-checks against the resident tables (see prewarmStyle), so the idle
    // prewarm of page N+1 serves the actual turn with zero SD reads. Retention
    // is bounded two ways in resetStyleMiniData(): a heap floor frees outright
    // under pressure, and sustained underuse (an outlier page's oversized bitmap
    // arena) frees after a few consecutive low-use rebuilds. freeStyleMiniData()
    // remains the full teardown (zeroes capacities) for style eviction / font
    // unload.
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;
    uint32_t miniIntervalCapacity = 0;
    uint32_t miniGlyphCapacity = 0;
    uint32_t miniBitmapCapacity = 0;
    // v154（codex P1-1）：降級時被丟棄的碼位（排序）。coverage 檢查把它們視為
    // 「已由 miss ring 承接」—— 否則同一頁的下一次 prewarm 永遠 covered=false，
    // idle-prewarm 與正式翻頁各重建一整套（metadata 重讀 + 階梯重跑）。
    uint32_t* miniDropped = nullptr;
    uint16_t miniDroppedCount = 0;
    uint16_t miniDroppedCapacity = 0;
    // Bitmap bytes the current page actually used (set by prewarmStyle), the
    // underuse-hysteresis signal; 0 = no bitmap built this scope (metadata-only
    // prewarm), which leaves the hysteresis counter untouched.
    uint32_t miniBitmapUsed = 0;
    uint8_t miniUnderuseRuns = 0;
    // True when the resident mini was built metadata-only (no bitmaps): it can
    // serve metadata requests but a full render request must rebuild.
    bool miniMetadataOnly = false;
    // Set by a rebuild, consumed by resetStyleMiniData: gates the underuse
    // hysteresis to one evaluation per rebuild (scopes reset twice, and subset
    // hits load nothing new to judge).
    bool miniHysteresisPending = false;

    // Per-page mini kern matrix (built by buildMiniKernMatrix on each full
    // prewarm). miniKernLeftClasses/miniKernRightClasses map ONLY the codepoints
    // used on the current page to renumbered class IDs (1..miniKern*ClassCount).
    // miniKernMatrix is a small miniKernLeftClassCount × miniKernRightClassCount
    // flat matrix. Typical Latin page: ~25×25 matrix = ~625 bytes per style vs
    // ~36KB for the full Literata matrix — ~50× reduction.
    EpdKernClassEntry* miniKernLeftClasses = nullptr;
    EpdKernClassEntry* miniKernRightClasses = nullptr;
    uint16_t miniKernLeftEntryCount = 0;
    uint16_t miniKernRightEntryCount = 0;
    uint8_t miniKernLeftClassCount = 0;
    uint8_t miniKernRightClassCount = 0;
    int8_t* miniKernMatrix = nullptr;
    // Kept-if-fits capacities, same rationale as the mini glyph buffers above.
    uint16_t miniKernLeftCapacity = 0;
    uint16_t miniKernRightCapacity = 0;
    uint32_t miniKernMatrixCapacity = 0;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // Shared on-demand overflow buffer (ring buffer of glyphs loaded via glyphMissHandler)
  // v154：8 → 32（舊樹值）。**與降級階梯是配套**（帳本明載）：階梯把最占空間的字
  // 丟給這個 ring 承接，8 格時被丟的 30–60 個字每一趟灰階都重新 miss + SD 讀；
  // 32 格讓一頁內被丟的字大多留在 ring。代價 = 24 格 × entry 大小的常駐 RAM。
  // v154（v55 回歸）：常駐的 .cpfont 檔柄。SdFat 的 open 要從根目錄掃整條路徑
  // （實測 12–18ms/次），而 glyph-miss 路徑原本【每個字】開一次 —— 降級階梯把字丟給
  // miss ring 之後，這個成本 × 丟棄數 × 灰階 14 趟就是災難。
  // ⚠️ 適用前提（CLAUDE.md A-4）：唯讀、且不會有第二個 handle 同時開它 —— .cpfont
  //    正是被點名合法的那類。freeAll() 負責關閉。
  HalFile sharedFile_;
  bool ensureFileOpen();

  static constexpr uint32_t OVERFLOW_CAPACITY = 32;
  struct OverflowEntry {
    EpdGlyph glyph;
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
  };
  OverflowEntry overflow_[OVERFLOW_CAPACITY] = {};
  uint32_t overflowCount_ = 0;
  uint32_t overflowNext_ = 0;

  // Compact advance-only table for layout measurement (per-style).
  // v164：buildAdvanceTable 的碼位去重暫存（16,392B，lazy、常駐到 freeAll）。
  // 見 .cpp 的常駐化註解；只有讀者字型會配（UI 備援不做版面量測）。
  uint32_t* cpScratch_ = nullptr;

  // Built by buildAdvanceTable(), queried by getAdvance().
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  // Per-style advance table. Sorted by codepoint for binary lookup.
  // Bounded to ADVANCE_CACHE_LIMIT entries; persists across layout passes
  // (across calls to clearCache()) so repeated indexing of the same font
  // amortizes SD reads. Buffer lives until font unload / clearPersistentCache();
  // resetAdvanceTables() zeroes the sizes without freeing.
  static constexpr uint32_t ADVANCE_CACHE_LIMIT = 768;
  AdvanceEntry* advanceTable_[MAX_STYLES] = {};
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  bool advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const;
  // Merge sortedNew (sorted by codepoint, no overlap with existing) into the
  // advance table for styleIdx, preserving sort order; cap-truncates the tail.
  void mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount);
  static uint8_t advanceSdProbeDepth_;

  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  // Per-scope variant: drop the page's data, keep the allocations (see the
  // PerStyle comment). May escalate to freeStyleMiniData under heap pressure
  // or sustained underuse.
  void resetStyleMiniData(PerStyle& s, bool heapTight);
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  void freeStyleMiniKern(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s);
  bool buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount);
  void applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask);
  template <typename Iter>
  int buildAdvanceTableRange(Iter begin, Iter end, bool includeSpace, bool includeHyphen, uint8_t styleMask,
                             const char* extraText = nullptr);
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
  static void (*buildProbeHook_)(uint8_t);

  // Static callback for EpdFontData::coverageHandler: answers hasCodepoint()
  // from the RAM-resident full interval table, without SD I/O.
  static bool onCoverageQuery(void* ctx, uint32_t codepoint);
};
