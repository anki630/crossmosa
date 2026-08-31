#include "HomeActivity.h"
#include <DataDir.h>

#include "util/DiagLog.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <JpegToBmpConverter.h>
#include <esp_heap_caps.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsServers) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  // v175（diag174）：縮圖的 JPEG 解碼要 53KB 總量、Epub::load 也要一塊；剛離開閱讀器時 SD 字型
  // （interval 表＋16KB 碼位緩衝）仍常駐，實測只剩 39KB → 每本 THUMBFAIL（why=cache-missing|cache-load-failed /
  // heap 39024<53248）。同 v5 連線前卸載：地板以下先卸字型，下次進閱讀器 ensureLoaded 自動重載。
  // 只在真的有縮圖要產時做一次（否則每次回主畫面都卸＝每次進書都重載）。
  bool reliefChecked = false;
  auto reliefIfLow = [&]() {
    if (reliefChecked) return;
    reliefChecked = true;
    const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (freeNow < 72 * 1024) {
      DiagLog::line("THUMBRELIEF free=%u", static_cast<unsigned>(freeNow));
      sdFontSystem.unloadForLowMemory(renderer);
    }
  };

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      // v174：縮圖比例 0.6 → 2:3。檔名只帶高度，「存在」不等於「是新比例」（A-20）——驗內容：
      // 位元圖寬度小於新目標寬度，就是舊比例裁過的，刪掉重產（自癒，使用者不必清快取）。
      // 0 byte 標記檔 parseHeaders 會失敗 → 視同存在、照舊跳過。
      bool needsThumb = !Storage.exists(coverPath.c_str());
      if (!needsThumb) {
        const int expectedWidth = (coverHeight * 2 + 1) / 3;
        HalFile existing;
        if (Storage.openFileForRead("HOME", coverPath, existing)) {
          Bitmap bmp(existing);
          const bool ok = bmp.parseHeaders() == BmpReaderError::Ok;
          const int w = ok ? bmp.getWidth() : 0;
          existing.close();
          if (ok && w < expectedWidth) {
            Storage.remove(coverPath.c_str());
            needsThumb = true;
            DiagLog::line("THUMBREGEN w=%d<%d %s", w, expectedWidth, book.path.c_str());
          }
        }
      }
      if (needsThumb) {
        reliefIfLow();
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, DataDir::path());
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            // v155（舊樹 v130 系）：純文字彈窗。逐本進度條只在「本與本之間」動一格，
            // 而單核心的縮圖產生把中間整段堵住 —— 條只是閃兩下永遠走不完，
            // 每動一格還多付一次 e-ink 部分刷新。文字一樣有告知效果，畫面安靜得多。
            GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            // v165（A-20）：【不要】抹掉 store 裡的封面路徑。失敗多半是暫時的
            // （記憶體緊、SD 忙），抹掉= 永久負快取，之後任何主題都不再嘗試——
            // 實機就是這樣全部消失的。留著路徑，下次進主畫面重試；
            // 真正永久失敗的（如 progressive JPEG 封面，JPEGDEC 不支援）每次
            // 快速失敗一次、顯示書脊佔位圖，誠實且無害。
            // v174：帶原因（Epub 出口＋JPEG 轉檔器錯誤＋當下最大連續塊）與書的路徑 —— diag173 的 12 筆
            // THUMBFAIL 只有快取雜湊，判不出是 progressive 封面還是記憶體。
            DiagLog::line("THUMBFAIL h=%d why=%s jpg=%s max=%u %s", coverHeight, epub.thumbFailReason(),
                          JpegToBmpConverter::lastError(),
                          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)),
                          book.path.c_str());
          }
          // v57：連【快照】一起丟，不能只清 coverRendered。
          // 否則下一次 render 會先 restoreCoverBuffer() 把舊快照寫回封面帶，再把新封面疊上去
          // —— 而 1-bit 縮圖只畫黑像素、白像素留原背景，舊快照的黑像素於是全數存活：
          // 舊 coverWidth 的圓角框邊線會殘留在新封面上，並在 storeCoverBuffer() 被重新快照，
          // 停留主畫面期間每次重繪都在。
          freeCoverBuffer();
          coverRendered = false;          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, DataDir::path());
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            // v57：同上 —— 兩個縮圖產生點都要丟快照，只改一處等於沒改。
            freeCoverBuffer();
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasOpdsServers);

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  // v194：封面抖動／檔柄配置失敗的證人（閱讀器不在場時也要收）。
  if (HalStorage::lastAllocFail[0] != '\0') {
    DiagLog::line("ALLOCFAIL %s", HalStorage::lastAllocFail);
    HalStorage::lastAllocFail[0] = '\0';
  }
  if (ditherLastAllocFail[0] != '\0') {
    DiagLog::line("ALLOCFAIL %s", ditherLastAllocFail);
    ditherLastAllocFail[0] = '\0';
  }
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasOpdsServers)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:
        onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card). backPressSeen guards against the stale
  // release of the Back press that closed the previous activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  int tx = 0;
  int ty = 0;
  if (!recentBooks.empty() && mappedInput.wasScreenTouchDown(tx, ty) && tx >= 0 && tx < renderer.getScreenWidth() &&
      ty >= metrics.homeTopPadding && ty < metrics.homeTopPadding + metrics.homeCoverTileHeight) {
    if (selectorIndex != 0) {
      selectorIndex = 0;
      requestUpdate();
    }
    return;
  }

  if (!recentBooks.empty() &&
      mappedInput.wasTapInRect(0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight)) {
    selectorIndex = 0;
    activateSelection();
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  // v180：列高由主題決定（Formosa Pro 撐滿可用高度），與 drawButtonMenu 同一個公式 → 命中幾何同源。
  const int menuAvail = renderer.getScreenHeight() - (metrics.headerHeight + metrics.homeTopPadding +
                                                      metrics.verticalSpacing + metrics.homeMenuTopOffset +
                                                      metrics.buttonHintsHeight);
  const int menuRowH = GUI.menuRowHeightFor(menuAvail, renderedMenuCount);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowH + metrics.menuSpacing, renderedMenuCount, 0,
                                              INT32_MAX, menuRowH);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsServers) {
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  // v57：原本第一次繪製只是設 firstRenderDone 然後 requestUpdate()，把封面載入延到
  // 【第二次完整繪製】才做 —— 但上面的 displayBuffer() 已經同步把畫面推上面板了，
  // 所以那第二次是一整次白刷（實測面板 441ms + 軟體重繪 40-80ms），而且穩態下兩次的
  // 畫面內容完全相同（縮圖早就產好時，loadRecentCovers 只做幾次 Storage.exists 就結束）。
  // goHome() 一律 replaceActivity 建新的 HomeActivity -> 這個雙刷【每次回主畫面都觸發】。
  // 直接在第一次繪製後就載入封面：使用者看到的順序不變（先看到選單，再跳「載入中」），
  // 但少掉一次全螢幕刷新。真的需要重繪時 loadRecentCovers 自己會 requestUpdate()。
  firstRenderDone = true;
  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }
