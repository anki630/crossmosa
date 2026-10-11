#include "FileBrowserActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/home/BookshelfActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/Favorites.h"
#include "util/TabSwitchProbe.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr size_t NAME_BUFFER_SIZE = 500;
}  // namespace

void FileBrowserActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    root.close();
    return;
  }

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    if ((!SETTINGS.showHiddenFiles && fileNameBuffer[0] == '.') ||
        strcmp(fileNameBuffer.get(), "System Volume Information") == 0) {
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(fileNameBuffer.get()) + "/");
    } else {
      std::string_view filename{fileNameBuffer.get()};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        if (FsHelpers::checkFileExtension(filename, ".bin")) {
          files.emplace_back(filename);
        }
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename)) {
        files.emplace_back(filename);
      }
    }
  }
  root.close();
  FsHelpers::sortFileList(files);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "malloc failed for name buffer");
    return;
  }

  selectorIndex = 0;

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  // v376：從書架的分頁列換過來（⑥ 或長按 ⑦⑧）→ 選取停在分頁列，跟書架換分頁一樣。
  //   tabHoldUsed_：長按換過來時 ⑦⑧ 還按著，放開之前不再換一格。
  if (showShelfTabs() && BookshelfActivity::sFolderOnTabs) {
    onTabs_ = true;
    tabHoldUsed_ = true;
  }
  BookshelfActivity::sFolderOnTabs = false;

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
  } else if (!root.isDirectory()) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  } else {
    loadFiles();
  }

  TabSwitchProbe::ready();  // 從書架換過來：讀完資料夾了
  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
  fileNameBuffer.reset();
}

// To avoid traversing directories twice (once for cache clearing, once for deletion),
// we do both in one pass here, instead of using Storage.removeDir
bool FileBrowserActivity::removeDirFile(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FileBrowser", "Failed to open for metadata clearing: %s", fullPath.c_str());
    return false;
  }

  if (!file.isDirectory()) {
    file.close();
    clearBookCache(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  if (!fileNameBuffer) {
    LOG_ERR("FileBrowser", "fileNameBuffer not allocated");
    return false;
  }

  // Stack of (dirPath, postOrder): postOrder=true means rmdir this path after children are processed.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir) {
      LOG_ERR("FileBrowser", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }
    if (!dir.isDirectory()) {
      LOG_ERR("FileBrowser", "Not a directory: %s", currentPath.c_str());
      return false;
    }

    // Push this dir for post-order rmdir (after all children are processed).
    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      if (strcmp(fileNameBuffer.get(), ".") == 0 || strcmp(fileNameBuffer.get(), "..") == 0) {
        continue;
      }
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') {
        entryPath += "/";
      }
      entryPath += fileNameBuffer.get();

      const bool isDir = entry.isDirectory();
      entry.close();

      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearBookCache(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FileBrowser", "Failed to remove file: %s", entryPath.c_str());
          return false;
        }
      }
    }
  }

  return true;
}

