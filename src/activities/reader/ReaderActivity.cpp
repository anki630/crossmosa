#include "ReaderActivity.h"

#include "util/DiagLog.h"
#include <DataDir.h>

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <optional>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

int ReaderActivity::initialRefreshCountdown() const {
  if (!allowFastInitialRefresh) return 0;

  const int refreshFrequency = SETTINGS.getRefreshFrequency();
  return refreshFrequency > 1 ? refreshFrequency : 2;
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  const unsigned long existsT0 = millis();  // v279：連 exists() 都算進去（SdFat 的 exists 是一次完整 open）
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  // v279：**開書的分項計時**。v278 量到「`WAKE toreader` → `BOOKDIR`」要 685ms，是喚醒路徑上
  //   最大的單一軟體成本，而那一段完全沒有儀器。這裡把它拆成 exists／建構／載入三段。
  const unsigned long openT0 = millis();
  auto epub = makeUniqueNoThrow<Epub>(path, DataDir::path());
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  const unsigned long openTCtor = millis();
  // First open: building the spine/TOC index (book.bin) takes a couple of seconds. Show the
  // indexing popup so it isn't a silent wait on the home screen. The cachePath/hash is known at
  // construction, so this check is valid before load(); a cached open loads in a blink -> no popup.
  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    // The popup replaces the restored Quick Resume frame, so the reader must clean it.
    allowFastInitialRefresh = false;
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }
  bool loaded;
  {
    // Lend the framebuffer's 48 KB to the container parse (expat + spine/TOC
    // build). The popup just displayed stays on the panel; whichever reader
    // activity follows redraws the full screen anyway.
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = epub->load(true, SETTINGS.embeddedStyle == 0);
  }
  // load() only reports that the metadata cache is readable; it says nothing about
  // whether the book has any content. A spine of 0 renders as the End-of-Book screen
  // (EpubReaderActivity's `currentSpineIndex == getSpineItemsCount()`), which reads as
  // "you finished this book" rather than "this book could not be parsed". Refuse it
  // here so the failure is reported as a failure.
  const unsigned long openTLoad = millis();
  // ⚠️ 不印路徑或書名（隱私）；`spine=` 是章節數，判讀時用得上。
  DiagLog::line("BOOKOPEN exists=%lu ctor=%lu load=%lu total=%lu cached=%d spine=%d",
                static_cast<unsigned long>(openT0 - existsT0), static_cast<unsigned long>(openTCtor - openT0),
                static_cast<unsigned long>(openTLoad - openTCtor), static_cast<unsigned long>(openTLoad - existsT0),
                uncached ? 0 : 1, loaded ? static_cast<int>(epub->getSpineItemsCount()) : -1);
  if (loaded && epub->getSpineItemsCount() > 0) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub (loaded=%d, spine=%d)", loaded ? 1 : 0,
          loaded ? epub->getSpineItemsCount() : -1);
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, DataDir::path());
  if (!xtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return nullptr;
  }
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, DataDir::path());
  if (!txt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return nullptr;
  }
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  activityManager.replaceActivity(
      std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub), initialRefreshCountdown()));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(
      std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc), initialRefreshCountdown()));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(
      std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt), initialRefreshCountdown()));
}

void ReaderActivity::onEnter() {
  // v280：這一段是喚醒路徑上最後一塊沒有儀器的地方（v279 量到 1.1–1.4 秒）。
  //   `trans` ＝ goToReader 到這裡（活動切換／建構／等下一輪 loop）、`base` ＝ Activity::onEnter()、
  //   `font` ＝ `sdFontSystem.ensureLoaded()`（首要嫌疑）。之後的 `BOOKOPEN` 接著往下量。
  const unsigned long enterT0 = millis();
  Activity::onEnter();
  const unsigned long enterTBase = millis();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  const unsigned long enterTFont = millis();
  {
    const unsigned long transStart = activityManager.takeReaderTransitionStartMs();
    DiagLog::line("READERENTER trans=%ld base=%lu font=%lu", transStart == 0 ? -1L : static_cast<long>(enterT0 - transStart),
                  static_cast<unsigned long>(enterTBase - enterT0), static_cast<unsigned long>(enterTFont - enterTBase));
  }

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }
