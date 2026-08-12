#include "EpubReaderActivity.h"
#include <DataDir.h>

#include <BuildScratch.h>
#include <Epub/BuildDiag.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>

#include "ble/BleRemoteManager.h"
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SilentRestart.h"
#include <SdCardFont.h>

#include "util/DiagLog.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include <Epub/blocks/ImageBlock.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>

#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "Epub/ParsedText.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"

namespace {
// ImageBlock low-memory relief hooks: temporarily unload the SD reading font so a
// first-time image decode gets the ~44 KB it needs, then reload it. ctx is the
// GfxRenderer the reader draws with. Both are no-ops if no SD font is loaded.
// v55 曾在這裡加過「常駐量 < 16KB 就不卸字型」的門檻(想幫明體使用者省下重載時間),
// **複查後撤除**。理由是它量錯了東西而且失效方向最壞:
//   ① 這個 hook 只在【圖片頁】觸發,而圖片頁的文字通常很少 → miniBitmap 很小 →
//      明體的常駐量正好落在 14KB 上下(兩個字面的 advance 表 768×8×2 = 12,288B 是大宗),
//      剛好被 16KB 擋掉。也就是說門檻專挑最需要救援的那些頁失效。
//   ② 「釋放多少位元組」本來就不是對的指標——解碼要的是【夠大的連續塊】,而釋放一塊
//      6KB 的表若正好夾在兩段空閒之間,合併後可能解鎖遠大於 6KB 的連續空間,非線性。
//   ③ 失效代價不對稱:多卸一次只是慢不到一秒;少卸一次會讓解碼失敗,而
//      ImageBlock::rememberImageFailure 會把那張圖記成佔位框、整個 session 不再重試
//      (v24 才修好的掉圖行為)。
// 先把 residentBytes 記進 diag.log 拿到真實數字,再談要不要優化。
void imageDecodeFreeHeap(void* ctx) {
  sdFontSystem.unloadForLowMemory(*static_cast<GfxRenderer*>(ctx));
}
void imageDecodeRestoreHeap(void* ctx) {
  sdFontSystem.ensureLoaded(*static_cast<GfxRenderer*>(ctx));
}


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
  Activity::onEnter();

  // v96: the remote only matters in here, and this is when it gets pressed -- run the
  // reconnect scan continuously for as long as the book is open. Paired with onExit().
  BLE_REMOTE.setReaderActive(true);

  if (!epub) {
    return;
  }

  // v54:進閱讀器的第一次 render 不再強制 HALF 刷新。原本成員初值 0 → displayWithRefreshCycle
  // 的 `<= 1` 判斷成立 → 首頁必走 HALF,而 HALF 在 X3 被驅動提升成「FULL+condition+settle」
  // 三段刷新(106 frames ≈ 2.8 秒)。改成一開始就給滿一個週期。
  // 觀察點(實機實測):從主畫面切進來畫面內容全變,不清底可能留殘影——不可接受就把這行拿掉。
  pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();

  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setMemoryReliefHooks(&imageDecodeFreeHeap, &imageDecodeRestoreHeap, &renderer);
  // Bold-reading mode is baked into layout at parse time (ParsedText::addWord); the
  // setting also participates in the section cache header, so a toggle re-lays out.
  ParsedText::setBoldBodyText(SETTINGS.boldBodyText != 0);

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  // v110:身分裡的「哪一本書」。cachePath 是 Epub 建構時由書檔路徑雜湊出來的成員,
  // 在閱讀器開著的期間不會變(移到 /Read 的重新命名發生在 onExit),所以算一次就夠。
  warmBookHash = WarmIdentity::fnv1a(epub->getCachePath().c_str());

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
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
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  BLE_REMOTE.setReaderActive(false);  // v96: back to the thin idle scan outside the reader

  // The relief hooks capture this activity's renderer; clear them before the
  // activity is deleted so a later non-reader image decode never calls back in.
  ImageBlock::setMemoryReliefHooks(nullptr, nullptr, nullptr);

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

  // Record whole-book progress on the recents entry; shown as "(NN%)" on the author line.
  // Three cases: at end-of-book the section is already reset, so record 100 explicitly;
  // mid-footnote the current position is the footnote target (often near the book's end),
  // not the reading origin, so skip and keep the last recorded value (progress.bin above
  // already persisted the correct origin); otherwise mirror openReaderMenu's computation
  // while the section is still alive.
  if (epub && currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    RECENT_BOOKS.setProgress(epub->getPath(), 100);
  } else if (footnoteDepth == 0 && epub && section && epub->getBookSize() > 0 && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    const int pct = std::max(0, std::min(100, static_cast<int>(bookProgress + 0.5f)));
    RECENT_BOOKS.setProgress(epub->getPath(), static_cast<uint8_t>(pct));
  }

  // v110:離開閱讀器(含睡眠,必經 onExit)是最後一個失效點——下一個進來的 activity
  // 不該繼承這頁的 warm identity。
  //
  // 這裡要 clearCache() 而不是只 invalidateWarm():retain 開著時最後一次 render 的
  // mini 資料(intervals+glyphs+bitmap+kern,v107 diag 實測密集中文頁 35,620 B)會一直
  // 掛在堆上,而身分才剛失效 ⇒ 它【永遠不可能】再命中,是純粹的死重。留著會真的傷到
  // 下一個畫面:HomeActivity::loadRecentCovers → generateThumbBmp 的 PNG 路徑要約 52KB
  // 連續塊,被這 35KB 卡住就解碼失敗,而 HomeActivity 會就此把封面從該筆最近閱讀移除。
  // clearCache() 自己第一行就是 invalidate(Task 2 做在機制裡),所以身分語意完全不變,
  // 而離開閱讀器之後的堆狀態與 v109 逐位元組相同。
  if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();
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
                           applyOrientation(menu.orientation);
                           toggleAutoPageTurn(menu.pageTurnOption);
                           // 字級與方向同語意:彈窗選定即生效(選單取消也套用)。設定先提交,重啟與否分開判斷:
                           // - 內建 fallback 字型(v30 OMIT_FONTS)四級同一字面 → 字面沒變就不重排重啟(免白重啟)
                           // v129:GO_HOME 已移出選單(Back 短按即回主畫面),原本「選了回主畫面就不重啟、
                           // 把重排延到下次開書」的特例隨之消失——現在改了字級/行距/粗體後離開選單一律照
                           // needsReflow 判斷。行為與原本「按 Back 離開選單」那條路徑一致(isCancelled=true
                           // 時 goingHome 本來就是 false),所以這不是新語意,只是少了一個出口。
                           const bool fontSizeChanged = menu.fontSize != SETTINGS.fontSize;
                           const bool lineSpacingChanged = menu.lineSpacing != SETTINGS.lineSpacing;
                           const bool boldChanged = menu.boldBody != SETTINGS.boldBodyText;
                           if (fontSizeChanged || lineSpacingChanged || boldChanged) {
                             const uint8_t oldFontSize = SETTINGS.fontSize;
                             SETTINGS.fontSize = menu.fontSize;
                             SETTINGS.lineSpacing = menu.lineSpacing;
                             SETTINGS.boldBodyText = menu.boldBody;
                             SETTINGS.saveToFile();
                             // 行距/粗體都在 section 檔頭,改了必然要重排;字級則要問登錄表字面
                             // 是否真的會變(sizeChangeTakesEffect 註解有 v38 誤判的教訓)
                             bool needsReflow = lineSpacingChanged || boldChanged;
                             if (fontSizeChanged) {
                               bool faceChanges = sdFontSystem.sizeChangeTakesEffect(oldFontSize, menu.fontSize);
#ifndef OMIT_FONTS
                               if (SETTINGS.sdFontFamilyName[0] == '\0') faceChanges = true;
#endif
                               needsReflow = needsReflow || faceChanges;
                             }
                             if (needsReflow) {
                               restartToReflow();
                               return;  // 重啟中
                             }
                           }
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                           }
                         });
}


void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
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
    // v110 複審修正:在 inflate 峰值之前清,而不是等下面那個 pump 分支。
    // 這裡的 startBuild 會把整個 spine 的 HTML inflate 進來(DEFLATE 的 32KB 回溯視窗),
    // 而 pump 的 clearCache(下一個 if 區塊)只會在 startBuild【回來之後】的下一輪才跑
    // ——峰值早就過了。保留中的 mini 快取(最壞約 35KB)在這個視窗裡是純負擔。
    if (auto* fcm = renderer.getFontCacheManager(); fcm && fcm->warmIdentity().valid) {
      fcm->clearCache();
    }
    if (!section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                             SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, buildViewportWidth,
                             buildViewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                             SETTINGS.imageRendering, SETTINGS.focusReadingEnabled,
                             SETTINGS.boldBodyText != 0)) {
      // Not fatal: the partial keeps serving its pages; crossing the watermark falls back to
      // the blocking extension in render(). Don't retry every tick.
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  // Drive any in-progress incremental section build forward, off the page-turn critical path,
  // but only within a small window ahead of the reader: an unbounded build monopolized the
  // RenderLock and locked out page turns. The build follows the reader instead, and instant
  // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // While extending a partial (rebuild from a previous session), pageCount is pinned at the
  // partial's watermark until the build catches up, so the window check would wrongly read
  // "far enough ahead" and stall the build at 0 pages -- then the first turn past the
  // watermark re-parses the whole chapter synchronously. Keep ticking until it finalizes.
  if (section && section->isBuilding() && !RenderLock::peek() &&
      (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD)) {
    RenderLock lock;
    // v110:spec 失效表 #3 的機械化。背景重排一旦開跑,身分就不可信(頁碼與版面都在變動中),
    // 而保留中的 mini 快取正是 build 視窗最不該握著的東西——那是 p2 最大的瞬時壓力源
    // (CLAUDE.md:ParsedText 逐詞小配置塞進約 65KB,最大連續塊被壓到 5-25KB;diag6 的
    // 14 次 alloc_fail 與 355 個 dropped 全部落在 build=1 內)。在 pump 的第一個 tick、
    // ParsedText 還沒開始堆積之前釋放,之後整個 build 視窗的記憶體行為就與 v109 相同。
    // valid 守衛讓後續每個 tick 免費:清完身分即失效,這個分支就不再進來。
    // 持 RenderLock 下執行:render task 此刻必不在 renderContents 裡動快取,跨任務安全。
    // 刻意放在下面的 isBuilding() 重檢【之前】——重檢失敗代表 build 剛好在這個空隙結束,
    // 那也正是「這份身分跨越過一次重排」的情況,寧可保守清掉(代價只有一次冷 render)。
    if (auto* fcm = renderer.getFontCacheManager(); fcm && fcm->warmIdentity().valid) {
      fcm->clearCache();
    }
    // Re-check under the lock: render() (which also holds the RenderLock) may have finalized the
    // build between the outer isBuilding() check and acquiring the lock here, in which case
    // buildSomeMore() would fail and wrongly reset the section. cppcheck can't see the cross-task
    // mutation, so it flags this as always true.
    // cppcheck-suppress knownConditionTrueFalse
    if (section->isBuilding()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        // The chapter re-paginated since the saved progress (settings changed): we now know the
        // real page count, so re-render at the remapped page. No-op for an unchanged resume.
        requestUpdate();
      }
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

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
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

  // Enter reader menu activity on short-press Confirm. A long-press that fired a bound
  // function (e.g. bookmark) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
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

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

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

void EpubReaderActivity::restartToReflow() {
  // 字級已寫入設定,section 檔頭不再匹配 → 重啟開書時自動重排本書一次(v31 行距/粗體同機制)。
  // ESP.restart 不經 onExit,故先手動補 onExit 的兩個不變量(對抗式複查抓到的繞過路徑):
  // ①註腳中:progress.bin 已被 render 寫成「註腳位置」,原點只在 RAM savedPositions →
  //   先把註腳前原點寫回(同 onExit;漏了=重開書落在註釋章,原位置永久遺失)。
  // ②最近閱讀 %:onExit 的 setProgress 不會跑 → 依 onExit 同式先更新(EoB=100/註腳跳過/一般三分支)。
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }
  if (epub && currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    RECENT_BOOKS.setProgress(epub->getPath(), 100);
  } else if (footnoteDepth == 0 && epub && section && epub->getBookSize() > 0 && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
    const int pct = std::max(0, std::min(100, static_cast<int>(bookProgress + 0.5f)));
    RECENT_BOOKS.setProgress(epub->getPath(), static_cast<uint8_t>(pct));
  }
  // 重啟路徑 = v31 OPDS 下載完開書同款;silentRestartToReader 自帶「載入中」彈窗,
  // 這裡不另畫提示(雙彈窗寬度不同會殘留外框、又多一次全螢幕刷新)。
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  silentRestartToReader();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
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
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
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

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this](const char* where) {
    renderer.clearScreen();
    // Show WHERE the build failed + spine index + heap + inflate/scratch breadcrumbs ON SCREEN (no
    // serial needed): `where` names the call site, inf/cl the DEFLATE-inflate path ('?'=inflate never
    // ran), m the largest free block. Report this whole line back for diagnosis.
    char msg[112];
    const char* reason =
        builddiag::note[0] ? builddiag::note : (builddiag::parseError[0] ? builddiag::parseError : "?");
    // v96: b= is the largest free block startBuild ACTUALLY saw (captured at its first line,
    // while the FrameBufferLoan still had the 52,272-byte framebuffer handed over); m= is the
    // block now, after the loan gave it back. Printing only m= made a well-fed build look starved.
    snprintf(msg, sizeof(msg), "Bfail sp=%d @%s %.20s b=%uk m=%uk", currentSpineIndex, where, reason,
             static_cast<unsigned>(builddiag::maxAllocAtBuildStart / 1024),
             static_cast<unsigned>(ESP.getMaxAllocHeap() / 1024));
    GUI.drawPopup(renderer, msg);
    automaticPageTurnActive = false;
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
    automaticPageTurnActive = false;    return;
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

  if (!section) {
    // v110:section (re)build 是所有跳頁/換章/註腳/百分比/方向/自動翻頁切換的收斂點
    // (它們一律先 section.reset() 再靠這個分支重建)。在這裡失效一次,涵蓋 spec
    // 失效 #1/#2/#3/#5 的大半;沒被這裡擋到的部分(不 reset 直接改 currentPage 的
    // progress-sync/applyDeferredReposition/pendingPageJump)由 render() 開頭逐欄位
    // 身分比對兜底。
    //
    // v110 複審修正:這裡要 clearCache() 不是 invalidateWarm()。invalidate 只翻旗標、
    // 一個位元組都不釋放,而換章/跳頁之後這份快取【永遠不可能再命中】(matches() 兩邊都
    // 要求 valid)—— 純死重。它接著會被一路握過 loadSectionFile / startBuild 的整份 HTML
    // inflate(32KB 連續塊,FrameBufferLoan 就是為它存在的)與同步 buildSomeMore 追趕,
    // 直到 renderContents 的 PrewarmScope ctor 才釋放。那正是 diag6 量到 14 次 alloc_fail
    // 與 355 個 dropped 的視窗,而 v109 進這個視窗時快取是空的。清掉才能讓整個章節建置
    // 視窗的記憶體狀態回到 v109;因為快取已死,清它一毛錢都不花(這條路徑本來就是冷 render)。
    if (auto* fcm = renderer.getFontCacheManager()) fcm->clearCache();
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
    const bool cacheLoaded = section->loadSectionFile(
        SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
        SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.boldBodyText != 0);
    if (cacheLoaded) {
      // Matching render params means identical pagination, so the saved page number is valid
      // as-is: consume any pending settings-change reposition. Without this, a chapter total
      // saved while the section was still building (i.e. a watermark, not the real count)
      // would remap the resume page against the finalized count and teleport the reader.
      cachedChapterTotalPageCount = 0;
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
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
        if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                        SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                        viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                        SETTINGS.imageRendering, SETTINGS.focusReadingEnabled,
                                        SETTINGS.boldBodyText != 0, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();  // restore before anything draws
          showBuildError("persist");
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
          // Lend the framebuffer's 48 KB to the blocking pre-render burst
          // (startBuild inflates the whole spine HTML — the memory peak). The
          // background buildSomeMore chunks in loop() do NOT get the loan: they
          // deliberately interleave with page renders. Restored before render.
          GfxRenderer::FrameBufferLoan loan(renderer);
          if (!section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                   SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                   viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                   SETTINGS.imageRendering, SETTINGS.focusReadingEnabled,
                             SETTINGS.boldBodyText != 0)) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            loan.end();  // restore before anything draws (showBuildError renders a popup)
            showBuildError("startB");
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump ? !section->findAnchor(pendingAnchor) : static_cast<int>(section->pageCount) <= target)) {
            // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
            // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
            // Otherwise: build until the target page exists. loop() builds the rest behind it.
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              loan.end();  // restore before anything draws (showBuildError renders a popup)
              showBuildError("bsm1");
              return;
            }
          }
          loan.end();
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
  // Lend the framebuffer's ~48KB as scratch for the whole-chapter HTML inflate (the DEFLATE 32KB
  // back-reference window) that startBuild() runs. Without it the inflate mallocs a 32KB contiguous
  // block from a possibly-fragmented heap and "Failed to stream item contents" spuriously fails a
  // perfectly readable chapter when the largest free block is just under 32KB (the fresh-open build
  // at createSectionFile already gets this loan; the partial-extension path was missing it). The
  // INDEXING popup drawn above is what the panel holds during the lend; end the loan before any
  // showBuildError()/page render draws (the loan restores the framebuffer white on scope exit).
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
      // v110 複審修正:同上,在 startBuild 的 inflate 峰值之前釋放保留中的 mini 快取。
      // 這條路是「跨過 partial 的水位線」——它自己的註解就說可能是【數十秒】的前綴重排,
      // 而 :1079 的失效點在 section 非 null 時根本不會執行,pump 的清除也進不來
      // (render() 此刻正持著 RenderLock)。valid 守衛讓後續每一圈免費。
      // ⚠️ 這一條與另外兩條不同,不是純粹的死重:這份身分的 pageNumber 就是即將要畫的
      // 這一頁,build 結束之後【本來可以】warm 命中。用一次冷 render(約 250-330ms SD)
      // 換這個歷史上會失敗的視窗回到 v109 的記憶體狀態 —— spec §7 的回退條件是
      // dropped/alloc_fail,不是多一次 prewarm。
      if (auto* fcm = renderer.getFontCacheManager(); fcm && fcm->warmIdentity().valid) {
        fcm->clearCache();
      }
      // Start a build to extend a partial toward the requested page.
      if (!section->isBuilding() &&
          !section->startBuild(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                               SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                               viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                               SETTINGS.imageRendering, SETTINGS.focusReadingEnabled,
                             SETTINGS.boldBodyText != 0)) {
        LOG_ERR("ERS", "Failed to start partial extension build");
        section.reset();
        loan.end();
        showBuildError("partExt");
        return;
      }
      // Extend until either the target page exists or the build completes.
      while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
        if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
          LOG_ERR("ERS", "Failed during incremental section build");
          section.reset();
          loan.end();
          showBuildError("bsm2");
          return;
        }
      }
    }
  }
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError("bsm3");
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
  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;    return;
  }

  updateBookmarkFlag();

  {
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    //
    // v110 複審修正:頁碼【捕捉一次,到處都用同一份】,這是 prefetchNextPage 早就遵守的
    // 紀律(它捕捉 next 之後不再重讀)。原本 render() 讀一次 currentPage 拿去 loadPage,
    // renderContents 再讀第二次去組身分,而中間隔著 loadPage 的 SD I/O(數十毫秒),
    // 主任務的手動/傾斜翻頁又是【不持鎖】改 currentPage 的(pageTurn 只有跨章分支才鎖)。
    // 一次落在那個空窗的按鍵 ⇒ 快取裡是第 N 頁的字,身分卻寫著 N+1 ⇒ 下一次 render
    // 假 warm 命中,整頁走 overflow ring、用的還是 N 的 mini kern 表,而 diag 印 warm=1、
    // prewarm_ms=0。捕捉之後,同一個空窗只會退化成一次無害的冷 render。
    const int pageNo = section->currentPage;
    auto p = section->loadPage(pageNo);
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
        renderer.displayBuffer();        return;
      }
      requestUpdate();  // Try again after clearing cache      return;
    }
    pageLoadRetryCount = 0;  // Reset the retry counter once a page loads cleanly

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), pageNo, orientedMarginTop, orientedMarginRight, orientedMarginBottom,
                   orientedMarginLeft);
    const auto renderMs = millis() - start;
    // v109: how long the user actually sat on the previous page, measured from the moment its
    // render finished to the moment this one started.
    //
    // This is the precondition for the one optimisation the numbers still allow. CLAUDE.md's
    // panel model says the 441 ms of `disp` is the panel pushing e-ink particles while the CPU
    // does nothing (waitBusy polls through delay(1), which is vTaskDelay -- it yields). The most
    // expensive part of the NEXT page is ~250-300 ms of SD seeks for its glyphs, and it would
    // fit inside that dead window. The blocker used to be that antialiasing's two extra passes
    // still needed the CURRENT page's glyphs after `disp`, leaving nowhere to stage page N+1 --
    // but with AA off that constraint is gone.
    //
    // Whether prefetching pays depends entirely on a number nobody has measured: if pages are
    // turned faster than the prefetch completes, it is wasted work and can make things worse.
    // Measure first; the reflow/prewarm code is where v54's critical and v90-v94's arena failure
    // both happened, and it does not get touched on a guess.
    static unsigned long lastRenderEndMs = 0;
    const unsigned long dwellMs = lastRenderEndMs == 0 ? 0 : (start - lastRenderEndMs);
    lastRenderEndMs = millis();
    LOG_DBG("ERS", "Rendered page in %dms", renderMs);

    // v53 量測(暫時)。n 是 render 次數不是翻頁次數(選單關閉/書籤/截圖也會重繪),
    // 但取樣密度足夠。核心待驗假說 = max_single:字型 prewarm 每個 style 各配一塊連續
    // miniBitmap(推估 35-50KB),與歷史量到的 40KB 連續區塊天花板高度吻合。
    // 逐次更新 running max,避免峰值落在沒被取樣的那 9 次(頁與頁的字數差異極大)。
    if (renderMs > diagMaxRenderMs) diagMaxRenderMs = static_cast<unsigned>(renderMs);
    const auto* font = sdFontSystem.currentReaderFont();
    if (font) {
      // stats_ 每次 render 被 PrewarmScope 歸零,故 running max / 累計都要自己留。
      if (font->getStats().maxSingleBitmapAlloc > diagMaxBitmapAlloc) {
        diagMaxBitmapAlloc = font->getStats().maxSingleBitmapAlloc;
      }
      diagAllocFailTotal += font->getStats().bitmapAllocFailures;
      diagDroppedTotal += font->getStats().bitmapGlyphsDropped;
      diagReuseHits += font->getStats().prevPageGlyphHits;
      diagReuseTotal += font->getStats().prevPageGlyphTotal;
    }
    // 慢頁(>2 秒)無條件記一筆:4-5 秒那種卡頓通常發生在進書的第 1 次 render,
    // 只靠每 10 次的取樣永遠抓不到它的細節(v53 複查指出)。
    const bool slowPage = renderMs > 2000;

    // v123:這段【不能】待在下面的取樣區塊裡。v122 把它放在每 10 頁才跑一次的分支中,
    // 而 lastFailPath 只有一格 —— 取樣之間發生的失敗會被後來的失敗覆寫。實測 diag122 兩行
    // IMGFAIL 內容完全相同(都是整頁大圖),第一章那個小圖的失敗就這樣消失了。
    // 現在每次 render 都讀走,一次失敗一行,不會互相蓋掉。
    //
    // 儀器本身:圖片失敗時把「哪一張、哪一個階段、解碼器自己的錯誤碼」寫出來。這台機器
    // 沒有序列埠,LOG_ERR 等於丟掉,而失敗出口有十幾個。lib 不能反向依賴 src/util/DiagLog,
    // 所以由這一層讀走並清空。
    if (ImageBlock::lastFailPath[0] != '\0') {
      DiagLog::line("IMGFAIL stage=%d code=%d path=%s", ImageToFramebufferDecoder::lastFailStage,
                    ImageToFramebufferDecoder::lastFailCode, ImageBlock::lastFailPath);
      ImageBlock::lastFailPath[0] = '\0';
      ImageToFramebufferDecoder::noteFailure(ImageToFramebufferDecoder::FAIL_NONE, 0);
    }
    // v125:成功畫下去的圖也要留下幾何 —— diag124 零 IMGFAIL 卻回報「圖出不來」,
    // 而「畫出來了但被縮成一條線」在失敗碼上完全看不見(v123 的縮放修正有這個可能)。
    if (ImageBlock::lastDrawGeom[0] != '\0') {
      DiagLog::line("IMGDRAW %s", ImageBlock::lastDrawGeom);
      ImageBlock::lastDrawGeom[0] = '\0';
    }

    if (++diagPageCounter % 10 == 0 || slowPage) {
      // 情境欄位:讓「4 秒到底是全刷還是圖片還是排版」下次不用靠推導
      DiagLog::line(
          "SEG n=%u prewarm=%u bw=%u disp=%u lsb=%u msb=%u gdisp=%u clean=%u | path=%u full_in=%d img=%u dec=%u "
          "build=%d strip=%u pf=%u wcum=%u/%u",
          diagPageCounter, diagSeg.prewarm, diagSeg.bwRender, diagSeg.display, diagSeg.grayLsb, diagSeg.grayMsb,
          diagSeg.grayDisplay, diagSeg.cleanup, diagSeg.path, pagesUntilFullRefresh, diagSeg.hasImg,
          diagSeg.decodeImg, section && section->isBuilding() ? 1 : 0, diagSeg.stripRows, diagPrefetchMs,
          static_cast<unsigned>(diagWarmCumHits), static_cast<unsigned>(diagWarmCumTotal));
      if (font) {
        const auto& st = font->getStats();
        DiagLog::line(
            "PAGE n=%u render_ms=%lu dwell_ms=%lu peak_render_ms=%u aa=%u prewarm_ms=%u sd_ms=%u seeks=%u glyphs=%u "
            "bitmap_cum=%u max_single=%u peak_single=%u alloc_fail=%u dropped=%u resident=%u warm=%u "
            "reuse=%u/%u cum_reuse=%u/%u",
            diagPageCounter, static_cast<unsigned long>(renderMs), dwellMs, diagMaxRenderMs, SETTINGS.textAntiAliasing,
            st.prewarmTotalMs, st.sdReadTimeMs, st.seekCount, st.uniqueGlyphs, st.bitmapBytes,
            st.maxSingleBitmapAlloc, diagMaxBitmapAlloc, diagAllocFailTotal, diagDroppedTotal,
            static_cast<unsigned>(sdFontSystem.residentBytes()), static_cast<unsigned>(diagWarmHit),
            st.prevPageGlyphHits, st.prevPageGlyphTotal, diagReuseHits, diagReuseTotal);
      } else {
        DiagLog::line("PAGE n=%u render_ms=%lu peak_render_ms=%u aa=%u (builtin font)", diagPageCounter,
                      static_cast<unsigned long>(renderMs), diagMaxRenderMs, SETTINGS.textAntiAliasing);

      }
      DiagLog::mem("page-turn");
      // v55:第一次取樣拍一張「穩態池佈局」當基準,之後只在【新出現降級】時再拍一次。
      // mem() 只說最大連續塊剩多少,答不出是誰卡在池中間——要抓那顆 ~9,984B 常駐塊得看區塊表。
      // 上限 3 次:降級若反覆發生,「有新降級就再拍一張」會讓【每個】取樣頁都付
      // 逐池 heap walk + 一次 SD 開關檔,反而把量測本身變成慢的原因。
      if (diagPoolDumps < 3 && (diagPageCounter == 10 || diagDroppedTotal != diagDroppedReported)) {
        diagDroppedReported = diagDroppedTotal;
        diagPoolDumps++;
        DiagLog::dumpPools(2048, "reader");
      }
    }
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

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  // v110:這一頁已經完整畫完並送上面板(renderContents 的 displayBuffer 是阻塞的,
  // drawPopup 也自己 displayBuffer),所以從這裡到 render() 返回之間,CPU 是空的而
  // 使用者正在讀這一頁 —— 把下一頁的字先讀進來,花的是 dwell 時間不是翻頁時間。
  // 仍持 RenderLock:loop() 的背景重排與自動翻頁都用 RenderLock::peek() 讓路,不會
  // 卡住;按鍵處理完全不碰鎖,所以中止訊號進得來。
  prefetchNextPage(SETTINGS.getReaderFontId(), orientedMarginTop, orientedMarginLeft);
}

// v110:預取下一頁的字型 mini 資料到【同一塊】快取,不新增任何常駐記憶體。
// 前提(spec §3):當前頁的字在 renderContents 收工之後確定無人使用 —— 灰階兩趟與
// cleanup 都在 renderContents 內完成,狀態列與彈窗走的是 UI 字型。
// 中止 = 乾淨放棄:快取清空 + 身分失效 ⇒ 下一次 render 走冷路徑 = v109 的行為。
void EpubReaderActivity::prefetchNextPage(const int fontId, const int marginTop, const int marginLeft) {
  // 先歸零:pf= 的語意是「即將顯示的這一頁,預取花了多久」。任何一道閘門擋下來都算
  // 「沒有預取」,留著上一次的數字會讓 warm=0 旁邊掛著一個漂亮的 pf=280(v54 的教訓:
  // 量測數字不得因為改動而靜靜變好看)。
  diagPrefetchMs = 0;
  // 重排中:頁數與版面都還沒定案(spec 失效 #3),而且背景重排正是 p2 最大的瞬時壓力源
  // (CLAUDE.md:diag6 的 14 次 alloc_fail 與 355 個 dropped 全部落在 build=1 內)——
  // 那個視窗裡不多握任何東西。
  if (!section || section->isBuilding()) return;
  // 捕捉一次。此後 currentPage 可能被主任務的 pageTurn 改掉,但身分的正確性來自
  // 「下一次 render 逐欄位比對」,不是這裡讀到的值,所以捕捉值永遠是誠實的答案。
  const int next = section->currentPage + 1;
  // 章末:下一頁在另一個 section,而換章一律 section.reset() → 冷路徑,預取無從幫忙。
  if (next >= static_cast<int>(section->pageCount)) return;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;
  // 內建備援字型:FontDecompressor 的 prewarm 是純 CPU 解壓、沒有中止路徑,而這個功能
  // 買的正是那 250-330ms 的 SD 讀取。刻意不預取。
  if (!sdFontSystem.currentReaderFont()) return;
  // 書籤彈窗還在畫面上:它的關閉重繪必定是【同一頁】,預取只會把那次 warm 命中換成冷路徑
  // (清掉本頁的快取去載 N+1,然後重繪本頁再載一次)。淨效果是這個手勢從 0 次 prewarm
  // 變成 2 次,還讓彈窗多留 300ms。彈窗關閉的那次 render 自己會再預取,什麼都沒少。
  if (showBookmarkMessage) return;
  // 按鍵已經排隊了:別開工(開了也只會在第一次輪詢就中止)。
  if (activityManager.isRenderPending()) return;

  const unsigned long t0 = millis();
  bool completed;
  {
    // ctor 的 clearCache() 清掉的正是【剛畫完那一頁】的快取 —— 此刻已無人使用。
    auto scope = fcm->createPrewarmScope();
    // ⚠️ loadPage 必須在 ctor【之後】:Page 物件與上一頁那 20-45KB 的 mini 資料若同時在世,
    // 峰值就是兩者之和,而這台機器是被最大連續塊掐住的。代價是「清完才發現載不到頁」時
    // 白清一次(LUT 空洞 / SD 錯誤,而且邊界檢查已經過了 → 罕見),下一次 render 走冷路徑。
    // 在 RAM 吃緊的裝置上這個取捨是對的。
    auto page = section->loadPage(next);
    // retain 刻意留到 loadPage 成功【之後】才開:否則這條 return 會留下一個「空的但被保留」
    // 的快取。那樣其實也不會出錯(ctor 的 clearCache 已經 invalidate 身分 ⇒ 永不 warm 命中),
    // 但正確性就要靠讀者推導兩層;現在解構子直接看到 retain=false → clearCache(),自明。
    if (!page) return;
    scope.setRetainCacheOnExit(true);
    // scan 模式:drawText 只 recordText 就 return,drawLine/fillRect/drawBitmap 與
    // ImageBlock::render 都在入口 return ⇒ framebuffer 一個位元組都不會動,面板上仍是
    // 剛顯示出去的這一頁。座標傳真值只是為了誠實(scan 模式根本不看)。
    page->render(renderer, fontId, marginLeft, marginTop);
    completed = scope.endScanAndPrewarmAbortable(&EpubReaderActivity::prefetchShouldAbort, this);
  }
  // scope 已解構:completed ⇒ retain=true,字留在快取裡;中止 ⇒ endScanAndPrewarmImpl
  // 已自動把 retain 解除,解構子剛剛做過 clearCache()(見 FontCacheManager.cpp)。

  // 預取自己的 stats 必須在這裡折進累計:下一次 render 的 PrewarmScope ctor 會 resetStats(),
  // 而 render() 的取樣區塊(SEG/PAGE 兩行)跑在預取【之前】⇒ 不折的話,預取造成的
  // alloc_fail / dropped / 單塊峰值會完全隱形,而那三個正是 spec 的回退判準。
  if (const auto* font = sdFontSystem.currentReaderFont()) {
    const auto& st = font->getStats();
    if (st.maxSingleBitmapAlloc > diagMaxBitmapAlloc) diagMaxBitmapAlloc = st.maxSingleBitmapAlloc;
    diagAllocFailTotal += st.bitmapAllocFailures;
    diagDroppedTotal += st.bitmapGlyphsDropped;
  }

  if (completed) {
    // 用捕捉的 next,不重讀 section->currentPage(主任務可能已經翻過去了)。
    fcm->adoptWarmIdentity(buildWarmIdentity(next));
    diagPrefetchMs = static_cast<unsigned>(millis() - t0);
  } else {
    // 半成品快取(已送出去的桶已經寫進去了)。scope 解構已經清過一次,這一行是保險:
    // clearCache() 是冪等的,而且它自己第一行就 invalidate 身分。刻意留著 —— 這條路徑
    // 的正確性不該只靠「另一個檔案的解構子記得幫忙」。
    fcm->clearCache();
    // diagPrefetchMs 維持 0:中止不算一次預取。
  }
}

