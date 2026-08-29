#include "ClearCacheActivity.h"
#include <DataDir.h>

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

namespace {

constexpr StrId kClearCacheLabels[] = {StrId::STR_CLEAR_CACHE_KEEP_PROGRESS, StrId::STR_CLEAR_CACHE_RESET_PROGRESS};

// v192：刪除流程零動態配置。固定緩衝放檔案靜態：函式棧上限 256B，name+路徑放棧會超。
constexpr size_t kNameBuf = 128;
constexpr size_t kPathBuf = 192;
// v192：<DataDir> + /.progress_tmp_ + 最長 127 字名稱；放不下就跳過該本。
constexpr size_t kTmpBuf = 288;
char gName[kNameBuf];
char gBookPath[kPathBuf];
char gSrcPath[kPathBuf];
char gTmpPath[kTmpBuf];

bool joinPath(char* out, size_t outSz, const char* a, const char* b) {
  const int n = snprintf(out, outSz, "%s/%s", a, b);
  return n > 0 && static_cast<size_t>(n) < outSz;
}

bool makeProgressTmpPath(char* out, size_t outSz, const char* bookName) {
  const int n = snprintf(out, outSz, "%s/.progress_tmp_%s", DataDir::path(), bookName);
  return n > 0 && static_cast<size_t>(n) < outSz;
}

bool isSafeCacheEntryName(const char* name) {
  if (!name || name[0] == '\0') return false;
  if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) return false;
  for (const char* p = name; *p; ++p) {
    if (*p == '/' || *p == '\\') return false;
  }
  return true;
}

}  // namespace

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  const char* options[] = {tr(STR_CANCEL), tr(STR_CLEAR_BUTTON)};
  confirmPopup.show(tr(STR_CLEAR_READING_CACHE), options, 2, 0, [this](int idx) {
    if (idx == 1) {
      askProgress();
    } else {
      goBack();
    }
  });
  requestUpdate();
}

void ClearCacheActivity::onExit() { Activity::onExit(); }

void ClearCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLEAR_READING_CACHE));

  if (state == WARNING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, tr(STR_CLEAR_CACHE_WARNING_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_CLEAR_CACHE_WARNING_2), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CLEAR_CACHE_WARNING_3), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_CLEAR_CACHE_WARNING_4), true);

    if (confirmPopup.processRender(renderer, mappedInput)) return;

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == ASK_PROGRESS) {
    // v192：每個狀態都要畫得出東西；詢問彈窗蓋在原本警告文案上。
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 60, tr(STR_CLEAR_CACHE_WARNING_1), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 30, tr(STR_CLEAR_CACHE_WARNING_2), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CLEAR_CACHE_WARNING_3), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_CLEAR_CACHE_WARNING_4), true);

    if (confirmPopup.processRender(renderer, mappedInput)) return;

    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CLEARING_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_CLEARED), true, EpdFontFamily::BOLD);
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CLEAR_CACHE_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_CHECK_SERIAL_OUTPUT));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::askProgress() {
  // v192：真正動手前追問保留／重設進度；手法照抄閱讀選單的兩段式詢問。
  state = ASK_PROGRESS;
  confirmPopup.show(StrId::STR_DELETE_CACHE, kClearCacheLabels, 2, 0, [this](int idx) { beginClear(idx == 0); });
  requestUpdate();
}

void ClearCacheActivity::beginClear(bool keepProgress) {
  LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear (keepProgress=%d)", keepProgress ? 1 : 0);
  {
    RenderLock lock(*this);
    state = CLEARING;
  }
  requestUpdateAndWait();
  clearCache(keepProgress);
}

void ClearCacheActivity::clearCache(bool keepProgress) {
  LOG_DBG("CLEAR_CACHE", "Clearing cache (keepProgress=%d)...", keepProgress ? 1 : 0);

  auto root = Storage.open(DataDir::path());
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  clearedCount = 0;
  failedCount = 0;

  // v192：openNextFile() 因 SD I/O 錯誤回傳空值時，無法與正常 EOF 區分，會被當成掃完。這是上游既有行為。
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    const size_t nlen = file.getName(gName, sizeof(gName));
    // v192：getName 失敗、空字、含斜線、或 . / .. 都跳過，不准拿去組路徑。
    if (nlen == 0 || nlen >= sizeof(gName) || !isSafeCacheEntryName(gName)) {
      LOG_ERR("CLEAR_CACHE", "skip unsafe cache entry name");
      failedCount++;
      file.close();
      continue;
    }

    if (!(file.isDirectory() && isBookCacheDirectoryName(gName))) {
      file.close();
      continue;
    }

    if (!joinPath(gBookPath, sizeof(gBookPath), DataDir::path(), gName)) {
      LOG_ERR("CLEAR_CACHE", "book path does not fit");
      failedCount++;
      file.close();
      continue;
    }

    LOG_DBG("CLEAR_CACHE", "Purging cache: %s keepProgress=%d", gBookPath, keepProgress ? 1 : 0);
    file.close();  // 刪前必須關

    if (!keepProgress) {
      if (Storage.removeDir(gBookPath)) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_CACHE", "Failed to removeDir: %s", gBookPath);
        failedCount++;
      }
      continue;
    }

    if (!joinPath(gSrcPath, sizeof(gSrcPath), gBookPath, "progress.bin")) {
      LOG_ERR("CLEAR_CACHE", "progress path does not fit");
      failedCount++;
      continue;
    }
    if (!makeProgressTmpPath(gTmpPath, sizeof(gTmpPath), gName)) {
      LOG_ERR("CLEAR_CACHE", "tmp path does not fit: %s", gName);
      failedCount++;
      continue;
    }

    // v192：失敗分支一律不刪 .progress_tmp_*；那是使用者進度的最後一份。
    if (Storage.exists(gTmpPath)) {
      if (!(Storage.exists(gBookPath) || Storage.mkdir(gBookPath))) {
        LOG_ERR("CLEAR_CACHE", "self-heal mkdir failed, progress at %s", gTmpPath);
        failedCount++;
        continue;
      }
      if (!Storage.rename(gTmpPath, gSrcPath)) {
        LOG_ERR("CLEAR_CACHE", "self-heal rename failed, progress at %s", gTmpPath);
        failedCount++;
        continue;
      }
    }

    const bool had = Storage.exists(gSrcPath);
    if (had && !Storage.rename(gSrcPath, gTmpPath)) {
      LOG_ERR("CLEAR_CACHE", "Failed to move progress aside: %s", gSrcPath);
      failedCount++;
      continue;
    }
    if (!Storage.removeDir(gBookPath)) {
      if (had && !Storage.rename(gTmpPath, gSrcPath)) {
        LOG_ERR("CLEAR_CACHE", "進度留在 %s", gTmpPath);
      }
      LOG_ERR("CLEAR_CACHE", "Failed to removeDir: %s", gBookPath);
      failedCount++;
      continue;
    }
    if (!Storage.mkdir(gBookPath)) {
      if (had) {
        // v192：進度在 tmp、家沒了；不刪 tmp，提前停。
        LOG_ERR("CLEAR_CACHE", "unrecoverable: mkdir failed, progress at %s", gTmpPath);
        failedCount++;
        root.close();
        state = FAILED;
        requestUpdate();
        return;
      }
      LOG_ERR("CLEAR_CACHE", "Failed to mkdir: %s", gBookPath);
      failedCount++;
      continue;
    }
    if (had && !Storage.rename(gTmpPath, gSrcPath)) {
      LOG_ERR("CLEAR_CACHE", "unrecoverable: progress stranded at %s", gTmpPath);
      failedCount++;
      root.close();
      state = FAILED;
      requestUpdate();
      return;
    }
    clearedCount++;
  }
  root.close();

  if (!keepProgress) {
    // v192：重設時只把最近閱讀清單的百分比歸零，不准刪 recent.json 的項目本身。
    const auto& books = RECENT_BOOKS.getBooks();
    for (const auto& book : books) {
      RECENT_BOOKS.setProgress(book.path, 0);
    }
  }

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      askProgress();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == ASK_PROGRESS) {
    if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
      if (!confirmPopup.isActive() && state == ASK_PROGRESS) {
        LOG_DBG("CLEAR_CACHE", "User cancelled progress prompt");
        goBack();
      }
      return;
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      goBack();
    }
    return;
  }
}