void FileBrowserActivity::loop() {
  // Long press BACK (1s+) goes to root folder (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && basepath != "/" && !lockLongPressBack) {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    requestUpdate();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  if (onTabs_) {
    loopOnTabs();
    return;
  }

  const int pathReserved = renderer.getLineHeight(UI_10_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, showShelfTabs(), true, false, pathReserved);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + tabBarSpace() + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;

  auto activateSelected = [this] {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    // Firmware picker: select file -> return path; navigate into directories normally.
    if (mode == Mode::PickFirmware && !isDirectory) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      ActivityResult res{FilePathResult{cleanBasePath + entry}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }

    if (mode == Mode::Books && mappedInput.getHeldTime() >= GO_HOME_MS) {
      // --- LONG PRESS ACTION: DELETE FILE OR DIRECTORY ---
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      const std::string fullPath = cleanBasePath + entry;

      auto handler = [this, fullPath](const ActivityResult& res) {
        // The confirmation popup acts on button press; if that button is still
        // held when we resume, swallow its release so it doesn't also act here
        // (Back would go up a directory, Confirm would open the selection).
        lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);
        lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
        if (!res.isCancelled) {
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          if (removeDirFile(fullPath)) {
            LOG_DBG("FileBrowser", "Deleted successfully");
            Favorites::removeBook(
                fullPath);  // v367：刪掉的書不再佔我的最愛的名額（整個資料夾：底下的身分留著，只佔名額）
            loadFiles();
            if (files.empty()) {
              selectorIndex = 0;
            } else if (selectorIndex >= files.size()) {
              // Move selection to the new "last" item
              selectorIndex = files.size() - 1;
            }

            requestUpdate(true);
          } else {
            LOG_ERR("FileBrowser", "Failed to delete: %s", fullPath.c_str());
          }
        } else {
          LOG_DBG("FileBrowser", "Delete cancelled by user");
        }
      };

      std::string heading = tr(STR_DELETE) + std::string("? ");

      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else {
        onSelectBook(basepath + entry);
      }
    }
    return;
  };

  int touchSel = static_cast<int>(selectorIndex);
  const auto listTouch = handleListTouch(touchSel, static_cast<int>(files.size()), contentTop, contentHeight, false);
  if (listTouch != ListTouchResult::None) {
    selectorIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) activateSelected();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else if (SETTINGS.uiTheme == CrossPointSettings::FORMOSA_COVER) {
        // v375：最上層 ⑤＝選取跳到分頁列（同書架：書上 ⑤ 先到分頁列，分頁列上 ⑤ 才回首頁）
        onTabs_ = true;
        tabNext_.reset();
        tabPrev_.reset();
        tabConfirmSeen_ = false;
        tabBackSeen_ = false;
        tabHoldUsed_ = true;  // 這次放開 ⑤ 之前若還按著 ⑦⑧，不算換分頁
        requestUpdate();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(files.size());
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextPageIndexClamped(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousPageIndexClamped(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndexClamped(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndexClamped(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(const std::string& filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  TabSwitchProbe::drawStart();
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  if (showShelfTabs()) {
    std::vector<TabInfo> tabs;
    tabs.reserve(3);
    if (BookshelfActivity::sHasFavTab) tabs.push_back({tr(STR_TAB_FAVORITES), false});
    tabs.push_back({tr(STR_TAB_ALL), false});
    tabs.push_back({tr(STR_TAB_FOLDERS), true});
    GUI.drawTabBar(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, tabs,
                   onTabs_);
  }
  const int contentTop = metrics.topPadding + metrics.headerHeight + tabBarSpace() + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  if (files.empty()) {
    const char* emptyMsg = (mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(),
        onTabs_ ? -1 : static_cast<int>(selectorIndex),  // 選取在分頁列：列表不標選取（停在第一頁）
        [this](int index) { return getFileName(files[index]); }, nullptr,
        [this](int index) { return UITheme::getFileIcon(files[index]); },
        [this](int index) { return getFileExtension(files[index]); }, false);
  }

  // Full path display
  {
    const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);
    const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(UI_10_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(UI_10_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(UI_10_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);
  }

  // Help text
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK)
                                               : showShelfTabs()          ? tr(STR_BOOKSHELF)
                                                                          : tr(STR_HOME))
                                            : tr(STR_BACK);
  // In PickFirmware mode, Confirm on a .bin returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && files[selectorIndex].back() != '/';
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const auto labels =
      onTabs_ ? mappedInput.mapLabels(
                    tr(STR_HOME),
                    shelfTabAfter(+1) == BookshelfActivity::kTabFav ? tr(STR_TAB_FAVORITES) : tr(STR_TAB_ALL), "‹", "›")
              : mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                      files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  TabSwitchProbe::beforeDisplay();
  renderer.displayBuffer();
  TabSwitchProbe::afterDisplay("folder", -1, renderer.lastRefreshBank());
}

// 分頁順序同書架：有最愛時「最愛｜全部｜資料夾」，沒有時「全部｜資料夾」；這裡永遠在最後一格「資料夾」，繞圈。
int FileBrowserActivity::shelfTabAfter(const int dir) const {
  if (dir > 0) return BookshelfActivity::sHasFavTab ? BookshelfActivity::kTabFav : BookshelfActivity::kTabAll;
  return BookshelfActivity::kTabAll;  // 往左一格永遠是「全部」
}

void FileBrowserActivity::switchShelfTab(const int dir) {
  activityManager.goToBookshelf({}, shelfTabAfter(dir));  // 換到「最愛」／「全部」＝回書架那個分頁，選取在分頁列
}

void FileBrowserActivity::loopOnTabs() {
  using B = MappedInputManager::Button;
  if (mappedInput.wasPressed(B::Confirm)) {
    tabConfirmSeen_ = true;
    tabConfirmAt_ = millis();  // v379 換分頁量測：按下的時間
  }
  if (mappedInput.wasPressed(B::Back)) tabBackSeen_ = true;
  if (mappedInput.wasReleased(B::Confirm) && tabConfirmSeen_) {
    tabConfirmSeen_ = false;
    TabSwitchProbe::begin("ok", "folder", tabConfirmAt_);
    switchShelfTab(+1);
    return;
  }
  if (mappedInput.wasReleased(B::Back) && tabBackSeen_) {
    tabBackSeen_ = false;
    onGoHome();
    return;
  }
  const uint32_t now = millis();
  if (!mappedInput.isPressed(B::NavNext) && !mappedInput.isPressed(B::NavPrevious)) tabHoldUsed_ = false;
  // 分頁列上長按換分頁的門檻同書架（v380：0.3 秒）
  const auto next = tabNext_.update(mappedInput.wasPressed(B::NavNext), mappedInput.wasReleased(B::NavNext),
                                    mappedInput.isPressed(B::NavNext), now, HoldRepeat::kTabStartMs);
  const auto prev = tabPrev_.update(mappedInput.wasPressed(B::NavPrevious), mappedInput.wasReleased(B::NavPrevious),
                                    mappedInput.isPressed(B::NavPrevious), now, HoldRepeat::kTabStartMs);
  if (next == HoldRepeat::Action::Long || prev == HoldRepeat::Action::Long) {
    if (tabHoldUsed_) return;
    tabHoldUsed_ = true;
    TabSwitchProbe::begin("hold", "folder", (next == HoldRepeat::Action::Long ? tabNext_ : tabPrev_).downAt());
    switchShelfTab(next == HoldRepeat::Action::Long ? +1 : -1);
    return;
  }
  if ((next == HoldRepeat::Action::Short || prev == HoldRepeat::Action::Short) && !files.empty()) {
    onTabs_ = false;
    selectorIndex = next == HoldRepeat::Action::Short ? 0 : files.size() - 1;
    requestUpdate();
  }
}

bool FileBrowserActivity::showShelfTabs() const {
  return mode == Mode::Books && SETTINGS.uiTheme == CrossPointSettings::FORMOSA_COVER;
}

int FileBrowserActivity::tabBarSpace() const {
  return showShelfTabs() ? UITheme::getInstance().getMetrics().tabBarHeight : 0;
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
