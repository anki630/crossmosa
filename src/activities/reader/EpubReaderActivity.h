#pragma once
#include <WarmIdentity.h>
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

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
  // Image pages use a dedicated double-FAST refresh path, so retain a manual
  // refresh request until renderContents can issue its clean base pass.
  bool forcedRefreshPending = false;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  // Visible-codepoint offset of the page currently on screen, captured when the page is loaded
  // (Page::visibleTextOffset). Lets saveProgress persist the offset without reopening section.bin.
  std::optional<uint32_t> currentPageVisibleOffset;
  // Explicit "land at this visible-codepoint offset in the target spine" request (bookmark open).
  // Resolved in render() once the section is loaded/built far enough, then cleared. Unlike a
  // settings-change reposition it always resolves by content, so it survives any re-pagination.
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  // "No dictionary set" popup, shown when a lookup is triggered without a configured dictionary.
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool ignoreNextConfirmRelease = false;
  bool currentPageBookmarked = false;
  // Idle-time glyph prewarm: after a page settles, scan the LIKELY next page
  // (scan mode draws nothing) and load its missing glyphs from SD during idle,
  // so the next turn's in-render prewarm is a cache hit instead of ~100 ms of
  // SD reads on the page-turn critical path. One attempt per position.
  unsigned long lastRenderCompleteMs = 0;

  // v110/v164 字型預取。warmBookHash 在 onEnter 算一次（書在開啟期間不會換路徑）。
  // diagWarmHit 是【本次 render】的命中旗標（每次 renderContents 覆寫）；cum 兩個是累計，
  // 只數翻頁不數重繪（lastWarmSpine/Page 去重）。
  // diagPrefetchMs 由 prefetchNextPage 寫入（成功=該趟毫秒數；被閘/中止/失敗=0）。
  uint32_t warmBookHash = 0;
  uint8_t diagWarmHit = 0;
  uint32_t diagWarmCumHits = 0;
  uint32_t diagWarmCumTotal = 0;
  int32_t lastWarmSpine = -1;
  int32_t lastWarmPage = -1;
  unsigned diagPrefetchMs = 0;
  // v174：預取閘門診斷。pg= 是【最近一次】prefetchNextPage 的出口碼：0=完成、1=重排中、2=章末、
  // 3=無 fcm、4=內建字型、5=書籤彈窗、6=按鍵排隊、7=堆積地板（清掉本頁快取之後量）、
  // 8=loadPage 失敗、9=中止。pmax= 是最大連續塊 KB：走到 pg=7 判斷點時改記【清快取之後】地板真正
  // 拿來判斷的值（v189），被閘 1–6 擋下時仍是進場值；pret= 是保留中的 mini 容量 KB（只在到達判斷點時
  // 才量，pg=1–6 時為 0＝「沒量」不是「沒保留」）。純診斷，不改行為。
  uint8_t diagPfGate = 0;
  uint16_t diagPfMaxKb = 0;
  uint16_t diagPfRetKb = 0;  // v189：預取當下保留中的 mini bitmap 容量（KB），pg=7 判讀用
  uint8_t pendingCacheReset_ = 0;  // v184：清快取詢問的答案（來自 MenuResult）
  uint16_t lastCssLoadSeq = 0;  // v177：每次載入印一行 CSSLOAD（序號變動）
  // v177（使用者提議）：預取的基準頁與目標頁。按鍵把 currentPage 推到目標頁＝順向翻頁 → 不中止，
  // 讓預取跑完，緊接著的 render 就是命中。其他變動（往回、跳頁、連按）才中止。
  int lastRenderedPage_ = -1;  // renderContents 剛畫完的頁碼（render 尾端預取的基準）
  int prefetchBase_ = -1;
  int prefetchTarget_ = -1;
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

  // v189：abortOnInput —— 從背景 tick（主任務、持鎖）呼叫時為 true：那裡沒有人在輪詢按鍵，
  // 預取的 ~300ms SD 讀取就是盲區，原始電平一有動靜就中止。render 尾端（render task）不開：
  // 主迴圈照常輪詢，而且順向翻頁時這趟預取正是下一頁要的，中止反而慢。
  void prefetchNextPage(int fontId, int marginTop, int marginLeft, int basePage, bool abortOnInput = false);
  static bool prefetchShouldAbort(void* ctx);
  static bool prefetchShouldAbortOrInput(void* ctx);
  WarmIdentity buildWarmIdentity(int pageNumber) const;
  void renderContents(std::unique_ptr<Page> page, int pageNo, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  // v189：4 → 1。排到底（見 BUILD_AHEAD_CAP）之後 tick 是背對背連續跑的，每 tick 幾頁只影響
  // 【延遲粒度】不影響完成時間。實測這台每頁排版 100–200ms（中文＋SD 字型；上游註解假設的 30ms
  // 是拉丁），4 頁＝持 RenderLock 0.4–0.8 秒，而按鍵是主任務輪詢邊緣：整個落在 tick 裡的短按
  // 會【完全看不到】。1 頁＋parseStep 之間的 shouldYield（原始按鍵電平＋去彈跳中）把盲區壓到
  // 一個 parseStep。⚠️ 一個 parseStep 不是「1KB」：expat 的 callback 裡會排完整個段落（可能好幾頁）、
  // 序列化到 SD、圖片探頭，都不能中途讓路——盲區的真實上界是 `BUILD end … stepmax=`，用量的不用猜
  // （複查 B-22）。stepmax 若常態 >60ms，下一步是在 layoutAndExtractLines 的行與行之間加讓路點。
  // v176 那條「多 2 頁少被閘幾頁」的理由已不存在：v189 的閘是 buildTickDue()，不是 isBuilding()。
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 1;

  // MEMFIX-PORT: background-build heap floor; portable
  // Skip background build ticks below this free-heap floor. The parse path grows
  // word vectors of heap strings — throwing allocations that abort() on OOM under
  // -fno-exceptions (field crash: bad_alloc in ParsedText::addWord during a
  // background tick under heap pressure). The tick is deferrable work:
  // page-turn transients free up between turns and the build resumes; the render
  // path still builds the page it actually needs regardless of this floor.
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  // Fragmentation floor for the same gate: a tick passed the free-heap floor at
  // 34.7 KB free but the largest block was ~11 KB, and a parse allocation inside the
  // tick aborted anyway. Free heap says how much memory exists; maxAlloc says whether
  // any single allocation can actually have it. 16 KB also keeps the advance-table
  // batch path (16 KB scratch) viable during builds.
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  // Gate for a background build tick: true when the heap can take parse allocations.
  // Updates buildHeapPaused as a side effect.
  bool buildTickHeapGate();
  // True while the background build is gated on the heap floors. Lets skipLoopDelay()
  // return the loop to normal delay/power-saving during the pause: isBuilding() stays
  // true the whole time, and without this the loop would spin at full CPU speed doing
  // no build work — indefinitely, if the build context itself keeps the heap low.
  bool buildHeapPaused = false;
  // Heap floor for optional render-adjacent work (idle prewarm). Page
  // deserialization (TextBlock word vectors/strings) and glyph caching allocate
  // through throwing paths that abort() on OOM; skip deferrable work below it.
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  // v189：背景建置改成【排到底】，這個值只是巨型單章的安全上限，不再是「跟著讀者走」的視窗。
  //
  // 上游的視窗設計（24 頁，v173 從 v130 搬回）假設每頁 30ms、「建置無界會獨佔 RenderLock」。
  // 這台實測每頁 100–200ms（中文＋SD 字型），而視窗的真正後果是：只要章節比視窗長，
  // 建置 context 就整章活著、isBuilding() 恆真 → 預取整章被閘（diag-prev188：一章 152 頁，
  // 前 127 頁 pg=1、每頁 ~950ms、warm=0；建置在 currentPage+24 ≥ 152 即第 128 頁才收尾，
  // 預測與實測差 2 頁內）。v130 的「57% warm」是那本書的章節多半短於 24 頁的產物。
  // 另一個後果：skipLoopDelay() 整章為真 → 主迴圈整章不省電，即使 4 次翻頁只有 1 次有 tick。
  //
  // 改法：tick 連續跑到 finalize（正常章節 20–30 秒的爆發期，之後 context 釋放、整章預取暢通、
  // 迴圈回到省電）；只有這次建置排出的頁數領先讀者超過這個上限時才閒置（1000 頁的單章＝最多
  // 20–50 秒後停下，不會分鐘級寫 SD；partial 延伸也受同一個上限，見 buildTickDue）。
  // 閒置時 isBuilding() 仍真，所以預取與 skipLoopDelay 都改看 buildTickDue()，不看 isBuilding()。
  // 建置壓力仍由 buildTickHeapGate（32KB／16KB 地板）＋v163 CSS 地板／v167 三閘／v169 AA 降級守著；
  // 按鍵由 1 頁／tick ＋ parseStep 之間的原始按鍵讓路守著（BACKGROUND_BUILD_PAGES_PER_TICK）。
  static constexpr int BUILD_AHEAD_CAP = 256;
  // v189：背景建置「這一圈該不該跑」。三個使用者：loop() 的 tick、預取的第一道閘（原本看
  // isBuilding()，那會把閒置在上限的巨型章也擋掉）、skipLoopDelay()（閒置就回省電）。
  // 不含 RenderLock／堆積地板／按鍵邊緣那些「這一圈剛好不能跑」的條件——那些不是「該不該」。
  // 進度看 builtPageCount()（這次建置排出的頁）而不是 pageCount：partial 延伸時 pageCount 釘在
  // watermark，看它會誤判「領先夠多」而停在 0 頁（上游原本用 isPartial() 整個繞過上限——那讓
  // 巨型章的 partial 延伸無界跑到底；改看 built 之後兩種建置都受同一個上限）。
  bool buildTickDue() const {
    return section && section->isBuilding() &&
           static_cast<int>(section->builtPageCount()) < section->currentPage + BUILD_AHEAD_CAP;
  }
  // v189（第二輪驗證）：預取的閘要有遲滯。巨型章閒置在上限時每翻一頁 built 就落後 1 頁 → tick 又「該跑」
  // → render 尾端的預取被 pg=1 擋掉（tick 在 render 之後才補那一頁）。只有落後超過這個餘裕才算「爆發期」。
  static constexpr int BUILD_PREFETCH_SLACK = 8;
  bool buildBurstActive() const {
    return section && section->isBuilding() &&
           static_cast<int>(section->builtPageCount()) < section->currentPage + BUILD_AHEAD_CAP - BUILD_PREFETCH_SLACK;
  }
  // v189：Section::buildSomeMore 在 parseStep 之間問「要不要讓路」。看的是 HalGPIO::inputActive()
  // （原始電平或去彈跳未收斂；不碰邊緣狀態）：按鍵在主任務輪詢，tick 持鎖期間主迴圈讀不到
  // update()，只有這樣才能在使用者按下去的那一個 parseStep 內把鎖還出去。
  static bool buildShouldYield(void* ctx);
  // v189 儀器（複查後重做：原本只在 tick 分支印、sync 收尾與 section.reset 都看不到，計數還會漏到
  // 下一章）。三個 startBuild 站點呼叫 noteBuildStart；結束由 tick 自己（done/lowmem/failed）或
  // loop() 開頭的轉態偵測（sync 收尾、reset）印 `BUILD end`。
  //   ticks   = 有進度的 tick（排出 ≥1 頁或收尾）   zero = 一頁都沒排就回來的 tick（讓路）
  //   yields  = 讓路【段落】數（同一次按住只算一次）  tickmax = 單一 tick 持鎖最久
  //   stepmax/steps = Section 記的單步最長／步數（＝按鍵盲區的真實上界）
  void noteBuildStart();
  void emitBuildEnd(const char* why);
  // v190：建置結束後重繪被記住失敗擋住的圖。每次 render 覆寫，-1＝沒有待癒合。
  int imageHealSpine_ = -1;
  int imageHealPage_ = -1;
  uint32_t imageHealSeen_ = 0;
  // v193：圖片頁延後解碼。deferredDecode* 每次 render 覆寫，-1＝沒有待補圖。
  // lastDeferredKey_＝「這一頁已經延後過一次」的身分，同一頁只延後一次，否則兩次 render 互鎖。
  int deferredDecodeSpine_ = -1;
  int deferredDecodePage_ = -1;
  struct LastDeferredKey {
    int spine = -1;
    int page = -1;
  } lastDeferredKey_;
  int lastAdvanceSpine_ = -1;  // v193：上次清空 advance 表的章；同章重建（方向／設定）不准清
  bool diagBuildActive = false;
  bool diagYieldRun = false;
  int diagBuildSpine = -1;  // 建置開始時的章（章切換後 currentSpineIndex 已是新章）
  uint16_t diagBuildPagesBuilt = 0;  // tick 最後看到的 builtPageCount（section 已換掉或已 null 時 pages= 用它）
  // v189：設定變更的延遲重定位只該對「落地那一頁」生效。landingPending_ 在 section 建立時設起，
  // 落地頁定案後蓋成 deferredLandingPage_（第二輪驗證：原本每次 render 都蓋，比對永遠相等＝形同虛設）。
  bool landingPending_ = false;
  unsigned long diagBuildStartMs = 0;
  uint32_t diagBuildTicks = 0;
  uint32_t diagBuildZeroTicks = 0;
  uint32_t diagBuildYields = 0;
  uint32_t diagBuildTickMaxMs = 0;
  // v189（複查 state-major）：設定變更後 render 落地時解析到的頁。applyDeferredReposition 在背景建置收尾
  // 才跑，排到底之後那是 20–30 秒後——讀者若已翻頁，拿舊 offset 重定位會把人拉回去。落地時 offset
  // 已解析成功就當場消耗快取；這個欄位是第二道保險：收尾時頁已不同就不再重定位。
  int deferredLandingPage_ = -1;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Deadline backstop for the predictive gates above: if the blocking build-to-target still
  // hasn't produced the landing page this long after the build started, surface the popup
  // mid-build. Builds that finish under the deadline stay popup-free.
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  // True only during onEnter's blocking build-to-target phase, until the popup has been
  // drawn. Gates showBuildPopup() so the parser's popup callback (which persists into
  // background buildSomeMore chunks) can never draw over a displayed page.
  bool buildPopupPending = false;
  // Draw the indexing popup mid-build (parser image-probe callback and deadline backstop).
  void showBuildPopup();
  // Map the cached content position into the rebuilt section (used after a
  // settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  void rememberCurrentContentOffset();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
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
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              int initialRefreshCountdown)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed + fast loop ticks while a section build runs: at the low-power
  // frequency a giant chapter's background rebuild stretches from ~40s to many
  // minutes, so the reader exits before it can finalize and the next open restarts
  // it from page 0. Reverts to normal power behavior the moment the build finishes,
  // and while the build is heap-paused (no work is happening, so spinning at full
  // speed would only burn battery; the paused gate still retries every loop pass).
  // v189：看 buildTickDue() 不看 isBuilding()——閒置在 BUILD_AHEAD_CAP 的建置沒有工作要做。
  // （舊視窗設計下這裡整章為真：4 次翻頁 3 次沒有 tick，CPU 卻整章全速。）
  bool skipLoopDelay() override { return buildTickDue() && !buildHeapPaused; }
  bool isReaderActivity() const override { return true; }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = 1;
      forcedRefreshPending = true;
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
