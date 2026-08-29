#include <SdCardFont.h>
#include <DataDir.h>
#include <Epub/ParsedText.h>
#include "util/BenchFlags.h"
#include "util/DiagLog.h"
#include "EpubReaderActivity.h"

#include <BitmapHelpers.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>

#include "../../util/BookmarkFile.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = std::string(DataDir::path()) + "/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  // v140 量測：這是「閱讀穩態」的基準線。dumpPools 才答得出【誰卡在 p2 中間】——
  // mem() 只說最大連續塊剩多少，而 ESP.getMaxAllocHeap() 是兩池取大者，混著看不出歸屬。
  DiagLog::mem("reader-enter");
  // v143：門檻從 2048 降到 64。
  // v142 之後 p2 的兩塊大常駐物已經穩定（43,008 mini bitmap ＋ 6,400 advance table），
  // 但 `used=52408 / maxfree=26912 / big=2` 這組數字自相矛盾 —— 63,156 可用卻拼不出
  // 超過 26,912 的一塊，代表【中間有小於 2048 的長壽小塊把它切開】，而 2048 的門檻
  // 剛好把兇手濾掉了。CLAUDE.md 記過這個形狀（圖片頁 render 會留一顆約 200 B 的長壽塊）。
  // 上限是 16 筆/池，所以降到 64 不會把 log 灌爆。
  DiagLog::dumpPools(64, "reader-enter");
  // v140 量測：那本 Kadokawa 書有 164KB CSS（style-advance 84,640 + style-standard 61,833），
  // 而新樹的 CSS 解析器【完全沒有】記憶體守衛（舊樹 v7 的 MIN_MAXBLOCK_FOR_CSS 沒搬回來）。
  // v7 那條地板要不要搬、門檻該設多少，全看這裡與 build-start 之間的落差 —— 在此之前不要猜。
  // ⚠️ 提醒：ESP.getMaxAllocHeap() 是 p2/p3 兩池取大者，所以「CSS 吃光 p3 但 p2 還空」時
  //    這個數字【不會掉】—— 判讀時一律看 dumpPools 的逐池數字，不要看單一讀數。
  DiagLog::line("embeddedStyle=%d", static_cast<int>(SETTINGS.embeddedStyle));
  Activity::onEnter();

  if (!epub) {
    return;
  }

  ImageBlock::clearSessionRenderFailures();
  // v31/v41 → v187：粗體閱讀在排版階段烤進去（ParsedText::addWord），並參與 section 檔頭比對。
  ParsedText::setBoldBodyText(SETTINGS.boldBodyText != 0);
  // Lazy image extraction: section builds only header-probe images, so the first
  // render of an image page pulls the file out of the EPUB through this hook.
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  // v190：圖片紓解只放可重建的 mini bitmap 快取（住在 p2，SEG pret= 常態 24–40KB）。
  // 卸載 SdCardFont 常駐表幫不到「p2 缺連續塊」——那些表開機配在 p3，卸了還會在 restore
  // 時切進 p2 中段，淨負值。WiFi／OPDS 那條仍走 unloadForLowMemory，語意不同，不准改。
  // ctx 傳 this 取 renderer；函式指標（非 std::function）維持 lib 不依賴 app 的分層。
  ImageBlock::setMemoryReliefHooks(
      [](void* ctx) {
        auto* self = static_cast<EpubReaderActivity*>(ctx);
        size_t released = 0;
        if (auto* fcm = self->renderer.getFontCacheManager()) {
          released = fcm->releaseRetainedCache();
        }
        // v190：released==0 也要印，否則分不出「沒跑」與「沒東西可放」。
        DiagLog::line("FONTREL img %u max=%u", static_cast<unsigned>(released),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()));
      },
      [](void* /*ctx*/) {
        // v190：relief 不再卸載常駐表，故無需重載；若日後有人恢復卸載，restore 必須一起恢復。
      },
      this);
  {
    // v190：只量 A1 取樣鉤子貴不貴，不讀來改按鍵行為。
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 16; ++i) {
      (void)gpio.inputActive();
    }
    const int64_t elapsed = esp_timer_get_time() - t0;
    DiagLog::line("KEYPROBE adc_us=%ld n=16", static_cast<long>(elapsed / 16));
  }
  // v175：ParsedText 守衛拒絕的【當下】拍池快照（fg-lowmem 的快照在建置脈絡釋放之後，看不到主嫌）。
  ParsedText::setRefusalHook(
      [](void*) {
        DiagLog::mem("ptx-refuse");
        DiagLog::dumpPools(2048, "ptx-refuse");
      },
      nullptr);
  // v191：site 7 必須從 EpdFont 回打，因為分層不准 EpdFont include Epub。
  SdCardFont::setBuildProbeHook([](uint8_t s) { ParsedText::noteBuildProbe(s); });

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        // UINT16_MAX is an in-memory navigation sentinel for "open previous
        // chapter on its last page". It should never be treated as persisted
        // resume state after sleep or reopen.
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  // Trigger first update
  // v110/v164：書的身分雜湊算一次（書在開啟期間不會換路徑）
  warmBookHash = WarmIdentity::fnv1a(epub->getCachePath().c_str());

  requestUpdate();
}

void EpubReaderActivity::onExit() {
  emitBuildEnd("exit");  // v189：離開（含休眠）時建置若還活著，這裡是最後一個能印 BUILD end 的地方
  // v31/v155：離開時把全書進度記進最近閱讀（主畫面續讀卡顯示「作者 (45%)」）。
  // 三情況（照舊樹）：讀完＝100；註腳中＝跳過（當前位置是註腳目標不是閱讀原點）；
  // 否則 章內進度 × spine 佔比。
  if (epub && currentSpineIndex > 0 && currentSpineIndex >= static_cast<int>(epub->getSpineItemsCount())) {
    RECENT_BOOKS.setProgress(epub->getPath(), 100);
  } else if (footnoteDepth == 0 && epub && section && epub->getBookSize() > 0 && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    const int pct = std::max(0, std::min(100, static_cast<int>(bookProgress + 0.5f)));
    RECENT_BOOKS.setProgress(epub->getPath(), static_cast<uint8_t>(pct));
  }

  Activity::onExit();

  // The extractor holds a raw pointer to this activity's epub; drop it before
  // the activity (and the shared_ptr) goes away.
  ImageBlock::setExtractor(nullptr, nullptr);
  ImageBlock::setMemoryReliefHooks(nullptr, nullptr, nullptr);
  ParsedText::setRefusalHook(nullptr, nullptr);
  SdCardFont::setBuildProbeHook(nullptr);  // v191：活動死了還掛著會打進已拆的探針狀態

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  // v175（diag174 定案）：離開時釋放保留中的字型快取（本頁 30–43KB 的 mini）。它原本一直活到
  // 下一次 render 的 PrewarmScope ctor 才清 —— 而主畫面就在那之前：縮圖的 JPEG 解碼要 53KB 總量，
  // 實測離開後只剩 39KB → 每本都 THUMBFAIL（heap 39024<53248）；下一次進書的同步建置也在這
  // 43KB 的陰影下起跑（fg-lowmem #2：p2 只剩 22KB）。
  // v188：v175 在這裡想釋放的是 30–43KB 的 mini 快取（給主畫面縮圖解碼），但 clearCache() 保留容量
  // → 之前其實是 no-op（diag187_2 仍有 THUMBFAIL）。改成真正釋放；字型物件仍載入，不用重載。
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseRetainedCache();
  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                             renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                             SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty()),
                         [this](const ActivityResult& result) {
                           // Always apply orientation change even if the menu was cancelled
                           const auto& menu = std::get<MenuResult>(result.data);
                           pendingCacheReset_ = menu.resetProgress;
                           applyOrientation(menu.orientation);
                           toggleAutoPageTurn(menu.pageTurnOption);
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                           }
                         });
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  // Below the floors: just wait. The tick is deferrable — page-turn transients
  // free up between turns and the tick retries every loop pass. Track the
  // paused state so skipLoopDelay() stops pinning the CPU at full speed while
  // no build work is actually happening (the gate can stay closed for a long
  // stretch if the retained build context itself holds the heap down).
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

// v189：背景建置的讓路判準。看 inputActive()（原始電平或去彈跳未收斂；~數十 µs 的 ADC 讀取），
// 不碰 InputManager 的邊緣狀態，所以 tick 結束後主迴圈的 update() 照常看到這次按下／放開。
// 長按（跳章／書籤）期間持續為真 → tick 每圈零步就回，建置等使用者放手；那是對的。
// yields 算【段落】：同一次按住只記一次（複查：每圈記一次的話一秒長按會灌進幾百）。
bool EpubReaderActivity::buildShouldYield(void* ctx) {
  auto* self = static_cast<EpubReaderActivity*>(ctx);
  if (!gpio.inputActive()) {
    self->diagYieldRun = false;
    return false;
  }
  if (!self->diagYieldRun) {
    self->diagYieldRun = true;
    self->diagBuildYields++;
  }
  return true;
}

void EpubReaderActivity::noteBuildStart() {
  if (diagBuildActive) emitBuildEnd("preempted");  // 前一個建置沒走到任何結束出口就被換掉（章切換）
  diagBuildActive = true;
  diagYieldRun = false;
  diagBuildSpine = currentSpineIndex;
  diagBuildStartMs = millis();
  diagBuildTicks = diagBuildZeroTicks = diagBuildYields = diagBuildTickMaxMs = 0;
  diagBuildPagesBuilt = 0;
  Section::buildStepMaxMs = Section::buildStepTotalUs = Section::buildStepCount = 0;
  ParsedText::buildGapMaxUs = ParsedText::buildGapSite = ParsedText::buildProbeCount = 0;
  SdCardFont::resetAdvanceDiag();  // v192：區間計數器歸零
}

void EpubReaderActivity::emitBuildEnd(const char* why) {
  if (!diagBuildActive) return;
  diagBuildActive = false;
  const uint32_t steps = Section::buildStepCount;
  // pages=：這個建置【收尾】了就是定案頁數；被換掉／null／還在建（exit）／lowmem（suspendBuild 已把
  // pageCount 退回舊 watermark）／failed（abandonBuild 歸零）都用 tick 最後看到的進度。
  const unsigned pages = (section && section->isBuildComplete() && diagBuildSpine == currentSpineIndex)
                             ? section->pageCount
                             : diagBuildPagesBuilt;
  // v192：尾端追加 amiss/asd/areject/aevict。
  DiagLog::line("BUILD end why=%s spine=%d ms=%lu pages=%u ticks=%lu zero=%lu yields=%lu tickmax=%lu stepmax=%lu "
                "stepavg=%lu steps=%lu gapmax=%lu gapsite=%u probes=%lu amiss=%lu asd=%lu areject=%lu aevict=%lu",
                why, diagBuildSpine, static_cast<unsigned long>(millis() - diagBuildStartMs), pages,
                static_cast<unsigned long>(diagBuildTicks), static_cast<unsigned long>(diagBuildZeroTicks),
                static_cast<unsigned long>(diagBuildYields), static_cast<unsigned long>(diagBuildTickMaxMs),
                static_cast<unsigned long>(Section::buildStepMaxMs),
                static_cast<unsigned long>(steps ? (Section::buildStepTotalUs / steps) / 1000 : 0),
                static_cast<unsigned long>(steps),
                static_cast<unsigned long>(ParsedText::buildGapMaxUs / 1000),
                static_cast<unsigned>(ParsedText::buildGapSite),
                static_cast<unsigned long>(ParsedText::buildProbeCount),
                static_cast<unsigned long>(SdCardFont::advanceMissCount_),
                static_cast<unsigned long>(SdCardFont::advanceSdReadCount_),
                static_cast<unsigned long>(SdCardFont::advanceRejectCount_),
                static_cast<unsigned long>(SdCardFont::advanceEvictCount_));

  // v190：只在 done/full 時重繪——其餘 why 不保證記憶體已釋放。
  // done（背景 tick 收尾）與 full（百分比跳頁同步建置）兩站都持 RenderLock；
  // requestUpdate() 只設旗標，真正 render 在鎖釋放後由 render task 取件，不會在鎖內重入。
  if (std::strcmp(why, "done") == 0 || std::strcmp(why, "full") == 0) {
    // v190：一律用 lastRenderedPage_（＝旗標蓋章時用的同一個值）。section->currentPage 是會被
    // 主任務在面板刷新那一秒改掉的易變成員（教訓 24），而這裡要問的是「使用者眼前那一頁」。
    const int curPage = lastRenderedPage_;
    if (imageHealPage_ >= 0 && imageHealPage_ == curPage && imageHealSpine_ == currentSpineIndex) {
      DiagLog::line("IMGHEAL redraw spine=%d page=%d max=%u", imageHealSpine_, imageHealPage_,
                    static_cast<unsigned>(ESP.getMaxAllocHeap()));
      imageHealPage_ = -1;
      requestUpdate();
    } else {
      DiagLog::line("IMGHEAL skip why=%s", imageHealPage_ < 0 ? "nopending" : "moved");
    }
  }
}

