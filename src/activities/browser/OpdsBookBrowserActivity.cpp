#include <algorithm>
#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/icons/search24.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/DiagLog.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int HEADER_Y = 15;
constexpr int HEADER_X = 16;
constexpr int SEARCH_ICON_SIZE = 24;
constexpr int SEARCH_ICON_MARGIN = 14;
constexpr int SEARCH_ICON_Y = 15;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

Rect searchIconRect(const GfxRenderer& renderer) {
  return Rect{renderer.getScreenWidth() - SEARCH_ICON_SIZE - SEARCH_ICON_MARGIN, SEARCH_ICON_Y, SEARCH_ICON_SIZE + 8,
              SEARCH_ICON_SIZE + 8};
}

bool contains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  // v5/v100 → v185 搬回：連 WiFi 前卸載 SD 內文字型（舊樹 OpdsBookBrowserActivity:71 原有，
  // rebase 時跟著 v100 的 BLE 段落一起掉了）。OPDS 清單只用內建 UI 字型；字型留著就是
  // WiFi 堆疊配置最常撞牆的那幾十 KB —— crash_report176 就是 WiFi 啟動時 p3 只剩 6.5KB 的
  // 硬重置（落回主畫面、log 裡沒有任何 FAILED 行 = 使用者說的「有時讀不到書單」）。
  // 離開時 onExit 的 silentRestart 會重載；沒重啟的路徑（WiFi 從未啟動）走 ensureLoaded。
  sdFontSystem.unloadForLowMemory(renderer);
  didUnloadFonts_ = true;
  DiagLog::mem("opds-fontfree");
  DiagLog::line("OPDS enter status=%d mode=%d server=%s", static_cast<int>(WiFi.status()),
                static_cast<int>(WiFi.getMode()), server.name.c_str());

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  entries.clear();
  navigationHistory.clear();

  // state 說明離開時人在哪（ERROR／BROWSING／WIFI_SELECTION／DOWNLOADING）；深睡拆除路徑
  // 不會 silentRestart（main.cpp deepSleepInProgress），所以不記「restart=」以免誤導。
  DiagLog::line("OPDS exit state=%d mode=%d", static_cast<int>(state), static_cast<int>(WiFi.getMode()));
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
  // ⚠️ 只在「WiFi 從未啟動」時重載（同 CrossPointWebServerActivity 的守衛）。按電源鍵深睡的拆除
  //    路徑上 silentRestart 是 no-op（main.cpp deepSleepInProgress）而 WiFi 堆疊還在 —— 那時重載
  //    會在低堆下失敗，ensureLoaded 失敗會 clearSdFontFamily 並存檔 = 使用者的字型設定永久消失
  //    （v185 複查四個驗證者一致抓到）。深睡醒來是整機重開，字型自然重載，這裡不必管。
  if (didUnloadFonts_ && WiFi.getMode() == WIFI_MODE_NULL) {
    sdFontSystem.ensureLoaded(renderer);
    didUnloadFonts_ = false;
  }
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      DiagLog::line("OPDS retry connected=%d", static_cast<int>(WiFi.status() == WL_CONNECTED));
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // （loop() 其實觀察不到 CHECK_WIFI／LOADING —— onEnter 與同步的 fetchFeed 早把狀態換掉了；
      //   保留原分支，但證人放在真正走得到的地方：navigateBack 根層、onExit、ActivityManager 的 HOME 手勢。）
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    auto activateSelected = [this] {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
      }
    };

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    int tx = 0;
    int ty = 0;
    if (!searchTemplate.empty() && mappedInput.wasScreenTapped(tx, ty) && contains(searchIconRect(renderer), tx, ty)) {
      launchSearch();
      return;
    }

    if (!entries.empty()) {
      // v48/v159：清單改共用 drawList（副標 72px 列）之後，觸控也改走 handleListTouch ——
      // 幾何由主題單一來源供給，不再自繪 30px 網格（畫多高、點擊區就多高）。
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int contentHeight =
          renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
      int touchSel = selectorIndex;
      const auto listTouch = handleListTouch(touchSel, static_cast<int>(entries.size()), contentTop, contentHeight,
                                             /*hasSubtitle=*/true);
      if (listTouch != ListTouchResult::None) {
        selectorIndex = touchSel;
        if (listTouch == ListTouchResult::Activated) activateSelected();
        return;
      }

      // v48：pageItems 與 drawList 同源（副標列高），不再用寫死的 PAGE_ITEMS
      const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up) {
        selectorIndex = ButtonNavigator::nextPageIndexClamped(selectorIndex, entries.size(), pageItems);
        requestUpdate();
        return;
      }
      if (swipe == MappedInputManager::SwipeDir::Down) {
        selectorIndex = ButtonNavigator::previousPageIndexClamped(selectorIndex, entries.size(), pageItems);
        requestUpdate();
        return;
      }

      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this, pageItems] {
        selectorIndex = ButtonNavigator::nextPageIndexClamped(selectorIndex, entries.size(), pageItems);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this, pageItems] {
        selectorIndex = ButtonNavigator::previousPageIndexClamped(selectorIndex, entries.size(), pageItems);
        requestUpdate();
      });
    }
  }
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // v37/v159：改走全機統一的 GUI.drawHeader（標題左豎條、樣式跟主題）。
  // v38：頁碼副標（僅瀏覽態且有內容；pageItems 與 loop/drawList 同源）。
  const auto& metrics = UITheme::getInstance().getMetrics();
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);
  const std::string pageText =
      (state == BrowserState::BROWSING && !entries.empty())
          ? UITheme::pageIndicatorText(selectorIndex, static_cast<int>(entries.size()), pageItems)
          : std::string();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle,
                 pageText.empty() ? nullptr : pageText.c_str());
  if (!searchTemplate.empty()) {
    const auto rect = searchIconRect(renderer);
    renderer.drawIcon(Search24Icon.bits, rect.x + 4, rect.y + 4, Search24Icon.w);
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    if (!errorDetail.empty()) {
      // v101/v158：HttpDownloader::lastError 的內容——使用者能唸給維護者聽，不用拔 SD 卡
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, errorDetail.c_str());
    }
    if (mappedInput.hasTouch()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_TAP_TO_RETRY));
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  const char* confirmLabel =
      (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
  const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (entries.empty()) {
    // 空 feed = 空狀態（不是錯誤）：告訴使用者現在能按什麼。
    // 根 feed 的 Back = 離開 OPDS（navigateBack→onGoHome），非根才是「回上一層」——文案依情境分流。
    const int midY = pageHeight / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, midY - renderer.getLineHeight(UI_12_FONT_ID) - 2,
                              tr(STR_OPDS_EMPTY_FEED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 2,
                              navigationHistory.empty() ? tr(STR_OPDS_EMPTY_HINT_ROOT) : tr(STR_OPDS_EMPTY_HINT));
  } else {
    // v48：改用全機共用清單元件（與最近閱讀完全同風格：72px 列、32px 圖示、
    // 選取樣式自動跟主題走——不再維護自繪複本）。書＝書本圖示、導覽項＝資料夾圖示（取代「> 」前綴）。
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(entries.size()), selectorIndex,
        [this](int index) { return entries[index].title; },
        [this](int index) {
          const auto& entry = entries[index];
          return (entry.type == OpdsEntryType::BOOK) ? entry.author : std::string();
        },
        [this](int index) { return entries[index].type == OpdsEntryType::BOOK ? UIIcon::Book : UIIcon::Folder; });
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    DiagLog::line("OPDS no server url");
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  // v102：TLS 握手是 ECDHE＋ECDSA、無硬體加速；閒置降頻（160→10 MHz）會把握手拖長 16 倍。
  // 把當下時脈與記憶體寫進 diag，失敗時才歸因得出來（與下面的 FAILED 行成對）。
  DiagLog::line("OPDS fetch start: cpu=%u MHz url=%s", static_cast<unsigned>(getCpuFrequencyMhz()), url.c_str());
  DiagLog::mem("opds-fetch-pre");
  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    errorDetail.clear();
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      // v101：畫面原本只有一句「failed to fetch feed」，真因（TLS 失敗、狀態碼、短讀…）
      // 全走 LOG_ERR = 這台沒有序列埠的機器上等於丟棄。寫進 DiagLog 並顯示在畫面上。
      DiagLog::line("OPDS fetch FAILED: %s", HttpDownloader::lastError);
      DiagLog::mem("opds-fetch-fail");
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      errorDetail = HttpDownloader::lastError;
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    DiagLog::line("OPDS parse FAILED");
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  // CrossMosa B11：OPDS 搜尋停用（v14 拔過，理由是裝置上沒有中文輸入法，
  // 對中文書庫無用；上游 1.5 的螢幕鍵盤仍然只有 QWERTY）。
  // 從【唯一的來源】切斷：searchTemplate 恆空，其餘 8 個 !empty() 的站點
  // （按鍵、觸控、圖示、標題內縮、按鈕提示、performSearch）全部自動失效，
  // 不留任何按了沒反應的死路徑。要還原就把這行改回來。
  searchTemplate = "";  // was: parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  const bool feedTruncated = parser.truncated();
  entries = std::move(parser).getEntries();

  entries.reserve(entries.size() + (prevUrl.empty() ? 0 : 1) + (nextUrl.empty() ? 0 : 1));
  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }
  if (feedTruncated) {
    LOG_INF("OPDS", "Feed truncated to fit memory");
  }

  // v13/v156/v159：返回中的游標還原。必須在偽項目（上一頁/下一頁列）插入【之後】才套——
  // 儲存時的 selectorIndex 是顯示座標（含偽項目）。夾限到實際筆數（feed 可能變了）；只消費一次。
  // ⚠️ v156 把這段放在中段，被這裡原本的「selectorIndex = 0」無條件蓋掉——還原從未生效過。
  selectorIndex = 0;
  if (pendingRestoreIndex >= 0) {
    if (!entries.empty()) {
      selectorIndex = std::min(pendingRestoreIndex, static_cast<int>(entries.size()) - 1);
      if (!pendingRestoreHref.empty()) {
        for (size_t i = 0; i < entries.size(); i++) {
          if (entries[i].href == pendingRestoreHref) {
            selectorIndex = static_cast<int>(i);
            break;
          }
        }
      }
    }
    pendingRestoreIndex = -1;
    pendingRestoreHref.clear();
  }
  state = BrowserState::BROWSING;  // 空 feed 也是 BROWSING：render 畫空狀態版面（空分類≠錯誤）
  requestUpdate();
}

