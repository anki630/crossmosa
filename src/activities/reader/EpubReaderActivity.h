#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <WarmIdentity.h>

#include <optional>

#include "BookmarkEntry.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  unsigned diagPageCounter = 0;      // v53 量測:render 次數(每 10 次寫一筆 diag;非純翻頁計數)
  unsigned diagMaxRenderMs = 0;      // running max:最慢的一次繪製
  unsigned diagMaxBitmapAlloc = 0;   // running max:單一 style 的 miniBitmap 單塊配置峰值
  unsigned diagAllocFailTotal = 0;   // 累計:撞到連續區塊天花板的次數(stats 每次 render 歸零,不累加就抓不到)
  unsigned diagDroppedTotal = 0;     // v55 累計:因配不下而被踢去 overflow ring 的字數(>0 = 曾進降級模式)
  unsigned diagDroppedReported = 0;  // 已傾印過池佈局的降級數,避免同一次降級每頁重印
  uint8_t diagPoolDumps = 0;         // 池佈局傾印次數上限(見 render 內註解)
  // v58:跨頁字型快取的可行性量測。每次 render 都累加(stats 每頁被歸零),
  // 所以比值涵蓋【每一頁】而不只是每 10 頁取樣的那一頁。
  unsigned diagReuseHits = 0;
  unsigned diagReuseTotal = 0;
  // v110 字型預取。warmBookHash 在 onEnter 算一次(書在開啟期間不會換路徑)。
  // diagWarmHit 是【本次 render】的命中旗標(每次 renderContents 覆寫);cum 兩個是累計,
  // 因為 PAGE/SEG 只每 10 次取樣一次,只看當下那一頁會嚴重低估/高估命中率。
  // diagPrefetchMs 由 prefetchNextPage 寫入(成功=該趟毫秒數;被閘/中止/失敗=0)。
  // ⚠️ 判讀注意:SEG 在預取【之前】發,所以第 n 次 render 的 pf= 講的是第 n-1 次
  // render 結尾那趟預取(差一次 render),且 SEG 每 10 次才取樣一次。
  uint32_t warmBookHash = 0;
  uint8_t diagWarmHit = 0;
  uint32_t diagWarmCumHits = 0;
  uint32_t diagWarmCumTotal = 0;
  // v110 最終複審 Finding D:wcum 的分母只數【翻頁】,不數同頁重繪。
  // 同頁重繪在結構上是有偏的:選單關閉必定 miss(預取已經把快取換成 N+1 了)、
  // 書籤彈窗關閉必定 hit(預取被彈窗閘門擋掉),兩個方向都會污染 spec §7
  // 「四分之三的翻頁 prewarm≈0」那個判準。上一次計數的位置:
  int32_t lastWarmSpine = -1;
  int32_t lastWarmPage = -1;
  unsigned diagPrefetchMs = 0;
  // v54:renderContents 內已算好的七段耗時(prewarm/bw/display/lsb/msb/gray_display/cleanup)。
  // 原本只走 LOG_DBG,在 gh_release 編譯期被移除 → 改存進成員讓 DiagLog 帶出去。
  // path:0=無灰階(AA 關且無圖) 1=tiled 灰階 2=fallback 3=scratch 配置失敗
  // ——沒有這欄就分不出「灰階四段是 0」與「根本沒跑灰階」。
  struct DiagSegments {
    unsigned prewarm, bwRender, display, grayLsb, grayMsb, grayDisplay, cleanup;
    unsigned path, hasImg, decodeImg;
    unsigned stripRows;  // v56:本頁灰階實際用的帶高(80 = 基本;176/264 = 有圖時放大成功)
  };
  DiagSegments diagSeg{};
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with IDENTICAL layout parameters to the pages already rendered (a mismatch
  // would paginate differently than the partial being extended). 0 = no render yet.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() doesn't retry (and log) every
  // tick; the blocking extension in render() remains the fallback past the watermark.
  bool partialRebuildStartFailed = false;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  // pageNo = render() 在 loadPage 之前捕捉的頁碼。刻意不在這裡重讀 section->currentPage:
  // 主任務的手動/傾斜翻頁不持鎖就改它,而 loadPage 中間隔著數十毫秒的 SD I/O
  //(v110 最終複審 Finding C)。
  void renderContents(std::unique_ptr<Page> page, int pageNo, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  // v110:組出「即將繪製的這一頁」的身分。欄位來源與 Section 檔頭那 11 個排版參數
  // (renderContents 上游 loadSectionFile/createSectionFile/startBuild 的引數列)逐一對齊
  // ——同一份設定值走同一條路徑取得,才不會出現「檔頭認為要重排、身分卻認為相符」的裂縫。
  // viewport 取自 buildViewportWidth/Height(render() 每次先更新),方向/邊距/狀態列/
  // 自動翻頁指示的影響全部已折入那兩個數字。
  WarmIdentity buildWarmIdentity(int pageNumber) const;
  // v110:render() 最末尾的下一頁字型預取。仍持 RenderLock、畫面已經送上面板,
  // 用的是「使用者停在這一頁」的那段時間(v109 量的 dwell_ms)。中止即乾淨放棄。
  void prefetchNextPage(int fontId, int marginTop, int marginLeft);
  // 預取的中止述詞。函式指標 + ctx(專案禁 std::function);它跑在 prewarm 的輪詢
  // 路徑上(每 8 個字一次),所以不得做 I/O、不得寫 DiagLog。
  static bool prefetchShouldAbort(void* ctx);
  void renderStatusBar() const;
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Bounded so a
  // background build chunk never noticeably delays input or a pending render (a chunk of N
  // pages is an uninterruptible ~N*30ms, kept well under the ~1s e-ink turn). The per-tick
  // value was raised (v8) to refill the look-ahead window faster on koboSpan-heavy CJK
  // chapters, whose per-page layout runs slower than the ~30ms the small default assumed.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 4;
  // How many pages to keep laid out ahead of the reader for a still-building section. The
  // background build stops once the watermark is this far ahead and resumes as the reader
  // advances; building unbounded instead locked up input by monopolizing the RenderLock, so
  // this stays bounded -- but pages stream to the .bin cache (not RAM), so a wider window costs
  // no heap. Raised (v8) from a tiny buffer: the old value assumed ~30ms/page so the reader
  // "can't out-click the builder", which breaks on koboSpan-heavy CJK chapters (slower layout)
  // and on jumps/fast turns -- too small a window let the reader cross the build front and eat a
  // synchronous buildSomeMore on the turn path (the "later pages lag" symptom). A giant
  // single-spine book still never finalizes its .bin in one sitting -- instant reopen comes from
  // Section::suspendBuild() persisting the pages already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 24;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark. Raised (v8): on a giant koboSpan CJK
  // chapter the from-0 re-layout is much slower than the ~100-300ms/page assumed, so 15 pages of
  // runway let the reader cross the watermark before the rebuild caught up -- forcing a
  // multi-second synchronous buildSomeMore on the render path (the main "resume then lag"
  // culprit). A wider margin starts the rebuild earlier so it finalizes before the reader
  // arrives; the cost is only some speculative CPU/SD if the reader never crosses the watermark.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 48;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void restartToReflow();
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed + fast loop ticks while a section build runs: at the low-power
  // frequency a giant chapter's background rebuild stretches from ~40s to many
  // minutes, so the reader exits before it can finalize and the next open restarts
  // it from page 0. Reverts to normal power behavior the moment the build finishes.
  bool skipLoopDelay() override { return section && section->isBuilding(); }
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