void EpubReaderActivity::showBuildPopup() {
  // Mid-build indexing popup: only during onEnter's blocking build-to-target phase
  // (buildPopupPending), at most once, and only when the framebuffer isn't on loan.
  // If it fires while the loan is active (e.g. the parser's size-based call during
  // startBuild), pending stays set and the deadline check retries after the loan.
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts.
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::loop() {
  // v146 儀器：把 lib 端記下的圖片失敗讀走並寫進 diag.log。
  // ⚠️ 必須【每一輪都讀】，不可以放進每 N 頁的取樣區塊 —— lastFailPath 只有一格，
  //    取樣之間發生的失敗會被後來的覆寫（教訓 B-23，v123 踩過：第一章那個小圖就是
  //    這樣消失的）。這裡的成本是一次字元比較。
  // ⚠️ lib/Epub 不能反向依賴 src/util/DiagLog，所以是 lib 記、src 讀（分層規則）。
  if (ImageBlock::lastFailPath[0] != '\0') {
    DiagLog::line("IMGFAIL %s", ImageBlock::lastFailPath);
    ImageBlock::lastFailPath[0] = '\0';
  }
  // v151：ParsedText 守衛的拒絕紀錄 —— v150 的「索引失敗」零證據就是因為只寫 LOG_ERR。
  if (SdCardFont::lastAllocFail[0] != '\0') {
    DiagLog::line("SDCFFAIL %s", SdCardFont::lastAllocFail);
    SdCardFont::lastAllocFail[0] = '\0';
  }
  if (ParsedText::lastRefusal[0] != '\0') {
    DiagLog::line("PTXREFUSE %s", ParsedText::lastRefusal);
    ParsedText::lastRefusal[0] = '\0';
  }
  // v194：nothrow 失敗出口的證人。沒有序列埠＝寫進 diag.log 否則就是丟掉。
  if (Page::lastAllocFail[0] != '\0') {
    DiagLog::line("ALLOCFAIL %s", Page::lastAllocFail);
    Page::lastAllocFail[0] = '\0';
  }
  if (HalStorage::lastAllocFail[0] != '\0') {
    DiagLog::line("ALLOCFAIL %s", HalStorage::lastAllocFail);
    HalStorage::lastAllocFail[0] = '\0';
  }
  if (ditherLastAllocFail[0] != '\0') {
    DiagLog::line("ALLOCFAIL %s", ditherLastAllocFail);
    ditherLastAllocFail[0] = '\0';
  }
  if (Section::lastPoisonAvoidedSpine >= 0) {
    DiagLog::line("SECTPOISON avoided spine=%d", Section::lastPoisonAvoidedSpine);
    Section::lastPoisonAvoidedSpine = -1;
  }
  if (Page::footnoteDrops != 0) {  // v187：記憶體不足或單段超過 32 連結而丟掉的註腳數
    DiagLog::line("FNDROP n=%u", static_cast<unsigned>(Page::footnoteDrops));
    Page::footnoteDrops = 0;
  }
  // v176/v177：按章 CSS 過濾的統計（每次載入印一行；why= 是掃描失敗原因碼，0=成功）。
  if (epub) {
    if (auto* css = epub->getCssParser()) {
      if (css->lastLoadSeq_ != lastCssLoadSeq) {
        lastCssLoadSeq = css->lastLoadSeq_;
        DiagLog::line("CSSLOAD kept=%u/%u classes=%u filtered=%u why=%u trunc=%u",
                      static_cast<unsigned>(css->lastLoadKept_), static_cast<unsigned>(css->lastLoadTotal_),
                      static_cast<unsigned>(css->lastLoadClasses_), css->lastLoadFiltered_ ? 1u : 0u,
                      static_cast<unsigned>(css->lastScanFail_), css->lastLoadTruncated_ ? 1u : 0u);
      }
    }
  }

  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // v164：upstream 的閒置預熱（400ms dwell 後掃下一頁）已由 v110 的 render 尾端預取
  // ＋WarmIdentity 取代並【移除】。兩者不能並存：這裡的 PrewarmScope ctor/dtor 會把
  // 預取剛採用的身分作廢（clearCache ⇒ invalidate），讓下一次翻頁退回冷路徑。
  // 實測（diag163.log，v155-162）它也從未兌現：EPUB prewarm 中位維持 287ms。

  // v189 儀器：建置在 tick 以外的地方結束（render 的同步收尾、section.reset）也要有 BUILD end。
  // render task 持鎖期間不判（它自己會在站點上印 full／full-failed；而 createSectionFile 站在 startBuild 之前
  // 就 noteBuildStart，這裡若不看鎖會搶先印一行 ms≈0 的 sync 把它吞掉——第三輪驗證）。
  if (diagBuildActive && !RenderLock::peek() && (!section || !section->isBuilding())) {
    emitBuildEnd(section ? "sync" : "reset");
  }

  // Lazily resume a partial's extension build once the reader nears its watermark. Far from
  // it the rebuild is all cost (whole-chapter re-layout from page 0) and no benefit this
  // session, so reopening a partial deliberately does NOT start it (see the deferral in
  // render()); crossing this margin is the signal that the reader will actually need pages
  // past the watermark soon. Uses the last render's viewport so pagination matches the
  // partial being extended.
  if (section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 &&
      !partialRebuildStartFailed &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    RenderLock lock;
    // v110/v164（最終複審 Finding B）：startBuild 會把整個 spine 的 HTML inflate 進來
    // （DEFLATE 32KB 回溯視窗），保留中的 mini 快取在這個峰值裡是純負擔 —— 在 inflate
    // 之前釋放。valid 守衛讓後續每一圈免費（清完身分即失效，不再進來）。
    // 不變量（codex 挑戰後固定）：「保留中且佔記憶體的快取 ⟺ 身分 valid」——
    // unloadAll 走的是刪整個字型物件（mini 隨之釋放）＋invalidate，不存在
    // 「invalid 但仍佔記憶體」的殘留；改動 FCM/unloadAll 語意時要重新檢這條。
    if (auto* fcm = renderer.getFontCacheManager()) {
      // v188：clearCache() 保留 mini 容量（防碎片化），但建置視窗是峰值，43KB 留著就是
      // 「記憶體不足」的來源（diag187_2）。這裡真正釋放；FONTREL 記下拿回多少。
      const size_t released = fcm->releaseRetainedCache();
      if (released) DiagLog::line("FONTREL lazy %u", static_cast<unsigned>(released));
    }
    // v189 證人：這一站原本在 diag.log 裡隱形（只有 FONTREL），partial 延伸的起點要能對到。
    DiagLog::mem("build-lazy");
    // Reuse the last render's viewport so the extension paginates identically to the partial.
    const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
    if (!section->startBuild(buildSpec)) {
      // Not fatal: the partial keeps serving its pages; crossing the watermark falls back to
      // the blocking extension in render(). Don't retry every tick.
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      noteBuildStart();
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  // Drive any in-progress incremental section build forward, off the page-turn critical path.
  // v189：排到底（BUILD_AHEAD_CAP 的註解有完整理由）。tick 背對背連續跑，每 tick 1 頁，
  // parseStep 之間以原始按鍵電平讓路（buildShouldYield）。
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // While extending a partial (rebuild from a previous session), pageCount is pinned at the
  // partial's watermark until the build catches up, so the cap check would wrongly read
  // "far enough ahead" and stall the build at 0 pages -- then the first turn past the
  // watermark re-parses the whole chapter synchronously. Keep ticking until it finalizes
  // (that is the isPartial() clause inside buildTickDue()).
  // v189：這一圈有按鍵邊緣就不跑——loop() 的按鍵處理在這段【之後】，先跑 tick 等於讓翻頁
  // 多等一個 tick；讓 handler 先跑，requestUpdate 之後 render task 拿鎖，下一圈 peek() 自然擋住。
  // 手還在鍵上（inputActive）也不進來：進來也只會零步讓路，卻要付鎖＋兩趟 heap walk（複查 perf）。
  // 堆積地板移到鎖內、FONTREL 之後：原本 FONTREL 在地板之內，地板因 mini 駐留而關上時
  // 「能開門的那把鑰匙」永遠跑不到（複查 memory-major：maxAlloc<16KB 就停到下一次冷 render 才解）。
  // v193（複查）：有待補圖時這一圈不跑 tick —— 排在 tick 之後才解碼，等於把解碼推到 p2/p3 最碎的
  // 時刻（tick 剛擴張完 ParsedText 的逐詞小配置），正是最容易配不到解碼器的那一刻。
  if (buildTickDue() && deferredDecodePage_ < 0 && !RenderLock::peek() && !gpio.wasAnyPressed() &&
      !gpio.wasAnyReleased() && !gpio.inputActive()) {
    RenderLock lock;
    // Re-check under the lock: render() (which also holds the RenderLock) may have finalized the
    // build between the outer check and acquiring the lock here, in which case buildSomeMore()
    // would fail and wrongly reset the section. cppcheck can't see the cross-task mutation, so it
    // flags this as always true.
    // cppcheck-suppress knownConditionTrueFalse
    bool ok = section->isBuilding() && buildTickHeapGate();
    // v110/v164（最終複審 Finding B）：建置峰值裡保留中的 mini 快取是純負擔。
    // v188：這裡【不能】每圈都釋放——那會變成「每頁配、每圈丟」的碎片機（驗證者算過：pmax<32KB
    // 佔三分之一的頁）。三個同步 startBuild 站點仍無條件釋放。
    // v189（第二輪驗證）：只在【地板真的把這一圈關掉】時才釋放——排到底之後 tick 背對背，
    // 若像 v188 那樣在 maxAlloc<32KB 就放（爆發期每次冷 render 之後 pmax 常態 ~28KB），等於每翻一頁
    // 就把 v142 keep-if-fits 保留的 mini 丟掉、下一頁在解析器的小配置風暴裡重配 30–44KB＝v142 的失敗形狀。
    // 地板關上時它是唯一能開門的鑰匙（原本它在地板之內永遠跑不到）；放了就重量一次地板。
    // ⚠️ v164 那條「保留中且佔記憶體 ⟺ 身分 valid」的不變量自 v188 起【不成立】：clearCache() 保留容量
    //    但作廢身分，爆發期每一次冷 render 之後都是「invalid 且佔著」——這正是這裡要放掉的狀態。
    // 預取剛採用的 warm 身分【不踢】（只在巨型章閒置在上限、讀者又翻了一頁時並存）。
    if (!ok && section->isBuilding() && buildHeapPaused) {
      if (auto* fcm = renderer.getFontCacheManager(); fcm && !fcm->warmIdentity().valid) {
        const size_t released = fcm->releaseRetainedCache();
        if (released) {
          DiagLog::line("FONTREL tick %u", static_cast<unsigned>(released));
          ok = section->isBuilding() && buildTickHeapGate();
        }
      }
    }
    if (ok) {
      const uint16_t builtBefore = section->builtPageCount();
      const unsigned long tickT0 = millis();
      const bool built = section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK, &EpubReaderActivity::buildShouldYield, this);
      // tickmax 在這一圈【所有】持鎖工作結束後才記（含收尾後的預取）——見下方 noteTick。
      const auto noteTick = [&]() {
        const uint32_t dt = static_cast<uint32_t>(millis() - tickT0);
        if (dt > diagBuildTickMaxMs) diagBuildTickMaxMs = dt;
      };
      if (!built) {
        noteTick();
        if (section->lastBuildWasLowMemory()) {
          // v149（codex 抓到的外層迴圈）：低記憶體中止時【不要】reset ——
          // reset -> 下一輪 render 重開 Section -> 從章首重建到同一個長段落 -> 再 OOM，
          // 無退避的正回饋。partial 已由 suspendBuild 保留；使用者已看到的頁面照常，
          // 越過 watermark 的下一次翻頁會在（可能已寬鬆的）當下記憶體條件重試。
          // v189（第二輪驗證抓到的既有迴圈）：partial 延伸在這裡被 OOM 打斷後，lazy 站的條件
          // （!isBuilding && isPartial && 距 watermark 15 頁內）下一圈就又成立 → 重新 inflate、從第 0 頁
          // 排到同一段、再 OOM，無限循環（每圈還多兩行 diag）。栓住 lazy 站；越過 watermark 的翻頁
          // 走 render 的同步延伸（那裡有 handleLowMemoryBuild 分流）。新 section 會重置這個栓。
          LOG_ERR("ERS", "Background build hit low memory; pausing (partial kept)");
          partialRebuildStartFailed = true;
          emitBuildEnd("lowmem");
        } else {
          LOG_ERR("ERS", "Background section build failed");
          emitBuildEnd("failed");
          section.reset();
          requestUpdate();
        }
      } else {
        const bool complete = section->isBuildComplete();
        if (complete || section->builtPageCount() != builtBefore) {
          diagBuildTicks++;
        } else {
          diagBuildZeroTicks++;
        }
        diagBuildPagesBuilt = complete ? section->pageCount : section->builtPageCount();
        if (!complete) {
          noteTick();
        } else if (applyDeferredReposition()) {
          // The chapter re-paginated since the saved progress (settings changed): we now know the
          // real page count, so re-render at the remapped page. No-op for an unchanged resume.
          noteTick();
          emitBuildEnd("done");
          requestUpdate();
        } else {
          // v170（EPUB→txt 手感調查的結論）：背景建置在使用者閱讀（dwell）期間完成時，
          // 沒有任何 render 會跟著發生 —— render 尾端的預取當初被 isBuilding 擋掉，
          // 這裡是唯一能補做的地方（codex 在 v164 複查就點過這個洞，當時認列為取捨；
          // diag169 實測命中率 34%、而命中頁的手感已與 txt 同級 —— 這個洞就是差距主體）。
          // 仍在 pump 的 RenderLock 內：render task 被排除，與 render 尾端呼叫同樣安全；
          // prefetchNextPage 自身的閘門照常把關。v189：這裡在主任務、持鎖，沒有人輪詢按鍵——
          // 手在鍵上就不做，做的時候原始電平一動就中止（abortOnInput）。
          // ⚠️ 中止粒度是字重桶之間：中文頁幾乎只有一桶，所以這 ~300ms 的 SD 預熱實際上是【每章一次】
          //    的不可中斷盲區（第二輪驗證）。要縮要動 SdCardFont::prewarmStyle 的讀取迴圈（v161 就記過）。
          //    tickmax 把它算進去，BUILD end 在它之後印。
          if (!gpio.inputActive()) prefetchNextPage(SETTINGS.getReaderFontId(), 0, 0, section->currentPage, true);
          noteTick();
          emitBuildEnd("done");
        }
      }
    }
  }

  // v193：延後解碼的補圖重繪。
  // ⚠️ 複查抓到的紅線：按鍵按著時**只能延後、不能放棄**——放棄之後 lastDeferredKey_ 仍蓋著這一頁，
  // 不會再產生第二次待辦，那張圖就永遠停在佔位框（一次無效點擊就足以觸發）。
  if (deferredDecodePage_ >= 0 && !RenderLock::peek()) {
    const int curPage = section ? section->currentPage : -1;
    const bool samePage = (currentSpineIndex == deferredDecodeSpine_ && curPage == deferredDecodePage_);
    if (!samePage) {
      DiagLog::line("IMGDEFER drop why=moved");
      deferredDecodePage_ = -1;
      lastDeferredKey_.spine = -1;
      lastDeferredKey_.page = -1;
    } else if (gpio.inputActive()) {
      // 手還按著：什麼都不做，待辦留著；放開之後下一圈再補（頁真的換了會走上面的 moved 分支）。
    } else {
      DiagLog::line("IMGDEFER redraw spine=%d page=%d", deferredDecodeSpine_, deferredDecodePage_);
      deferredDecodePage_ = -1;
      requestUpdate();
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) || ReaderUtils::isTouchMenuGesture(mappedInput)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block. A Confirm release after a long-press function (bookmark/sync) fired is left
  // to the regular Confirm handler below, which consumes it via ignoreNextConfirmRelease.
  if (atEndOfBook && endOfBookOptions.menuActive() &&
      !(ignoreNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm))) {
    std::string openPath;
    switch (endOfBookOptions.handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        currentSpineIndex = std::max(epub->getSpineItemsCount() - 1, 0);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  // Enter reader menu activity on short-press Confirm or a downward swipe from the top edge. A long-press
  // that fired a bound function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || ReaderUtils::isTouchMenuGesture(mappedInput)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      openReaderMenu();
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Short press Back restores position when viewing a footnote (takes priority over navigation)
  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, epub ? epub->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<EpubReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (endOfBookOptions.menuActive()) {
      // Selection movement was handled above; absorb leftover page-turn triggers so
      // e.g. "previous" at the top of the list doesn't jump back into the book
      return;
    }
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);

      // Preferred path: a bookmark carrying an exact content offset. It is immune to
      // re-pagination, so resolve by content instead of trusting a page number saved under
      // possibly-different settings.
      if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
        RenderLock lock(*this);
        if (section && currentSpineIndex == sync.spineIndex) {
          // Already in this chapter and laid out: resolve straight away, no reload.
          const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
          section->currentPage = page.value_or(std::max(0, sync.page));
        } else {
          // Different chapter: reload and let render() build to the offset before drawing.
          currentSpineIndex = sync.spineIndex;
          pendingOffsetJump = sync.visibleTextOffset;
          nextPageNumber = std::max(0, sync.page);  // hint until the offset resolves
          section.reset();
        }
        return;
      }

      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      if (currentSpineIndex != targetSpineIndex) {
        RenderLock lock(*this);
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        RenderLock lock(*this);
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) {
                               // TextSettingsActivity saves on each change; no save needed here.
                               // Font/size/spacing/margin changes invalidate the current
                               // layout: preserve position and force a re-layout, mirroring
                               // applyOrientation()'s reflow.
                               RenderLock lock(*this);
                               ParsedText::setBoldBodyText(SETTINGS.boldBodyText != 0);  // v187：重排前同步旗標
                               if (section) {
                                 rememberCurrentContentOffset();
                                 cachedSpineIndex = currentSpineIndex;
                                 cachedChapterTotalPageCount = section->pageCount;
                                 nextPageNumber = section->currentPage;
                               }
                               section.reset();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult& result) {});
          break;
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        // CrossMosa：原本是 if (epub && section)。section 唯一的賦值點在後面的 render，
        // 而 render 在「全書完」分支就 return 了 -> section 永遠是 null ->
        // 選單進得去、選下去靜默無作用直接回主畫面。
        // 而「全書完」正是 ns0: 前綴那個 bug 的症狀 —— 對症的入口在對症的情境下必定失效。
        if (epub) {
          // ⚠️ section 可能是 null（見上）—— 這正是放寬條件的理由，所以本體【必須】容忍它。
          // 放寬 if (A && B) -> if (A) 卻沒改本體，就是把「按了沒反應」換成「按了當機」。
          // section.reset() 對空的 unique_ptr 是安全的。
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section ? section->currentPage : 0;
          uint16_t backupPageCount = section ? section->pageCount : 0;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (pendingCacheReset_) {
            // v184：使用者選「連進度一起重設」—— 不寫回備份位置，主畫面卡片的百分比同步歸零。
            RECENT_BOOKS.setProgress(epub->getPath(), 0);
          } else if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
          pendingCacheReset_ = 0;
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn) {
    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this]() {
    // v171：這一行讓「無效的書籍檔」事件可歸因（diag169_2 使用者回報有索引錯誤，
    // 但 log 裡零痕跡 —— 這個 lambda 原本只寫 LOG_ERR = 無序列埠即丟棄）。
    DiagLog::line("EPUB showBuildError spine=%d", currentSpineIndex);
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  // v163：前景建置失敗的分流。低記憶體中止 ≠ 壞檔 —— v149 只把這個分辨做在背景
  // loop() 路徑，前景站點一律「reset + 無效的書籍檔」，於是暫時性 OOM 以壞檔的
  // 面貌現形、重試又成功（丹布朗三本解剖定案：時報模板 CSS 的建置期常駐壓力）。
  // 低記憶體時：不 reset（v149 教訓：reset → 從章首重建到同一段落 → 再 OOM，
  // 無退避正回饋；partial 已由 suspendBuild 保留）、顯示誠實訊息；下一次翻頁走
  // 既有的 partial-extension 路徑續建，屆時暫態壓力（背景重排視窗）多半已過。
  // ⚠️ 刻意【不】在這裡卸字型救記憶體：排版量寬要用 SD 字型的 advance 資料，
  // 建置中途卸字型會排出不同分頁並固化進 section 快取 —— 比失敗更糟。
  // 回 true = 低記憶體路徑已處理，呼叫端直接 return（不 reset、不報壞檔）。
  const auto handleLowMemoryBuild = [this]() -> bool {
    if (!section || !section->lastBuildWasLowMemory()) return false;
    LOG_ERR("ERS", "Foreground build hit low memory; keeping partial, will retry on next action");
    DiagLog::line("EPUB fg-build lowmem: partial kept, no reset");
    // v171：現場拍池。diag169_2 兩次事件 defFree 只剩 7KB，但無快照 → 兇手匿名。
    // 這條路徑一個 session 最多幾次，dumpPools 的成本在這裡可付。
    DiagLog::mem("fg-lowmem");
    DiagLog::dumpPools(2048, "fg-lowmem");
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_BUILD_LOW_MEMORY));
    automaticPageTurnActive = false;
    return true;
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    // Sole load site: runs on the render task (serialized by RenderLock); the main
    // task only reads the suggestions once the loaded flag is published
    endOfBookOptions.loadOnce(epub->getPath());
    renderer.clearScreen();
    endOfBookOptions.render(renderer, mappedInput);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  // Capture for loop()'s lazy partial-extension start (must match this render's layout params).
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    // v193：唯一收斂點——所有換章／重建都 section.reset() 後走到這裡。
    // 只在 spine 真的變了才清 advance 表；同章重建（方向、設定、partial 重開）不准清。
    if (currentSpineIndex != lastAdvanceSpine_) {
      if (SdCardFont* rf = sdFontSystem.currentReaderFont()) {
        const uint32_t used = rf->resetAdvanceTables();
        DiagLog::line("ADVRESET spine=%d used=%u", currentSpineIndex, static_cast<unsigned>(used));
      }
      lastAdvanceSpine_ = currentSpineIndex;
    }
    // v193（複查）：延後鑰匙要在【每一次建新 Section】時失效，不能只在換章時 ——
    // 同章重建（改方向、改設定、partial 重開）之後同樣的 (spine,page) 已經是不同內容的頁，
    // 沿用舊鑰匙會讓那一頁被當成「已經延後過」而回到同步解碼的 10 秒停頓。
    lastDeferredKey_.spine = -1;
    lastDeferredKey_.page = -1;
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;
    landingPending_ = true;  // v189：這次 render 是落地，落地頁定案後蓋 deferredLandingPage_

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    // v187 證人：快取被丟掉的原因（1 版號／2 參數／3 CSS 截斷重排／4 partial 壞）；沒有快取不記。
    if (!cacheLoaded && section->lastLoadReject() != 0) {
      DiagLog::line("SCTLOAD reject=%u spine=%d", static_cast<unsigned>(section->lastLoadReject()), currentSpineIndex);
    }
    if (cacheLoaded) {
      // Matching render params means identical pagination, so the saved page number is valid
      // as-is: consume any pending settings-change reposition. Without this, a chapter total
      // saved while the section was still building (i.e. a watermark, not the real count)
      // would remap the resume page against the finalized count and teleport the reader.
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    // Land this render by content offset when one applies. An explicit bookmark jump
    // (pendingOffsetJump) always wins -- it is a deliberate navigation to a stored content anchor.
    // Otherwise fall back to the settings-change reposition: read after the cache-hit reset above,
    // a spec match means the saved page number still names the same content so there is nothing to
    // reposition, while a page jump or fragment anchor is a deliberate navigation that outranks it.
    const std::optional<uint32_t> offsetJump =
        pendingOffsetJump.has_value() ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    // v189（第二輪驗證）：同一章的頁跳／錨點跳贏過設定重定位（上面註解就這麼說），那快取也該死在這裡——
    // 留著的話背景建置收尾時會拿選單前的 offset 把人從錨點拉走（改字級＋選章節同一次選單就會）。
    if (!pendingOffsetJump.has_value() && (pendingPageJump.has_value() || !pendingAnchor.empty())) {
      cachedVisibleTextOffset.reset();
      cachedChapterTotalPageCount = 0;
    }
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      // Jumps that need the final pagination or the anchor map -- explicit page jumps,
      // fragment anchors, percent jumps, and cross-setting progress repositioning -- can't
      // resolve their landing page until the whole chapter is laid out, so they take the full
      // (blocking) build with the indexing popup. Everything else -- plain forward reads, resume,
      // and explicit page jumps -- only needs a specific page, so it builds incrementally to that
      // page and finishes the rest in loop(). The settings-change reposition (cachedChapterTotal*)
      // is NOT a full-build trigger: it's deferred to applyDeferredReposition() once the real page
      // count is known, so it never blocks the first page.
      // Only a percent jump truly needs the whole chapter up front (percent -> page needs the final
      // page count). Anchor jumps (TOC / chapter select / footnotes) resolve incrementally below --
      // the anchor is recorded as its page is laid out, so a chapter-top anchor lands on page 0
      // without indexing the whole chapter.
      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        // The popup's own refresh is a plain FAST, so force the page that replaces it onto the HALF
        // ghost-cleanup path -- otherwise the "INDEXING" text ghosts under the rendered page.
        pagesUntilFullRefresh = 1;
        // No popup redraws while the framebuffer is lent to the build below;
        // the panel holds the popup displayed above (e-ink is persistent).
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        // Lend the framebuffer's 48 KB to the blocking full build; restored
        // (white) at scope exit, and the page render below redraws everything.
        GfxRenderer::FrameBufferLoan loan(renderer);
        noteBuildStart();  // v189：第四個建置站點（百分比跳頁的整章同步建置），一樣要有 BUILD end
        const bool fullOk = section->createSectionFile(renderSpec, popupFn);
        emitBuildEnd(fullOk ? "full" : "full-failed");
        if (!fullOk) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          loan.end();  // restore before anything draws
          if (handleLowMemoryBuild()) return;  // v165：OOM 分流（不 reset）
          section.reset();
          showBuildError();
          return;
        }
        loan.end();
      } else {
        // Lay out just enough to show the landing page; loop() builds the rest behind it. Show the
        // indexing popup up front only when the build will actually be slow: a large spine (its
        // whole HTML must be inflated before page 1 can lay out -- the giant single-spine case), or
        // a deep resume/jump that must lay out many pages to reach the landing page. Tiny sections
        // build in a blink and stay popup-free.
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        // Landing well inside a partial: the page (or anchor, via the on-disk map) is already
        // servable, so don't restart the extension build now -- it re-lays out the WHOLE chapter
        // from page 0 (minutes of background CPU + SD writes on a giant spine), pure waste when
        // the reader never nears the watermark this session. loop() starts it lazily once the
        // reader is within PARTIAL_REBUILD_START_MARGIN pages of the watermark.
        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          // Popup only when the build will actually be slow: a big spine whose HTML still needs
          // inflating (the multi-second cost), or a deep page target. A reopen with cached HTML builds
          // fast, so no popup -- that's what made an already-indexed book look like it was reindexing.
          // A partial cache that already covers the target page shows it instantly: never popup.
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            // An anchor jump's cost is bounded by the anchor's page, not `target`. An anchor already
            // in the on-disk map (partial or finalized cache) lands instantly: no popup. Otherwise it
            // lies beyond the indexed watermark and the build may lay out the whole spine to find it,
            // so gate on spine size alone -- laying out a big spine takes seconds even with cached
            // HTML. Ordinary chapter-top TOC jumps resolve on page 0 and stay popup-free.
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts under the page.
            pagesUntilFullRefresh = 1;
          }
          // Mid-build popup surfacing for slow builds the predictive gates can't
          // see (image extraction/probing inside a single page, or any chunk
          // overrunning the deadline). The parser fires the callback before the
          // first image probe; buildPopupPending gates it to this blocking phase
          // so a background build in loop() can never draw over a displayed page.
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            // Lend the framebuffer's 48 KB to startBuild only (the spine HTML
            // inflation peak). The chunk loop below runs without it so the popup
            // can draw mid-build; background chunks never had the loan either.
            GfxRenderer::FrameBufferLoan loan(renderer);
            // v140 量測：這是開書時走的同步建置路徑，也是記憶體壓力最高的視窗
            // （diag6 的 14 次 alloc_fail 全落在 build=1 期間）。dumpPools 才答得出
            // 【誰卡在 p2 中間】—— ESP.getMaxAllocHeap() 是兩池取大者，混著看不出歸屬。
            // ⚠️ 儀器放在 src 端而不是 lib/Epub 裡：lib 不能反向依賴 src/util/DiagLog
            //    （舊樹 ImageBlock.h 的註解寫明了這條分層）。
            // ⚠️ v141：這裡【只留 mem()，不做 dumpPools】。
            // v140 把 dumpPools 放在這一點是我的錯 —— 它要走【兩趟】完整 heap walk
            // （期間持有 heap 鎖）再寫一次 SD，而這一刻正是建置最忙、SD 也在被讀的時候。
            // v140 實機在 build-start 的傾印之後約 0ms 就重開機（panic reason 為空
            // ＝看門狗／硬重置，不是 abort），使用者當時還同時在用網路傳檔。
            // 無法證明是儀器造成的，但【儀器本身不該影響被觀測的東西】—— 先把它拿掉，
            // 少一個變數。而且 v140 要的答案已經拿到了（p2 全空、framebuffer 在 p3）。
            if (auto* fcm = renderer.getFontCacheManager()) {
              // v188：clearCache() 保留 mini 容量（防碎片化），但建置視窗是峰值，43KB 留著就是
              // 「記憶體不足」的來源（diag187_2）。這裡真正釋放；FONTREL 記下拿回多少。
              const size_t released = fcm->releaseRetainedCache();
              if (released) DiagLog::line("FONTREL %u", static_cast<unsigned>(released));
            }
            DiagLog::mem("build-start");
            started = section->startBuild(renderSpec, [this] { showBuildPopup(); });
            if (started) noteBuildStart();
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            buildPopupPending = false;
            if (handleLowMemoryBuild()) return;  // v165：startBuild 的 OOM 出口也分流
            section.reset();
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
            // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
            // Re-pagination: build until the content the reader was on has been laid out. Costs the
            // same parse work as the old page target did -- it is the same content -- but it stops
            // at the right place, so the landing page is known before anything is drawn.
            // Otherwise: build until the target page exists. loop() builds the rest behind it.
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              // The predictive gates guessed fast but the build blew the silent budget.
              showBuildPopup();
            }
            if (!section->isBuilding()) {
              // 建置結束：與 build-start 對照，看建置本身吃掉多少、結束後有沒有還回來。
              DiagLog::mem("build-end");
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              buildPopupPending = false;
              if (handleLowMemoryBuild()) return;
              section.reset();
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }

    // The chapter re-paginated, so nextPageNumber above named the old pagination's page.
    // The build loop stopped once this offset was laid out, so resolve it now, before the
    // first draw. Leaving it to applyDeferredReposition() is what made the stale page paint
    // first and then jump when the background build finished the chapter.
    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        // v189（複查 state-major）：落地已經用掉這個 offset 了，設定變更的快取當場消耗。留著的話
        // applyDeferredReposition 會在背景建置收尾時（排到底＝改設定後 20–30 秒）拿同一個 offset
        // 再算一次頁碼、蓋掉 currentPage——讀者若已翻頁就被拉回去。上游的視窗設計把收尾推到章末
        // 附近，所以以前很少踩到；只有 offset 解析失敗（頁還沒排到）才留給收尾走百分比備援。
        cachedVisibleTextOffset.reset();
        cachedChapterTotalPageCount = 0;
      }
    }
    pendingOffsetJump.reset();  // one-shot explicit jump: consumed on this render

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  // Extend the build to the requested page if needed (for partials and in-progress builds).
  // This runs every render, so it covers both the first page and any forward turn that gets
  // ahead of the background builder; pages already built do no work here.
  //
  // Crossing a partial's watermark before the extension rebuild has caught up means a
  // synchronous wait spanning the remaining prefix re-layout -- potentially tens of
  // seconds on a giant spine. Show the indexing popup so it isn't a silent freeze
  // (the page that replaces it takes the HALF ghost-cleanup path). Ordinary window
  // catch-ups on a non-partial build are a page or two and stay popup-free.
  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    // v110/v164：這條可能是數十秒的前綴重排 —— inflate 峰值前釋放保留中的 mini 快取。
    if (auto* fcm = renderer.getFontCacheManager()) {
      // v188：clearCache() 保留 mini 容量（防碎片化），但建置視窗是峰值，43KB 留著就是
      // 「記憶體不足」的來源（diag187_2）。這裡真正釋放；FONTREL 記下拿回多少。
      const size_t released = fcm->releaseRetainedCache();
      if (released) DiagLog::line("FONTREL ext %u", static_cast<unsigned>(released));
    }
    // Start a build to extend a partial toward the requested page.
    if (!section->isBuilding()) {
      DiagLog::mem("build-ext");  // v189 證人：這一站原本只有 FONTREL（且 released=0 時什麼都沒有）
      if (!section->startBuild(renderSpec)) {
        LOG_ERR("ERS", "Failed to start partial extension build");
        if (handleLowMemoryBuild()) return;  // v165：startBuild 的 OOM 出口也分流
        section.reset();
        showBuildError();
        return;
      }
      noteBuildStart();
    }
    // Extend until either the target page exists or the build completes.
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        if (handleLowMemoryBuild()) return;
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        if (handleLowMemoryBuild()) return;
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  // The requested page is now as built as it will get. If it still lands past the end,
  // clamp to the last real page: the UINT16_MAX "last page" sentinel from backward chapter
  // navigation, an explicit jump beyond a finished chapter, or a stale saved position.
  // Guarded on !isBuilding() because a still-building section's pageCount is only the current
  // watermark (not the final count) and has already been driven far enough by the loops above.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  // Apply a deferred settings-change reposition now that the real page count is known (a no-op for
  // a plain resume / unchanged pagination). If still building, this defers to loop() on completion.
  if (landingPending_) {  // v189：只在落地那次蓋章；之後每次 render 不動它，收尾時頁已不同＝讀者翻過了
    landingPending_ = false;
    deferredLandingPage_ = section->currentPage;
  }
  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    // v110/v164 複審紀律（B-24）：頁碼【捕捉一次，到處都用同一份】。主任務的翻頁是
    // 不持鎖改 currentPage 的；loadPage 的 SD I/O（數十毫秒）之間再重讀就會拿到下一頁，
    // 快取身分於是掛錯頁 —— 假 warm 命中、整頁走 overflow ring 而 diag 印 warm=1。
    const int pageNo = section->currentPage;
    auto p = section->loadPage(pageNo);
    if (!p && section->lastLoadWasLowMemory()) {
      // v152：低記憶體的 loadPage 失敗是【暫時的】—— pxc slot 在本輪 render 結束就釋放。
      // 走原本的 clearCache/reset 會刪掉章節快取、在記憶體最緊的時刻強迫全量重建。
      // 改成：記進 diag、跳過本輪、requestUpdate 讓下一輪重試（次數共用既有上限）。
      DiagLog::line("PAGELOAD deferred: low memory, retry %d", pageLoadRetryCount + 1);
      if (++pageLoadRetryCount <= MAX_PAGE_LOAD_RETRIES) {
        requestUpdate();
        return;
      }
      LOG_ERR("ERS", "Page load low-memory retries exhausted");
      // 連續多輪都瀕死：落回原本的重建路徑（下面），至少讓使用者不卡白頁。
    }
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      // Retrying rebuilds a transiently corrupt section and usually recovers, but a page that keeps
      // failing would loop forever on a blank screen, so bound the retries before giving up.
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      // Abandon (not suspend) any active build BEFORE clearing: clearCache deletes the files,
      // and the destructor's suspend would otherwise commit tables into a deleted handle.
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;  // Reset so a later user-initiated navigation can try afresh
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();  // Try again after clearing cache
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;  // Reset the retry counter once a page loads cleanly

    // Cache this page's content offset (read alongside the page, no extra file open) so
    // saveProgress and addBookmark can use it without reopening section.bin.
    currentPageVisibleOffset = p->visibleTextOffset;

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), pageNo, orientedMarginTop, orientedMarginRight, orientedMarginBottom,
                   orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
  }
  // Only persist when the position actually changed. render() also runs on menu,
  // bookmark and screenshot re-renders, and writeAtomic is several FAT ops for 6 bytes.
  // Every real page turn changes currentPage, so progress durability is unaffected.
  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }

  // v110/v164：這一頁已完整畫完並送上面板，從這裡到 render() 返回之間 CPU 是空的而
  // 使用者正在讀 —— 把下一頁的字先讀進來，花的是 dwell 時間不是翻頁時間。
  // 仍持 RenderLock：loop() 的背景重排與自動翻頁都用 RenderLock::peek() 讓路；
  // 按鍵處理不碰鎖，中止訊號（isRenderPending）進得來（本樹 FCM 的中止粒度是字重桶之間）。
  prefetchNextPage(SETTINGS.getReaderFontId(), orientedMarginTop, orientedMarginLeft, lastRenderedPage_);
}

// v110/v164：預取下一頁的字型 mini 資料到【同一塊】快取，不新增任何常駐記憶體。
// 前提：當前頁的字在 renderContents 收工之後確定無人使用 —— 灰階兩趟與 cleanup 都在
// renderContents 內完成，狀態列與彈窗走 UI 字型。中止＝乾淨放棄：快取清空＋身分失效
// ⇒ 下一次 render 走冷路徑（＝無預取行為）。
void EpubReaderActivity::prefetchNextPage(const int fontId, const int marginTop, const int marginLeft,
                                          const int basePage, const bool abortOnInput) {
  // 先歸零：pf= 的語意是「即將顯示的這一頁，預取花了多久」。任何一道閘門擋下來都算
  // 「沒有預取」，留著上一次的數字會讓 warm=0 旁邊掛著漂亮的 pf=280。
  diagPrefetchMs = 0;
  diagPfMaxKb = static_cast<uint16_t>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) / 1024);
  diagPfRetKb = 0;
  // 重排中：頁數與版面未定案，且背景重排正是最大瞬時壓力源 —— 那個視窗裡不多握任何東西。
  // v189：看 buildTickDue() 不看 isBuilding()——排到底之後 isBuilding() 只在爆發期（20–30 秒）
  // 與「巨型章閒置在 BUILD_AHEAD_CAP」時為真；後者沒有工作在跑、頁面也早就可讀
  // （loadPage 走 build_->lut），沒理由整章擋預取。堆積地板（下面的 pg=7）照常把關。
  if (!section || buildBurstActive()) {
    diagPfGate = 1;
    return;
  }
  // 捕捉一次。此後 currentPage 可能被主任務改掉，但身分的正確性來自「下一次 render
  // 逐欄位比對」，不是這裡讀到的值，所以捕捉值永遠是誠實的答案。
  // v177（使用者提議＋diag176 定案）：目標頁以【剛畫完的那一頁】為基準，不讀 currentPage ——
  // 按鍵在 render 進行中就把 currentPage 推到 N+1，舊寫法會拿 N+2 當目標、白做一趟還讓 N+1 冷掉
  // （diag176：26 頁「預取完成卻冷」）。而 isRenderPending() 在本樹看不到排隊的按鍵
  // （requestedUpdate 在 loop 結尾就被 exchange(false) 消掉），所以改用頁碼本身判斷方向：
  // currentPage 已經是 next ＝ 順向翻頁 → 照做，中止檢查也放行；其他變動才擋／中止。
  if (basePage < 0) {
    diagPfGate = 1;
    return;
  }
  const int next = basePage + 1;
  {
    const int cur = section->currentPage;
    if (cur != basePage && cur != next) {
      diagPfGate = 6;
      return;
    }
  }
  prefetchBase_ = basePage;
  prefetchTarget_ = next;
  // 章末：下一頁在另一個 section，換章一律 section.reset() → 冷路徑，預取無從幫忙。
  if (next >= static_cast<int>(section->pageCount)) {
    diagPfGate = 2;
    return;
  }
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) {
    diagPfGate = 3;
    return;
  }
  // 內建備援字型：FontDecompressor 的 prewarm 是純 CPU 解壓，這個功能買的是 SD 讀取。
  if (!sdFontSystem.currentReaderFont()) {
    diagPfGate = 4;
    return;
  }
  // 書籤彈窗還在畫面上：關閉重繪必定是【同一頁】，預取只會把那次 warm 命中換成冷路徑。
  if (showBookmarkMessage) {
    diagPfGate = 5;
    return;
  }

  const unsigned long t0 = millis();
  // codex 複查（v164）：身分快照在 scan【之前】建立，完成後只 adopt 這份快照 ——
  // 延後到完成才讀 SETTINGS 的話，快照與快取內容可能來自兩套設定（選單改字級的窄窗），
  // 身分就蓋在別套設定建的快取上。
  const WarmIdentity target = buildWarmIdentity(next);
  bool completed;
  {
    // ctor 的 clearCache() 清掉的正是【剛畫完那一頁】的快取 —— 此刻已無人使用。
    auto scope = fcm->createPrewarmScope();
    // v174（diag173 定案）：v167 的堆積地板原本放在 ctor 清快取【之前】量 —— 量到的最大連續塊
    // 被本頁自己 30–40KB 的 mini 壓在 20–26KB（p2 只剩它旁邊的碎塊），整本書 97% 被自己的
    // 快取擋在門外（v173 warm 2–3%，pmax 20–26KB）。改到清掉之後量：那才是預取真正面對的水位。
    // 本頁快取此刻已無人需要（下一頁身分必不同，冷路徑本來就會清它；同頁重繪只有書籤彈窗
    // 一條，已在上面擋掉）。地板 32KB：mini 20–40KB，SdCardFont 自己還有逐字地板與階梯降級。
    // v189：v188 讓 clearCache() 保留 mini 容量（防碎片化）之後，「清掉之後量」的前提失效——
    // 量到的是 mini 仍駐留的水位，而預取接下來會【就地重用】那塊容量（keep-if-fits），根本不需要
    // 再配。把保留容量加回來才是預取真正面對的餘裕（diag-prev188：pmax=28 且 mini 40–44KB 駐留，
    // 89 頁裡 75 頁被這裡擋掉；用 v188 的量法，這條地板等於「有留快取就不預取」）。
    // 容量取各字面最大者：下一頁主字面若換成另一個字面（例如整頁粗體），實際會重配、可能走階梯
    // 降級——後果是丟幾個字進 miss ring，不是當機，且很少見。
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const size_t retained = fcm->retainedMiniBitmapCapacity(fontId);
    diagPfMaxKb = static_cast<uint16_t>(largest / 1024);  // v189：pmax= 改記地板真正拿來判斷的那個值（ctor 之後）
    diagPfRetKb = static_cast<uint16_t>(retained / 1024);
    if (largest + retained < 32 * 1024) {
      diagPfGate = 7;
      // v189 證人（每次開機一次）：pmax 卡在 28KB 是誰切的池——v187 的 dump 指向一組
      // 17408+6400+6400 的字型常駐表落在 p2 中段（第二個 SdCardFont 物件），這裡在 mini 仍駐留
      // 的狀態下傾印一次，正是要抓的那個佈局。
      static bool dumped = false;
      if (!dumped) {
        dumped = true;
        DiagLog::dumpPools(1024, "pf-floor");
      }
      return;
    }
    // ⚠️ loadPage 必須在 ctor【之後】：Page 物件與上一頁的 mini 資料若同時在世，
    // 峰值就是兩者之和，而這台機器是被最大連續塊掐住的。
    auto page = section->loadPage(next);
    // retain 留到 loadPage 成功之後才開：失敗這條 return 不留「空但被保留」的快取。
    if (!page) {
      diagPfGate = 8;
      return;
    }
    scope.setRetainCacheOnExit(true);
    // scan 模式：drawText 只 recordText，framebuffer 一個位元組都不會動。
    page->render(renderer, fontId, marginLeft, marginTop);
    completed = scope.endScanAndPrewarmAbortable(
        abortOnInput ? &EpubReaderActivity::prefetchShouldAbortOrInput : &EpubReaderActivity::prefetchShouldAbort, this);
  }
  // 預取自己的 stats 折進 SDCFFAIL/dropped 判讀鏈（下一次 render 的 ctor 會 resetStats）。
  diagPfGate = completed ? 0 : 9;
  if (completed) {
    // 用進場時的快照，不重讀 currentPage 也不重讀 SETTINGS。
    fcm->adoptWarmIdentity(target);
    diagPrefetchMs = static_cast<unsigned>(millis() - t0);
  } else {
    // 半成品快取。scope 解構已清過一次；這一行是保險（clearCache 冪等且自 invalidate）——
    // 這條路徑的正確性不該只靠另一個檔案的解構子記得幫忙。
    fcm->clearCache();
  }
}

