#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"
#include "ReaderRenderSpec.h"

class Page;
class GfxRenderer;
class ChapterHtmlSlimParser;
class CssParser;

class Section {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  HalFile file;

  void writeSectionFileHeader(const ReaderRenderSpec& spec);
  uint32_t onPageComplete(std::unique_ptr<Page> page);

  // Page-offset table entry, kept in RAM while an incremental build is running so
  // already-built pages can be located in the partially-written .bin.
  struct PageLutEntry {
    uint32_t fileOffset;
    uint16_t paragraphIndex;
    uint16_t listItemIndex;
    uint32_t visibleTextOffset;
  };
  // Held only while an incremental build is in progress (see startBuild). Carries the
  // live parser plus the strings it references (the parser stores them by reference)
  // and the in-RAM page-offset table.
  struct BuildContext {
    std::unique_ptr<ChapterHtmlSlimParser> parser;
    std::vector<PageLutEntry> lut;
    std::string parsePath;
    std::string contentBase;
    std::string imageBasePath;
    std::string htmlPath;
    std::string tmpHtmlPath;
    bool reusedHtml = false;
    CssParser* cssParser = nullptr;
    // v187：這次建置的 CSS 載入狀態（見 CSS_STATE_*），commit 時補進檔頭。
    uint8_t cssState = 0;
    // HTML byte progress, for estimating the section's total page count while it's still building.
    uint32_t bytesConsumed = 0;
    uint32_t totalBytes = 0;
    // Exponentially-smoothed page-count estimate (0 = not yet seeded) and the bytesConsumed at its
    // last update. The raw byte-ratio estimate jitters as the build crosses dense/sparse regions;
    // the EMA is stepped once per build advance (not per redraw) to damp that wobble.
    float smoothedEstimate = 0;
    uint32_t smoothedAtConsumed = 0;
  };
  std::unique_ptr<BuildContext> build_;
  bool buildComplete_ = false;
  bool lastBuildWasLowMemory_ = false;  // v149，見 lastBuildWasLowMemory()
  // v187：loadSectionFile 因「CSS 截斷版面」丟掉快取後設為 true；接下來的重建若又截斷，就寫
  // CSS_STATE_TRUNCATED_FINAL 進檔頭，之後不再為此重排（收斂：每章最多多排一次）。
  bool cssRetry_ = false;
  // v187：最近一次 loadSectionFile 丟掉快取的原因（0 無／1 版號／2 參數／3 CSS 截斷重排／4 partial 尾段壞）。
  uint8_t lastLoadReject_ = 0;
  mutable bool lastLoadWasLowMemory_ = false;  // v152，見 lastLoadWasLowMemory()
  // Pages laid out by the active build (== build_->lut.size()). Distinct from pageCount,
  // which is the pages *available to read* and also counts a loaded partial file's pages.
  uint16_t builtPageCount_ = 0;
  // A partial section file (suspended build from a previous session) is loaded at filePath.
  // Its pages 0..partialPageCount_-1 are readable while a rebuild extends past them.
  bool partial_ = false;
  uint16_t partialPageCount_ = 0;
  // Parse watermark from the partial's trailer, for estimating the total page count.
  uint32_t partialBytesConsumed_ = 0;
  uint32_t partialTotalBytes_ = 0;
  bool finalizeBuild();
  // Write the LUTs/anchor map (and, for a partial, the watermark trailer), patch the
  // header, stamp the version byte, and swap the tmp .bin over filePath.
  bool commitBuildFile(uint8_t version, uint32_t bytesConsumed, uint32_t totalBytes);
  // Builds write here and are swapped over filePath only on commit, so a prior
  // partial/finalized file stays readable while a rebuild is in progress.
  std::string binTmpPath() const { return filePath + ".part"; }
  std::unique_ptr<Page> loadPageAt(int page) const;
  // Read a page already laid out by the in-progress build (page < build LUT size), from
  // the partially-written tmp .bin without disturbing the build's write cursor.
  std::unique_ptr<Page> loadPageDuringBuild(int page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  // Constructor and destructor are out-of-line: BuildContext holds a unique_ptr to the
  // forward-declared ChapterHtmlSlimParser, whose full definition is only visible in the .cpp.
  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer);
  ~Section();
  bool loadSectionFile(const ReaderRenderSpec& spec);
  bool clearCache() const;
  bool createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn = nullptr);

  // Incremental build: lay out the section a few pages at a time so a large chapter
  // can show its first page immediately and keep the UI responsive while the rest
  // builds. createSectionFile() above is the one-shot wrapper over these.
  //   if (!startBuild(...)) fail;
  //   each tick: buildSomeMore(N); render up to pageCount; when isBuildComplete() stop.
  bool startBuild(const ReaderRenderSpec& spec, const std::function<void()>& popupFn = nullptr);
  // Lay out up to maxPages more pages (maxPages <= 0 = build to completion). Returns
  // false on error (the build is abandoned). Sets isBuildComplete() when finished.
  // v189：shouldYield 在每個 parseStep（1KB 一步）之間被問一次；回 true 就立刻回傳 true
  // （可能一頁都沒排完，這是正常的：背景 tick 下一圈接著做）。背景建置用它看原始按鍵電平，
  // 讓持 RenderLock 的盲區縮到一個 parseStep。nullptr ＝ 不讓路（同步站點）。
  bool buildSomeMore(int maxPages, bool (*shouldYield)(void*) = nullptr, void* yieldCtx = nullptr);

  /// v149：最近一次建置失敗是否為【低記憶體中止】（sticky，startBuild 時清除）。
  /// 呼叫端據此分流：低記憶體 -> 不要 reset、不要立刻重建（那是無退避的正回饋迴圈：
  /// 重建從章首跑到同一個長段落、再撞同一次 OOM）；真 parse error 才走原本的錯誤路徑。
  bool lastBuildWasLowMemory() const { return lastBuildWasLowMemory_; }
  uint8_t lastLoadReject() const { return lastLoadReject_; }
  // v194：src 讀走後歸零。-1＝沒有待寫的 SECTPOISON 行。
  static int lastPoisonAvoidedSpine;

  /// v152：最近一次 loadPage 回 nullptr 是否因為【低記憶體地板】。
  /// 呼叫端據此分流：低記憶體 -> 不要 clearCache／abandonBuild（那會在記憶體最緊的
  /// 時刻強迫全量重建）；跳過本輪 render、下一輪重試 —— pxc slot 在 render 結束釋放，
  /// 下一輪幾乎必然成功。真的讀檔損壞才走原本的清快取重建。
  bool lastLoadWasLowMemory() const { return lastLoadWasLowMemory_; }
  bool isBuilding() const { return static_cast<bool>(build_); }
  bool isBuildComplete() const { return buildComplete_; }
  // v189：這次建置已排出的頁數（partial 延伸時 pageCount 釘在 watermark，這個才是進度）。
  uint16_t builtPageCount() const { return builtPageCount_; }
  // v189 儀器（B-22：盲區要量不要猜）：單一 parseStep 的最長／累計耗時與步數。
  // lib 不能反向依賴 DiagLog，所以 lib 記、活動讀（與 Page::footnoteDrops 同型）。由活動歸零。
  static uint32_t buildStepMaxMs;   // 最長一步（含收尾那一步），ms
  static uint32_t buildStepTotalUs;  // 累計，µs（millis 解析度會把 <1ms 的步算成 0）
  static uint32_t buildStepCount;
  // Best-known total page count: the exact pageCount once finalized, or a smoothed byte-based
  // estimate (pages so far scaled by totalBytes/bytesConsumed, damped by an EMA) while a giant spine
  // is still building, so "page X of Y" / progress don't read off the small build watermark.
  uint16_t estimatedTotalPages() const;
  void abandonBuild();
  // Persist an in-progress build as a partial section file (version sentinel + LUTs +
  // watermark trailer) instead of discarding it, so the next open of this spine can show
  // its pages instantly and only rebuild in the background. Called by the destructor, so
  // any teardown path (exit, sleep, navigation) keeps the work already done. Keeps a
  // pre-existing partial when it covers more pages than this build reached.
  void suspendBuild();
  // True when a partial file was loaded: pageCount is a watermark, not the chapter total.
  bool isPartial() const { return partial_; }

  // Unified page read: from the active build if it has reached the page, otherwise from
  // the on-disk file (finalized section, or a partial the rebuild hasn't caught up to).
  std::unique_ptr<Page> loadPage(int page);

  std::string getTextFromSectionFile();

  // Resolve an anchor from the in-progress build first, then the on-disk anchor map
  // (covers finalized sections and partials from a previous session).
  std::optional<uint16_t> findAnchor(const std::string& anchor) const;

  // True if this spine's unzipped HTML is already cached, so a build won't pay the (multi-second on a
  // giant spine) zip inflation. Lets the reader skip the indexing popup on a fast reopen/rebuild.
  bool hasHtmlCache() const;

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up an anchor among the pages built so far by the in-progress build, so an anchor jump
  // (TOC / chapter select, usually the chapter top = page 0) can resolve without laying out the
  // whole chapter. Returns nullopt if the anchor hasn't been reached yet (build more) or no build.
  std::optional<uint16_t> findAnchorDuringBuild(const std::string& anchor) const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // Exact zero-based visible Unicode-codepoint offset where a rendered page
  // starts. Available from both an active build and finalized/partial caches.
  std::optional<uint32_t> getVisibleTextOffsetForPage(uint16_t page) const;

  // Derive the local page containing an exact visible-text offset. When
  // preferFirstAtOffset is true, ties caused by zero-width content such as an
  // image-only page select the first page at that offset.
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t offset, bool preferFirstAtOffset = false) const;

  // True once the active build has laid out a page starting at or past `offset`, i.e.
  // getPageForVisibleTextOffset() can resolve it from the build without laying out more.
  // Lets a caller build to a content target instead of a page number, which is what a
  // re-pagination needs: the old page index no longer names the same content.
  // False with no build running -- there is nothing left to wait for, so the on-disk
  // cache answers directly.
  bool buildReachedVisibleTextOffset(uint32_t offset) const {
    return build_ && !build_->lut.empty() && offset <= build_->lut.back().visibleTextOffset;
  }
};
