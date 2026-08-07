#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>

#include "ble/BleRemoteManager.h"

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/DiagLog.h"
#include "util/OpdsFilename.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
// v48:清單改用 GUI.drawList(與最近閱讀完全同風格,72px 列+32px 圖示),不再自繪。
// 每頁項數改與 RecentBooksActivity 同源(getNumberOfItemsPerPage),loop 與 render 各自計算同式,
// 翻頁邏輯(v15 latch/到底載下一伺服器頁)不變。
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;
// 進入下一層前，若最大連續塊 >= 此值才快取當層 feed（保留給下一次 WiFi/TLS handshake 的
// ~16-32KB 記錄緩衝）；不足則不快取，返回時 fallback 重抓。可調。（F2 資料夾快取用。）
constexpr size_t MIN_MAXBLOCK_FOR_CACHE = 64 * 1024;
// 伺服器頁快取存 SD 堆疊（不佔 RAM）：離開頁前寫 SD 檔、釋放 RAM，連線才不會 failed to fetch；
// 往回從 SD 讀回（不連網、快）。方向+深度配對：back 堆疊 b0/b1…、fwd 堆疊 f0/f1…（見 cachePath）。
constexpr const char* OPDS_CACHE_DIR = "/.opdscache";
constexpr uint32_t OPDS_CACHE_MAGIC = 0x4F504331;   // 'OPC1' 版本/健檢
constexpr uint32_t OPDS_CACHE_MAX_ENTRIES = 200;    // 反序列化上限，防壞檔亂配置
constexpr uint16_t OPDS_CACHE_MAX_STRLEN = 2048;    // 單一字串長度上限
constexpr int OPDS_CACHE_MAX_STACK = 100;           // 單一堆疊深度上限（防病態；SD ~15KB/頁，正常翻頁遠不及）
constexpr unsigned long BACK_HOLD_HOME_MS = 1000;  // 長按 Back 超過此毫秒 = 回主畫面(v38:對齊「1000=跳脫動作」檔,與 FileBrowser 回根/reader 回家一致;原 550)
constexpr size_t MAX_HISTORY_DEPTH = 32;          // history 深度上限，防病態/循環目錄無限成長
}  // namespace

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  // Free the SD reader font (~40-90KB) before WiFi + TLS. OPDS renders only with the
  // builtin UI fonts, so this frees a large contiguous block for the wolfSSL handshake's
  // ~16KB record buffer (the connect-failure cause). Reload is automatic: onExit reboots
  // once WiFi came up, else ReaderActivity::onEnter re-runs ensureLoaded. Mirrors v5.
  DiagLog::mem("opds-enter");
  // v100: OPDS was missed when v84 enumerated the WiFi entry points that must yield the radio
  // (WifiSelectionActivity x2, CrossPointWebServerActivity x2). It uses WiFi + TLS like both of
  // them, so BLE has been running underneath every OPDS session since BLE shipped. Two costs,
  // and CLAUDE.md names both: NimBLE holds ~34KB of the pool that is "OPDS/WiFi 唯一能供應
  // 40-55KB 連續塊的池", and BLE shares the 2.4GHz radio with WiFi.
  //
  // It surfaced now because v99 switched the reconnect scan to ACTIVE for the name-match
  // fallback -- active scanning transmits SCAN_REQ, which disturbs WiFi far more than passive
  // listening did. The scan was always there; v99 made it loud.
  //
  // Same terminal-until-reboot semantics as the other four call sites; onExit already reboots
  // once WiFi came up, which brings BLE back.
  BLE_REMOTE.stopForWifi();  // BLE/WiFi 互斥:讓出無線電與 heap,直到重開機
  sdFontSystem.unloadForLowMemory(renderer);
  DiagLog::mem("opds-fontfree");  // 字型卸載後:量出 v5 保險機制實際騰出多少

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  clearServerPageCache();
  searchTemplate = "";
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  entries.clear();
  navigationHistory.clear();
  clearServerPageCache();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
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
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
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
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    // 長按 Back：不論多深，直接回主畫面（不逐層退、不逐層重抓）。
    // 用 Back 自己的按下時間，而非全域 getHeldTime()（後者算最早按住的鍵，會造成
    // 「按住 Down 翻頁時再按 Back」誤觸回主畫面）。
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      backPressStartMs = millis();
    }
    if (!backHoldFired && mappedInput.isPressed(MappedInputManager::Button::Back) && backPressStartMs != 0 &&
        millis() - backPressStartMs > BACK_HOLD_HOME_MS) {
      backHoldFired = true;
      navigationHistory.clear();
      onGoHome();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const int last = static_cast<int>(entries.size()) - 1;
        // 頁導覽項（fetchFeed 固定把「上一頁」插在首、「下一頁」放在末）走側向載入。
        if (selectorIndex == 0 && !prevPageUrl.empty()) {
          loadServerPage(prevPageUrl, false);  // 上一頁 = 往回
        } else if (selectorIndex == last && !nextPageUrl.empty()) {
          loadServerPage(nextPageUrl, true);  // 下一頁 = 往前
        } else {
          const auto& entry = entries[selectorIndex];
          entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      backPressStartMs = 0;
      if (backHoldFired) {
        backHoldFired = false;  // 這次 release 是長按的收尾，已回過 home，吞掉
      } else {
        navigateBack();
      }
    }
    // search 已移除：此裝置無中文鍵盤，對中文書庫無用（原本 Left 在最上面呼叫鍵盤搜尋）。

    if (!entries.empty()) {
      // 長按 = 整個「按住→放開」只做【一次】動作：第一次連續觸發就做，之後壓制到放開為止。
      // 旗標只由放開邊緣(wasReleased)清除，不用 isPressed —— 避免載入下一頁的阻塞式 fetch
      // 之後 ADC 去彈跳誤讀成「放開又按」而重複觸發（v13/v14 的「連續翻不停」根因）。
      if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) navNextActionDone = false;
      if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) navPrevActionDone = false;

      // 短按（未達長按門檻）：移動 1 本，循環（到底回頂 / 到頂回底）。維持不變。
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      // 長按：一次動作。若「已在底部」→ 載入下一伺服器頁（要再長按一次才翻）；否則往下翻一頁
      // (clamp 停在底、不 wrap)。往上對稱：已在頂部 → 上一伺服器頁；否則往上翻一頁。
      const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);
      buttonNavigator.onNextContinuous([this, pageItems] {
        if (navNextActionDone) return;
        navNextActionDone = true;
        const int last = static_cast<int>(entries.size()) - 1;
        if (selectorIndex >= last && !nextPageUrl.empty()) {
          loadServerPage(nextPageUrl, true);
        } else {
          selectorIndex = ButtonNavigator::nextPageIndexClamped(selectorIndex, entries.size(), pageItems);
          requestUpdate();
        }
      });
      buttonNavigator.onPreviousContinuous([this, pageItems] {
        if (navPrevActionDone) return;
        navPrevActionDone = true;
        if (selectorIndex <= 0 && !prevPageUrl.empty()) {
          loadServerPage(prevPageUrl, false);
        } else {
          selectorIndex = ButtonNavigator::previousPageIndexClamped(selectorIndex, entries.size(), pageItems);
          requestUpdate();
        }
      });
    }
  }
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Show server name in header if available, otherwise generic title(v37:改走全機統一的 GUI.drawHeader)
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  // 頁碼副標(v38;僅瀏覽態且有內容時顯示。v48:pageItems 與 loop/drawList 同源)
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, true);
  const std::string pageText = (state == BrowserState::BROWSING && !entries.empty())
                                   ? UITheme::pageIndicatorText(selectorIndex, static_cast<int>(entries.size()),
                                                                pageItems)
                                   : std::string();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, headerTitle,
                 pageText.empty() ? nullptr : pageText.c_str());

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    // 間距 = UI_12 行框(34px)+ 4px,與同檔空狀態版面同式(複查抓到寫死 -24 會讓兩行筆畫交疊 3px)
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2,
                              tr(STR_ERROR_MSG), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 2, errorMessage.c_str());
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
      entries.empty() ? ""
                      : ((entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN));
  const char* searchLabel = tr(STR_DIR_UP);  // search 已移除（無中文鍵盤）
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (entries.empty()) {
    // 空 feed = 空狀態(不是錯誤):告訴使用者現在能按什麼。
    // 根 feed 的 Back = 離開 OPDS(navigateBack→onGoHome),非根才是「回上一層」——文案依情境分流(複查)
    const int midY = pageHeight / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, midY - renderer.getLineHeight(UI_12_FONT_ID) - 2,
                              tr(STR_OPDS_EMPTY_FEED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 2,
                              navigationHistory.empty() ? tr(STR_OPDS_EMPTY_HINT_ROOT) : tr(STR_OPDS_EMPTY_HINT));
    renderer.displayBuffer();
    return;
  }

  // v48:改用全機共用清單元件(與最近閱讀完全同風格:72px 列、32px 圖示、同 offsets、
  // 選取樣式自動跟主題走——不再維護自繪複本)。書=書本圖示、導覽/翻頁項=資料夾圖示(取代「> 」前綴)。
  const int contentHeight = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, contentHeight}, static_cast<int>(entries.size()), selectorIndex,
      [this](int index) { return entries[index].title; },
      [this](int index) {
        const auto& entry = entries[index];
        return (entry.type == OpdsEntryType::BOOK) ? entry.author : std::string();
      },
      [this](int index) { return entries[index].type == OpdsEntryType::BOOK ? UIIcon::Book : UIIcon::Folder; });
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  // v102: log the CPU clock. The TLS handshake here is ECDHE + an ECDSA P-256 certificate on a
  // chip with no ECC acceleration, and the idle timer drops the core from 160 MHz to 10 MHz after
  // 3 s of no button presses -- a 16x slowdown that would turn a ~2 s handshake into ~32 s. The
  // observed failure is a 30.7 s timeout, which fits suspiciously well, but nothing in the log
  // actually says what the clock was. Now it does.
  DiagLog::line("OPDS fetch start: cpu=%u MHz url=%s", static_cast<unsigned>(getCpuFrequencyMhz()), url.c_str());
  DiagLog::mem("opds-fetch-pre");  // TLS 握手 + feed 解析前(歷史上 failed-to-fetch 的臨界點)
  const unsigned long fetchStartMs = millis();
  OpdsParser parser;
  bool fetchOk = false;
  {
    // v103: lend the framebuffer to the TLS handshake, exactly as the reader lends it to a
    // chapter inflate. Bisected on hardware: v83 (no BLE) fetches this same feed on this same
    // network; v102 fails. The difference is the largest contiguous block at fetch time --
    // ~62,000 on v83 against 45,044 here -- and CLAUDE.md pins the requirement at 40-55KB,
    // with v21 having measured the failure point at ~39KB. 45,044 sits at the bottom of that
    // band and the handshake eats into it further (32,756 by the time it gives up).
    //
    // The cause is structural and cannot be given back at runtime: linking NimBLE costs 28KB
    // and displaces the framebuffer into the pool OPDS depends on for large blocks (the layout
    // CLAUDE.md's rejected-list item 1 warns about). stopForWifi() returns NimBLE's runtime
    // heap but not the link-time layout. Handing over the framebuffer's 52,272 bytes for the
    // duration of the blocking fetch restores the headroom v83 had.
    //
    // Safe because nothing draws during the fetch: it blocks the loop, and the error/success
    // handling below only sets state + requestUpdate(), with render() running on a later
    // iteration -- after the loan's scope has restored the buffer.
    GfxRenderer::FrameBufferLoan loan(renderer);
    OpdsParserStream stream{parser};
    fetchOk = HttpDownloader::fetchUrl(url, stream, server.username, server.password);
  }
  {
    if (!fetchOk) {
      // v101: the screen only ever said "failed to fetch feed". The actual reason (bad status,
      // TLS failure, redirect loop, short read...) was logged with LOG_ERR, i.e. to a serial
      // port this device does not have. Put it where it can be read.
      DiagLog::line("OPDS fetch FAILED after %lu ms (cpu=%u MHz): %s",
                    static_cast<unsigned long>(millis() - fetchStartMs), static_cast<unsigned>(getCpuFrequencyMhz()),
                    HttpDownloader::lastError);
      DiagLog::mem("opds-fetch-fail");
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    // Distinct from the fetch failure above: the bytes arrived but the XML did not parse.
    DiagLog::line("OPDS parse FAILED (fetch succeeded)");
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  DiagLog::mem("opds-fetch-post");
  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  const bool feedTruncated = parser.truncated();
  // 存成成員供邊界翻頁/側向載入使用（url 為本 feed 絕對位址，見函式開頭）。
  nextPageUrl = nextUrl.empty() ? std::string() : UrlUtils::buildUrl(url, nextUrl);
  prevPageUrl = prevUrl.empty() ? std::string() : UrlUtils::buildUrl(url, prevUrl);
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

  selectorIndex = 0;
  state = BrowserState::BROWSING;  // 空 feed 也是 BROWSING:render 畫空狀態版面(空分類≠錯誤)
  requestUpdate();
}

void OpdsBookBrowserActivity::releaseEntries() { std::vector<OpdsEntry>().swap(entries); }

void OpdsBookBrowserActivity::pushHistoryLevel() {
  // 記住當層：路徑 + 游標；記憶體夠就連 entries + 分頁 URL 一起快取，返回可秒回。
  // 呼叫時機必須在 currentPath 被改成子層之前。
  HistoryEntry parent;
  parent.path = currentPath;
  parent.selectorIndex = selectorIndex;
  if (ESP.getMaxAllocHeap() >= MIN_MAXBLOCK_FOR_CACHE) {
    parent.cachedEntries = std::move(entries);  // entries 變空
    parent.cachedNextPageUrl = std::move(nextPageUrl);
    parent.cachedPrevPageUrl = std::move(prevPageUrl);
  } else {
    releaseEntries();  // 不快取：先釋放當層 feed，返回時重抓
  }
  // 病態/循環目錄的深度上限：超過就丟最舊一層，避免 history 無限成長（-fno-exceptions 下
  // vector 重配失敗會 abort）。真實目錄極少 >數層，正常永不觸發。
  if (navigationHistory.size() >= MAX_HISTORY_DEPTH) {
    navigationHistory.erase(navigationHistory.begin());
  }
  navigationHistory.push_back(std::move(parent));
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  // entry 參照 entries 內元素；pushHistoryLevel() 在低記憶體分支會釋放 entries，
  // 故先把要用的 href 複製出來，避免 use-after-free。
  const std::string href = entry.href;
  clearServerPageCache();  // 進資料夾＝換 feed 情境，舊頁快取作廢並釋放 RAM
  pushHistoryLevel();

  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
    return;
  }
  clearServerPageCache();  // 返回上一層＝換 feed 情境，舊頁快取作廢並釋放 RAM
  HistoryEntry parent = std::move(navigationHistory.back());
  navigationHistory.pop_back();
  currentPath = std::move(parent.path);

  if (!parent.cachedEntries.empty()) {
    // 有快取：不連網、瞬間還原上一層與游標。
    releaseEntries();
    entries = std::move(parent.cachedEntries);
    nextPageUrl = std::move(parent.cachedNextPageUrl);
    prevPageUrl = std::move(parent.cachedPrevPageUrl);
    const int last = static_cast<int>(entries.size()) - 1;
    selectorIndex = parent.selectorIndex < 0 ? 0 : (parent.selectorIndex > last ? last : parent.selectorIndex);
    state = BrowserState::BROWSING;
    requestUpdate(true);
    return;
  }

  // 無快取：重抓（fetchFeed 為同步阻塞），完成後還原游標。
  releaseEntries();
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate();
  fetchFeed(currentPath);
  if (state == BrowserState::BROWSING && !entries.empty()) {
    const int last = static_cast<int>(entries.size()) - 1;
    selectorIndex = parent.selectorIndex < 0 ? 0 : (parent.selectorIndex > last ? last : parent.selectorIndex);
    requestUpdate();
  }
}

namespace {
// 長度前綴字串序列化到 HalFile（len 上限保護見 OPDS_CACHE_MAX_STRLEN）。
bool writeCacheStr(HalFile& f, const std::string& s) {
  const uint16_t len = static_cast<uint16_t>(s.size() > OPDS_CACHE_MAX_STRLEN ? 0 : s.size());
  if (f.write(&len, sizeof(len)) != sizeof(len)) return false;
  if (len == 0) return true;
  return f.write(s.data(), len) == len;
}
bool readCacheStr(HalFile& f, std::string& out) {
  uint16_t len = 0;
  if (f.read(&len, sizeof(len)) != static_cast<int>(sizeof(len))) return false;
  if (len > OPDS_CACHE_MAX_STRLEN) return false;  // 壞檔保護
  out.assign(len, '\0');
  if (len == 0) return true;
  return f.read(&out[0], len) == static_cast<int>(len);
}
// 堆疊快取檔名：back → /.opdscache/b<depth>.bin、fwd → f<depth>.bin。
void cachePath(char* buf, size_t n, bool back, int depth) {
  snprintf(buf, n, "%s/%c%d.bin", OPDS_CACHE_DIR, back ? 'b' : 'f', depth);
}
}  // namespace

void OpdsBookBrowserActivity::releaseCurrentPageRam() {
  // 真釋放目前頁的 RAM：entries 用 swap 釋放底層緩衝（clear() 只解構、保留 capacity → 會卡連線），URL 也 swap。
  releaseEntries();
  std::string().swap(nextPageUrl);
  std::string().swap(prevPageUrl);
}