void OpdsBookBrowserActivity::releaseEntries() { std::vector<OpdsEntry>().swap(entries); }

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back({currentPath, selectorIndex, entry.href});
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  releaseEntries();
  selectorIndex = 0;
  pendingRestoreIndex = -1;  // 前進到新層：不還原任何舊游標
  pendingRestoreHref.clear();
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    DiagLog::line("OPDS home: back at root state=%d", static_cast<int>(state));
    onGoHome();
  } else {
    currentPath = navigationHistory.back().path;
    pendingRestoreIndex = navigationHistory.back().selectorIndex;  // v13/v156：feed 載入後還原
    pendingRestoreHref = navigationHistory.back().href;
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    releaseEntries();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  // opdsDownloadFolder is already a null-terminated char[64]; use it directly —
  // no std::string copy. exists()/mkdir() take const char*.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    // exists()-guard first: mkdir's return-on-existing is unconfirmed, and every
    // existing caller checks exists() before mkdir. On real failure, fall back
    // to SD root so the download is never lost.
    LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  // downloadToFile() needs a std::string, and titles are unbounded (a fixed
  // char[] would truncate). Cold path (a multi-second download follows), so one
  // reserve'd, in-place-appended owning string is the right call.
  std::string filename;
  filename.reserve(96);
  if (haveFolder) filename += folder;
  filename += '/';
  filename += opdsBookFilename(book.author, book.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      nullptr, server.username, server.password);

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    state = BrowserState::BROWSING;
  } else {
    LOG_ERR("OPDS", "Download failed: %d", static_cast<int>(result));
    DiagLog::line("OPDS download FAILED code=%d %s", static_cast<int>(result), HttpDownloader::lastError);
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back({currentPath, selectorIndex,
                               entries.empty() ? std::string() : entries[selectorIndex].href});
  currentPath = url;                         // <-- add this

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  releaseEntries();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  DiagLog::line("OPDS wifi-stage connected=%d", static_cast<int>(connected));
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
