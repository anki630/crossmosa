#include "HomeActivity.h"

#include <Bitmap.h>
#include <DataDir.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Utf8.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

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
#include "util/DiagLog.h"
#include "util/RecentCoverLoader.h"

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
  // 2026-10-07：本體搬到 RecentCoverLoader（Formosa Cover 的首頁與書架共用），行為不變。
  //   v57：產生縮圖之後連【快照】一起丟、要求重畫 —— 兩個縮圖產生點都走 onChanged。
  RecentCoverLoader::ensureThumbs(renderer, recentBooks, coverHeight, [this] {
    freeCoverBuffer();
    coverRendered = false;
    requestUpdate();
  });
  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();
  backPressSeen = false;  // v357：兩個防護都從進場重新算（不假設物件是新建的）
  confirmPressSeen = false;

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
  // v357：確認鍵的「按下」要在任何提早返回之前收（同一輪有觸控時也不漏掉；codex）。
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen = true;
  // v194：封面抖動／檔柄配置失敗的證人（閱讀器不在場時也要收）。
  DiagLog::crumb("ALLOCFAIL", HalStorage::lastAllocFail, sizeof(HalStorage::lastAllocFail));
  DiagLog::crumb("X4DIFF", HalDisplay::lastForcedDiff, sizeof(HalDisplay::lastForcedDiff));  // v357 bench
  DiagLog::crumb("ALLOCFAIL", ditherLastAllocFail, sizeof(ditherLastAllocFail));
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
  const int menuAvail =
      renderer.getScreenHeight() - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                                    metrics.homeMenuTopOffset + metrics.buttonHintsHeight);
  const int menuRowH = GUI.menuRowHeightFor(menuAvail, renderedMenuCount);
  const auto menuTouch =
      mappedInput.rowTouch(menuRow, menuTop, menuRowH + metrics.menuSpacing, renderedMenuCount, 0, INT32_MAX, menuRowH);
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && confirmPressSeen) {
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

  // v311 證人：首頁「開機後第一次繪製」比之後多約 600ms（v310 log：1,144ms vs 508–571ms），
  //   而那段沒有任何儀器。差別就在這一行 —— 快照沒還原（bufferRestored=0）時主題要從 SD 讀
  //   三張縮圖來畫，之後的重繪走 restoreCoverBuffer() 的 RAM 快照。只在慢的那次記（≥50ms 或
  //   非快照），安靜的重繪不洗版。有了數字才知道「縮圖放 RAM 常駐」值不值得。
  // 呼叫前先存一份：主題可能透過 reference 改掉 bufferRestored／coverRendered，
  // 證人要記的是「進入這一行時有沒有快照」，不是呼叫後的狀態（codex 指出）。
  const bool restoredAtEntry = bufferRestored;
  const bool renderedAtEntry = coverRendered;
  const unsigned long coverT0 = millis();
  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));
  const unsigned long coverMs = millis() - coverT0;
  if (!restoredAtEntry || coverMs >= 50) {
    DiagLog::line("HOMECOVER ms=%lu restored=%d rendered=%d books=%u", coverMs, restoredAtEntry ? 1 : 0,
                  renderedAtEntry ? 1 : 0, static_cast<unsigned>(recentBooks.size()));
  }

  // Build menu items dynamically
  // v275：「最近閱讀比瀏覽檔案更常用」→ 最近閱讀排第一。
  // ⚠️ 順序寫在**三個**地方：這裡、`menuItemToIndex`、`indexToMenuItem`（HomeActivity.h）。
  //    只改這裡會讓按下去跑到隔壁那一項（教訓：條目與處理常式必須一起搬）。
  std::vector<const char*> menuItems = {tr(STR_MENU_RECENT_BOOKS), tr(STR_BROWSE_FILES), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Recent, Folder, Transfer, Settings};

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
