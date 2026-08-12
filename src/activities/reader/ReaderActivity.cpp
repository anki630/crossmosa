#include "ReaderActivity.h"
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
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, DataDir::path());
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  // First open: building the spine/TOC index (book.bin) takes a couple of seconds. Show the
  // indexing popup so it isn't a silent wait on the home screen. The cachePath/hash is known at
  // construction, so this check is valid before load(); a cached open loads in a blink -> no popup.
  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
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
  if (loaded && epub->getSpineItemsCount() > 0) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub (loaded=%d, spine=%d)", loaded ? 1 : 0,
          loaded ? epub->getSpineItemsCount() : -1);
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
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt)));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  if (FsHelpers::hasXtcExtension(initialBookPath)) {
    // XTC support was removed; a stale path can still arrive once from pre-removal APP_STATE.
    // Bail out before the indexing popup / epub cache dir get created for a file that can never open.
    onGoBack();
    return;
  }

  sdFontSystem.ensureLoaded(renderer);

  currentBookPath = initialBookPath;
  if (isBmpFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
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