// 跑在 SdCardFont::prewarmStyle 的輪詢路徑上(每 8 個字一次)。只讀一個 volatile bool,
// 不做 I/O、不寫 DiagLog(v98 的教訓:在別人的堆疊上寫 log 會炸掉那個任務)。
bool EpubReaderActivity::prefetchShouldAbort(void* ctx) {
  // ctx 目前用不到:pending 旗標在全域的 ActivityManager 上(Activity 沒有 manager 成員,
  // Activity::requestUpdate() 走的也是同一個全域)。仍照契約收下 this,日後要加「讀取
  // 活動自身狀態」的條件時不必動到呼叫點。
  (void)ctx;
  return activityManager.isRenderPending();
}

bool EpubReaderActivity::applyDeferredReposition() {
  if (cachedChapterTotalPageCount == 0 || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  // Only remap when the chapter actually re-paginated (e.g. after a settings change). A plain
  // resume has identical pagination, so section->pageCount == cachedChapterTotalPageCount and
  // nothing moves.
  if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
    const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
    int newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
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
  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}

WarmIdentity EpubReaderActivity::buildWarmIdentity(int pageNumber) const {
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
  w.boldBodyText = SETTINGS.boldBodyText != 0;
  w.valid = true;
  return w;
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int pageNo, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  diagSeg = {};  // v54:每次進來先清空,否則關掉抗鋸齒時會印出上一次 tiled render 的殘值
  const int fontId = SETTINGS.getReaderFontId();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  // v110:先問「快取裡的字是不是就是這一頁的」。相符就整段跳過(掃描 + prewarm 的
  // 250-330ms SD 讀取都省下),不相符才走原本的冷路徑。
  auto* fcm = renderer.getFontCacheManager();
  // v110 複審修正:用【load 進來的那一頁】的頁碼,不重讀 section->currentPage
  //(主任務可能已經翻過去了,見 render() 的捕捉點)。身分於是永遠指著快取裡真正那一頁。
  const WarmIdentity current = buildWarmIdentity(pageNo);
  const bool warmHit = fcm && fcm->warmIdentity().matches(current);
  diagWarmHit = warmHit ? 1 : 0;
  // v110 複審修正:wcum 只數翻頁不數重繪 —— 選單關閉必 miss(那次 render 尾端的預取已經
  // 把快取換成 N+1 了)、書籤彈窗關閉必 hit(預取被彈窗閘門擋掉),混進去會把 §7 的判準
  // 兩個方向都污染掉。diagWarmHit 維持 per-render(PAGE 行印的就是「這一次」的結果)。
  if (currentSpineIndex != lastWarmSpine || pageNo != lastWarmPage) {
    diagWarmCumTotal++;
    if (warmHit) diagWarmCumHits++;
    lastWarmSpine = currentSpineIndex;
    lastWarmPage = pageNo;
  }
  std::optional<FontCacheManager::PrewarmScope> scope;
  if (!warmHit) {
    scope.emplace(fcm->createPrewarmScope());  // ctor 清掉舊快取(可能是別頁的殘留)
    // 解構時保不保留,取決於背景章節重排有沒有在跑。
    // 背景重排視窗是 p2 最大的瞬時壓力源(CLAUDE.md:ParsedText 的逐詞小配置會塞進約
    // 65KB,最大連續塊被壓到 5-25KB;diag6 的 14 次 alloc_fail 與 355 個 dropped
    // 【全部】落在 build=1 的視窗內)。重排期間不保留 = 這些視窗內的記憶體行為與 v109
    // 逐位元組相同,而 spec 的回退條件正是 dropped/alloc_fail > 0。
    // 代價只有「重排期間的同頁重繪/預取不會命中」,而預取本來就被 isBuilding 擋掉(Task 5)。
    //
    // ⚠️ 這個開關只管【render 當下已經在重排】的情況。「重排是在上一次保留之後才開跑的」
    // 那四個入口(render() 的 !section 重建、跨水位線的阻塞式延伸、loop() 的延遲延伸起跑、
    // pump 的第一個 tick)各自在 startBuild 之前清一次 —— 少了那四處,上面那句
    // 「與 v109 逐位元組相同」對每一次換章的第一個 render 都是假的(最終複審 Finding B)。
    scope->setRetainCacheOnExit(!section->isBuilding());
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
    // v110 複審修正:採用【有條件】。nullptr 述詞 = 永遠不中止,所以 false 在這條路徑上
    // 只可能是硬失敗(配置失敗 / SD 開檔、seek、短讀)—— 而硬失敗已經把該字面的快取整組
    // 釋放掉了。失敗的 prewarm 不採用:身分蓋在空快取上會讓 warm=1 掩蓋 ring 慢頁
    // (spec §4 row 10 的「失敗」半邊,原本只實作了「中斷」那半邊)。
    //
    // 成功時採用:一次【完整未中止】的 prewarm 剛結束 ⇒ 快取內容就是這一頁。採用身分之後,
    // 同一頁的重繪(關選單/切書籤/截圖)下一次就直接命中,不必等 Task 5 的預取。
    // retain 為 false 時仍然採用是刻意的:scope 解構(在本函式結束、也就是這一行【之後】)
    // 會 clearCache(),而 clearCache() 第一行就是 invalidate ⇒ 那一次 render 最終仍以
    // 「無效身分」收場。順序自洽,少一個分支。
    if (scope->endScanAndPrewarmAbortable(nullptr, nullptr)) {
      fcm->adoptWarmIdentity(current);
    }
  } else {
    // 只歸零統計、不碰快取:PAGE 行印的是 per-render 差分,沿用上一頁的數字會讓
    // prewarm_ms/sd_ms 看起來像是這一頁付的。
    fcm->resetStats();
  }
  const auto tPrewarm = millis();

  // v54 量測:**峰值在世**取樣。v53 的 page-turn 取樣點在 renderContents 回傳之後,
  // 那時 PrewarmScope 已解構、miniBitmap 已釋放 → 峰值從來沒被量到(v53 複查抓到)。
  // 這裡是 miniBitmap 仍在世的時刻,才是真正的天花板。每 10 次取樣一次(對應下一筆 SEG)。
  if (diagPageCounter % 10 == 9) DiagLog::mem("prewarm-live");
  // 取樣本身要走 heap_caps_walk + 一次 SD 開關檔,不能算進下面的繪製段(否則有取樣的那筆
  // SEG 其 bw 欄會被污染,與慢頁觸發的那些不可比)。
  const auto tDiagDone = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  diagSeg.hasImg = pageHasImages ? 1 : 0;  // v54:SEG 的圖片欄位要在 page 被 move 走之前取
  diagSeg.decodeImg = pageHasImagesNeedingDecode ? 1 : 0;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
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
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();
  // v54:前三段對【所有】分支都成立(關掉抗鋸齒時只有這三段,但 disp 正是面板刷新時間,
  // 也就是「進書第一頁不強制全刷」要驗的那個數字),先填好再進灰階分支。
  diagSeg.prewarm = static_cast<unsigned>(tPrewarm - t0);
  diagSeg.bwRender = static_cast<unsigned>(tBwRender - tDiagDone);
  diagSeg.display = static_cast<unsigned>(tDisplay - tBwRender);
  diagSeg.path = needsAnyGrayscale ? 2 : 0;  // 進了 tiled 分支會覆寫成 1

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    // v56:有圖的頁改用【更高的帶】。直向時實體橫帶 = 邏輯直條(phyY 由邏輯 X 決定),
    // 所以圖片的每一列在每一趟都要讀——一張 528×728 的 2bpp 圖 = 96KB,7 趟 × 2 個平面
    // 就是【每頁從 SD 讀 1.35MB】,實測 lsb+msb 2,358ms。
    // 逐列只讀「帶內欄位」救不了:列距只有 132B,每個 512B 磁區照樣要抓,I/O 一點都沒省。
    // 唯一有效的槓桿是【減少趟數】:264 列 = 2 趟,讀取量降到 384KB(約 674ms)。
    // 只對有圖的頁放大——純文字頁的 glyph 在解碼前就被剔除了,趟數多寡幾乎不影響它,
    // 卻要白付一塊更大的連續配置。配不到就逐級退回 176 → 80 → 完全不做抗鋸齒(既有行為)。
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    // 依下面 attempts[] 的順序試,配得到就用(不做「先探測再釋放重配」——那會多一次
    // 釋放/重配的空窗)。⚠️ 那個順序【不是】依帶高排的,理由見 attempts[] 上方。
    // pageHasImages 用的是實際變數而非 diagSeg:診斷儀器是暫時的,移除時不該悄悄改掉行為。
    // v60:兩個平面【一趟】畫完(GRAYSCALE_BOTH)。兩個平面的位元來自同一個取樣值,
    // 第二趟從頭到尾是重複工作:同一份版面走訪、同一批 getGlyph、同一次點陣解碼。
    // 代價是第二塊同樣大小的 scratch(80 列 = 7,920B)。實測 73 個真實配置時刻裡,
    // 今天這一塊就有 2 次配不到,兩塊是 4 次——多出來的失敗率約 3%,而失敗就退回
    // 下面的兩趟路徑(逐位元組同輸出),所以這是純上檔。
    int stripRows = STRIP_ROWS;

    std::unique_ptr<uint8_t[]> scratch;     // LSB 平面(單趟模式下就是唯一那塊)
    std::unique_ptr<uint8_t[]> scratchMsb;  // 只有合併模式會有
    bool mergedPlanes = false;

    // ⚠️ 順序依【總走訪趟數】排,不是依帶高排(所以帶高是 264,176,264,176,80,80,
    // 刻意不單調——別「順手把它排回去」)。528 列下的趟數:
    //   merged@264=2、merged@176=3、single@264=4、single@176=6、merged@80=7、single@80=14。
    // 若先把所有合併檔位試完再試單塊,當 p2 最大連續塊落在 [17,424, 34,848) 這段時,
    // 會選到 merged@80(7 趟)而放棄仍配得到的單塊檔位:L≥26,136 時是 single@264(4 趟),
    // L 在 [17,424, 26,136) 時是 single@176(6 趟)——兩者都比 7 趟好。在圖片頁上每一趟
    // 都要把整張快取圖從 SD 重讀一次(直向時實體帶 = 邏輯直條,bandColRange 只能剪欄
    // 不能剪列),等於把 v56 的成果吐回去一部分。
    struct Attempt {
      int rows;
      bool merged;
    };
    Attempt attempts[6];
    int attemptCount = 0;
    if (pageHasImages) {
      attempts[attemptCount++] = {264, true};   // 2 趟
      attempts[attemptCount++] = {176, true};   // 3 趟
      attempts[attemptCount++] = {264, false};  // 4 趟
      attempts[attemptCount++] = {176, false};  // 6 趟
    }
    attempts[attemptCount++] = {STRIP_ROWS, true};   // 7 趟
    attempts[attemptCount++] = {STRIP_ROWS, false};  // 14 趟(= v59 行為)

    for (int i = 0; i < attemptCount && !scratch; i++) {
      const size_t bytes = static_cast<size_t>(gwBytes) * attempts[i].rows;
      auto lsb = makeUniqueNoThrow<uint8_t[]>(bytes);
      if (!lsb) continue;
      if (attempts[i].merged) {
        auto msb = makeUniqueNoThrow<uint8_t[]>(bytes);
        if (!msb) continue;  // lsb 在這一圈結束時釋放,不會卡著半塊
        scratchMsb = std::move(msb);
        mergedPlanes = true;
      }
      scratch = std::move(lsb);
      stripRows = attempts[i].rows;
    }
    diagSeg.stripRows = static_cast<unsigned>(stripRows);
    if (!scratch) {
      diagSeg.path = 3;  // scratch 配置失敗(與一般 fallback 的 path=2 區分)
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * stripRows);
    } else if (mergedPlanes) {
      // 合併路徑:每條帶只走訪一次,兩個平面同時填,填完各自推進控制器。
      // DTM1(LSB)與 DTM2(MSB)是兩塊獨立的控制器 RAM,每次 writeGrayscalePlaneStrip
      // 都是自帶 PTL 視窗的完整交易,所以交錯推送與原本「先整個 LSB 再整個 MSB」等價。
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_BOTH);
      for (int y = 0; y < gh; y += stripRows) {
        const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
        renderer.beginStripTarget(scratch.get(), scratchMsb.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        renderer.writeGrayscalePlaneStrip(false, scratchMsb.get(), y, rows);
      }
      const auto tGrayPlanes = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      // ⚠️ 合併之後 lsb/msb 不再是兩件可比的事:整段柵格化記在 lsb,msb 記 0。
      // path=4 就是為了讓下一次讀 log 的人不會把「msb 掉到 0」誤讀成 50% 的改善
      // (v54 那個「量測數字反而變漂亮」的形狀)。
      diagSeg.grayLsb = static_cast<unsigned>(tGrayPlanes - tDisplay);
      diagSeg.grayMsb = 0;
      diagSeg.grayDisplay = static_cast<unsigned>(tGrayDisplay - tGrayPlanes);
      diagSeg.cleanup = static_cast<unsigned>(tCleanup - tGrayDisplay);
      diagSeg.path = 4;  // tiled 灰階,兩平面單趟
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += stripRows) {
        const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += stripRows) {
        const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
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

      // v54:改走 DiagLog(原 LOG_DBG 在 gh_release 的 LOG_LEVEL=1 下編譯期就被拿掉,
      // 導致「翻頁到底卡在 SD、CPU 還是面板」這個核心問題量不到)。
      diagSeg.grayLsb = static_cast<unsigned>(tGrayLsb - tDisplay);
      diagSeg.grayMsb = static_cast<unsigned>(tGrayMsb - tGrayLsb);
      diagSeg.grayDisplay = static_cast<unsigned>(tGrayDisplay - tGrayMsb);
      diagSeg.cleanup = static_cast<unsigned>(tCleanup - tGrayDisplay);
      diagSeg.path = 1;  // tiled 灰階
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
          LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, millis() - t0);
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

      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, millis() - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, millis() - t0);
    }
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

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    // No TOC entry for this spine (e.g. cover page): leave the title empty instead of a placeholder
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
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

  const std::string bmPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
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
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  const bool ok = JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str());
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
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
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