void OpdsBookBrowserActivity::clearOneStack(bool back) {
  // 從 index 0 掃到缺口（檔案連續寫入，中間不會有洞），刪掉該方向所有 SD 檔——【不靠 RAM 深度計數】：
  // 進活動時 depth 歸零,若靠計數會漏刪上次當機/斷電殘留的檔;掃到缺口也順便清掉寫失敗的半截檔。
  char buf[48];
  for (int i = 0; i <= OPDS_CACHE_MAX_STACK; i++) {  // 上限保護
    cachePath(buf, sizeof(buf), back, i);
    if (!Storage.exists(buf)) break;
    Storage.remove(buf);
  }
  (back ? backServerDepth : fwdServerDepth) = 0;
}

void OpdsBookBrowserActivity::clearServerPageCache() {
  clearOneStack(true);
  clearOneStack(false);
}

bool OpdsBookBrowserActivity::writePageToSd(const char* path) {
  if (entries.empty()) return false;
  if (!Storage.exists(OPDS_CACHE_DIR)) Storage.mkdir(OPDS_CACHE_DIR);
  HalFile f;
  if (!Storage.openFileForWrite("OPDS", path, f)) return false;
  bool ok = true;
  const uint32_t magic = OPDS_CACHE_MAGIC;
  const uint32_t count = static_cast<uint32_t>(entries.size());
  ok = ok && f.write(&magic, sizeof(magic)) == sizeof(magic);
  ok = ok && f.write(&count, sizeof(count)) == sizeof(count);
  for (const auto& e : entries) {
    if (!ok) break;
    const uint8_t type = static_cast<uint8_t>(e.type);
    ok = ok && f.write(&type, sizeof(type)) == sizeof(type);
    ok = ok && writeCacheStr(f, e.title) && writeCacheStr(f, e.author) && writeCacheStr(f, e.href) &&
         writeCacheStr(f, e.id);
  }
  ok = ok && writeCacheStr(f, nextPageUrl) && writeCacheStr(f, prevPageUrl);
  return ok;  // HalFile 解構自動關檔（DESTRUCTOR_CLOSES_FILE）
}

