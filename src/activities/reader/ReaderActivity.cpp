#include "ReaderActivity.h"

#include <DataDir.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <XmlParserUtils.h>

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
#include "util/DiagLog.h"

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
  unsigned long popupMs = 0;  // v357：「建立索引」彈窗那一次刷新（BOOKOPEN 的 load= 含它）
  if (uncached) {
    // The popup replaces the restored Quick Resume frame, so the reader must clean it.
    allowFastInitialRefresh = false;
    const unsigned long popupT0 = millis();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    popupMs = millis() - popupT0;
  }
  bool loaded;
  {
    // Lend the framebuffer's 48 KB to the container parse (expat + spine/TOC
    // build). The popup just displayed stays on the panel; whichever reader
    // activity follows redraws the full screen anyway.
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    takeXmlControlDrops();  // v345：歸零 → 下面 BOOKOPEN 的 xmlfix 只算這次開書
    loaded = epub->load(true, SETTINGS.embeddedStyle == 0);
  }
  // v350：固定版面清單讀不到、壞了、或對不上這份 book.bin（舊韌體建過索引的書，升級後第一次開都會走到）→
  //   解一次 OPF（manifest＋spine）補清單。不重建索引、不動排好的章節，所以不顯示「建立索引」；但跟建索引一樣
  //   借 framebuffer —— 這一趟吃的記憶體跟建索引的 OPF 那一段相同，而 -fno-exceptions 下配不到＝當機（複查）。
  //   借出去再還回來的 framebuffer 是全白的 → 第一頁不走快速刷新（跟建索引那條路相同的處理）。
  const bool layoutParse = loaded && epub->layoutComputePending();
  if (layoutParse) {
    allowFastInitialRefresh = false;
    GfxRenderer::FrameBufferLoan loan(renderer);
    epub->computePendingLayout();
  }
  // load() only reports that the metadata cache is readable; it says nothing about
  // whether the book has any content. A spine of 0 renders as the End-of-Book screen
  // (EpubReaderActivity's `currentSpineIndex == getSpineItemsCount()`), which reads as
  // "you finished this book" rather than "this book could not be parsed". Refuse it
  // here so the failure is reported as a failure.
  const unsigned long openTLoad = millis();  // v350：含上面固定版面清單的補算（FXLINFO 的 ms 是其中那一段）
  // ⚠️ 不印路徑或書名（隱私）；`spine=` 是章節數，判讀時用得上。
  // v345（帳本 D14）：xmlfix＝這次開書時 XML 解析器濾掉幾個控制字元（> 0 ＝ 這本書的描述檔本來會被拒收）。
  DiagLog::line("BOOKOPEN exists=%lu ctor=%lu load=%lu total=%lu cached=%d spine=%d xmlfix=%lu",
                static_cast<unsigned long>(openT0 - existsT0), static_cast<unsigned long>(openTCtor - openT0),
                static_cast<unsigned long>(openTLoad - openTCtor), static_cast<unsigned long>(openTLoad - existsT0),
                uncached ? 0 : 1, loaded ? static_cast<int>(epub->getSpineItemsCount()) : -1,
                static_cast<unsigned long>(takeXmlControlDrops()));
  // v357：建索引的分段（開書量測：大書 2–8.6 秒，這一段原本只有 LOG_DBG）。popup＝「建立索引」彈窗那次（含它的刷新：
  //   drawPopup 是阻塞刷新）；opf／cssfind／toc／bin／css／secrm／reload／layout＝Epub::IndexProfile
  //   各段、total＝整段； ok＝load() 成功。只在這次有試著建索引時印（失敗也印：做完的段有數字、沒做到的是 0，total
  //   到失敗為止）。
  if (const auto& ip = epub->indexProfile(); ip.built) {
    DiagLog::line(
        "BOOKIDX popup=%lu opf=%lu cssfind=%lu toc=%lu bin=%lu css=%lu secrm=%lu reload=%lu layout=%lu total=%lu "
        "spine=%u toc_n=%u css_n=%u ok=%d",
        popupMs, static_cast<unsigned long>(ip.opfMs), static_cast<unsigned long>(ip.cssFindMs),
        static_cast<unsigned long>(ip.tocMs), static_cast<unsigned long>(ip.binMs),
        static_cast<unsigned long>(ip.cssParseMs), static_cast<unsigned long>(ip.sectionsRmMs),
        static_cast<unsigned long>(ip.reloadMs), static_cast<unsigned long>(ip.layoutMs),
        static_cast<unsigned long>(ip.totalMs), static_cast<unsigned>(ip.spines), static_cast<unsigned>(ip.tocs),
        static_cast<unsigned>(ip.cssFiles), loaded ? 1 : 0);
    // v358：各段花在 SD 介面裡的時間 —— r／w／s／m＝讀／寫（含 flush）／移位置／開關檔等雜項的次數，*ms＝花在上面的毫秒
    //   （牆上時間，含 SdFat 自己的 CPU 與等鎖；lk＝其中等鎖的部分），rkb＝實際讀到的 KB（下限）。
    //   段的毫秒（BOOKIDX）扣掉 rms＋wms＋sms＋mms，剩下的是解析、inflate 等 CPU 加上被別的 task 搶走的時間。
    //   計時本身每個操作約 1–2 µs。
    //   v359：wmax／mmax＝這一段裡最久的那一次寫／開關檔（ms），wslow＝單次 ≥ 50 ms 的寫有幾次 ——
    //   分得出「一次卡 1 秒」（wslow=1、wmax≈wms）跟「每次都慢」（wslow 大、wmax 小）。
    if (!ip.ioArmed) {
      DiagLog::line("BOOKIO skipped why=busy");
    } else {
      static constexpr const char* kIoNames[] = {"opf", "toc", "bin", "css", "layout", "all"};
      static_assert(sizeof(kIoNames) / sizeof(kIoNames[0]) == Epub::IndexProfile::kIoPhases, "BOOKIO names");
      const auto ms = [](const uint32_t us) {
        return static_cast<unsigned long>((static_cast<uint64_t>(us) + 500) / 1000);
      };
      for (int i = 0; i < Epub::IndexProfile::kIoPhases; i++) {
        const HalStorage::IoStats& s = ip.io[i];
        DiagLog::line(
            "BOOKIO ph=%s r=%lu rms=%lu rkb=%lu w=%lu wms=%lu s=%lu sms=%lu m=%lu mms=%lu lk=%lu wmax=%lu wslow=%lu "
            "mmax=%lu",
            kIoNames[i], static_cast<unsigned long>(s.reads), ms(s.readUs),
            static_cast<unsigned long>(s.readBytes / 1024), static_cast<unsigned long>(s.writes), ms(s.writeUs),
            static_cast<unsigned long>(s.seeks), ms(s.seekUs), static_cast<unsigned long>(s.metas), ms(s.metaUs),
            ms(s.lockUs), ms(s.writeMaxUs), static_cast<unsigned long>(s.writeSlow), ms(s.metaMaxUs));
      }
    }
  }
  // v350 證人：固定版面清單從哪裡來（見 Epub::LayoutInfoStats）、花多久、有幾項；loan＝這次有沒有借 framebuffer。
  {
    const auto& ls = epub->layoutInfoStats();
    DiagLog::line("FXLINFO load=%s ms=%lu fixed=%u loan=%u", ls.load, static_cast<unsigned long>(ls.ms),
                  static_cast<unsigned>(epub->fixedLayoutSpineCount()), layoutParse ? 1u : 0u);
  }
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
    DiagLog::line(
        "READERENTER trans=%ld base=%lu font=%lu", transStart == 0 ? -1L : static_cast<long>(enterT0 - transStart),
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
