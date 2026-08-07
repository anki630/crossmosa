#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <HalStorage.h>  // HalFile(v55 共用檔柄)

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
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_STYLES = 4;
  // v110 字型預取:prewarm() 被 shouldAbort 中止時的回傳值。刻意選一個不會與
  // 「未載入」(-1)或「缺字數」(>=0)撞號的值,呼叫端才分得出「中止」與「失敗/缺字」。
  // 中止 ⇒ 快取【不完整】:呼叫端必須當成沒有 prewarm 過(見 FontCacheManager)。
  static constexpr int PREWARM_ABORTED = -2;
  // v110 複審修正:硬失敗(配置失敗 / 開檔、seek、短讀)的回傳值。
  // 原本這些路徑回傳 cpCount —— 與「這套字型就是沒有這 N 個字」用同一個正值通道,
  // 而它們的後果完全相反:硬失敗已經 freeStyleMiniData() 把整組快取釋放掉了,
  // 呼叫端若把它當成功,就會替一份空快取蓋上 valid 身分 ⇒ 下一次 render 無聲地
  // 整頁走 32 格 overflow ring(warm=1、prewarm_ms=0,而 dropped/alloc_fail 全是 0
  // ⇒ spec §7 的回退判準看不見它)。這正是中止路徑早就防住、卻漏掉的同一類缺陷。
  // 「缺字」(validCount==0 那條)刻意【不】用這個值:重跑一次會得到一模一樣的快取,
  // 採用身分是正確的。
  static constexpr int PREWARM_FAILED = -3;

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
  // v110: shouldAbort/abortCtx are optional. When supplied, the polled callback
  // is checked periodically inside the SD read loops; returning true aborts the
  // prewarm, frees whatever this call had built so far, and returns
  // PREWARM_ABORTED. Defaults keep every existing call site byte-identical.
  // v110 複審修正:任何字面發生硬失敗 → 回傳 PREWARM_FAILED(其餘字面仍照常
  // 預載完,與 v109 的「單一字面失敗不影響其他字面」相同)。
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false,
              bool (*shouldAbort)(void*) = nullptr, void* abortCtx = nullptr);

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from words (no MAX_PAGE_GLYPHS cap),
  // batch-reads advanceX from SD, stores in a sorted per-style table.
  // extraText: optional additional codepoints to warm in the same SD pass
  // (e.g. shaped Arabic presentation forms the measurement path will look up).
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F, const char* extraText = nullptr);
  int buildAdvanceTable(const std::vector<std::string>& words, bool includeHyphen, uint8_t styleMask = 0x0F,
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

  // Drop the persistent advance cache. Call when unloading the SD font or
  // when font/size/family/glyph-table state changes.
  void clearPersistentCache();

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
    // v53 量測:單一 style 的 miniBitmap 單塊配置峰值。bitmapBytes 是跨 style 累加值,
    // 而「40KB 連續區塊天花板」要問的是【單次最大連續配置】——混排 regular+bold 的頁
    // (標題/強調,極常見)累加值會是兩塊之和,系統性高估峰值單塊需求。
    uint32_t maxSingleBitmapAlloc = 0;
    uint32_t bitmapAllocFailures = 0;  // 撞到連續區塊天花板的次數(該頁字型降級,無畫面提示)
    // v55:因為配不下而被踢去 overflow ring 的字數。>0 代表該頁已在降級模式,
    // 畫面仍正確但那些字每次重繪都要回 SD 讀一次(灰階頁會乘上重繪趟數)。
    uint32_t bitmapGlyphsDropped = 0;
    // v58 量測(評估「跨頁字型快取」是否值得做):這一頁要用的字,有幾個【上一頁也用過】。
    // 量法零額外儲存——prewarmStyle 在 freeStyleMiniData() 之前,上一頁的 miniIntervals
    // 還完整在原地,直接拿新字集去查它即可,不必記住上一頁的碼位。
    // 這正好就是「跨頁快取會命中幾次」的定義,不是近似值。
    uint32_t prevPageGlyphHits = 0;
    uint32_t prevPageGlyphTotal = 0;
  };
  // v55:目前常駐在堆上的位元組數(interval 表 + kern/lig + advance 快取 + 本頁 mini 資料)。
  // 用來回答「現在把字型卸掉,到底騰得出多少」——圖片解碼前的 relief 若釋放量太小,
  // 只是白付一次 ~868ms 重載。粗估值,不含 overflow ring(數 KB)與配置器標頭。
  size_t residentBytes() const;

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

    // Mini EpdFontData built during prewarm
    EpdFontData miniData{};
    uint32_t miniBitmapBytes = 0;  // v55:本頁 miniBitmap 實配大小(residentBytes 用)
    // v55:miniGlyphs / miniIntervals 的【實配】筆數。降級會縮小 miniGlyphCount 但不重配陣列,
    // 計帳若用縮小後的數字會低報常駐量——而低報的後果是該卸字型時沒卸(圖片掉圖)。
    uint32_t miniAllocCount = 0;
    // v59 量測:上一次繪製 prewarm 過的碼位(遞增)。**必須另外存**——v58 原本想省掉這塊,
    // 直接在 freeStyleMiniData() 之前拿舊的 miniIntervals 來比對,但 PrewarmScope 的解構子
    // 每次 render 結束都會 clearCache() → freeStyleMiniData(),所以下一頁看到的恆是空表,
    // 量出來永遠 0/N(diag6.log 的 cum_reuse=0/5151 就是這樣來的,不是中文真的零重疊)。
    // 一次配滿 PREV_CP_CAP 後就地重用、永不成長搬遷(硬限制 6 的教訓)。
    uint32_t* prevCodepoints = nullptr;
    uint16_t prevCodepointCount = 0;
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;

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

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};
  // v55:整段閱讀期間持有的 .cpfont 檔柄。glyph miss 原本每次都 Storage.openFileForRead,
  // 而 SdFat 的 exists() 本身就是一次完整 open → 每個字要把路徑從根目錄線性掃兩遍,
  // 實測 12-18ms/字;乘上灰階 15 趟重繪就是那次 25 秒翻頁。共用檔柄後只剩 seek+read(~3ms)。
  // 生命週期:惰性開啟,freeAll()(含 unloadForLowMemory)時關閉。
  HalFile sharedFile_;
  bool ensureFileOpen();
  // ⚠️ 不變量(v57 擴大使用範圍後要特別留意):prewarmStyle 的 metadata 與 bitmap 讀取迴圈
  // 用「這次的索引是不是上次+1」來跳過多餘 seek,也就是【依賴共用檔柄的位置連續性】。
  // 任何新的共用檔柄使用者若會在那兩個迴圈【中間】seek,就會讀到錯的位元組。
  // 目前的使用者都安全:onGlyphMiss 只在 render 期間(prewarm 已結束)被呼叫;
  // buildMiniKernMatrix 由 prewarmStyle 在其自身讀取迴圈【結束後】才呼叫。
  // fetchAdvancesForCodepoints 刻意仍用自己的區域檔柄(它跑在排版階段,與 render 的交錯
  // 不易靜態證明)。要再加使用者前,先確認它不會插進上述迴圈中間。

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // Shared on-demand overflow buffer (ring buffer of glyphs loaded via glyphMissHandler)
  // v55:8 → 32。一頁中文有 60-110 個不重複字,8 格等於命中率實質為 0——每個字每一趟
  // 灰階重繪都要重新從 SD 讀。32 格(每格 bitmap 動態配置 ~400B,上限約 13KB)在
  // 「部分快取」情境下能真正接住。
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
  // Built by buildAdvanceTable(), queried by getAdvance().
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  // Per-style advance table. Sorted by codepoint for binary lookup.
  // Bounded to ADVANCE_CACHE_LIMIT entries; persists across layout passes
  // (across calls to clearCache()) so repeated indexing of the same font
  // amortizes SD reads. Cleared only on font unload or clearPersistentCache().
  // v55 起【一次就配滿這個上限】,不再隨用量成長。取捨:純英文書等用不到幾百個碼位的內容,
  // 等於每個字面多佔 768×8 = 6,144B 的長壽區塊(兩個字面 12,288B)。這是刻意付的成本——
  // 成長搬遷會在 p2 中間留下永久空洞,把最大連續塊從 115,616 砍到 42,312(見 CLAUDE.md 硬限制 6),
  // 而固定大小的區塊配一次就不再移動。12KB 換掉 73KB 的連續塊損失,划算。
  static constexpr uint32_t ADVANCE_CACHE_LIMIT = 768;
  AdvanceEntry* advanceTable_[MAX_STYLES] = {};
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  bool advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const;
  // Merge sortedNew (sorted by codepoint, no overlap with existing) into the
  // advance table for styleIdx, preserving sort order; cap-truncates the tail.
  void mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount);

  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  // 降級配置時保留給 TLSF 標頭/對齊與其他小配置的邊際。整塊 miniBitmap 要的是
  // 「當下最大連續區塊」,貼著上限要必失敗,所以先退 4KB 再談預算。
  static constexpr uint32_t BITMAP_BUDGET_MARGIN = 4096;
  // v59:上一頁碼位清單的固定容量。實測每頁每字面最多 98 個字,192 留兩倍餘裕;
  // 192 × 4B = 768B/字面(實際只有 present 的字面會配,通常是正體+粗體兩個)。
  static constexpr uint16_t PREV_CP_CAP = 192;
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
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly,
                   bool (*shouldAbort)(void*) = nullptr, void* abortCtx = nullptr);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
};