bool OpdsBookBrowserActivity::readPageFromSd(const char* path) {
  HalFile f;
  if (!Storage.openFileForRead("OPDS", path, f)) return false;
  uint32_t magic = 0;
  uint32_t count = 0;
  if (f.read(&magic, sizeof(magic)) != static_cast<int>(sizeof(magic)) || magic != OPDS_CACHE_MAGIC) return false;
  if (f.read(&count, sizeof(count)) != static_cast<int>(sizeof(count)) || count > OPDS_CACHE_MAX_ENTRIES) return false;
  std::vector<OpdsEntry> loaded;
  loaded.reserve(count);
  for (uint32_t i = 0; i < count; i++) {
    OpdsEntry e;
    uint8_t type = 0;
    if (f.read(&type, sizeof(type)) != static_cast<int>(sizeof(type))) return false;
    e.type = static_cast<OpdsEntryType>(type);
    if (!readCacheStr(f, e.title) || !readCacheStr(f, e.author) || !readCacheStr(f, e.href) || !readCacheStr(f, e.id))
      return false;
    loaded.push_back(std::move(e));
  }
  std::string nUrl;
  std::string pUrl;
  if (!readCacheStr(f, nUrl) || !readCacheStr(f, pUrl)) return false;
  // 全部成功才套用（失敗不動 entries）。
  entries = std::move(loaded);
  nextPageUrl = std::move(nUrl);
  prevPageUrl = std::move(pUrl);
  return true;
}

