#include <SdCardFont.h>
#include <DataDir.h>
#include <Epub/ParsedText.h>
#include "util/BenchFlags.h"
#include "util/DiagLog.h"
#include "EpubReaderActivity.h"

#include <BitmapHelpers.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
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
#include "ReaderBookmarksActivity.h"
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

namespace {
void checkPrebuildFuse();  // v257：定義在下方（預排保險絲，與 g_buildInputCaught 同一個匿名命名空間區塊）
}  // namespace

void EpubReaderActivity::onEnter() {
  // ⭐ **當前文件的軸向，onEnter 的第一件事。**
  //    排版（readerRenderSpec）、繪製（兩個 VerticalScope）、按鍵
  //    （MappedInputManager::bookTurnsRightToLeft）全部只讀 `SETTINGS.documentIsVertical()`。
  //    ⚠️ 寫在最前面是刻意的：本函式下方有 `if (!epub) return;`，寫在它後面就會有一條
  //      「進了閱讀器但沒寫軸向」的路徑，留著上一本書的值（複查抓到）。
  //      三個閱讀器因此形狀一致：軸向都是 onEnter 的第一個敘述。
  //    ⚠️ 它是執行期欄位，不進 settings.json。
  SETTINGS.activeDocumentVertical =
      epub ? (SETTINGS.resolveVerticalFor(epub->hasRtlPageProgression()) ? 1 : 0) : 0;
  axisAtEnter_ = SETTINGS.documentIsVertical();
  checkPrebuildFuse();  // v257：每次開機第一次進閱讀器時看一次（不影響上面「軸向是第一個敘述」的約定）
  DiagLog::line("BOOKDIR rtl=%d setting=%d resolved=%d", (epub && epub->hasRtlPageProgression()) ? 1 : 0,
                static_cast<int>(SETTINGS.readerVerticalLayout), SETTINGS.documentIsVertical() ? 1 : 0);

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
  // v248：JPEG／PNG 第一次打開時直接從書裡解碼（不先抽到 SD）；失敗 ImageBlock 自己退回上面的抽圖。
  ImageBlock::setStreamSource(epub.get(), [](void* ctx, const char* src, ZipEntryReader& reader, size_t bufSize) {
    return static_cast<Epub*>(ctx)->openItemReader(src, reader, bufSize);
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
  // v278：**喚醒路徑的分項計時**（純觀測）。網友回報「休眠重開比原廠慢」，實測整段約 3.9 秒，
  //   其中 `KEYPROBE` 到 `ADVRESET` 之間有 **1,056ms 完全沒有儀器**。這一段就是它的前半
  //   （onEnter 的尾巴：方向、快取目錄、進度檔、APP_STATE、最近閱讀、書籤），後半在 render 那邊。
  //   ℹ️ 每次進閱讀器都會印一行（不是只有喚醒那次）——一次一行不會灌爆 log，而且樣本更多。
  const unsigned long wakeT0 = millis();
  unsigned long wakeTOrient = wakeT0, wakeTCache = wakeT0, wakeTProg = wakeT0, wakeTState = wakeT0,
                wakeTRecent = wakeT0;
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  wakeTOrient = millis();

  epub->setupCacheDir();
  wakeTCache = millis();


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
  wakeTProg = millis();
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

  // v279：**這兩個 SD 寫檔延到第一頁畫出來之後**。
  //   v278 的 `WAKEPROF` 量到它們是 onEnter 尾段的全部成本（`state=61–330ms` ＋ `recent=169ms`），
  //   而「記住最後開的書」「加進最近閱讀」跟**畫出這一頁**一點關係都沒有 —— 它們卻擋在前面。
  //   ⚠️ **「掉電也不會丟」是錯的**（複查抓到，照實記著）：`onExit`／休眠會走到收尾，但**斷電、
  //      電池拔除、brownout 不會** —— 那時候「最後開的書」與「最近閱讀」都會停在上一本。
  //      窗口約一秒（第一頁畫完就寫掉），而舊版把寫入放在 onEnter 也有同一類的窗口（只是更小）。
  //      為了把窗口關到只剩「非正常斷電」，`onExit` 也會強制補寫一次（見 flushDeferredOpenState）。
  //   ⚠️ 只登記、不做事；真正的寫入在 `loop()` 看到第一頁畫完之後（見 `flushDeferredOpenState()`）。
  APP_STATE.openEpubPath = epub->getPath();
  deferredOpenStatePending_ = true;
  // 重進閱讀器（從註腳／選單回來）會再跑一次 onEnter，旗標必須跟著重設 ——
  //   不重設的話它還留著上一次的 true，這次的延後寫入就會在**還沒畫任何東西之前**就執行（複查抓到）。
  firstRenderDone_.store(false, std::memory_order_relaxed);
  wakeTState = millis();
  wakeTRecent = millis();

  loadCachedBookmarks();

  // Trigger first update
  // v110/v164：書的身分雜湊算一次（書在開啟期間不會換路徑）
  const unsigned long wakeTMarks = millis();
  warmBookHash = WarmIdentity::fnv1a(epub->getCachePath().c_str());

  // v278：`enterDone` 是 requestUpdate() 之前的時刻；ADVRESET 會印出它到自己之間還差多少
  //   （`sinceEnter=`），兩者相加就把那 1,056ms 拆成「onEnter」與「繪圖任務啟動之後」兩半。
  // ⚠️ **先 requestUpdate()、再寫 log**（複查抓到）：`DiagLog::line` 會做 SD I/O，
  //    寫在前面等於**讓儀器自己拖慢它要量的那條路**，而且那段延遲還會被算進 `sinceEnter`。
  const unsigned long enterDone = millis();
  readerEnterDoneMs_.store(static_cast<uint32_t>(enterDone), std::memory_order_relaxed);

  requestUpdate();

  DiagLog::line("WAKEPROF orient=%lu cache=%lu prog=%lu state=%lu recent=%lu marks=%lu total=%lu",
                static_cast<unsigned long>(wakeTOrient - wakeT0), static_cast<unsigned long>(wakeTCache - wakeTOrient),
                static_cast<unsigned long>(wakeTProg - wakeTCache), static_cast<unsigned long>(wakeTState - wakeTProg),
                static_cast<unsigned long>(wakeTRecent - wakeTState),
                static_cast<unsigned long>(wakeTMarks - wakeTRecent), static_cast<unsigned long>(enterDone - wakeT0));
}

void EpubReaderActivity::onExit() {
  emitBuildEnd("exit");
  // v279（複查）：延後的寫入若還沒發生就在這裡補完 —— 否則「開書後馬上退出／休眠」會讓
  //   這本書沒進最近閱讀（`APP_STATE` 下面本來就會存，但最近閱讀沒有別的補救點）。
  //   ⚠️ 擺在 `emitBuildEnd` **之後**（複查第二輪）：先把診斷寫出去，再做這兩個慢 I/O。
  flushDeferredOpenState(/*force=*/true);  // v189：離開（含休眠）時建置若還活著，這裡是最後一個能印 BUILD end 的地方
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
  ImageBlock::setStreamSource(nullptr, nullptr);
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
  stopNextChapterPrebuild("exit");  // v257：預排中的下一章以 partial 落地（或丟掉 tmp）
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

// v252：在背景建置的排版探針點讀按鍵（主任務、持 RenderLock 的 tick 內；見 ParsedText::setBuildInputPollHook）。
// 為什麼：主迴圈一圈只在開頭 gpio.update() 一次，接著的 tick 一步 0.3–0.8 秒；按下又放開整個落在步裡的短按，
// update() 永遠看不到按下的電平 ＝ 按鍵消失。diag251 一本長章節的書：29.6 秒的建置期間翻頁 0 次成功、使用者「翻不過去」。
// gpio.pollDuringBusyWork() 推進去彈跳並把事件存著，下一圈開頭的 update() 才交出去（電源鍵／USB／休眠計時在
// activity loop 之前讀事件 —— 見 HalGPIO.h）；有待處理的事件就回 true，Section 在這一步結束時讓路。
namespace {
uint32_t g_buildInputCaught = 0;  // v252 證人：接到輸入事件的 tick 數（BUILDPROF in=）
bool g_buildInputCaughtThisTick = false;

// v257（codex 複查）：預排是「沒人叫它就自己跑」的工作 —— 若它在某本書的某一章會讓機器重開，醒來回到閱讀器 2 秒後
//   又會自己跑一次＝當機迴圈。保險絲：預排的建置步驟前後在 RTC 不初始化的記憶體（軟重開、panic、看門狗都保留）
//   寫下「進行中」；閱讀器進場時若看到它還在，代表上一次重開發生在預排中 → 這次開機停用預排、寫一行證人。
//   兩個值互為反碼，冷開機的隨機內容幾乎不可能剛好湊成。
//   ⚠️ volatile（codex 第二輪）：這兩個是內部連結的變數，中間的函式呼叫看不到它們 —— 不加 volatile，編譯器可以把
//   「寫入進行中」當成死寫入消掉（解構子馬上又寫 0），保險絲就形同虛設。巢狀使用以深度計數，最外層結束才清。
volatile RTC_NOINIT_ATTR uint32_t g_prebuildInFlightMagic;
volatile RTC_NOINIT_ATTR uint32_t g_prebuildInFlightInv;
constexpr uint32_t PREBUILD_INFLIGHT_MAGIC = 0x4E584254u;  // "NXBT"
bool g_prebuildFuseChecked = false;
bool g_prebuildDisabledThisBoot = false;
uint8_t g_prebuildInFlightDepth = 0;
struct PrebuildInFlight {
  PrebuildInFlight() {
    if (g_prebuildInFlightDepth++ == 0) {
      g_prebuildInFlightMagic = PREBUILD_INFLIGHT_MAGIC;
      g_prebuildInFlightInv = ~PREBUILD_INFLIGHT_MAGIC;
    }
  }
  ~PrebuildInFlight() {
    if (g_prebuildInFlightDepth > 0 && --g_prebuildInFlightDepth == 0) {
      g_prebuildInFlightMagic = 0;
      g_prebuildInFlightInv = 0;
    }
  }
  PrebuildInFlight(const PrebuildInFlight&) = delete;
  PrebuildInFlight& operator=(const PrebuildInFlight&) = delete;
};
void checkPrebuildFuse() {
  if (g_prebuildFuseChecked) return;
  g_prebuildFuseChecked = true;
  if (g_prebuildInFlightMagic == PREBUILD_INFLIGHT_MAGIC && g_prebuildInFlightInv == ~PREBUILD_INFLIGHT_MAGIC) {
    g_prebuildDisabledThisBoot = true;
    DiagLog::line("NEXTBUILD disabled why=reset-during-prebuild");
  }
  g_prebuildInFlightMagic = 0;
  g_prebuildInFlightInv = 0;
}
}  // namespace

bool EpubReaderActivity::pollInputDuringBuild() {
  gpio.pollDuringBusyWork();
  if (!gpio.hasPendingInput()) return false;
  if (!g_buildInputCaughtThisTick) {
    g_buildInputCaughtThisTick = true;
    g_buildInputCaught++;
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
  g_buildInputCaught = 0;
  ParsedText::buildProf = ParsedText::BuildProf{};  // v252
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
  // v252：分項（ms）。巢狀：xml ⊃ cd,el；cd,el ⊃ lay（layCd 是在 cd 裡的）；lay ⊃ adv,proc；proc ⊃ ser。
  //   step＝建置步總時間（Section::buildStepTotalUs）；step − rd − xml ≈ 步外的收尾／commit。asdms＝SD 查字寬的時間。
  {
    const auto& bp = ParsedText::buildProf;
    const auto ms = [](const uint64_t us) { return static_cast<unsigned long>(us / 1000); };
    DiagLog::line("BUILDPROF spine=%d pages=%u step=%lu rd=%lu xml=%lu cd=%lu el=%lu lay=%lu layCd=%lu adv=%lu proc=%lu "
                  "ser=%lu img=%lu asdms=%lu words=%lu lays=%lu cds=%lu in=%lu cjkhit=%lu fetchn=%lu fetchms=%lu scanms=%lu "
                  "xchk=%lu xbad=%lu scandefer=%lu",
                  diagBuildSpine, pages, ms(Section::buildStepTotalUs), ms(bp.rdUs), ms(bp.xmlUs), ms(bp.cdUs),
                  ms(bp.elUs), ms(bp.layUs), ms(bp.layCdUs), ms(bp.advUs), ms(bp.procUs), ms(bp.serUs), ms(bp.imgUs),
                  ms(SdCardFont::advanceSdReadUs_), static_cast<unsigned long>(bp.words),
                  static_cast<unsigned long>(bp.layCalls), static_cast<unsigned long>(bp.cdCalls),
                  static_cast<unsigned long>(g_buildInputCaught),
                  // v253：cjkhit＝漢字字寬直接回答的次數；fetchn／fetchms＝仍去 SD 逐筆讀的筆數與時間；scanms＝掃描
                  static_cast<unsigned long>(SdCardFont::advanceCjkHitCount_),
                  static_cast<unsigned long>(SdCardFont::advanceFetchReadCount_), ms(SdCardFont::advanceFetchUs_),
                  ms(SdCardFont::advanceScanUs_),
                  // xchk／xbad：頁面預載時核對快路徑的字數／不符（開機以來累計；xbad>0 會停用快路徑）
                  static_cast<unsigned long>(SdCardFont::advanceCjkCrossChecks_),
                  static_cast<unsigned long>(SdCardFont::advanceCjkCrossMismatch_),
                  static_cast<unsigned long>(SdCardFont::advanceScanDeferred_));  // v255：該掃但不在允許時機的次數
  }

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

// ---------------------------------------------------------------------------------------------
// v257：預排下一章（見 EpubReaderActivity.h 的 nextSection_ 註解）。三個函式都只在主任務 loop() 或持 RenderLock 的
// render／onExit 呼叫；nextSection_ 的建立、建置、釋放一律在 RenderLock 內，與 render 的 !section 分支互斥。
void EpubReaderActivity::maybeStartNextChapterPrebuild() {
  if (g_prebuildDisabledThisBoot) return;
  if (!epub || !section || section->isBuilding() || section->isPartial() || nextSection_ || diagBuildActive) return;
  if (buildViewportWidth == 0 || footnoteDepth > 0) return;  // 註腳裡：「下一章」不是讀者要去的地方
  const int nextSpine = currentSpineIndex + 1;
  if (nextSpine >= epub->getSpineItemsCount() || nextPrebuildSpine_ == nextSpine) return;
  const unsigned long now = millis();
  if (now - lastPageTurnTime < NEXT_PREBUILD_IDLE_MS) return;
  if (nextPrebuildWaiting_ && now - nextPrebuildWaitStartMs_ < 10000) return;

  RenderLock lock;
  // 鎖內重查：render 可能剛換掉 section。
  if (!section || section->isBuilding() || section->isPartial() || nextSection_ || currentSpineIndex + 1 != nextSpine) {
    return;
  }
  // codex（v257）：任何配置之前先過堆積地板 —— Section 物件、快取表頭／LUT、HTML inflate 都在這之後，
  //   其中有會 abort 的容器配置。地板關著：先放掉保留中的字型快取再量一次；仍關著就退避 10 秒。
  if (!buildTickHeapGate()) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      const size_t released = fcm->releaseRetainedCache();
      if (released) DiagLog::line("FONTREL next %u", static_cast<unsigned>(released));
    }
    if (!buildTickHeapGate()) {
      nextPrebuildWaiting_ = true;
      nextPrebuildWaitStartMs_ = millis();
      DiagLog::line("NEXTBUILD wait spine=%d why=heap max=%u free=%u", nextSpine,
                    static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(ESP.getFreeHeap()));
      return;
    }
  }
  nextPrebuildWaiting_ = false;
  nextPrebuildSpine_ = nextSpine;  // 從這裡起不論結果都算已嘗試，不重試
  const PrebuildInFlight inFlight;
  const ReaderRenderSpec spec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
  std::unique_ptr<Section> s(new (std::nothrow) Section(epub, nextSpine, renderer));
  if (!s) {
    DiagLog::line("NEXTBUILD skip spine=%d why=alloc", nextSpine);
    return;
  }
  if (s->loadSectionFile(spec)) {
    if (!s->isPartial()) {
      nextPrebuiltReadySpine_ = nextSpine;
      DiagLog::line("NEXTBUILD cached spine=%d pages=%u", nextSpine, static_cast<unsigned>(s->pageCount));
      // 讀者已經停在章末：render 尾端那次預取當時還不知道下一章有快取（pg=2），這裡補做跨章預取。
      s.reset();
      if (section->currentPage + 1 >= static_cast<int>(section->pageCount) && !gpio.inputActive()) {
        prefetchNextPage(SETTINGS.getReaderFontId(), 0, 0, section->currentPage, true);
      }
    } else {
      // partial 交給一般路徑：翻進去時直接服務已排的頁、接近水位才延伸。這裡不去延伸它。
      DiagLog::line("NEXTBUILD skip spine=%d why=partial pages=%u", nextSpine, static_cast<unsigned>(s->pageCount));
    }
    return;
  }
  // 與 lazy 延伸站相同：startBuild 要把整章 HTML inflate 出來（記憶體峰值），保留中的字型 mini 快取是純負擔。
  if (auto* fcm = renderer.getFontCacheManager()) {
    const size_t released = fcm->releaseRetainedCache();
    if (released) DiagLog::line("FONTREL next %u", static_cast<unsigned>(released));
  }
  // codex 第二輪：探過快取之後、真正 startBuild 之前再量一次地板（探快取本身也會配置）。
  if (!buildTickHeapGate()) {
    DiagLog::line("NEXTBUILD fail spine=%d why=heap-after-probe max=%u", nextSpine,
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return;
  }
  DiagLog::mem("build-next");
  // BUILDPROF 那組計數器目前沒有別的建置在用（目前 section 不在建置、diagBuildActive 為假），借來量預排。
  ParsedText::buildProf = ParsedText::BuildProf{};
  Section::buildStepMaxMs = Section::buildStepTotalUs = Section::buildStepCount = 0;
  SdCardFont::resetAdvanceDiag();
  const unsigned long startT0 = millis();
  if (!s->startBuild(spec)) {
    DiagLog::line("NEXTBUILD fail spine=%d why=start lowmem=%u", nextSpine, s->lastBuildWasLowMemory() ? 1u : 0u);
    return;  // s 的解構子：沒有 build_，no-op
  }
  nextSection_ = std::move(s);
  nextBuildSpec_ = spec;  // v258：接手時比對
  nextBuildCssSeq_ = epub->getCssParser() ? epub->getCssParser()->mutationSeq_ : 0;
  resetNextBuildWitness();
  nextBuildStartMs_ = millis();
  nextBuildTicks_ = 0;
  nextBuildTurnSinceTick_ = false;
  // startms：開始建置本身（含整章 HTML 從書裡解壓到 SD）持鎖多久 —— 這一段不能中途讓路，要實機量。
  DiagLog::line("NEXTBUILD start spine=%d startms=%lu html=%u", nextSpine,
                static_cast<unsigned long>(nextBuildStartMs_ - startT0), nextSection_->hasHtmlCache() ? 1u : 0u);
}

void EpubReaderActivity::resetNextBuildWitness() {
  nextBuildBusyMs_ = nextBuildHeapBlocks_ = nextBuildHeapBlockedMs_ = nextBuildWarmBlocks_ = 0;
  nextBuildMinFreeKb_ = nextBuildMinMaxKb_ = 0;
  nextBuildSteals_ = nextBuildStealsNoGain_ = 0;
  nextBuildStealNoGain_ = false;
  nextBuildBlockedRunMs_ = 0;
  nextBuildLastBlockMs_ = 0;
  nextBuildLastTickBlocked_ = false;
}

void EpubReaderActivity::noteNextBuildHeapBlocked(const bool warmHeld) {
  const unsigned long now = millis();
  ++nextBuildHeapBlocks_;
  // 只累計「緊接著又被擋」的間隔（主迴圈每圈 ≤50ms）；中間隔著翻頁暫停的不算，那段不是地板造成的。
  // v259：stall 的累計只算【不是】被預取擋住的那種（真的缺記憶體）。被預取擋住的有停留讓路（steal）與進章接手兩道收尾，
  //   停掉反而會落地 partial、進章從第 1 頁重排（diag258 第 7 章：1,287 次被擋 1,286 次是預取，74 秒後 stall）。
  if (nextBuildLastTickBlocked_ && now - nextBuildLastBlockMs_ < 200) {
    nextBuildHeapBlockedMs_ += now - nextBuildLastBlockMs_;
    if (!warmHeld) nextBuildBlockedRunMs_ += now - nextBuildLastBlockMs_;
  }
  nextBuildLastBlockMs_ = now;
  nextBuildLastTickBlocked_ = true;
  const uint32_t freeKb = static_cast<uint32_t>(ESP.getFreeHeap() / 1024);
  const uint32_t maxKb = static_cast<uint32_t>(ESP.getMaxAllocHeap() / 1024);
  if (nextBuildHeapBlocks_ == 1 || freeKb < nextBuildMinFreeKb_) nextBuildMinFreeKb_ = freeKb;
  if (nextBuildHeapBlocks_ == 1 || maxKb < nextBuildMinMaxKb_) nextBuildMinMaxKb_ = maxKb;
}

void EpubReaderActivity::nextBuildWitness(char* buf, const size_t n) const {
  snprintf(buf, n, "busy=%lu blk=%lu/%lums warm=%lu steal=%lu/%lu minfree=%luK minmax=%luK",
           static_cast<unsigned long>(nextBuildBusyMs_), static_cast<unsigned long>(nextBuildHeapBlocks_),
           static_cast<unsigned long>(nextBuildHeapBlockedMs_), static_cast<unsigned long>(nextBuildWarmBlocks_),
           static_cast<unsigned long>(nextBuildSteals_), static_cast<unsigned long>(nextBuildStealsNoGain_),
           static_cast<unsigned long>(nextBuildMinFreeKb_), static_cast<unsigned long>(nextBuildMinMaxKb_));
}

void EpubReaderActivity::tickNextChapterPrebuild() {
  RenderLock lock;
  if (g_prebuildDisabledThisBoot || !nextBuildTickDue()) return;
  if (!buildTickHeapGate()) {
    // codex（v257 第二輪）：地板一直關著、預排卻握著建置內容等下去 ＝ 長期壓著前景的記憶體。
    //   【真的缺記憶體而被擋】連續 60 秒就停（有頁就 partial 落地）；讀者翻頁的暫停不算，v259 起被預取擋住的也不算
    //   （diag258：那段 23 次翻頁 22 次 warm，握著建置內容沒有拖累前景）。
    auto* fcm = renderer.getFontCacheManager();
    const bool warmHeld = fcm && fcm->warmIdentity().valid;
    // codex（v259）：「被預取擋住」只有在讓路真的能打開地板時才算數 —— 讓過一次卻沒打開，之後就當成真的缺記憶體（算進 stall）。
    //   codex 第二輪：讓到上限之後也一樣（不能再讓＝被擋就是真的在等），算進 stall。
    noteNextBuildHeapBlocked(warmHeld && !nextBuildStealNoGain_ && nextBuildSteals_ < NEXT_PREBUILD_MAX_STEALS);  // v258 證人
    // v258（codex）：60 秒改用「連續被擋」的累計（與證人同一個數）。v257 用第一次被擋的時間點起算，
    //   中間讀者翻頁、tick 根本沒跑的時間也算進去 → 翻了一分多鐘之後第一次被擋就立刻 stall。
    if (nextBuildBlockedRunMs_ >= 60000) {
      stopNextChapterPrebuild("stall");
      return;
    }
    if (!fcm) return;
    if (warmHeld) {
      ++nextBuildWarmBlocks_;
      // v259：讀者停在這一頁夠久 → 下一頁的預取讓路給預排。diag258 證人：正常閱讀（約 5.7 秒一頁）的每個停留裡，
      //   預取握著的字型快取讓預排的地板一直關著（minfree 27K／minmax 7K），1,287 次被擋 1,286 次是這個原因。
      //   代價：預排還沒排完讀者就翻頁 → 那一頁冷（字要當場讀，約 +0.3 秒）；預排排完時會補做預取（done 分支）。
      //   門檻 3 秒：比預排本身的 2 秒停留再多 1 秒 —— 翻得比 3 秒快的讀者頁頁照舊 warm，預排等到進章時接手。
      //   codex（v259）防乒乓：①讓過一次卻沒打開地板（碎片化或別的東西佔著）→ 這個預排不再讓；
      //   ②每個預排最多讓 NEXT_PREBUILD_MAX_STEALS 次（預排 16–31 頁／秒，一次 3 秒以上的停留通常就排完一章，
      //   讓到上限還沒完＝超長章或讀者停留很短，再讓只是每頁白讀一次字、多一個冷頁）；③手已經在鍵上就不讓。
      if (millis() - lastPageTurnTime < NEXT_PREBUILD_STEAL_DWELL_MS || nextBuildStealNoGain_ ||
          nextBuildSteals_ >= NEXT_PREBUILD_MAX_STEALS || gpio.inputActive() || gpio.hasPendingInput()) {
        return;
      }
      const size_t released = fcm->releaseRetainedCache();  // 同時作廢 warm 身分
      ++nextBuildSteals_;
      const bool opened = buildTickHeapGate();
      if (!opened) {
        nextBuildStealNoGain_ = true;
        ++nextBuildStealsNoGain_;
      }
      DiagLog::line("FONTREL steal %u opened=%u n=%lu", static_cast<unsigned>(released), opened ? 1u : 0u,
                    static_cast<unsigned long>(nextBuildSteals_));
      if (!opened) return;
    } else {
      const size_t released = fcm->releaseRetainedCache();
      if (released) DiagLog::line("FONTREL nexttick %u", static_cast<unsigned>(released));
      if (!buildTickHeapGate()) return;
    }
  }
  nextBuildBlockedRunMs_ = 0;
  nextBuildLastTickBlocked_ = false;
  const PrebuildInFlight inFlight;
  nextBuildTurnSinceTick_ = false;
  // 與目前章節的 tick 同一組保護：探針點輪詢按鍵（v252）、允許 CJK 掃描且按鍵即中止（v255）。
  struct InputPollScope {
    InputPollScope() {
      g_buildInputCaughtThisTick = false;
      if (!gpio.hasTouch()) ParsedText::setBuildInputPollHook(&EpubReaderActivity::pollInputDuringBuild);
    }
    ~InputPollScope() { ParsedText::setBuildInputPollHook(nullptr); }
  };
  struct CjkScanAllowScope {
    CjkScanAllowScope() {
      SdCardFont::openCjkScanWindow(+[](void*) -> bool { return gpio.hasPendingInput(); }, nullptr);
    }
    ~CjkScanAllowScope() { SdCardFont::closeCjkScanWindow(); }
  };
  bool built;
  const unsigned long tickT0 = millis();
  {
    const InputPollScope inputPoll;
    const CjkScanAllowScope scanAllow;
    built = nextSection_->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK, &EpubReaderActivity::buildShouldYield, this);
  }
  nextBuildBusyMs_ += static_cast<uint32_t>(millis() - tickT0);
  ++nextBuildTicks_;
  if (!built) {
    stopNextChapterPrebuild(nextSection_->lastBuildWasLowMemory() ? "lowmem" : "failed");
    return;
  }
  if (!nextSection_->isBuildComplete()) return;
  nextPrebuiltReadySpine_ = nextPrebuildSpine_;
  const auto ms = [](const uint64_t us) { return static_cast<unsigned long>(us / 1000); };
  char wit[160];
  nextBuildWitness(wit, sizeof(wit));
  DiagLog::line("NEXTBUILD done spine=%d pages=%u ms=%lu ticks=%lu step=%lu adv=%lu fetchn=%lu scanms=%lu %s",
                nextPrebuildSpine_, static_cast<unsigned>(nextSection_->pageCount),
                static_cast<unsigned long>(millis() - nextBuildStartMs_), static_cast<unsigned long>(nextBuildTicks_),
                ms(Section::buildStepTotalUs), ms(ParsedText::buildProf.advUs),
                static_cast<unsigned long>(SdCardFont::advanceFetchReadCount_), ms(SdCardFont::advanceScanUs_), wit);
  nextSection_.reset();  // 檔案已 commit；解構子沒有 build_ 可停
  // 預排期間 render 尾端的預取被擋（pg=10），這裡補做目前這一頁的下一頁（若是章末，會跨進剛排好的下一章）。
  if (section && !gpio.inputActive()) {
    prefetchNextPage(SETTINGS.getReaderFontId(), 0, 0, section->currentPage, true);
  }
}

void EpubReaderActivity::stopNextChapterPrebuild(const char* why) {
  if (!nextSection_) return;
  char wit[160];
  nextBuildWitness(wit, sizeof(wit));
  DiagLog::line("NEXTBUILD stop spine=%d why=%s pages=%u ms=%lu ticks=%lu %s", nextPrebuildSpine_, why,
                static_cast<unsigned>(nextSection_->builtPageCount()),
                static_cast<unsigned long>(millis() - nextBuildStartMs_), static_cast<unsigned long>(nextBuildTicks_),
                wit);
  const PrebuildInFlight inFlight;  // codex 第二輪：落地 partial（suspendBuild 會寫 SD）也在保險絲範圍內
  nextSection_.reset();  // ~Section → suspendBuild：有完整的頁就以 partial 落地（低記憶體中止的不落地，v149/v194）
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
  flushDeferredOpenState();  // v279：第一頁畫完之後才寫那兩個檔（見 onEnter）
  // v260：翻頁鍵、返回、電源的按下讓正在進行的補圖解碼（render task、有 arm 的那一段）停下。放在最前面：下面有各種提早
  //   return，而跨章翻頁的 pageTurn 會等 RenderLock —— 序號要在那之前前進，render task 才會先放手。
  //   codex：不含確認鍵（選單／書籤長按）與上下鍵（截圖組合的另一顆）—— 那些按了不會離開這一頁，中止只會讓 4–5 秒的解碼重做。
  //   v264：書首的「上一頁」不中止 —— 那一下哪裡都不會去，中止只會讓封面這種大圖的解碼整個重做。
  //   方向照翻頁判斷同一個函式算（直排書的前排左右鍵是對調的；側鍵已由 MappedInputManager 換好）。
  //   一次 loop 只取一次書首快照，上下兩處共用 —— 兩處各取一次會不一致（codex：上面保住解碼、下面卻真的翻頁）。
  const bool atBookStartThisPoll = atBookStart();
  {
    using Btn = MappedInputManager::Button;
    const auto front = ReaderUtils::frontPageButtons(mappedInput);
    const bool prevPress = mappedInput.wasPressed(Btn::PageBack) || mappedInput.wasPressed(front.prev);
    const bool otherPress = mappedInput.wasPressed(Btn::PageForward) || mappedInput.wasPressed(front.next) ||
                            mappedInput.wasPressed(Btn::Back) || mappedInput.wasPressed(Btn::Power);
    if (otherPress || (prevPress && !atBookStartThisPoll)) {
      ImageToFramebufferDecoder::noteUserInput();
    } else if (prevPress && bookStartDecodeKept_ < 255) {
      ++bookStartDecodeKept_;  // 證人跟著下面那一行空按一起印，不另外寫 SD
    }
  }
  // v146 儀器：把 lib 端記下的圖片失敗讀走並寫進 diag.log。
  // ⚠️ 必須【每一輪都讀】，不可以放進每 N 頁的取樣區塊 —— lastFailPath 只有一格，
  //    取樣之間發生的失敗會被後來的覆寫（教訓 B-23，v123 踩過：第一章那個小圖就是
  //    這樣消失的）。這裡的成本是一次短臨界區內的字元比較。
  // ⚠️ lib/Epub 不能反向依賴 src/util/DiagLog，所以是 lib 記、src 讀（分層規則）。
  // v249：讀寫都走 Breadcrumb.h 的跨 task 交接（lib 多在 render task 寫，這裡是 main loop）。
  DiagLog::crumb("IMGFAIL", ImageBlock::lastFailPath, sizeof(ImageBlock::lastFailPath));
  // v246 儀器：第一次解碼一張圖的時間分解（lib 記、這裡讀；同上的分層與先到先得）。
  DiagLog::crumb("IMGDEC", ImageBlock::lastDecodeWitness, sizeof(ImageBlock::lastDecodeWitness));
  // v151：ParsedText 守衛的拒絕紀錄 —— v150 的「索引失敗」零證據就是因為只寫 LOG_ERR。
  DiagLog::crumb("SDCFFAIL", SdCardFont::lastAllocFail, sizeof(SdCardFont::lastAllocFail));
  DiagLog::crumb("ADVSCAN", SdCardFont::lastAdvScan, sizeof(SdCardFont::lastAdvScan));  // v253
  DiagLog::crumb("PTXREFUSE", ParsedText::lastRefusal, sizeof(ParsedText::lastRefusal));
  // v194：nothrow 失敗出口的證人。沒有序列埠＝寫進 diag.log 否則就是丟掉。
  DiagLog::crumb("ALLOCFAIL", Page::lastAllocFail, sizeof(Page::lastAllocFail));
  DiagLog::crumb("ALLOCFAIL", HalStorage::lastAllocFail, sizeof(HalStorage::lastAllocFail));
  DiagLog::crumb("ALLOCFAIL", ditherLastAllocFail, sizeof(ditherLastAllocFail));
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

  // v257（codex 複查）：執行期保險 —— 目前這章若在建置（照設計不會發生：換章／重建都先經過 render 的停止點），
  //   預排立刻停掉，不讓兩個建置共用同一個 CSS 解析器。
  if (nextSection_ && section && (section->isBuilding() || section->isPartial()) && !RenderLock::peek()) {
    RenderLock lock;
    if (nextSection_ && section && (section->isBuilding() || section->isPartial())) stopNextChapterPrebuild("conflict");
  }

  // v259（codex）：預排壽命上限 —— 與下面「能不能 tick／開始」的條件無關，只要拿得到鎖就檢查。
  //   被預取擋住的等待不再算進 stall 之後，預排的建置內容可能一路握到章末（進章時接手）；翻得快的讀者 tick 根本不跑。
  if (nextSection_ && millis() - nextBuildStartMs_ >= NEXT_PREBUILD_MAX_AGE_MS && !RenderLock::peek()) {
    RenderLock lock;
    if (nextSection_ && millis() - nextBuildStartMs_ >= NEXT_PREBUILD_MAX_AGE_MS) stopNextChapterPrebuild("age");
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
      !gpio.wasAnyReleased() && !gpio.inputActive() && !gpio.hasPendingInput()) {
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
      // v252：tick 內（主任務、持 RenderLock）才開啟探針點的按鍵輪詢；RAII 保證任何出口都關掉。
      // 有觸控的板子不開（codex 複查）：InputManager::update() 會清掉觸控的單次事件，而這裡只保存按鍵與 USB。
      //   X3／X4 沒有觸控。
      struct InputPollScope {
        InputPollScope() {
          g_buildInputCaughtThisTick = false;
          if (!gpio.hasTouch()) ParsedText::setBuildInputPollHook(&EpubReaderActivity::pollInputDuringBuild);
        }
        ~InputPollScope() { ParsedText::setBuildInputPollHook(nullptr); }
      };
      // v255：背景排版的 tick 才准做 CJK 字寬掃描（使用者沒有在等這一步）。
      //   掃描途中每批看一次有沒有攔到按鍵（InputPollScope 的輪詢存下來的），有就中止、下次再掃。
      struct CjkScanAllowScope {
        CjkScanAllowScope() {
          SdCardFont::openCjkScanWindow(+[](void*) -> bool { return gpio.hasPendingInput(); }, nullptr);
        }
        ~CjkScanAllowScope() { SdCardFont::closeCjkScanWindow(); }
      };
      bool built;
      {
        const InputPollScope inputPoll;
        const CjkScanAllowScope scanAllow;
        built = section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK, &EpubReaderActivity::buildShouldYield, this);
      }
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
    // v252（codex 複查）：排版中接到的按鍵要到下一圈開頭 update() 才交出去，但輪詢已經讓 isPressed／getHeldTime
    //   前進了 —— 這一圈剩下的按鍵處理（書籤長按、翻頁）若照跑，看到的是「狀態新、事件舊」。直接結束這一圈，
    //   下一圈所有處理（main 的電源鍵／USB／休眠計時、這裡的翻頁）看到同一份一致的輸入。
    if (gpio.hasPendingInput()) return;
  }

  // v257：預排下一章 —— 目前這章沒有在排版（上面的 tick 沒有要跑）時才輪到它。按鍵、待補圖、render 排隊都讓。
  if (epub && section && !section->isBuilding() && deferredDecodePage_ < 0 && !RenderLock::peek() &&
      !gpio.wasAnyPressed() && !gpio.wasAnyReleased() && !gpio.inputActive() && !gpio.hasPendingInput()) {
    if (nextBuildTickDue()) {
      tickNextChapterPrebuild();
    } else {
      maybeStartNextChapterPrebuild();
    }
    if (gpio.hasPendingInput()) return;  // 同 v252：探針點攔到的按鍵留給下一圈一致地處理
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
    // v264：第一章第一頁再長按往回 —— 下面的 section.reset() 會把整章重新載入再畫一次，而位置根本沒變。
    if (!nextTriggered && atBookStartThisPoll) {
      noteBookStartNoop("skip");
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
    // v264（codex）：轉方向整頁都要重畫 —— 正在補的圖必須停下。書首的「上一頁」在最上面刻意沒有中止，
    //   而長按往回在這個設定下是【真的有動作】，所以在這裡補一次；轉向後那張圖會照新方向重新解。
    ImageToFramebufferDecoder::noteUserInput();
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

  // v264：書首按上一頁 —— pageTurn(false) 在這裡什麼都不改，卻會整頁重畫、還讓預排暫停 2 秒。
  //   放在延遲證人之前：被吸收的按鍵不能留下 pressMs_ 殘值（同下面 v243 的理由）。
  //   同一輪若也收到「下一頁」（傾斜／觸控／電源鍵可能同時觸發），上一頁不可能成立 → 讓下一頁生效，
  //   不要照既有「上一頁優先」的仲裁把整輪吃掉（codex）。
  bool goForward = nextTriggered;
  if (prevTriggered) {
    if (atBookStartThisPoll) {
      if (!nextTriggered) {
        noteBookStartNoop("prev");
        return;
      }
      noteBookStartNoop("prev+next");
      goForward = true;
    } else {
      goForward = false;  // 既有仲裁：同時觸發時上一頁優先
    }
  }

  // v243：按鍵到換頁的延遲證人（EPLAT）。記在真的要翻頁這裡，不記在上面：被吸收的按鍵
  // （書尾選單、截圖組合鍵）不會觸發繪製，殘值會算到之後某一頁頭上（codex 複查）。
  // CAS：只記最早一次，且不會在 render 取走舊值的同時吃掉這一次。盡力而為 —— 只會少樣本，不會錯記。
  uint32_t expectedPress = 0;
  pressMs_.compare_exchange_strong(expectedPress, std::max<uint32_t>(1, millis()));
  pageTurn(goForward);
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
                               // ⭐⭐ **文字方向現在可以在閱讀中改，所以軸向必須在這裡重解。**
                               //    `activeDocumentVertical` 是 onEnter 算好的快照；不重解的話
                               //    使用者在 overlay 裡把方向從橫改直，下面的強制重排會用**舊軸向**
                               //    ——看起來像「設定沒生效」，而且排版與繪製之後才會各自追上。
                               //    ⚠️ 這一行與 onEnter 那一行是同一個運算式，兩處必須一起改。
                               SETTINGS.activeDocumentVertical =
                                   epub ? (SETTINGS.resolveVerticalFor(epub->hasRtlPageProgression()) ? 1 : 0) : 0;
                               axisAtEnter_ = SETTINGS.documentIsVertical();
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
          std::make_unique<ReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
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

// v264：主任務讀的「現在畫的就是書首」。旗標由 render task 發佈；有重畫請求在等就當它過時。
bool EpubReaderActivity::atBookStart() const {
  return bookStartShown_.load(std::memory_order_relaxed) && !activityManager.isRenderPending();
}

// v264：render task 在「跳頁都套用完、要畫哪一頁已定案」時呼叫。只有這裡碰 section。
// v279：延後的開書狀態寫檔（`APP_STATE` ＋ 最近閱讀）。
//   條件：**第一頁已經推上面板**（`firstRenderDone_`）**而且**沒有繪製在途 ——
//   後者是為了不要跟 render task 搶 SD（字型預熱、section 讀取都在那條路上）。
//   做完就把旗標關掉，一次開書只做一次。
void EpubReaderActivity::flushDeferredOpenState(const bool force) {
  if (!deferredOpenStatePending_) return;
  if (!force) {
    if (!firstRenderDone_.load(std::memory_order_relaxed)) return;
    if (activityManager.isRenderPending()) return;
  }
  if (!epub) {
    deferredOpenStatePending_ = false;
    return;
  }
  deferredOpenStatePending_ = false;
  const unsigned long t0 = millis();
  APP_STATE.saveToFile();
  const unsigned long t1 = millis();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  DiagLog::line("OPENSTATE state=%lu recent=%lu", static_cast<unsigned long>(t1 - t0),
                static_cast<unsigned long>(millis() - t1));
}

void EpubReaderActivity::publishBookStart() {
  bookStartShown_.store(currentSpineIndex == 0 && section && section->currentPage == 0, std::memory_order_relaxed);
}

// v264 證人：書首空按被吸收（B-22：保護有沒有真的跑到，要能在 log 裡看到）。每次空按一行、每本書最多 8 行。
void EpubReaderActivity::noteBookStartNoop(const char* why) {
  if (bookStartNoopLogged_ >= 8) return;
  ++bookStartNoopLogged_;
  // kept＝這本書到目前為止「書首按上一頁而沒有中止解碼」的累計次數。
  // 累計而不是單次旗標：按下與放開可能落在不同輪（長按設定），單次旗標會記到別的按鍵頭上（codex）。
  DiagLog::line("BOOKSTART noop why=%s kept=%u", why, static_cast<unsigned>(bookStartDecodeKept_));
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
    } else {
      return;  // v264：書首，沒有上一頁 —— 不重畫、不更新翻頁時刻（loop 已先擋；這裡守住之後新增的呼叫者）
    }
  }
  lastPageTurnTime = millis();
  nextBuildTurnSinceTick_ = true;  // v257：預排暫停到讀者再停 2 秒
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  // v279（複查第一輪）：「第一頁畫完」**要在 render() 真的結束時才發佈**，而且用 RAII —— 這個函式
  //   有很多條離開路徑（空章節、低記憶體、重排中…），放在某一行後面就是賭那條路會走到。
  //   ⚠️ 原本放在 `clearScreen()` 之後是**錯的**：那時候還在讀 SD（字型預熱、section），
  //      主任務會在 render 還在跑的時候就去寫檔 —— 正是這一版想避免的事。
  //   ℹ️ `isRenderPending()` 只代表「有排隊中的重畫」，不代表「正在畫」，所以不能只靠它。
  struct FirstRenderPublisher {
    std::atomic<bool>& flag;
    ~FirstRenderPublisher() { flag.store(true, std::memory_order_relaxed); }
  } firstRenderPublisher{firstRenderDone_};

  const uint32_t renderStartMs = millis();  // v243 EPLAT：在最前面取，才量得到排隊等鎖的時間
  // v264（codex 第二輪）：render task 一接手，`isRenderPending()` 就變 false，而這次要畫的可能是別頁 ——
  //   上一次發佈的「書首」在這段空窗會變成過時的 true，把該有的上一頁吃掉。接手就先作廢，
  //   等下面定案再發佈；中間主任務讀到 false ＝ 照舊行為（保守方向，只會少擋、不會誤擋）。
  bookStartShown_.store(false, std::memory_order_relaxed);
  const uint32_t pressMs = pressMs_.exchange(0);
  // v261（codex）：上一次的尾段先取走、立刻作廢 —— 提早 return 的 render 不會留下舊值給下一次（-1＝不知道）。
  const int32_t prevRenderTailMs = lastRenderTailMs_;
  lastRenderTailMs_ = -1;
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

  // v288：這裡原本還有一條分支，在「沒有狀態列／只有進度條」且自動翻頁開著時，
  //   多留一條狀態列的高度給那個指示器。自動翻頁移除之後那條分支永遠不成立，一併拿掉。
  orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  // Capture for loop()'s lazy partial-extension start (must match this render's layout params).
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    // v257：預排的下一章。先停掉還在跑的（有頁就以 partial 落地，下面的 loadSectionFile 直接用），
    //   並記下這一章是不是剛好已經預排完成（CHAPTER 證人）。
    const bool prebuiltHit = nextPrebuiltReadySpine_ == currentSpineIndex;
    // v258：接手。v257 在這裡一律停掉預排（落地 partial），接著一般路徑再從第 1 頁重排整章 ——
    //   解析器讀到一半的狀態存不下來，所以已排的頁全部重做一次（diag257：第 5 章 61 頁、第 6 章 11 頁）。
    //   讀者一路翻進（或長按跳到）正在預排的那一章時，直接把預排中的 Section 當成這一章：它的建置照常在背景繼續，
    //   已排的頁（記憶體裡的頁表＋tmp 檔）立刻可讀，跟一般「開章→排到落地頁→背景排完」是同一個狀態。
    //   只在【完全等價】時接手：同一個 spine、排版規格逐欄相同、落地是第 0 頁、沒有任何跳頁／錨點／百分比／
    //   書籤 offset／設定重定位、不在註腳裡。其餘照 v257 停下。
    //   CSS 解析器：預排開始之後沒有別的建置用過它（所有 startBuild 站點都會先經過這裡或 loop 的執行期保險把預排停掉），
    //   所以它還是這一章的內容。advance 表會在下面照常清（v254 起排版結果不依賴表的歷史）。
    const bool plainForwardEntry =
        !pendingPercentJump && !pendingPageJump.has_value() && !pendingOffsetJump.has_value() && pendingAnchor.empty() &&
        footnoteDepth == 0 && nextPageNumber == 0 &&
        !(cachedVisibleTextOffset.has_value() && currentSpineIndex == cachedSpineIndex);
    //   （cachedChapterTotalPageCount 不列入：它只在 spine＝cachedSpineIndex 時由建置收尾的 applyDeferredReposition 使用，
    //    接手與一般路徑在收尾時走同一個函式，語意相同；列進來會讓開書後第一次換章幾乎都不接手。）
    const bool adoptPrebuild = !g_prebuildDisabledThisBoot && nextSection_ && nextPrebuildSpine_ == currentSpineIndex &&
                               nextSection_->isBuilding() && plainForwardEntry && nextBuildSpec_ == renderSpec &&
                               (epub->getCssParser() ? epub->getCssParser()->mutationSeq_ : 0) == nextBuildCssSeq_;
    if (adoptPrebuild) {
      char wit[160];
      nextBuildWitness(wit, sizeof(wit));
      DiagLog::line("NEXTBUILD adopt spine=%d pages=%u ms=%lu ticks=%lu %s", currentSpineIndex,
                    static_cast<unsigned>(nextSection_->builtPageCount()),
                    static_cast<unsigned long>(millis() - nextBuildStartMs_), static_cast<unsigned long>(nextBuildTicks_),
                    wit);
    } else {
      stopNextChapterPrebuild(nextPrebuildSpine_ == currentSpineIndex ? "enter" : "reset");
    }
    nextPrebuildSpine_ = -1;
    nextPrebuiltReadySpine_ = -1;
    nextPrebuildWaiting_ = false;
    const unsigned long chapterOpenT0 = millis();
    // v193：唯一收斂點——所有換章／重建都 section.reset() 後走到這裡。
    // 只在 spine 真的變了才清 advance 表；同章重建（方向、設定、partial 重開）不准清。
    if (currentSpineIndex != lastAdvanceSpine_) {
      if (SdCardFont* rf = sdFontSystem.currentReaderFont()) {
        uint32_t kept = 0;
        const uint32_t used = rf->resetAdvanceTables(&kept);
        // v278：`sinceEnter` ＝ onEnter 結束到這裡的毫秒（喚醒路徑的後半段）。
        // ⚠️ **取走即失效**（`exchange(0)`，複查抓到）：不清掉的話，同一次進閱讀器之後的每一次換章
        //    都會印出「距離上一次 onEnter」——那是另一件事，會把判讀帶歪。之後的行印 -1。
        const uint32_t enterMark = readerEnterDoneMs_.exchange(0, std::memory_order_relaxed);
        DiagLog::line("ADVRESET spine=%d used=%u kept=%u sinceEnter=%ld", currentSpineIndex,
                      static_cast<unsigned>(used), static_cast<unsigned>(kept),
                      enterMark == 0 ? -1L
                                     : static_cast<long>(static_cast<uint32_t>(millis()) -
                                                         enterMark));  // v253：kept＝CJK 掃描就緒後留著的標點／拉丁字寬
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
    if (adoptPrebuild) {
      section = std::move(nextSection_);  // v258：接手（nextSection_ 變成空，下面三行照常清旗標）
    } else {
      section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    }
    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;
    landingPending_ = true;  // v189：這次 render 是落地，落地頁定案後蓋 deferredLandingPage_

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
    // v258：接手的 Section 已經在建置中，不能再讀快取檔（它的 tmp 檔還開著、頁表在記憶體）。
    const bool cacheLoaded = !adoptPrebuild && section->loadSectionFile(renderSpec);
    // v187 證人：快取被丟掉的原因（1 版號／2 參數／3 CSS 截斷重排／4 partial 壞）；沒有快取不記。
    if (!adoptPrebuild && !cacheLoaded && section->lastLoadReject() != 0) {
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
          if (adoptPrebuild) {
            // v258：已經在建置中 —— 不 startBuild（不重新解壓、不重建解析器）。BUILD end 證人照常從這裡起算。
            // codex：一般路徑在 startBuild 前會放掉保留中的字型快取、並給解析器 popup callback；接手時補上這兩件。
            //   快取只在落地頁還沒排出來（下面要同步排）時放 —— 頁已經在的話，接下來只是畫頁，保留的容量正好重用。
            if (static_cast<int>(section->pageCount) <= target) {
              if (auto* fcm = renderer.getFontCacheManager()) {
                const size_t released = fcm->releaseRetainedCache();
                if (released) DiagLog::line("FONTREL adopt %u", static_cast<unsigned>(released));
              }
            }
            section->setBuildPopupFn([this] { showBuildPopup(); });
            started = true;
            noteBuildStart();
          } else {
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
            // ⭐ **只排到【需要的那一頁】為止，不要固定排 8 頁。**（維護者 2026-09-11 回報：
            //    從目錄連結跳章節時「畫面靜止不動好像在做 indexing，但沒有任何提示」。）
            //    `BUILD_PAGES_PER_CHUNK = 8` 是上游的值，配它註解裡假設的每頁約 30ms
            //    ＝ 一個 chunk 240ms。**這台每頁 100–200ms**（中文＋SD 字型，v189 量的）
            //    → 一個 chunk 就是 0.8–1.6 秒，而目錄連結的目標是第 0 頁：
            //    迴圈只跑【一個】chunk 就結束，卻排了 8 頁 —— 實測 diag232 有 4.7 秒。
            //    ⚠️ 期限彈窗因此永遠不會響：它在迴圈【頂端】，而迴圈只有一輪、
            //    在 t≈0 被評估。這是「儀器放在跑不到的地方」的又一個實例（教訓 B-22）。
            //    → 需求導向的 chunk：目標頁明確時只排差額；錨點／offset 跳頁不知道還差多遠，
            //    一次一頁，讓迴圈條件與期限檢查都有機會在每一頁之後重新評估。
            const int needPages =
                (anchorJump || offsetJump.has_value()) ? 1 : (target + 1 - static_cast<int>(section->pageCount));
            const int chunkPages = needPages < 1                     ? 1
                                   : needPages > BUILD_PAGES_PER_CHUNK ? BUILD_PAGES_PER_CHUNK
                                                                      : needPages;
            if (!section->buildSomeMore(chunkPages)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              buildPopupPending = false;
              if (handleLowMemoryBuild()) return;
              section.reset();
              showBuildError();
              return;
            }
            // 一個 chunk 自己就超過預算時，迴圈條件可能已經滿足 → 上面的檢查不會再跑。
            // 在這裡補一次，讓「等很久」至少在畫面被換掉之前有個交代。
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup();
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }
    // v257 證人：開章花多久（到落地頁可畫為止）、有沒有快取、是不是預排命中。cache 0＝沒有（當場排）1＝完整 2＝partial。
    // v258：prebuilt=2＝接手了正在預排的建置（cache=0，但沒有重排）。
    DiagLog::line("CHAPTER enter spine=%d cache=%u prebuilt=%u open=%lu", currentSpineIndex,
                  cacheComplete ? 1u : (section->isPartial() ? 2u : 0u), adoptPrebuild ? 2u : (prebuiltHit ? 1u : 0u),
                  static_cast<unsigned long>(millis() - chapterOpenT0));

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

  // v261 證人：render 前段分段計時（diag260：冷章快翻時每頁 renderContents 之前多了約 1.2 秒，只有約 0.15 秒是排版步驟）。
  //   sec＝進 render 到這裡（含 !section 開章）；ext＝下面兩個「排到要顯示的那一頁」迴圈；mid＝之後到讀頁之前（書籤旗標等）；
  //   load＝讀頁；steps／pages＝ext 裡同步跑了幾步、排了幾頁。印在 EPLAT。
  const uint32_t tPhaseSection = millis();
  const uint32_t stepsBeforeExt = Section::buildStepCount;
  const int pagesBeforeExt = static_cast<int>(section->pageCount);
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
  // v262：只排到要顯示的那一頁（需求導向），不要一次排 8 頁。diag261 的 EPLAT 分段：新章快翻時每 9–11 次翻頁有一次
  //   ext=1.2–1.4 秒 pages=9–11 的突波（這台每頁 100–200ms，上游的 8 頁假設每頁 30ms）—— 總工作量一樣，但突波讓按鍵排隊。
  //   排不到的頁照舊由 loop() 的背景 tick 在空檔補（落地迴圈 v232 已經是同一個做法）。
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      const int needPages = section->currentPage + 1 - static_cast<int>(section->pageCount);
      const int chunkPages = needPages < 1 ? 1 : (needPages > BUILD_PAGES_PER_CHUNK ? BUILD_PAGES_PER_CHUNK : needPages);
      if (!section->buildSomeMore(chunkPages)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        if (handleLowMemoryBuild()) return;
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  const uint32_t tPhaseExt = millis();
  uint32_t renderTailStartMs = 0;  // v261：renderContents 之後的尾段起點（見 lastRenderTailMs_）
  bool renderTailStarted = false;
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
  // v264：唯一的發佈點 —— 這裡「要畫哪一頁」才真的定案（跳頁、offset、錨點、百分比、重排後的重新定位全部套用完），
  //   而且還在畫之前，所以接下來那張封面圖解碼的整段時間旗標都是對的。codex 第二輪：更早發佈會留下
  //   「已經不是書首、旗標還說是」的空窗（applyDeferredReposition 會換頁），那段時間該有的上一頁會被吃掉。
  applyDeferredReposition();
  publishBookStart();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
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
    const uint32_t tPhaseMid = millis();
    auto p = section->loadPage(pageNo);
    const uint32_t tPhaseLoad = millis();
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
    // v243：放在 renderContents【之後】而不是它裡面的 SEG 行 —— renderContents 有好幾條繪製分支
    // （tiled／tiled-async／fallback），各印各的 SEG；裝進其中一條就是賭那條會跑（CLAUDE.md B-22）。
    // 這裡是所有分支的共同出口。lat 含灰階那幾趟（抗鋸齒關著時就是黑白上面板的時刻）。
    if (pressMs != 0 && static_cast<uint32_t>(renderStartMs - pressMs) < 10000) {  // 無號減法跨得過繞回
      const uint32_t doneMs = millis();
      const SdCardFont* font = sdFontSystem.currentReaderFont();
      // v261：sec／ext／mid／load 見上；ptail＝【上一次】render 在 renderContents 之後的尾段（存進度＋預取等），
      //   它會算進這一次的 wait（按鍵排在它後面）。
      DiagLog::line("EPLAT wait=%u lat=%u render=%u afail=%u rescue=%u sec=%u ext=%u steps=%u pages=%d mid=%u load=%u ptail=%d",
                    static_cast<unsigned>(renderStartMs - pressMs), static_cast<unsigned>(doneMs - pressMs),
                    static_cast<unsigned>(doneMs - start),
                    font ? static_cast<unsigned>(font->getStats().bitmapAllocFailures) : 0u,
                    font ? static_cast<unsigned>(font->getStats().bitmapExactRescues) : 0u,
                    static_cast<unsigned>(tPhaseSection - renderStartMs), static_cast<unsigned>(tPhaseExt - tPhaseSection),
                    static_cast<unsigned>(Section::buildStepCount - stepsBeforeExt),
                    static_cast<int>(section ? section->pageCount : 0) - pagesBeforeExt,
                    static_cast<unsigned>(tPhaseMid - tPhaseExt), static_cast<unsigned>(tPhaseLoad - tPhaseMid),
                    static_cast<int>(prevRenderTailMs));
    }
    lastRenderCompleteMs = millis();
    renderTailStartMs = lastRenderCompleteMs;
    renderTailStarted = true;
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

  // v260（codex 第三輪）：補圖那一遍被按鍵中止時 framebuffer 是重建的文字版／佔位框版，不是面板上真正完成的畫面 ——
  //   截圖留到下一次完整上面板的繪製（中止會留著待補圖，手放開後 loop 會補畫這一頁）。
  if (pendingScreenshot && !imagePassAborted_) {
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
  if (renderTailStarted) lastRenderTailMs_ = static_cast<int32_t>(millis() - renderTailStartMs);  // v261：下一次 EPLAT 的 ptail=
}

// v110/v164：預取下一頁的字型 mini 資料到【同一塊】快取，不新增任何常駐記憶體。
// 前提：當前頁的字在 renderContents 收工之後確定無人使用 —— 灰階兩趟與 cleanup 都在
// renderContents 內完成，狀態列與彈窗走 UI 字型。中止＝乾淨放棄：快取清空＋身分失效
// ⇒ 下一次 render 走冷路徑（＝無預取行為）。
void EpubReaderActivity::prefetchNextPage(const int fontId, const int marginTop, const int marginLeft,
                                          const int basePage, const bool abortOnInput) {
  // 先歸零：pf= 的語意是「即將顯示的這一頁，預取花了多久」。任何一道閘門擋下來都算
  // 「沒有預取」，留著上一次的數字會讓 warm=0 旁邊掛著漂亮的 pf=280。
  // 直排：繪製端的旗標必須與這一頁【被排版時】的軸向一致，否則轉置編碼會被當成
  // 橫排座標畫（xpos 被當 X、focusSuffixX 被當 Y）＝ 一團亂碼且不報錯。
  // 兩者同源於 SETTINGS.documentIsVertical() —— 這是 readerRenderSpec() 用的同一個函式，
  // 而檔頭比對保證用別的軸向排過的快取會被丟掉重排，所以不會不同步。
  const GfxRenderer::VerticalScope verticalScope(renderer, SETTINGS.documentIsVertical());
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
  // v257：預排下一章正在跑（上一次預排 tick 之後讀者沒翻過頁），同樣是建置的記憶體峰值 —— 不多握字型快取
  //   （預排完成時會補做一次預取）。讀者翻頁之後預排暫停 2 秒，這段期間預取照常（codex：不能讓整段預排都是冷翻頁）。
  if (nextSection_ && nextSection_->isBuilding() && !nextBuildTurnSinceTick_) {
    diagPfGate = 10;
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
  // 章末：下一頁在另一個 section，換章一律 section.reset() → 冷路徑。
  // v257：下一章已經預排好（完整快取）時，把它的第一頁的字先讀進來 —— 換章的 render 以 (spine+1, 第 0 頁) 的身分命中。
  if (next >= static_cast<int>(section->pageCount)) {
    if (prefetchIntoNextChapter(fontId, marginTop, marginLeft, abortOnInput)) return;
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

bool EpubReaderActivity::prefetchIntoNextChapter(const int fontId, const int marginTop, const int marginLeft,
                                                 const bool abortOnInput) {
  const int nextSpine = currentSpineIndex + 1;
  if (g_prebuildDisabledThisBoot || !epub || nextSpine >= epub->getSpineItemsCount() ||
      nextPrebuiltReadySpine_ != nextSpine || nextSection_ || buildViewportWidth == 0 || footnoteDepth > 0) {
    return false;
  }
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm || !sdFontSystem.currentReaderFont() || showBookmarkMessage) return false;

  const unsigned long t0 = millis();
  // 身分：跟換章那次 render 的 buildWarmIdentity(0) 同一組欄位，只是 spine 換成下一章。
  WarmIdentity target = buildWarmIdentity(0);
  target.spineIndex = nextSpine;
  bool completed = false;
  {
    auto scope = fcm->createPrewarmScope();  // ctor 清掉剛畫完那一頁的快取（同一般預取）
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    const size_t retained = fcm->retainedMiniBitmapCapacity(fontId);
    diagPfMaxKb = static_cast<uint16_t>(largest / 1024);
    diagPfRetKb = static_cast<uint16_t>(retained / 1024);
    if (largest + retained < 32 * 1024) {  // 同一般預取的地板
      diagPfGate = 7;
      return true;
    }
    // 暫時的 Section 只用來讀快取裡的第 0 頁（header＋LUT＋一頁），不建置。
    const PrebuildInFlight inFlight;  // 同一個保險絲：這條也是「沒人叫它就自己跑」的新路徑
    std::unique_ptr<Section> next(new (std::nothrow) Section(epub, nextSpine, renderer));
    if (!next || !next->loadSectionFile(SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight)) ||
        next->isPartial()) {
      diagPfGate = 11;
      nextPrebuiltReadySpine_ = -1;  // 快取不在或不再相符（例如設定剛改）：不再嘗試
      return true;
    }
    auto page = next->loadPage(0);
    if (!page) {
      diagPfGate = 8;
      return true;
    }
    scope.setRetainCacheOnExit(true);
    page->render(renderer, fontId, marginLeft, marginTop);  // scan 模式，framebuffer 不動
    completed = scope.endScanAndPrewarmAbortable(
        abortOnInput ? &EpubReaderActivity::prefetchShouldAbortOrInput : &EpubReaderActivity::prefetchShouldAbort, this);
  }
  diagPfGate = completed ? 0 : 9;
  if (completed) {
    fcm->adoptWarmIdentity(target);
    diagPrefetchMs = static_cast<unsigned>(millis() - t0);
  } else {
    fcm->clearCache();
  }
  DiagLog::line("XPREFETCH spine=%d ok=%u ms=%lu", nextSpine, completed ? 1u : 0u,
                static_cast<unsigned long>(millis() - t0));
  return true;
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
  w.lineHeightEmBits = WarmIdentity::floatBits(static_cast<float>(
      renderer.getReaderLineHeight(SETTINGS.getReaderFontId(), SETTINGS.getReaderLinePitchEm())));
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
// v267：灰階帶狀暫存的配置階梯（BW 那一趟之前與之後共用同一支）。回傳實際列數，0 ＝沒配到。
//   v266 實機量到：擋住的是**最大連續塊**（決策當下 11–14KB），總量卻有 33–44KB ——
//   像素快取以 16KB 為單位把 p2 切走 2–5 塊，而灰階這塊必須連續（面板吃單一緩衝）。
//   所以 v267 改成「BW 之前就配」，趁堆還沒被切碎；這支函式兩處共用，門檻與階梯維持 v265 的值。
//   配前：free ≥ bytes+24KB 且 maxAlloc ≥ bytes+8KB；配後**再量一次**（配置本身的開銷、別的 task 插隊都會吃掉餘裕）。
// v267（codex 第三輪）：**用「沒有預配的話會怎樣」來決定**，不能用預配之後量到的尾段 ——
//   預配本身會讓像素快取少載一塊 16KB，那塊就變成尾段，然後被拿來正當化自己（循環論證）。
//   這裡在 BW 之前用算術把兩種情況都推出來：
//     快取大小 ≈ ceil(寬/4)×高（2 bits/px）；快取載入器的閘門是「剩餘 ≥ 這塊(16KB)＋24KB」，
//     每載一塊剩餘就少 16KB → 沒預配能載 n0 塊、預配 B 位元組後能載 n1 塊，差額就是被擠掉的量。
//   值得的條件（少載 D、趟數從 N 降到 M）：tail0 × (N − M) > D × M。
//   兩邊都只是估算（TLSF 取整、別的 task 插隊都會讓實際值不同），所以證人要印出實際結果對照。
constexpr int GRAY_STRIP_ROWS_TEXT = 80;  // 純文字頁的帶高（＝換基底後一直用的固定值）
constexpr int kGrayStripLadder[] = {264, 176, 132};

// v267：配一塊【指定列數】的灰階帶狀暫存。門檻與 v265 相同：
//   配前 free ≥ bytes+24KB 且 maxAlloc ≥ bytes+8KB；配完**再量一次**（配置本身的開銷、別的 task 插隊都會吃掉餘裕）。
//   ⚠️ 只配指定的那一階 —— 規劃時算過「這一階值不值得」（planGrayStrip），配的時候不可以自己換一階（codex 抓到）。
static bool allocGrayStripRows(std::unique_ptr<uint8_t[]>& out, const int rows, const int gwBytes) {
  constexpr size_t GRAY_STRIP_MIN_FREE_AFTER = 24 * 1024;
  constexpr size_t GRAY_STRIP_MIN_MAX_AFTER = 8 * 1024;
  const size_t bytes = static_cast<size_t>(gwBytes) * static_cast<size_t>(rows);
  if (ESP.getFreeHeap() < bytes + GRAY_STRIP_MIN_FREE_AFTER) return false;
  if (ESP.getMaxAllocHeap() < bytes + GRAY_STRIP_MIN_MAX_AFTER) return false;
  auto buf = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!buf) return false;
  if (ESP.getFreeHeap() < GRAY_STRIP_MIN_FREE_AFTER || ESP.getMaxAllocHeap() < GRAY_STRIP_MIN_MAX_AFTER) {
    return false;  // buf 在這裡解構＝還回去（剛切出來立刻還，會與鄰居合併、不留洞）
  }
  out = std::move(buf);
  return true;
}

// BW 之後的補救路徑用：此時像素快取大小已定，拉高不會再擠掉它 → 哪一階配得到就用哪一階。
static int allocGrayStripLadder(std::unique_ptr<uint8_t[]>& out, const int gh, const int gwBytes, unsigned& freeAt,
                                unsigned& maxAt) {
  freeAt = ESP.getFreeHeap();
  maxAt = ESP.getMaxAllocHeap();
  for (const int rows : kGrayStripLadder) {
    if (rows > gh) continue;
    if (allocGrayStripRows(out, rows, gwBytes)) return rows;
  }
  return 0;
}

// v267（codex 第三輪）：**用「沒有預配的話會怎樣」來決定**，不能用預配之後量到的尾段 ——
//   預配本身會讓像素快取少載一塊 16KB，那塊就變成尾段，然後被拿來正當化自己（循環論證）。
//   這裡在 BW 之前用算術把兩種情況都推出來：
//     快取大小 ＝ ceil(寬/4)×高（2 bits/px）；快取載入器的閘門是「剩餘 ≥ 這塊(16KB)＋24KB」，
//     每載一塊剩餘就少 16KB → 沒預配能載 n0 塊、預配 B 位元組後能載 n1 塊，差額就是被擠掉的量。
//   值得的條件（少載 D、趟數從 N 降到 M）：tail0 × (N − M) > D × M。
//   估算會有誤差（TLSF 取整、別的 task 插隊），所以證人把估的 `est=` 與實際的 `tail=` 都印出來對照。
//   ⭐ v267 實機結果：**沒有預配的頁，`est=` 與實際 `tail=` 逐位元組相同**（76,544）→ 反事實模型是對的。
//      有預配的頁模型偏樂觀（預測 slot 還能載 1–2 塊、實際 0 塊）—— 因為這裡只看 free 總量、沒看最大連續塊。
//      loaded0 與 loaded1 同時被高估 ⇒ 尾段被低估；排擠量是兩者的差，**差值仍可能被低估**
//      （codex：同時高估不等於差值保守，這是論證不是保證）。所以成本才多算一塊 16KB 當邊際，
//      而 `est=` 對 `tail=` 這個證人就是用來抓模型偏掉的 —— 偏了就調，不要靠推論。
struct GrayStripPlan {
  int rows = 0;              // 規劃要配的帶高（0＝不預配）
  uint32_t tailWithout = 0;  // 沒預配時預估的尾段（證人）
  uint32_t displaced = 0;    // 預估因為預配而少載的快取量（留不留那塊也要扣掉它，見下）
};
// v268：**每一階都同時檢查「划不划算」與「配不配得到」**，取第一個兩邊都過的。
//   v267 是「先選一階、再去配、配不到就放棄」—— 實機有兩頁 est=76,544／disp=0（拉高零代價）卻什麼都沒配到，
//   因為規劃選了 264、當下最大塊只配得下 176。配不到的那一階不會留下任何東西（閘門先擋，或 buf 立刻還回去）。
static GrayStripPlan planAndAllocGrayStrip(std::unique_ptr<uint8_t[]>& out, const uint32_t pxcBytes, const int gh,
                                           const int gwBytes) {
  constexpr uint32_t CHUNK = 16 * 1024;         // ImageBlock 的 PXC_CHUNK_SIZE
  constexpr uint32_t SLOT_RESERVE = 24 * 1024;  // ImageBlock 的 PXC_HEAP_RESERVE
  GrayStripPlan plan;
  if (pxcBytes == 0) return plan;
  const uint32_t freeNow = ESP.getFreeHeap();
  // 忠實照 loadPxcSlot 的迴圈算：每次要的是 min(剩下的, 16KB)，閘門是「free ≥ 這次要的＋24KB」，
  //   載完 free 就少那麼多。**最後一塊通常不滿 16KB**，用整塊去算會低估載入量、把尾段算大（codex：
  //   估錯的方向就是會誤判成「值得拉高」，然後自己把尾段製造出來）。
  const auto simulateLoad = [&](const uint32_t reserved) -> uint32_t {
    if (freeNow < reserved) return 0;
    uint32_t avail = freeNow - reserved;
    uint32_t loaded = 0;
    while (loaded < pxcBytes) {
      const uint32_t want = (pxcBytes - loaded) < CHUNK ? (pxcBytes - loaded) : CHUNK;
      if (avail < want + SLOT_RESERVE) break;
      avail -= want;
      loaded += want;
    }
    return loaded;
  };
  const uint32_t loaded0 = simulateLoad(0);
  plan.tailWithout = pxcBytes > loaded0 ? pxcBytes - loaded0 : 0;
  if (plan.tailWithout == 0) return plan;  // 整張進得了 RAM → 拉高只會把尾段製造出來
  const int passesText = ((gh + GRAY_STRIP_ROWS_TEXT - 1) / GRAY_STRIP_ROWS_TEXT) * 2;
  // 值得的條件：少載 D、趟數 N→M ⇒ tail0 × (N − M) > D × M（＝淨收益 > 0，單位是「少讀的位元組」）。
  // ⚠️ 成本多算一塊 16KB 當安全邊際 —— 配置器取整、別的 task 插隊都可能讓實際多擠掉一塊。
  // v268：**依模型淨收益排序再去配**，不是「最大的划算就好」（codex）：矮一階擠掉的快取更少，
  //   淨收益有可能反而大。配不到就換下一個候選（矮的要的連續塊更小，常常配得到）。
  struct Cand {
    int rows;
    int64_t net;
    uint32_t displaced;
  };
  Cand cands[sizeof(kGrayStripLadder) / sizeof(kGrayStripLadder[0])];
  int candCount = 0;
  for (const int rows : kGrayStripLadder) {
    if (rows > gh) continue;
    const uint32_t bytes = static_cast<uint32_t>(gwBytes) * static_cast<uint32_t>(rows);
    const uint32_t loaded1 = simulateLoad(bytes);
    const uint32_t displaced = loaded0 > loaded1 ? loaded0 - loaded1 : 0;
    const int passesTall = ((gh + rows - 1) / rows) * 2;
    if (passesTall >= passesText) continue;
    const int64_t gain = static_cast<int64_t>(plan.tailWithout) * (passesText - passesTall);
    const int64_t cost = (static_cast<int64_t>(displaced) + CHUNK) * passesTall;
    if (gain <= cost) continue;
    cands[candCount++] = Cand{rows, gain - cost, displaced};
  }
  // 插入排序：淨收益大的先試；同分時帶高大的先（趟數少，CPU 也省）。最多 3 個候選。
  for (int i = 1; i < candCount; i++) {
    const Cand key = cands[i];
    int j = i - 1;
    while (j >= 0 && (cands[j].net < key.net || (cands[j].net == key.net && cands[j].rows < key.rows))) {
      cands[j + 1] = cands[j];
      j--;
    }
    cands[j + 1] = key;
  }
  for (int i = 0; i < candCount; i++) {
    if (!allocGrayStripRows(out, cands[i].rows, gwBytes)) continue;
    plan.rows = cands[i].rows;
    plan.displaced = cands[i].displaced;
    break;
  }
  return plan;
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int pageNo, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  // v191：每頁一次抽取機會；灰階帶迴圈在本函式內，不在這裡清。
  // 直排：繪製端的旗標必須與這一頁【被排版時】的軸向一致，否則轉置編碼會被當成
  // 橫排座標畫（xpos 被當 X、focusSuffixX 被當 Y）＝ 一團亂碼且不報錯。
  // 兩者同源於 SETTINGS.documentIsVertical() —— 這是 readerRenderSpec() 用的同一個函式，
  // 而檔頭比對保證用別的軸向排過的快取會被丟掉重排，所以不會不同步。
  // 證人：畫的軸向與進入時的軸向不一致 ＝ 上面那個時序危害真的發生了。
  //    症狀在畫面上是「一團亂碼」而且不報錯，所以它只能靠 log 被看見。
  if (SETTINGS.documentIsVertical() != axisAtEnter_ && !axisSplitLogged_) {
    axisSplitLogged_ = true;
    DiagLog::line("AXISSPLIT enter=%d now=%d", axisAtEnter_ ? 1 : 0, SETTINGS.documentIsVertical() ? 1 : 0);
  }
  const GfxRenderer::VerticalScope verticalScope(renderer, SETTINGS.documentIsVertical());
  // 直排證人（B-22）。
  // ⚠️ **v206 的教訓**：上一版把這行放在 `SEG tiled-async` 那個分支裡，而那個分支在這台
  //    機器上【一次都沒跑過】（實測 diag206.log 出現 0 次；真正跑的是 SEG prewarm ×83、
  //    SEG tiled ×9）。整輪實機測試因此拿不到任何直排的證據 —— 本專案第四次踩同一條。
  //    → 改成 RAII：解構子在 renderContents 的【每一條】離開路徑都會跑，
  //      不必去數這個函式有幾個 return、也不必在四個 SEG 站點各貼一次。
  struct VertWitness {
    const GfxRenderer& r;
    ~VertWitness() {
      if (!r.isVerticalLayout()) return;
      DiagLog::line("VERT vdraw=%u vrot=%u", static_cast<unsigned>(r.takeVerticalDrawCount()),
                    static_cast<unsigned>(r.takeVerticalRotCount()));
    }
  } vertWitness{renderer};

  // v260：這次繪製開始時的按鍵序號（補圖解碼的中止基準，見 ImageToFramebufferDecoder::armInputAbort）。
  const uint32_t renderInputSeq = ImageToFramebufferDecoder::currentInputSeq();
  imagePassAborted_ = false;
  ImageBlock::clearRetryableFailures();
  // v256：沒有背景排版在跑的畫頁，才准暫時性失敗的圖用額度重試（排版中記憶體最緊，重試多半白費；排版結束的 IMGHEAL 重畫就會落在這裡）。
  ImageBlock::setTransientRetryAllowed(section && !buildBurstActive());
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
    // v245：這一遍當文字頁畫了、但沒有任何一張圖真的被延後（例如圖超出邊界）→ 仍要補一次重繪，
    // 讓那一頁走完整圖片流程（與 v244 以前的結果相同），不准停在「只有字」（codex 複查）。
    bool forceRedraw = false;
    DeferHeavyGuard(EpubReaderActivity* s, bool defer, int sp, int pg)
        : self(s), spine(sp), page(pg), startCount(ImageBlock::deferredDecodeCount()) {
      ImageBlock::setDeferHeavyDecode(defer);
    }
    ~DeferHeavyGuard() {
      ImageBlock::setDeferHeavyDecode(false);
      const uint32_t seen = ImageBlock::deferredDecodeCount();
      if (seen > startCount || forceRedraw) {
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
    ~PxcSlotGuard() {
      // v249 儀器：這一頁像素快取的 RAM／SD 用量（解構子＝所有離開路徑都會印，教訓 B-22）。只在有畫圖時印。
      // 先放掉 slot 再寫 log（codex：寫 log 要開檔配置，別在 slot 還握著 RAM 的時候寫）。free／max 是放掉之前的值。
      const ImageBlock::PxcStats st = ImageBlock::pxcStats;
      const unsigned heldFree = ESP.getFreeHeap();
      const unsigned heldMax = ESP.getMaxAllocHeap();
      ImageBlock::pxcStats = ImageBlock::PxcStats{};
      ImageBlock::releaseRenderCache();
      if (st.ramPasses + st.sdPasses + st.otherPasses > 0) {
        DiagLog::line("PXCSLOT total=%u loaded=%u ram=%u sd=%u sdKB=%u sdMs=%u other=%u/%uKB/%ums buf=%u abandon=%u "
                      "free=%u max=%u",
                      static_cast<unsigned>(st.totalBytes), static_cast<unsigned>(st.loadedBytes),
                      static_cast<unsigned>(st.ramPasses), static_cast<unsigned>(st.sdPasses),
                      static_cast<unsigned>(st.sdBytes / 1024), static_cast<unsigned>(st.sdMs),
                      static_cast<unsigned>(st.otherPasses), static_cast<unsigned>(st.otherBytes / 1024),
                      static_cast<unsigned>(st.otherMs), static_cast<unsigned>(st.sdBufMin),
                      static_cast<unsigned>(st.abandons), heldFree, heldMax);
      }
    }
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
  // v245（實機回報「第一次開圖片先出方框、閃好幾次才出圖」）：v193 的延後讓第一次看到的圖片頁把
  //   下面的圖片流程跑兩遍 —— 先閃佔位框頁（v148）→ 〔HALF〕→ 圖區塗白 FAST → 佔位框 FAST → 灰階 →
  //   下一頁強制 HALF；補圖那一遍再全部來一次（而且第一遍設的 pagesUntilFullRefresh=1 讓它必 HALF）。
  //   diag244-2 書首封面：第一遍 total=2,468、第二遍 bw=7,133 total=9,444。
  // 改成：延後的那一遍【當文字頁畫】—— 不閃佔位框、圖區留白（ImageBlock 延後時不畫框）、走一般翻頁節奏、
  //   不跑圖片灰階、不把下一頁設成 HALF；補圖那一遍才走完整圖片流程，而且面板上既然就是這頁的文字版，
  //   跳過「先閃佔位框頁」。
  const bool deferredTextOnlyDraw = firstDraw && pageHasImagesNeedingDecode;
  const bool deferredKeyMatches = !firstDraw && deferredTextOnlySpine_ == renderSpine && deferredTextOnlyPage_ == pageNo;
  const bool followsDeferredDraw = deferredKeyMatches && renderer.displayFrameSeq() == deferredTextOnlyFrameSeq_;
  // codex 複查：身分對得上、但中間有別的畫面上過面板（選單等）→ 面板基底不可信，補圖那一遍強制清底。
  const bool deferredBaseStale = deferredKeyMatches && !followsDeferredDraw;
  deferredTextOnlySpine_ = -1;  // 讀完即清：只給緊接著的那一次用
  deferredTextOnlyPage_ = -1;
  const bool imagePipeline = pageHasImages && !deferredTextOnlyDraw;
  if (deferredTextOnlyDraw) DiagLog::line("IMGDEFER textonly spine=%d page=%d", renderSpine, pageNo);
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  // The reader starts with zero here, which means the normal refresh cycle
  // would use a HALF refresh for its first page. Keep that same clean base for
  // image pages: their double-FAST path otherwise runs directly over the
  // retained frame after a silent restart (for example, when returning from
  // KOReader sync), leaving the old UI mixed with the image.
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1 || deferredBaseStale;
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
  const bool needsAnyGrayscale = needsTextGrayscale || imagePipeline;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Whole-plane buffering only pays when the BW refresh genuinely runs async
  // underneath it; on blocking panels (X3) it would just spend ~50 KB for the
  // identical serial timing. Image pages take the blocking double-FAST path
  // below (no async refresh is ever started), so they'd spend the buffers with
  // nothing in flight to overlap.
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !imagePipeline;
  // v267：**灰階的帶狀暫存在這裡就先配**（BW 那一趟之前）—— 見 planGrayStrip／allocGrayStripRows 的註解：
  //   晚到就只剩像素快取切剩的 11–14KB 碎片。只對「有圖、而且這次不用解碼」的頁預配：
  //   第一次解碼的頁要把堆留給解碼器與 inflate，而且那些頁的快取本來就整張進得了 RAM（實機 sd=0KB）。
  //   預配之後若發現尾段其實很小（小圖頁），下面會當場放掉、走回 80 列。
  std::unique_ptr<uint8_t[]> grayScratch;
  int grayScratchRows = 0;
  uint32_t grayStripTailEst = 0;       // 沒預配時預估的尾段（證人；0＝沒算過）
  uint32_t grayStripDisplacedEst = 0;  // 預估因為預配而少載的快取量（證人：對照實際的 tail=）
  int grayStripRetryRows = 0;          // MSB 之前重試配到的帶高（0＝沒配到／沒試）
  // [0]＝BW 之前的預配，[1]＝BW 之後的補救，[2]＝MSB 之前的重試（0＝那一次沒試）
  unsigned grayStripFree[3] = {0, 0, 0};
  unsigned grayStripMax[3] = {0, 0, 0};
  if (tiledGrayscale && !overlapRefresh && imagePipeline && !pageHasImagesNeedingDecode && page->imageCount() == 1) {
    // ⚠️ **只對單圖頁預配**（codex）：多圖頁只有第一張能進 slot，用外框估會高估它的快取大小 →
    //   可能核准了預配、反而把尾段製造出來。多圖頁走 BW 之後的補救路徑（那時尾段是真的）。
    int16_t bx = 0, by = 0, bw = 0, bh = 0;
    uint32_t pxcBytes = 0;
    if (page->getImageBoundingBox(bx, by, bw, bh) && bw > 0 && bh > 0) {
      pxcBytes = static_cast<uint32_t>((bw + 3) / 4) * static_cast<uint32_t>(bh);  // 2 bits/px
    }
    const int gwBytes = renderer.getDisplayWidthBytes();
    grayStripFree[0] = ESP.getFreeHeap();  // 證人：決策當下的堆（成敗都記，配不到時這兩個數字才是要查的）
    grayStripMax[0] = ESP.getMaxAllocHeap();
    const auto plan = planAndAllocGrayStrip(grayScratch, pxcBytes, renderer.getDisplayHeight(), gwBytes);
    grayStripTailEst = plan.tailWithout;
    grayScratchRows = plan.rows;
    grayStripDisplacedEst = plan.displaced;
  }
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  // v260：補圖那一遍被按鍵中止 → 整遍放棄。面板與記憶體裡的畫面要一致（codex：之後的書籤彈窗、截圖是畫在
  //   framebuffer 上的），所以把面板上那一版（文字版或佔位框版）重畫回 framebuffer 再離開，不上面板、不跑灰階。
  //   placeholderShown：這一遍已經把佔位框版送上面板了。
  const auto abortImagePass = [&](const bool placeholderShown, const uint32_t decodeMs) {
    imagePassAborted_ = true;  // render() 尾端：這一次不截圖
    deferHeavyGuard.forceRedraw = true;  // 待補圖留著：翻走了 loop 會 drop；沒翻走（手放開後）loop 再補
    if (manualRefreshPending) forcedRefreshPending = true;  // codex：這一遍沒有上面板，手動清底的要求不能被吃掉
    if (followsDeferredDraw || placeholderShown) {
      // 面板上就是這一頁（文字版或佔位框版）→ 重補時跳過預閃。
      deferredTextOnlySpine_ = renderSpine;
      deferredTextOnlyPage_ = pageNo;
      deferredTextOnlyFrameSeq_ = renderer.displayFrameSeq();
    }
    // 字型快取若在解碼期間被紓解作廢，重建一次（否則下面整頁走 miss ring）。
    if (fcm && !fcm->warmIdentity().matches(current)) {
      scope.reset();
      scope.emplace(fcm->createPrewarmScope());
      scope->setRetainCacheOnExit(!section->isBuilding());
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan only
      if (scope->endScanAndPrewarmAbortable(nullptr, nullptr)) fcm->adoptWarmIdentity(current);
    }
    renderer.clearScreen();
    if (placeholderShown) {
      page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      ImageBlock::setDeferHeavyDecode(true);  // 沒快取的圖跳過（不解碼、不畫框）＝文字版
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      ImageBlock::setDeferHeavyDecode(false);
    }
    renderStatusBar();
    DiagLog::line("IMGABORT spine=%d page=%d ms=%u text=%u ph=%u", renderSpine, pageNo, static_cast<unsigned>(decodeMs),
                  followsDeferredDraw ? 1u : 0u, placeholderShown ? 1u : 0u);
  };

  if (pageHasImagesNeedingDecode && !deferredTextOnlyDraw) {
    // v260：繪製開始後已經按了翻頁／返回 → 連佔位框預閃都不做，直接放棄這一遍。
    if (ImageToFramebufferDecoder::inputSeqChangedSince(renderInputSeq)) {
      abortImagePass(false, 0);
      return;
    }
    if (followsDeferredDraw) {
      // v245：面板上已經是這一頁的文字版（圖區留白），再閃一次佔位框頁只會多出一個方框。
      DiagLog::line("IMGDEFER nopreflash spine=%d page=%d", renderSpine, pageNo);
    } else {
      page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderStatusBar();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
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
    const uint32_t tImgDecStart = millis();  // v246 儀器
    const uint32_t abortsBefore = ImageBlock::decodeAbortCount();
    {
      // v260：這一段（冷圖片的解碼）允許按鍵中止，基準是繪製開始時的序號。RAII：renderImages 任何離開路徑都 disarm。
      struct InputAbortArm {
        explicit InputAbortArm(const uint32_t seq) { ImageToFramebufferDecoder::armInputAbort(true, seq); }
        ~InputAbortArm() { ImageToFramebufferDecoder::armInputAbort(false, 0); }
      } arm{renderInputSeq};
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    const uint32_t tImgDecEnd = millis();
    // codex 第二輪：最後一次解碼回呼之後、renderImages 返回之前按的鍵，計數不會變 —— 序號也要看。
    //   這時圖可能已經解完寫進快取（沒有白做），但後面的預熱＋雙 FAST＋灰階約 2 秒不該擋住使用者要的下一頁。
    if (ImageBlock::decodeAbortCount() != abortsBefore || ImageToFramebufferDecoder::inputSeqChangedSince(renderInputSeq)) {
      // 已經解完、寫好快取的其他圖不受影響；被中止的那張半截快取已由轉換器刪除。
      abortImagePass(!followsDeferredDraw, tImgDecEnd - tImgDecStart);
      return;
    }
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
    // v246 儀器：補圖那一遍的 bw＝這兩段＋整頁從快取畫一次。IMGDEC（loop 印）拆的是 imgs 裡的單張。
    DiagLog::line("IMGPAGE imgs=%u rescan=%u spine=%d page=%d", static_cast<unsigned>(tImgDecEnd - tImgDecStart),
                  static_cast<unsigned>(millis() - tImgDecEnd), renderSpine, pageNo);
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();
  if (deferredTextOnlyDraw && ImageBlock::deferredDecodeCount() == deferHeavyGuard.startCount) {
    deferHeavyGuard.forceRedraw = true;
    DiagLog::line("IMGDEFER textonly-nodefer spine=%d page=%d", renderSpine, pageNo);
  }

  // v260（codex 第二輪）：補圖那一遍在上面板之前再看一次 —— 重掃預熱約 0.3 秒期間按的鍵。圖已在快取，放棄只是不上面板。
  if (pageHasImagesNeedingDecode && !deferredTextOnlyDraw && ImageToFramebufferDecoder::inputSeqChangedSince(renderInputSeq)) {
    abortImagePass(!followsDeferredDraw, 0);
    return;
  }

  if (imagePipeline) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // v270（維護者：「封面進去會閃幾下，體感很慢」）：**圖區先塗白，再清底。**
      //   原本的順序是「清底（此時 framebuffer 裡是含圖的完整頁 → 封面出現）→ 塗白（封面被擦掉）→ 再畫回去」，
      //   那次「出現又擦掉」是純粹多出來的一次全螢幕刷新（0.44 秒）＋一次閃動。
      //   改成先塗白：清底那一次顯示的就是「文字清乾淨、圖區留白」，接著一次快速刷新把圖放上去。
      //   ⚠️ 上游那條「不要用 HALF 直接上圖」的理由保持不變 —— 圖區最後仍然是【從白色用 FAST 畫上去】，
      //      粒子狀態與原本的雙 FAST 相同（最後一步同樣是 FAST），灰階的微調才推得動。
      //      ⚠️ 這是波形層的推論，程式證不了 —— 靠實機目視（殘影／灰階變差就退版）。
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      if (cleanImageBasePending) {
        // 圖片頁刻意留在 HALF（=GC 清底，v130 同）；scrub bench 時記下來，免得這次閃黑被算到 scrub 頭上。
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        if (ReaderUtils::scrubCleanActive(renderer)) DiagLog::line("CLEAN img bank=%u", static_cast<unsigned>(renderer.lastRefreshBank()));
      } else {
        // 沒有清底需求時維持原本的雙 FAST（第一次就是這個「塗白」）。
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }

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
  // re-rendered ceil(H/stripRows) times per plane, but renderCharImpl culls
  // out-of-band glyphs before decode so the cost stays close to one render.
  // Both text (drawPixel) and images (DirectPixelWriter) honor the active
  // strip target. When the BW refresh above went out async, the plane
  // rendering below overlaps the panel's refresh time; only the controller
  // RAM writes wait for BUSY.
  // v269（實機回報：從封面跳回主畫面「灰灰的、像褪色的報紙」）：
  //   抗鋸齒是在黑白之後把灰「推」上面板；收尾的 cleanup 把控制器平面改寫成黑白畫面，
  //   但面板上留著那些灰 → 下一次整頁刷新若走差分（主畫面用的就是差分），灰就留在原地。
  //   閱讀器內部本來就用 `pagesUntilFullRefresh` 處理同一件事（圖片頁之後強制清潔），跨 activity 沒有人接手。
  //   **在畫的這個 task 直接把「面板髒了」記到顯示層**，由下一次整頁刷新取用（不論是哪個畫面、哪個 task）。
  //   只記圖片頁：整頁的大片灰才看得出來；純文字頁字緣的灰不值得每次回主畫面多付一次清潔刷新（約 0.3 秒＋閃一下）。
  const bool grayPanelDirtyThisPage = needsAnyGrayscale && imagePipeline;

  if (tiledGrayscale) {
    // v265：**有圖的頁把灰階帶拉高**。v56 做過這個自適應（有圖 264、純文字 80，階梯 264→176→80），
    //   換基底時掉回固定 80（memory `tuned-constants-are-features` 的又一例）。
    //   為什麼只能減趟數：直向的實體帶在邏輯上是【直條】（phyY 由邏輯 X 決定），圖片每一列都落在每一趟裡 ——
    //   列剪枝在直向省不到東西，每列只取帶內位元組窗也不行（列距 < 磁區）。v56 實測 SD 讀取 1,345KB → 384KB。
    //   v263／v264 的 log 同樣對得上：全頁圖的像素快取約 92KB，RAM 只放得下 16–82KB，
    //   放不下的尾段**每一趟都重讀一次** → 一頁 656KB–1,182KB、SD 花 0.35–2.41 秒，`lsb`／`msb` 從 0.14 秒漲到 1.06 秒。
    //   只對有圖的頁放大：純文字頁的字在解碼前就被剔除，趟數對它幾乎沒差，不必白付一塊連續配置。
    int stripRows = GRAY_STRIP_ROWS_TEXT;  // 實際採用的帶高（證人印在 SEG tiled 的 strip=）
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    // Render one plane band-by-band into a whole-plane buffer without touching
    // the controller, so it can run while the refresh is still in flight.
    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += stripRows) {
        const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
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
      // v269：**先記再推**。記的是「面板即將留著抗鋸齒的灰」，下一次整頁刷新要走清潔路徑。
      //   順序很重要（codex）：彈窗那類繪製是主任務直接推面板的，若先推灰再記，
      //   中間插進來的那一次差分刷新就會畫在灰上面。先記則是「它取走義務、它負責清」。
      //   取用端一定是整頁刷新（displayBuffer／refreshDisplay），所以義務不會被沒清面板的人吃掉。
      if (grayPanelDirtyThisPage) renderer.noteGrayPanelDirty();
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
      // v265（codex）：要不要拉高，看的不是「這頁有圖」，而是**這頁的像素快取確定放不完**。
      //   ① 有快取、這次不用解碼的頁：slot 在 BW 那一趟載過了，`total > loaded` 就是每一趟都要重讀的尾段。
      //   ② 第一次解碼的頁：slot 要到第一趟灰階才載，這裡 stats 還是 0 → 不拉高（那些頁實機 sd=0KB）。
      //   ③ 小圖的頁（快取整張進 RAM）也不拉高。
      const uint32_t pxcTail = (ImageBlock::pxcStats.totalBytes > ImageBlock::pxcStats.loadedBytes)
                                   ? ImageBlock::pxcStats.totalBytes - ImageBlock::pxcStats.loadedBytes
                                   : 0;
      constexpr uint32_t PXC_TAIL_WORTH_TALL_STRIP = 8 * 1024;  // 尾段太小省不到什麼，不值得佔這塊
      // 多圖頁：slot 只給第一張（`pxcSlotHash == 0` 才認領，之後不會被換掉 —— ImageBlock.cpp 的註解與程式碼），
      //   其餘每一張**整張**每趟重讀（other*）。所以 slot 那張沒有尾段、但同頁另一張在重讀時，一樣值得減趟數。
      const uint32_t pxcOther = ImageBlock::pxcStats.otherBytes;
      const bool tallStripWorthIt =
          pageHasImages && (pxcTail >= PXC_TAIL_WORTH_TALL_STRIP || pxcOther >= PXC_TAIL_WORTH_TALL_STRIP);
      // v267：BW 之前預配的那塊 —— 該不該拉高在預配時就用「沒預配會怎樣」算過了（planGrayStrip），
      //   這裡只再確認一次「這一頁真的有東西在重讀」：估錯而整張都進了 RAM（尾段與別張都 0）就當場放掉。
      std::unique_ptr<uint8_t[]> scratch;
      if (grayScratchRows > 0) {
        // 留不留：**只要真的有東西在重讀就留**。理由（codex 第五輪指出、反過來證明的）：
        //   排擠若真的發生了，那一塊快取已經收不回來 —— 同樣的尾段，趟數少的嚴格優於趟數多的。
        //   放掉只會變成「尾段照樣大、又回到 20 趟」，比不預配還慢。
        //   防止「預配把尾段製造出來」是 planGrayStrip 的工作（反事實估算＋多算一塊的安全邊際），不是這裡。
        //   這裡只處理估錯到「整張都進了 RAM」那一種：什麼都沒重讀 → 這塊沒用，放掉還記憶體。
        if (pxcTail > 0 || pxcOther > 0) {
          scratch = std::move(grayScratch);
          stripRows = grayScratchRows;
        } else {
          grayScratch.reset();
        }
      }
      // 沒預配過的頁（例如這次要解碼的頁）才走 BW 之後的補救 —— 此時 slot 大小已定，量到的尾段是真的，
      //   不會有循環論證。已經預配過又被放掉的頁不再試（那代表尾段本來就小）。
      //   ⚠️ v266 實機：這個時間點的最大連續塊通常只剩 5–7KB，所以補救很少成功。
      if (!scratch && grayScratchRows == 0 && tallStripWorthIt) {
        const int rows = allocGrayStripLadder(scratch, gh, gwBytes, grayStripFree[1], grayStripMax[1]);
        if (rows > 0) stripRows = rows;
      }
      if (!scratch) {
        stripRows = GRAY_STRIP_ROWS_TEXT;
        scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * GRAY_STRIP_ROWS_TEXT);
      }
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * stripRows);
        // v249 證人：v249 讓圖片快取在記憶體緊時也先佔一部分 RAM —— 若因此擠掉這塊暫存，這一頁就沒有灰階，
        // 而原本唯一的跡象是「SEG tiled 那行不見」（codex 複查：要明確量）。
        //   這條分支之後不再畫圖（灰階整頁跳過），slot 先放掉再寫 log；free／max 是放掉之前的值。
        const unsigned skipFree = ESP.getFreeHeap();
        const unsigned skipMax = ESP.getMaxAllocHeap();
        const unsigned slotLoaded = ImageBlock::pxcStats.loadedBytes;
        ImageBlock::releaseRenderCache();
        DiagLog::line("GRAYSKIP scratch=%d img=%u slot=%u free=%u max=%u", gwBytes * stripRows,
                      pageHasImages ? 1u : 0u, slotLoaded, skipFree, skipMax);
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
        for (int y = 0; y < gh; y += stripRows) {
          const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        // v267（codex 第四輪）：**沒有預配過**的頁（多圖頁、這次要解碼的頁）在 MSB 之前再試一次 ——
        //   LSB 那一輪的暫存放掉之後堆可能鬆一點，配到就後半段受惠（v266 就是這條，拿掉會比 v266 差）。
        //   只給沒有預配的頁：預配被放掉的頁代表尾段本來就小，再配一次只是白佔。
        //   LSB 那一輪已經整輪送進面板（`SPI.writeBytes` 同步），換掉暫存不影響它。
        if (grayScratchRows == 0 && tallStripWorthIt && stripRows == GRAY_STRIP_ROWS_TEXT) {
          std::unique_ptr<uint8_t[]> retryBuf;
          unsigned retryFree = 0, retryMax = 0;
          const int rows = allocGrayStripLadder(retryBuf, gh, gwBytes, retryFree, retryMax);
          grayStripFree[2] = retryFree;  // 成敗都記：配不到時這兩個數字才是要查的東西
          grayStripMax[2] = retryMax;
          if (rows > 0) {
            scratch = std::move(retryBuf);  // 舊的 80 列在指派時還回去
            stripRows = rows;
            grayStripRetryRows = rows;
          }
        }

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
        // v269：**先記再推**。記的是「面板即將留著抗鋸齒的灰」，下一次整頁刷新要走清潔路徑。
        //   順序很重要（codex）：彈窗那類繪製是主任務直接推面板的，若先推灰再記，
        //   中間插進來的那一次差分刷新就會畫在灰上面。先記則是「它取走義務、它負責清」。
        //   取用端一定是整頁刷新（displayBuffer／refreshDisplay），所以義務不會被沒清面板的人吃掉。
        if (grayPanelDirtyThisPage) renderer.noteGrayPanelDirty();
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
                      "warm=%u pf=%u wcum=%lu/%lu img=%u dec=%u pg=%u pmax=%u pret=%u strip=%d tail=%u oth=%u "
                      "sfree=%u/%u/%u smax=%u/%u/%u spre=%d/%d est=%u dspl=%u gdirty=%u",
                      tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay,
                      tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0,
                      static_cast<unsigned>(diagWarmHit), diagPrefetchMs,
                      static_cast<unsigned long>(diagWarmCumHits), static_cast<unsigned long>(diagWarmCumTotal),
                      static_cast<unsigned>(pageHasImages ? 1 : 0),
                      static_cast<unsigned>(pageHasImagesNeedingDecode ? 1 : 0), static_cast<unsigned>(diagPfGate), static_cast<unsigned>(diagPfMaxKb), static_cast<unsigned>(diagPfRetKb), stripRows, static_cast<unsigned>(pxcTail), static_cast<unsigned>(pxcOther), grayStripFree[0], grayStripFree[1], grayStripFree[2],
                      grayStripMax[0], grayStripMax[1], grayStripMax[2], grayScratchRows, grayStripRetryRows,
                      static_cast<unsigned>(grayStripTailEst), static_cast<unsigned>(grayStripDisplacedEst),
                      static_cast<unsigned>(grayPanelDirtyThisPage ? 1 : 0));
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
      // v269：**先記再推**。記的是「面板即將留著抗鋸齒的灰」，下一次整頁刷新要走清潔路徑。
      //   順序很重要（codex）：彈窗那類繪製是主任務直接推面板的，若先推灰再記，
      //   中間插進來的那一次差分刷新就會畫在灰上面。先記則是「它取走義務、它負責清」。
      //   取用端一定是整頁刷新（displayBuffer／refreshDisplay），所以義務不會被沒清面板的人吃掉。
      if (grayPanelDirtyThisPage) renderer.noteGrayPanelDirty();
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

  // v245：延後的那一遍畫完了 —— 記下面板上這一幀的身分，給緊接著的補圖那一遍判斷要不要跳過預閃。
  //   只在正常走完時記（上面 storeBwBuffer 失敗的 return 不記 → 保守地照舊預閃）。
  if (deferredTextOnlyDraw) {
    deferredTextOnlySpine_ = renderSpine;
    deferredTextOnlyPage_ = pageNo;
    deferredTextOnlyFrameSeq_ = renderer.displayFrameSeq();
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

  if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
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