// 跑在 FontCacheManager 字重桶之間的輪詢（本樹的中止粒度）。只讀一個旗標，不做 I/O。
bool EpubReaderActivity::prefetchShouldAbort(void* ctx) {
  // v177：只有 currentPage 跑到「基準頁／目標頁」以外才中止（往回、跳頁、連按兩次）。
  // 順向翻到目標頁＝這趟預取正是下一個 render 要的，跑完比中止再冷路徑快 ~300ms。
  auto* self = static_cast<EpubReaderActivity*>(ctx);
  if (!self->section) return true;
  const int cur = self->section->currentPage;
  return cur != self->prefetchBase_ && cur != self->prefetchTarget_;
}

bool EpubReaderActivity::prefetchShouldAbortOrInput(void* ctx) {
  return prefetchShouldAbort(ctx) || gpio.anyButtonDownRaw();
}

WarmIdentity EpubReaderActivity::buildWarmIdentity(const int pageNumber) const {
  WarmIdentity w;
  w.bookHash = warmBookHash;
  w.spineIndex = currentSpineIndex;
  w.pageNumber = pageNumber;
  w.fontId = SETTINGS.getReaderFontId();
  w.viewportWidth = buildViewportWidth;
  w.viewportHeight = buildViewportHeight;
  w.lineCompressionBits = WarmIdentity::floatBits(SETTINGS.getReaderLineCompression());
  w.paragraphAlignment = SETTINGS.paragraphAlignment;
  w.imageRendering = SETTINGS.imageRendering;
  w.extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0;
  w.hyphenationEnabled = SETTINGS.hyphenationEnabled != 0;
  w.embeddedStyle = SETTINGS.embeddedStyle != 0;
  w.focusReadingEnabled = SETTINGS.focusReadingEnabled != 0;
  w.boldBodyText = SETTINGS.boldBodyText != 0;  // v187：切粗體必使字型預熱身分失效
  w.valid = true;
  return w;
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  // v189：落地之後讀者已經翻頁 → 舊 offset／舊頁數比例都不再描述「他在看的那一頁」，消耗掉、不動頁碼。
  if (deferredLandingPage_ >= 0 && section->currentPage != deferredLandingPage_) {
    cachedChapterTotalPageCount = 0;
    cachedVisibleTextOffset.reset();
    return false;
  }
  bool changed = false;
  // Re-derive the page from the saved content offset after a settings reflow.
  // Older 4/6-byte progress files retain the page-fraction fallback.
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  cachedChapterTotalPageCount = 0;  // consumed; don't read cached progress again
  cachedVisibleTextOffset.reset();
  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  std::optional<uint32_t> offset;
  if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    // The on-screen page's offset was captured at load; reuse it to avoid a fresh section-file
    // open on every page turn. Any other page (rare) falls back to a direct lookup.
    offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                 ? currentPageVisibleOffset
                 : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, offset);
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int pageNo, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  // v191：每頁一次抽取機會；灰階帶迴圈在本函式內，不在這裡清。
  ImageBlock::clearRetryableFailures();
  const auto t0 = millis();
  lastRenderedPage_ = pageNo;  // v177：render 尾端預取的基準頁
  // v193：同一頁只延後一次。進場時若身分還沒蓋過這一頁 → 整輪（BW＋灰階各帶）都延後解碼；
  // 進場就先寫 lastDeferredKey_，第二次 renderContents 看到同一把鑰匙就不再延後。
  // v193（複查）：currentSpineIndex 是會被主任務在面板刷新那一秒改掉的易變成員（教訓 24）。
  // 這裡只讀一次，兩個 guard 都吃這份快照——否則解構時讀到的可能已經是【下一章】，
  // 補圖／癒合的待辦就會綁到別頁去。
  const int renderSpine = currentSpineIndex;
  const bool firstDraw = lastDeferredKey_.spine != renderSpine || lastDeferredKey_.page != pageNo;
  if (firstDraw) {
    lastDeferredKey_.spine = renderSpine;
    lastDeferredKey_.page = pageNo;
  }
  struct DeferHeavyGuard {
    EpubReaderActivity* self;
    int spine;
    int page;
    uint32_t startCount;
    DeferHeavyGuard(EpubReaderActivity* s, bool defer, int sp, int pg)
        : self(s), spine(sp), page(pg), startCount(ImageBlock::deferredDecodeCount()) {
      ImageBlock::setDeferHeavyDecode(defer);
    }
    ~DeferHeavyGuard() {
      ImageBlock::setDeferHeavyDecode(false);
      const uint32_t seen = ImageBlock::deferredDecodeCount();
      if (seen > startCount) {
        self->deferredDecodeSpine_ = spine;
        self->deferredDecodePage_ = page;
        DiagLog::line("IMGDEFER page=%d spine=%d", page, spine);
      } else {
        // v195：diag194 有 14 次 IMGDEFER page= 只配對到 8 次 redraw，而「放棄」的證人幾乎不可能觸發
        // ——因為翻頁時新的一次 render 會走到這裡把待辦【靜默清掉】。少了這一行就分不出
        // 「使用者翻走了」與「我們把它弄丟了」。只在真的有待辦時印，純文字頁不會產生噪音。
        if (self->deferredDecodePage_ >= 0) {
          DiagLog::line("IMGDEFER clear spine=%d page=%d why=render", self->deferredDecodeSpine_,
                        self->deferredDecodePage_);
        }
        self->deferredDecodeSpine_ = -1;
        self->deferredDecodePage_ = -1;
      }
    }
  } deferHeavyGuard{this, firstDraw, renderSpine, pageNo};
  // v190：每次 render 結束都覆寫 heal 旗標，避免舊頁 sticky。解構涵蓋所有 return。
  struct HealNoteGuard {
    EpubReaderActivity* self;
    // v190：起點在建構時抓，量的是【本次 render 之內】的增量。若拿「上次 render 結束時」當基準，
    // 任何在兩次 render 之間發生的 render-remembered 都會記到下一頁頭上——乾淨的純文字頁會被
    // 標成待癒合（審查抓到）。
    int spine;
    int page;
    uint32_t startCount = ImageBlock::rememberedPlaceholderCount();
    ~HealNoteGuard() {
      const uint32_t seen = ImageBlock::rememberedPlaceholderCount();
      self->imageHealSeen_ = seen;
      if (seen > startCount) {
        // v193（複查）：同樣改吃快照 —— v190 這裡也在解構時讀易變成員，是同一個潛在的錯頁。
        self->imageHealSpine_ = spine;
        self->imageHealPage_ = page;
      } else {
        self->imageHealSpine_ = -1;
        self->imageHealPage_ = -1;
      }
    }
  } healNoteGuard{this, renderSpine, pageNo};
  const int fontId = SETTINGS.getReaderFontId();

  // The image pixel-cache RAM slot lives for exactly one page render (it feeds
  // the BW double-refresh and every grayscale band pass); release it on every
  // exit so nothing stays resident across page turns.
  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  // v110/v164：先問「快取裡的字是不是就是這一頁的」。相符就整段跳過（掃描＋prewarm 的
  // 250-330ms SD 讀取都省下），不相符才走冷路徑。⚠️ scope 用 optional 放在函式作用域：
  // 抗鋸齒兩趟在下面才跑、還要用這一頁的字 —— scope 提早解構（retain=false 時）會把
  // 快取清掉，AA 整頁走 miss ring（丹布朗 3-5 秒 AA 延遲的形狀）。
  auto* fcm = renderer.getFontCacheManager();
  const WarmIdentity current = buildWarmIdentity(pageNo);
  const bool warmHit = fcm && fcm->warmIdentity().matches(current);
  diagWarmHit = warmHit ? 1 : 0;
  // wcum 只數翻頁不數重繪 —— 選單關閉必 miss（該次 render 尾端的預取已把快取換成 N+1）、
  // 書籤彈窗關閉必 hit（預取被彈窗閘門擋掉），混進去會把判準兩個方向都污染。
  if (currentSpineIndex != lastWarmSpine || pageNo != lastWarmPage) {
    diagWarmCumTotal++;
    if (warmHit) diagWarmCumHits++;
    lastWarmSpine = currentSpineIndex;
    lastWarmPage = pageNo;
  }
  std::optional<FontCacheManager::PrewarmScope> scope;
  if (!warmHit) {
    scope.emplace(fcm->createPrewarmScope());  // ctor 清掉舊快取（可能是別頁的殘留）
    // 背景重排視窗是最大瞬時壓力源（diag6：14 次 alloc_fail 全落在 build=1 內）——
    // 重排期間不保留，記憶體行為與無預取時逐位元組相同。
    scope->setRetainCacheOnExit(!section->isBuilding());
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
    // 採用【有條件】：nullptr 述詞不會中止，false 只可能是硬失敗（該字面快取已整組釋放）。
    // 失敗不採用 —— 身分蓋在空快取上會讓 warm=1 掩蓋 ring 慢頁。
    if (scope->endScanAndPrewarmAbortable(nullptr, nullptr)) {
      fcm->adoptWarmIdentity(current);
    }
  } else {
    // 只歸零統計、不碰快取：診斷行印 per-render 差分，沿用上一頁的數字會誤導。
    fcm->resetStats();
  }
  // codex 複查：pf 只在「這一頁正是預取的那頁」時才有意義 —— 跳頁/返回時歸零，
  // 免得 warm=0 旁掛著別頁的漂亮 pf。
  if (!warmHit) diagPrefetchMs = 0;
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  // The reader starts with zero here, which means the normal refresh cycle
  // would use a HALF refresh for its first page. Keep that same clean base for
  // image pages: their double-FAST path otherwise runs directly over the
  // retained frame after a silent restart (for example, when returning from
  // KOReader sync), leaving the old UI mixed with the image.
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  // v169（diag168 定案）：字型 mini 這一頁被降級（ladder 丟字）時跳過文字 AA ——
  // AA 兩趟會把每個被丟的字再走兩次 miss ring（每字一次 SD 往返），實測 lsb 趟
  // 3.4-4.4 秒且 img=0（純文字頁），正是使用者的「翻頁後 3-5 秒才有 AA、期間按鍵
  // 無效」。降級只發生在建置視窗的高壓頁；壓力一過 AA 自動回來。BW 文字仍完整
  // 可讀 —— 按「不卡是主判準」（維護者拍板），一頁暫時沒有 AA 好過凍住 4 秒。
  bool fontDegradedThisRender = false;
  if (const auto* rf = sdFontSystem.currentReaderFont()) {
    fontDegradedThisRender = rf->getStats().bitmapGlyphsDropped > 0;
  }
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing && !fontDegradedThisRender;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Whole-plane buffering only pays when the BW refresh genuinely runs async
  // underneath it; on blocking panels (X3) it would just spend ~50 KB for the
  // identical serial timing. Image pages take the blocking double-FAST path
  // below (no async refresh is ever started), so they'd spend the buffers with
  // nothing in flight to overlap.
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();

    // v148（codex 複查後重排順序）：冷圖片的解碼【提前到這裡】做完，再重建 prewarm。
    //
    // 原本冷圖片是在下面的 page->render() 中途才解碼，而低記憶體 relief 會卸載字型
    // —— 把上面剛建好的整頁 prewarm（mini glyph/bitmap/advance）全部摧毀。之後
    // 灰階分帶把整頁重畫 14 趟，且 strip culling 在 getGlyph() 【之後】——
    // 每一帶都整頁從 SD 重新載字，最壞情況一頁多花數秒、低堆積下還可能掉字。
    //
    // 順序改成：佔位框已閃現（使用者有回饋）→ renderImages 解碼＋寫 .pxc 快取
    // （relief 在這裡觸發，此刻的 prewarm 反正要重建，摧毀無所謂）→ 重跑一次
    // scan+prewarm（字型已由 relief 的 restore 載回）→ 之後 BW 與灰階全部快取命中。
    // 代價：冷圖片頁多付一次 prewarm（約 300ms，只在首次看到該頁時）。
    page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderer.clearScreen();
    // v164：rescan 沿用函式作用域的 optional scope —— 舊寫法的區域 scope 在區塊尾解構，
    // BW 與 AA 都在解構之後才跑。解碼期間的 relief（unloadAll）已把 warm 身分機制性失效
    // （SdCardFontManager 的掛鉤），這裡重建快取並在成功時重新採用。
    scope.reset();
    scope.emplace(fcm->createPrewarmScope());
    scope->setRetainCacheOnExit(!section->isBuilding());
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan only
    if (scope->endScanAndPrewarmAbortable(nullptr, nullptr)) {
      fcm->adoptWarmIdentity(current);
    }
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // Image pages intentionally bypass the regular refresh cadence. Preserve
      // a pending clean base before their double-FAST grayscale pipeline.
      if (cleanImageBasePending) {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        // 圖片頁刻意留在 HALF（=GC 清底，v130 同）；scrub bench 時記下來，免得這次閃黑被算到 scrub 頭上。
        if (ReaderUtils::scrubCleanActive(renderer)) DiagLog::line("CLEAN img bank=%u", static_cast<unsigned>(renderer.lastRefreshBank()));
      }
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      if (ReaderUtils::scrubCleanActive(renderer)) DiagLog::line("CLEAN img bank=%u", static_cast<unsigned>(renderer.lastRefreshBank()));
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    // Async form: start the waveform and return so the grayscale plane rendering
    // below overlaps the panel's refresh time instead of following it.
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band, leaving the BW
  // framebuffer intact so no full-frame storeBwBuffer is needed; controller
  // RAM is re-synced from the live framebuffer afterward. The page is
  // re-rendered ceil(H/STRIP_ROWS) times per plane, but renderCharImpl culls
  // out-of-band glyphs before decode so the cost stays close to one render.
  // Both text (drawPixel) and images (DirectPixelWriter) honor the active
  // strip target. When the BW refresh above went out async, the plane
  // rendering below overlaps the panel's refresh time; only the controller
  // RAM writes wait for BUSY.
  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    // Render one plane band-by-band into a whole-plane buffer without touching
    // the controller, so it can run while the refresh is still in flight.
    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    // Tiered on heap pressure: two plane buffers hide both plane renders
    // inside the refresh wait; one hides the LSB render (its buffer is reused
    // for MSB after streaming); none falls back to the strip-scratch flow with
    // no overlap. Each buffer is only attempted when it leaves ~60 KB free so
    // the pass never starves concurrent allocations: the next page re-render
    // allocates through throwing std::string paths that abort() on OOM under
    // -fno-exceptions, so a plane buffer that "fits" but eats the render
    // headroom is worse than the strip fallback. Blocking panels skip the
    // buffers entirely (nothing to overlap).
    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    // Free-heap alone ignores fragmentation: taking the largest block for a
    // plane can leave only slivers behind even when total headroom looks fine.
    // Require the block to fit the plane with 16 KB contiguous to spare, which
    // also keeps the advance-table batch scratch viable mid-render (same
    // rationale as BACKGROUND_BUILD_MIN_MAX_ALLOC).
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
      DiagLog::line("SEG tiled-async prewarm=%lu bw=%lu disp=%lu gray=%lu wait=%lu gdisp=%lu total=%lu",
                    tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay,
                    tWait - tGrayRender, tGrayDisplay - tGrayWrite, tEnd - t0);
    } else {
      // Per-strip scratch tier: blocking panels (X3) and the OOM fallback.
      // The strip writes below need the panel idle, so wait out any pending
      // async refresh first (no-op on blocking panels).
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        // Bands may be streamed in any order: X4 windows each via setRamArea,
        // X3 via PTL.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        // MSB plane.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        // BW framebuffer is intact; re-sync controller RAM for the next
        // differential page turn directly from it.
        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
        // v153：X3 抗鋸齒的主路徑 —— 使用者回報「AA 沒顯示出來畫面就不動」，這一行是唯一證人。
        DiagLog::line("SEG tiled prewarm=%lu bw=%lu disp=%lu lsb=%lu msb=%lu gdisp=%lu clean=%lu total=%lu "
                      "warm=%u pf=%u wcum=%lu/%lu img=%u dec=%u pg=%u pmax=%u pret=%u",
                      tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay,
                      tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0,
                      static_cast<unsigned>(diagWarmHit), diagPrefetchMs,
                      static_cast<unsigned long>(diagWarmCumHits), static_cast<unsigned long>(diagWarmCumTotal),
                      static_cast<unsigned>(pageHasImages ? 1 : 0),
                      static_cast<unsigned>(pageHasImagesNeedingDecode ? 1 : 0), static_cast<unsigned>(diagPfGate), static_cast<unsigned>(diagPfMaxKb), static_cast<unsigned>(diagPfRetKb));
      }
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
      // v153：這幾行計時一直存在，但 LOG_DBG 在 gh_release（LOG_LEVEL=1）展開為空 ——
      // 使用者回報「翻頁很慢」時我們手上沒有任何逐頁毫秒數。鏡射進 diag.log。
DiagLog::line("SEG prewarm=%lums bw_render=%lums display=%lums total=%lums warm=%u pf=%u wcum=%lu/%lu pg=%u pmax=%u pret=%u", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0, static_cast<unsigned>(diagWarmHit), diagPrefetchMs, static_cast<unsigned long>(diagWarmCumHits), static_cast<unsigned long>(diagWarmCumTotal), static_cast<unsigned>(diagPfGate), static_cast<unsigned>(diagPfMaxKb), static_cast<unsigned>(diagPfRetKb));
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
      // v153：這幾行計時一直存在，但 LOG_DBG 在 gh_release（LOG_LEVEL=1）展開為空 ——
      // 使用者回報「翻頁很慢」時我們手上沒有任何逐頁毫秒數。鏡射進 diag.log。
DiagLog::line(
              "SEG prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
      // v153：這幾行計時一直存在，但 LOG_DBG 在 gh_release（LOG_LEVEL=1）展開為空 ——
      // 使用者回報「翻頁很慢」時我們手上沒有任何逐頁毫秒數。鏡射進 diag.log。
DiagLog::line("SEG prewarm=%lums bw_render=%lums display=%lums total=%lums warm=%u pf=%u wcum=%lu/%lu pg=%u pmax=%u pret=%u", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0, static_cast<unsigned>(diagWarmHit), diagPrefetchMs, static_cast<unsigned long>(diagWarmCumHits), static_cast<unsigned long>(diagWarmCumTotal), static_cast<unsigned>(diagPfGate), static_cast<unsigned>(diagPfMaxKb), static_cast<unsigned>(diagPfRetKb));
    }
  }

  // v167（crash_report166 定案）：走到這裡 = 這一頁畫完了。若這輪渲染把堆積打到了
  // 地板（丹布朗類書的建置＋AA 疊加），保留中的 mini 快取就是下一輪的死重 —— 放手，
  // 寧可下一頁冷（scope 解構時 clearCache＋身分失效）。
  if (scope.has_value() && heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 24 * 1024) {
    scope->setRetainCacheOnExit(false);
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book. Use the estimated total while a giant spine is still building so
  // "page X of Y" and the progress bar don't read off the small build watermark.
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->estimatedTotalPages();
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section->isBuilding());
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) {
    return;
  }
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    // Record the exact content offset so the bookmark lands correctly after any re-pagination.
    // currentPageVisibleOffset was captured for this very page at its last render.
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