void OpdsBookBrowserActivity::loadServerPage(const std::string& pageUrl, const bool forward) {
  // 側向載入伺服器頁（不加深 history）。方向+深度【SD 堆疊】快取（不佔 RAM，這趟看過的頁都留）：
  //   往前：離開頁 push back 堆疊、還原 fwd 堆疊頂；往回：離開頁 push fwd 堆疊、還原 back 堆疊頂。
  // 關鍵：離開頁寫 SD 後【真釋放 RAM】(releaseCurrentPageRam)，連線時 RAM 才夠（v20 實測：握任何頁會 failed to fetch）。
  const std::string target = pageUrl;  // 快照（pageUrl 常指向 this->next/prevPageUrl，下面會清）
  const bool stashBack = forward;                                   // 往前 → push 進 back 堆疊
  int& stashDepth = forward ? backServerDepth : fwdServerDepth;
  const bool restoreBack = !forward;                                // 往前 → 從 fwd 堆疊還原
  int& restoreDepth = forward ? fwdServerDepth : backServerDepth;
  char buf[48];

  // 1. 離開的當前頁 push 到 stash 堆疊頂（成功才增深度）；太深就整個放掉重來（防病態）。寫完真釋放 RAM。
  if (stashDepth >= OPDS_CACHE_MAX_STACK) clearServerPageCache();  // 會把 back/fwd 深度歸零
  cachePath(buf, sizeof(buf), stashBack, stashDepth);
  if (writePageToSd(buf)) {
    stashDepth++;
  } else {
    // 寫失敗 → 若只刪半截檔、深度不動，堆疊會出現「位置錯位」→ 之後反向導覽會位置式還原到【錯的頁】
    // (currentPath 與顯示內容不符)。故整個 stash 堆疊作廢(含半截檔)，之後改連網抓正確內容。
    clearOneStack(stashBack);
  }
  releaseCurrentPageRam();
  currentPath = target;
  selectorIndex = 0;

  // 2. 從 restore 堆疊頂還原（若有）→ 不連網、快。壞檔/遺失則該方向堆疊作廢，改連網。
  if (restoreDepth > 0) {
    cachePath(buf, sizeof(buf), restoreBack, restoreDepth - 1);
    if (Storage.exists(buf) && readPageFromSd(buf)) {
      Storage.remove(buf);
      restoreDepth--;
      state = BrowserState::BROWSING;
      requestUpdate(true);
      return;
    }
    clearOneStack(restoreBack);  // 壞檔 → 這方向堆疊作廢
  }

  // 3. 未命中 → 連網抓（RAM 已釋放，連線不會失敗）。
  releaseCurrentPageRam();
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  fetchFeed(target);
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
  // v107: stash the feed to SD and free its RAM before the download's TLS handshake.
  //
  // This project already knew the rule — the comment on loadServerPage() says it outright:
  // 「離開頁寫 SD 後【真釋放 RAM】,連線時 RAM 才夠(v20 實測:握任何頁會 failed to fetch)」
  // — but it was only ever applied to page navigation, never to the download. diag106-2 shows
  // the cost: browsing fetches ran with int_max=26,612 and succeeded, while the download began
  // at 13,812 and its handshake failed. The entry list is what sits in between.
  //
  // Restored right after, so returning to the list costs one SD read instead of a re-fetch.
  // If either the write or the read-back fails we still download (the stash is an optimisation,
  // not a dependency); the list then simply comes back empty and the next navigation re-fetches.
  constexpr const char* kDlStashPath = "/.opdscache/dl.bin";
  const bool stashed = writePageToSd(kDlStashPath);
  if (stashed) {
    releaseCurrentPageRam();
  }
  DiagLog::mem("opds-dl-pre");
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

  DiagLog::mem("opds-dl-post");
  if (stashed) {
    readPageFromSd(kDlStashPath);  // best effort; an empty list just means the next nav re-fetches
  }

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    state = BrowserState::BROWSING;
    // Offer to open the fresh download right away. Confirm reuses the silent-restart-to-reader
    // path (main.cpp boot routing via APP_STATE.openEpubPath), which also performs the WiFi
    // teardown + heap defrag that leaving OPDS needs anyway. Cancel stays in the list with the
    // browsing position and page cache untouched, so multi-book download sessions keep flowing.
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DOWNLOAD_COMPLETE), tr(STR_OPEN_BOOK_NOW)),
        [this, filename](const ActivityResult& res) {
          if (res.isCancelled) return;
          APP_STATE.openEpubPath = filename;
          APP_STATE.saveToFile();
          WiFi.disconnect(false);
          delay(30);
          silentRestartToReader();
        });
  } else {
    LOG_ERR("OPDS", "Download failed: %d", static_cast<int>(result));
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

  pushHistoryLevel();  // 記住搜尋前的清單（路徑+游標+可選快取），可 Back 回去
  currentPath = url;

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
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
