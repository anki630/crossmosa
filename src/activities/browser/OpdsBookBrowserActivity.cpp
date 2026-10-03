#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <DataDir.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>

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
#include "util/BookCacheUtils.h"
#include "util/DiagLog.h"
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
// v346：長按「退出」回主畫面的門檻，與檔案瀏覽（回最上層）、閱讀器（回書架）的長按同一個值。
constexpr unsigned long BACK_HOLD_HOME_MS = 1000;
// v346：換書庫頁要翻頁鍵本身按滿這麼久（與 ButtonNavigator 的長按門檻同值，但用這顆鍵自己的計時）。
constexpr unsigned long NAV_HOLD_MS = 500;
// v346：history 上限（v13 原有，換基底時漏搬）。長按翻頁讓人容易一口氣翻過很多頁，
// 每翻一頁都會進一層 history；超過就丟最舊的，不讓它無限長大。
constexpr size_t MAX_HISTORY_DEPTH = 32;

// v347：SD 頁快取的檔案格式（每次進書庫整個資料夾清掉重來，所以不用跨版本相容，只要能認出壞檔）。
//   u32 magic 'OPC2' | u8 版本 | u8 旗標（bit0 清單頭是「上一頁」列、bit1 清單尾是「下一頁」列） | u16 筆數
//   | u32 這次瀏覽的 nonce | u32 快取編號
//   每筆：u8 類型 | title | author | href | id（每個字串 u16 長度＋位元組）
//   | u32 CRC32（前面全部）
// 讀回時檔頭、每個長度、CRC、檔尾都要對（codex 複查）：寫到一半、SD 靜默損壞、
// 卡片回報成功其實沒寫（讀到舊內容 → nonce 或編號對不上）都會被擋下，改成重新連線。
constexpr uint32_t FEED_CACHE_MAGIC = 0x3243504F;  // "OPC2"
constexpr uint8_t FEED_CACHE_VERSION = 1;
constexpr uint8_t FEED_CACHE_FLAG_PREV = 1;
constexpr uint8_t FEED_CACHE_FLAG_NEXT = 2;
// 上限跟 OpdsParser 一致（最多 62 筆＋2 列翻頁；字串上限是位元組）：超過就不是我們寫的檔。
constexpr uint16_t FEED_CACHE_MAX_ENTRIES = 64;
constexpr size_t FEED_CACHE_MAX_TITLE = 160;
constexpr size_t FEED_CACHE_MAX_AUTHOR = 120;
constexpr size_t FEED_CACHE_MAX_HREF = 768;
constexpr size_t FEED_CACHE_MAX_ID = 128;
constexpr size_t FEED_CACHE_HEAP_MARGIN = 8 * 1024;  // 讀回之前，記憶體至少要多留這麼多

void feedCacheDir(char* buf, const size_t n) { snprintf(buf, n, "%s/opdscache", DataDir::path()); }

void feedCachePath(char* buf, const size_t n, const uint32_t id) {
  snprintf(buf, n, "%s/opdscache/%lu.bin", DataDir::path(), static_cast<unsigned long>(id));
}

// 寫與讀都邊做邊算 CRC；兩邊的呼叫順序與大小完全一樣，所以算出來的值可以直接比。
struct FeedCacheWriter {
  HalFile& f;
  uint32_t crc = 0;
  bool ok = true;
  void put(const void* p, const size_t n) {
    if (!ok || n == 0) return;
    ok = f.write(p, n) == n;
    crc = esp_rom_crc32_le(crc, static_cast<const uint8_t*>(p), n);
  }
  void str(const std::string& s, const size_t maxLen) {
    if (s.size() > maxLen) ok = false;
    const uint16_t len = static_cast<uint16_t>(s.size());
    put(&len, sizeof(len));
    put(s.data(), len);
  }
};

// 讀失敗一律當壞檔，不信任半截的長度（教訓 A-6：readPod 讀失敗不會改寫目標）。
struct FeedCacheReader {
  HalFile& f;
  uint32_t crc = 0;
  bool ok = true;
  void get(void* p, const size_t n) {
    if (!ok || n == 0) return;
    ok = f.read(p, n) == static_cast<int>(n);
    if (ok) crc = esp_rom_crc32_le(crc, static_cast<const uint8_t*>(p), n);
  }
  void str(std::string& s, const size_t maxLen) {
    uint16_t len = 0;
    get(&len, sizeof(len));
    if (!ok || len > maxLen) {
      ok = false;
      return;
    }
    s.resize(len);
    get(&s[0], len);
  }
};

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
  {
    // v200：render 跑在獨立 task（ActivityManager::renderTaskLoop），而 render() 裡的
    // lambda 是【延遲】讀 entries[index] 的 —— 主任務在那當中改動 entries 就是 UAF。
    // crash_report199（rst=4、panic reason 空 = 硬體例外不是 abort）的堆疊正是
    // renderTaskLoop → render():293 → drawList → vector<OpdsEntry>::operator[]。
    // ⚠️ 鎖的範圍要小：onEnter 後面的 checkAndConnectWifi() 實測要 3.9 秒，絕不能包進來。
    RenderLock lock;
    entries.clear();
  }
  navigationHistory.clear();
  clearFeedCache();                    // v347：上次沒清到的（當機、斷電、從別的路徑離開）一起清掉；這次的快取從空的開始
  feedCacheNonce = esp_random() | 1u;  // 這次瀏覽的身分：讀到別次留下的檔（卡片說寫了其實沒寫）就認得出來
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  hasPrevPageItem = hasNextPageItem = false;
  historyTruncated = false;
  backPressStartMs = 0;
  backHoldFired = false;
  nextHoldAtEdge = nextHoldDone = prevHoldAtEdge = prevHoldDone = false;
  setError({});
  setStatus(tr(STR_CHECKING_WIFI));
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
  // ⛔ v200：這裡【不可以】拿 RenderLock。onExit 是由 ActivityManager::exitActivity(const
  // RenderLock&) 呼叫的，而那個函式的註解白紙黑字寫著「lock must be held by the caller」。
  // renderingMutex 是 xSemaphoreCreateMutex()（**非**遞迴），再拿一次就是永久死鎖。
  // 對比 onEnter：管理器在呼叫它之前明確 lock.unlock()（ActivityManager.cpp:150），所以那裡要加。
  entries.clear();
  navigationHistory.clear();
  // v347：SD 頁快取不在這裡清 —— onExit 持有 RenderLock，遞迴刪檔不該放在鎖裡（codex 複查）。
  // 兩個「回主畫面」的出口（根層按退出、長按退出）離開前自己清；其他路徑（睡眠、當機）留到下次 onEnter。

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
  // v346：翻頁鍵的長按旗標只在放開邊緣清，而且放在所有提早 return 之前 —— 換頁失敗進 ERROR、
  // 或清單是空的時候放開，也要清得掉（codex 複查）。
  if (mappedInput.wasReleased(MappedInputManager::Button::NavNext)) nextHoldAtEdge = nextHoldDone = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::NavPrevious)) prevHoldAtEdge = prevHoldDone = false;

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
    if (handleBackHold()) return;  // v346：連不上、讀不到的畫面一樣能長按「退出」直接回主畫面
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      DiagLog::line("OPDS retry connected=%d", static_cast<int>(WiFi.status() == WL_CONNECTED));
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        setStatus(tr(STR_LOADING));
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (!consumeBackHoldRelease()) navigateBack();
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
    if (handleBackHold()) return;  // v346：長按「退出」→ 主畫面，不必一層一層退、一層一層重新連線

    auto activateSelected = [this] {
      if (selectorIndex < 0 || selectorIndex >= static_cast<int>(entries.size())) return;
      // v347：頭尾的翻頁列走 openPageItem（跟上一層配得起來就退回去讀 SD 快取，不重新連線）
      if (hasNextPageItem && selectorIndex == static_cast<int>(entries.size()) - 1) {
        openPageItem(true);
      } else if (hasPrevPageItem && selectorIndex == 0) {
        openPageItem(false);
      } else {
        const auto& entry = entries[selectorIndex];
        entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
      }
    };

    // v346：開項目、下載、退一層都是同步的，回來時 entries 已經換成別頁 —— 這一輪到此為止，
    // 不讓同一輪的其他按鍵作用在新的清單上（codex 複查）。
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
      return;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      if (!consumeBackHoldRelease()) navigateBack();
      return;
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

      // v346（v13④／v15 搬回）：按下翻頁鍵的那一刻，記下游標是不是正停在頭尾的翻頁列（見標頭註解）。
      if (mappedInput.wasPressed(MappedInputManager::Button::NavNext)) {
        nextHoldAtEdge = hasNextPageItem && selectorIndex == static_cast<int>(entries.size()) - 1;
        nextHoldDone = false;
        nextPressStartMs = millis();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
        prevHoldAtEdge = hasPrevPageItem && selectorIndex == 0;
        prevHoldDone = false;
        prevPressStartMs = millis();
      }

      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, entries.size());
        requestUpdate();
      });
      // 長按：一次翻一整頁、到底（頂）停住（v31/v156）。按下時就停在「下一頁」（「上一頁」）那一列的話，
      // 改成載入書庫的下一頁（上一頁）——＝在那一列按確認——而且這次按住只做這一次。
      // 旗標放成員、不另外捕捉區域變數：這個工具鏈（RV32 libstdc++）的 std::function 內建空間是 8 bytes，
      // this＋pageItems 剛好放得下；多捕捉一個參照就會每一輪 loop 都在堆上配置一次，而 OPDS 正是最缺記憶體的地方。
      pagedThisLoop = false;
      buttonNavigator.onNextContinuous([this, pageItems] {
        if (nextHoldDone) return;
        if (nextHoldAtEdge && hasNextPageItem && !entries.empty()) {
          if (millis() - nextPressStartMs < NAV_HOLD_MS) return;  // 翻頁鍵本身還沒按滿（見標頭）
          nextHoldDone = true;
          pagedThisLoop = true;
          DiagLog::line("OPDS page: hold next depth=%u", static_cast<unsigned>(navigationHistory.size()));
          openPageItem(true);  // v347：跟上一層配得起來就是退回去（讀 SD 快取）
          return;
        }
        selectorIndex = ButtonNavigator::nextPageIndexClamped(selectorIndex, entries.size(), pageItems);
        requestUpdate();
      });
      if (pagedThisLoop) return;  // entries 已經換成新的一頁，後面的按鍵處理不能再碰
      buttonNavigator.onPreviousContinuous([this, pageItems] {
        if (prevHoldDone) return;
        if (prevHoldAtEdge && hasPrevPageItem && !entries.empty()) {
          if (millis() - prevPressStartMs < NAV_HOLD_MS) return;
          prevHoldDone = true;
          pagedThisLoop = true;
          DiagLog::line("OPDS page: hold prev depth=%u", static_cast<unsigned>(navigationHistory.size()));
          openPageItem(false);
          return;
        }
        selectorIndex = ButtonNavigator::previousPageIndexClamped(selectorIndex, entries.size(), pageItems);
        requestUpdate();
      });
      if (pagedThisLoop) return;
    }
  }
}

// v346（v13③ 搬回）：長按「退出」→ 直接回主畫面。按住滿 BACK_HOLD_HOME_MS 就觸發，不用等放開
// （跟檔案瀏覽、閱讀器的長按一樣）。onGoHome 只是排入切換，實際換頁在這一輪 loop 結束後，呼叫端要直接 return。
bool OpdsBookBrowserActivity::handleBackHold() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    backPressStartMs = millis();
    backHoldFired = false;
  }
  if (backHoldFired || backPressStartMs == 0 || !mappedInput.isPressed(MappedInputManager::Button::Back) ||
      millis() - backPressStartMs < BACK_HOLD_HOME_MS) {
    return false;
  }
  backHoldFired = true;
  DiagLog::line("OPDS home: back hold depth=%u state=%d", static_cast<unsigned>(navigationHistory.size()),
                static_cast<int>(state));
  clearFeedCache();  // v347：同 navigateBack 的根層
  onGoHome();
  return true;
}

bool OpdsBookBrowserActivity::consumeBackHoldRelease() {
  const bool wasHold = backHoldFired;
  backPressStartMs = 0;
  backHoldFired = false;
  return wasHold;
}

// v361（上游 #3547）：原本永遠回 true —— 停在書單畫面、WiFi 開著，機器永遠不睡。
// 只有「正在等網路」的狀態擋休眠；書單與錯誤畫面照設定的時間睡（WiFi 開著所以走真關機，醒來回主畫面）。
// WiFi 選擇與搜尋鍵盤是推在上面的子畫面，休眠看的是它們自己（ActivityManager::preventAutoSleep 只問最上層），
// 跟上游不同：我們不替它們擋，那兩個畫面停著不動照樣會睡。
bool OpdsBookBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::ERROR:
      return false;
  }
  return false;
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

  const char* confirmLabel = (selectorIndex >= 0 && selectorIndex < static_cast<int>(entries.size()) &&
                              entries[selectorIndex].type == OpdsEntryType::BOOK)
                                 ? tr(STR_DOWNLOAD)
                                 : tr(STR_OPEN);  // v200：selectorIndex 在鎖外被寫，這裡自己夾限，不依賴它的時序
  const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (entries.empty()) {
    // 空 feed = 空狀態（不是錯誤）：告訴使用者現在能按什麼。
    // 根 feed 的 Back = 離開 OPDS（navigateBack→onGoHome），非根才是「回上一層」——文案依情境分流。
    const int midY = pageHeight / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, midY - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_OPDS_EMPTY_FEED),
                              true, EpdFontFamily::BOLD);
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
    setError(tr(STR_NO_SERVER_URL));
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
    setError({});  // v200：連同 errorMessage 一起在鎖內清
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      // v101：畫面原本只有一句「failed to fetch feed」，真因（TLS 失敗、狀態碼、短讀…）
      // 全走 LOG_ERR = 這台沒有序列埠的機器上等於丟棄。寫進 DiagLog 並顯示在畫面上。
      DiagLog::line("OPDS fetch FAILED: %s", HttpDownloader::lastError);
      DiagLog::mem("opds-fetch-fail");
      state = BrowserState::ERROR;
      setError(tr(STR_FETCH_FEED_FAILED), HttpDownloader::lastError);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    DiagLog::line("OPDS parse FAILED");
    state = BrowserState::ERROR;
    setError(tr(STR_PARSE_FEED_FAILED));
    requestUpdate();
    return;
  }

  // CrossMosa B11：OPDS 搜尋停用（v14 拔過，理由是裝置上沒有中文輸入法，
  // 對中文書庫無用；上游 1.5 的螢幕鍵盤仍然只有 QWERTY）。
  // 從【唯一的來源】切斷：searchTemplate 恆空，其餘 8 個 !empty() 的站點
  // （按鍵、觸控、圖示、標題內縮、按鈕提示、performSearch）全部自動失效，
  // 不留任何按了沒反應的死路徑。要還原就把這行改回來。
  // v200：原本這裡有第二次 `searchTemplate = "";`。onEnter 已經設空，而本分支搜尋恆停用，
  // 那次寫入純屬多餘 —— 拿掉它就【完全不再】於 render 執行期間寫這個字串，競態自然消失。
  // 要是哪天把搜尋打開，這裡要改成走 setStatus/setError 那樣的鎖內 helper。
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  const bool feedTruncated = parser.truncated();
  {
    // v200：從這裡到 selectorIndex 定案為止，entries 與 selectorIndex 必須對 render task
    // 原子地一起換掉 —— drawList 拿的是 entries.size() 與 selectorIndex，兩者不一致就會
    // 索引越界。這一段全是記憶體操作，沒有 I/O，鎖的時間很短。
    RenderLock lock;
    entries = std::move(parser).getEntries();

    entries.reserve(entries.size() + (prevUrl.empty() ? 0 : 1) + (nextUrl.empty() ? 0 : 1));
    if (!prevUrl.empty()) {
      entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
    }
    if (!nextUrl.empty()) {
      entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
    }
    hasPrevPageItem = !prevUrl.empty();  // v346：長按翻頁用這兩個旗標認頭尾的翻頁列
    hasNextPageItem = !nextUrl.empty();
    if (feedTruncated) {
      LOG_INF("OPDS", "Feed truncated to fit memory");
    }
    applyPendingRestoreLocked();
  }  // v200：RenderLock 作用域結束
  state = BrowserState::BROWSING;  // 空 feed 也是 BROWSING：render 畫空狀態版面（空分類≠錯誤）
  requestUpdate();
}

void OpdsBookBrowserActivity::setStatus(std::string v) {
  assert(!RenderLock::heldByCurrentTask());  // 從持鎖路徑呼叫＝永久死鎖，斷在現場而不是掛在那裡
  RenderLock lock;
  statusMessage = std::move(v);
}

void OpdsBookBrowserActivity::setError(std::string msg, std::string detail) {
  assert(!RenderLock::heldByCurrentTask());
  RenderLock lock;
  errorMessage = std::move(msg);
  errorDetail = std::move(detail);
}

void OpdsBookBrowserActivity::releaseEntries() {
  // v200：swap 會【釋放】舊緩衝區，而 render task 的 lambda 可能正在讀它。
  // 三個呼叫點（navigateToEntry／navigateBack／performSearch）都由 loop() 觸發、未持鎖。
  RenderLock lock;
  std::vector<OpdsEntry>().swap(entries);
  hasPrevPageItem = hasNextPageItem = false;  // v346：清單清空，頭尾的翻頁列也不在了
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry, const HistoryLevel::Via via) {
  // 先把「載入中」畫出來再存檔：LOADING 畫面不讀 entries（render 只在 BROWSING 讀清單），
  // 存檔的 SD 寫入就落在面板刷新的空檔裡，按下去立刻有反應（codex 複查）。
  state = BrowserState::LOADING;
  setStatus(tr(STR_LOADING));
  requestUpdate(true);

  if (navigationHistory.size() >= MAX_HISTORY_DEPTH) {
    removeFeedCache(navigationHistory.front().cacheId);  // v347：丟掉的那一層，它的快取檔也不要了
    navigationHistory.erase(navigationHistory.begin());  // v346：丟最舊的一層（見 MAX_HISTORY_DEPTH）
    historyTruncated = true;
  }
  // v347：要離開的這一頁寫到 SD（entries 還在 RAM、還沒釋放；saveFeedCache 只讀不改 entries，
  // 所以 entry 這個參照之後照樣有效）。存不了就是 0，回來時照舊重新連線。
  const uint32_t cacheId = saveFeedCache();
  navigationHistory.push_back({currentPath, selectorIndex, entry.href, cacheId, via});
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  releaseEntries();
  selectorIndex = 0;
  pendingRestoreIndex = -1;  // 前進到新層：不還原任何舊游標
  pendingRestoreHref.clear();
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty() && historyTruncated && !currentPath.empty()) {
    // v346：history 丟過最舊的層，退到這裡還不是書庫最上層 → 先回最上層，下一次 Back 才離開書庫。
    historyTruncated = false;
    DiagLog::line("OPDS back: history truncated, to root");
    currentPath.clear();
    pendingRestoreIndex = -1;
    pendingRestoreHref.clear();
    state = BrowserState::LOADING;
    setStatus(tr(STR_LOADING));
    releaseEntries();
    selectorIndex = 0;
    requestUpdate();
    fetchFeed(currentPath);
  } else if (navigationHistory.empty()) {
    DiagLog::line("OPDS home: back at root state=%d", static_cast<int>(state));
    clearFeedCache();  // v347：在這裡清（未持 RenderLock），不放 onExit（見 clearFeedCache）
    onGoHome();
  } else {
    const uint32_t cacheId = navigationHistory.back().cacheId;
    currentPath = navigationHistory.back().path;
    pendingRestoreIndex = navigationHistory.back().selectorIndex;  // v13/v156：feed 載入後還原
    pendingRestoreHref = navigationHistory.back().href;
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    setStatus(tr(STR_LOADING));
    releaseEntries();  // 先釋放目前這一頁，讀快取也需要 RAM
    selectorIndex = 0;
    // v347：這一層離開時存過 SD → 直接讀回來，不用重新連線、也不用先畫「載入中」。
    // 讀完就刪：之後再從這一層往前走，會存一份新的。
    if (cacheId != 0) {
      const bool hit = loadFeedCache(cacheId);
      removeFeedCache(cacheId);
      if (hit) return;
    }
    requestUpdate();
    fetchFeed(currentPath);
  }
}

// v347：翻頁列（「下一頁」「上一頁」，按確認或長按）。這一頁如果是從隔壁那頁用反方向的翻頁列走過來的，
// 往回翻就是回到那一頁 → 走「退出」：讀 SD 快取、游標回到原位、history 不會越翻越長。
// 否則照舊往前載入（並記下是從哪個方向來的，讓之後的反方向翻頁配得起來）。
void OpdsBookBrowserActivity::openPageItem(const bool next) {
  if (entries.empty()) return;
  const auto cameFrom = next ? HistoryLevel::Via::PrevPage : HistoryLevel::Via::NextPage;
  if (!navigationHistory.empty() && navigationHistory.back().via == cameFrom) {
    DiagLog::line("OPDS page: %s pairs with history, back", next ? "next" : "prev");
    navigateBack();
    return;
  }
  navigateToEntry(next ? entries.back() : entries.front(),
                  next ? HistoryLevel::Via::NextPage : HistoryLevel::Via::PrevPage);
}

// v13/v156/v159：返回中的游標還原。必須在偽項目（上一頁/下一頁列）插入【之後】才套——
// 儲存時的 selectorIndex 是顯示座標（含偽項目）。夾限到實際筆數（feed 可能變了）；只消費一次。
// ⚠️ v156 把這段放在中段，被原本的「selectorIndex = 0」無條件蓋掉——還原從未生效過。
// v347：從 fetchFeed 抽出來，SD 快取讀回的清單也走同一套。呼叫端必須持有 RenderLock。
void OpdsBookBrowserActivity::applyPendingRestoreLocked() {
  selectorIndex = 0;
  if (pendingRestoreIndex >= 0) {
    if (!entries.empty()) {
      const int idx = std::min(pendingRestoreIndex, static_cast<int>(entries.size()) - 1);
      selectorIndex = idx;
      // v347（codex 複查）：原位置的 href 還對得上就留在原位 —— SD 快取讀回的是一模一樣的清單，
      // 從頭找 href 會在同一個 href 出現兩次時跳到第一個。對不上（feed 變了）才找離原位最近的同 href 項目。
      if (!pendingRestoreHref.empty() && entries[idx].href != pendingRestoreHref) {
        int best = -1;
        for (int i = 0; i < static_cast<int>(entries.size()); i++) {
          if (entries[i].href == pendingRestoreHref && (best < 0 || std::abs(i - idx) < std::abs(best - idx))) {
            best = i;
          }
        }
        if (best >= 0) selectorIndex = best;
      }
    }
    pendingRestoreIndex = -1;
    pendingRestoreHref.clear();
  }
}

// v347：把目前的清單寫到 SD。回傳快取編號；任何一步失敗回 0（呼叫端照舊，回來時重新連線）。
uint32_t OpdsBookBrowserActivity::saveFeedCache() {
  if (entries.empty() || entries.size() > FEED_CACHE_MAX_ENTRIES) return 0;
  const unsigned long t0 = millis();
  const uint32_t id = ++feedCacheSeq;
  char path[80];
  feedCachePath(path, sizeof(path), id);
  HalFile f;
  if (!Storage.openFileForWrite("OPDS", path, f)) {
    // 每次進書庫都把資料夾清掉，所以第一次存一定開不了 → 建資料夾再試一次。
    // 先開再建：mkdir 很慢（v283），而且「已存在時回傳什麼」沒確認過，不拿它的回傳值做判斷。
    char dir[64];
    feedCacheDir(dir, sizeof(dir));
    Storage.mkdir(dir);
    if (!Storage.openFileForWrite("OPDS", path, f)) {
      DiagLog::line("OPDS cache save FAILED open id=%lu", static_cast<unsigned long>(id));
      return 0;
    }
  }
  FeedCacheWriter w{f};
  const uint32_t magic = FEED_CACHE_MAGIC;
  const uint8_t version = FEED_CACHE_VERSION;
  const uint8_t flags =
      static_cast<uint8_t>((hasPrevPageItem ? FEED_CACHE_FLAG_PREV : 0) | (hasNextPageItem ? FEED_CACHE_FLAG_NEXT : 0));
  const uint16_t count = static_cast<uint16_t>(entries.size());
  w.put(&magic, sizeof(magic));
  w.put(&version, sizeof(version));
  w.put(&flags, sizeof(flags));
  w.put(&count, sizeof(count));
  w.put(&feedCacheNonce, sizeof(feedCacheNonce));
  w.put(&id, sizeof(id));
  for (size_t i = 0; w.ok && i < entries.size(); i++) {
    const auto& e = entries[i];
    const uint8_t type = static_cast<uint8_t>(e.type);
    w.put(&type, sizeof(type));
    w.str(e.title, FEED_CACHE_MAX_TITLE);
    w.str(e.author, FEED_CACHE_MAX_AUTHOR);
    w.str(e.href, FEED_CACHE_MAX_HREF);
    w.str(e.id, FEED_CACHE_MAX_ID);
  }
  const uint32_t crc = w.crc;
  w.put(&crc, sizeof(crc));
  const size_t bytes = f.size();
  const bool ok = f.close() && w.ok;  // close 會把緩衝寫出去；失敗時也要先關才能刪
  if (!ok) {
    Storage.remove(path);
    DiagLog::line("OPDS cache save FAILED write id=%lu", static_cast<unsigned long>(id));
    return 0;
  }
  DiagLog::line("OPDS cache save id=%lu n=%u bytes=%u ms=%lu", static_cast<unsigned long>(id),
                static_cast<unsigned>(count), static_cast<unsigned>(bytes), millis() - t0);
  return id;
}

// v347：讀回快取並換上。只要有任何一點不對（檔頭、筆數、長度、CRC、翻頁列、記憶體不夠）就回 false，
// 目前的 entries 不動，呼叫端改成重新連線。
bool OpdsBookBrowserActivity::loadFeedCache(const uint32_t id) {
  const unsigned long t0 = millis();
  char path[80];
  feedCachePath(path, sizeof(path), id);
  HalFile f;
  if (!Storage.openFileForRead("OPDS", path, f)) {
    DiagLog::line("OPDS back: cache miss id=%lu open", static_cast<unsigned long>(id));
    return false;
  }
  FeedCacheReader r{f};
  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t flags = 0;
  uint16_t count = 0;
  uint32_t nonce = 0;
  uint32_t fileId = 0;
  r.get(&magic, sizeof(magic));
  r.get(&version, sizeof(version));
  r.get(&flags, sizeof(flags));
  r.get(&count, sizeof(count));
  r.get(&nonce, sizeof(nonce));
  r.get(&fileId, sizeof(fileId));
  if (!r.ok || magic != FEED_CACHE_MAGIC || version != FEED_CACHE_VERSION ||
      (flags & ~(FEED_CACHE_FLAG_PREV | FEED_CACHE_FLAG_NEXT)) != 0 || count == 0 || count > FEED_CACHE_MAX_ENTRIES ||
      nonce != feedCacheNonce || fileId != id) {
    DiagLog::line("OPDS back: cache miss id=%lu header", static_cast<unsigned long>(id));
    return false;
  }
  // -fno-exceptions：配置失敗會 abort，不會丟例外 → 先看記憶體夠不夠，不夠就改重新連線。
  // 最大連續塊要放得下整個 vector；總量要放得下所有字串（大約就是檔案大小）。
  // 這是估計不是保證（其他 task 也在配置），但讀回的就是剛才解析器握過的同一份清單，
  // 而目前這一頁在呼叫之前已經釋放了。
  const size_t maxBlock = ESP.getMaxAllocHeap();
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t vecBytes = count * sizeof(OpdsEntry);
  if (maxBlock < vecBytes + FEED_CACHE_HEAP_MARGIN || freeHeap < vecBytes + f.size() + 2 * FEED_CACHE_HEAP_MARGIN) {
    DiagLog::line("OPDS back: cache miss id=%lu lowmem max=%u free=%u", static_cast<unsigned long>(id),
                  static_cast<unsigned>(maxBlock), static_cast<unsigned>(freeHeap));
    return false;
  }
  std::vector<OpdsEntry> loaded;
  loaded.reserve(count);
  for (uint16_t i = 0; r.ok && i < count; i++) {
    uint8_t type = 0;
    OpdsEntry e;
    r.get(&type, sizeof(type));
    // 明列兩種合法值，不靠 enum 的排列順序（codex 第二輪）
    if (type != static_cast<uint8_t>(OpdsEntryType::NAVIGATION) && type != static_cast<uint8_t>(OpdsEntryType::BOOK)) {
      r.ok = false;
    }
    r.str(e.title, FEED_CACHE_MAX_TITLE);
    r.str(e.author, FEED_CACHE_MAX_AUTHOR);
    r.str(e.href, FEED_CACHE_MAX_HREF);
    r.str(e.id, FEED_CACHE_MAX_ID);
    if (!r.ok) break;
    e.type = static_cast<OpdsEntryType>(type);
    loaded.push_back(std::move(e));
  }
  const uint32_t computed = r.crc;
  uint32_t stored = 0;
  r.get(&stored, sizeof(stored));
  if (!r.ok || stored != computed || f.available() != 0) {
    DiagLog::line("OPDS back: cache miss id=%lu body n=%u", static_cast<unsigned long>(id),
                  static_cast<unsigned>(loaded.size()));
    return false;
  }
  const bool prev = (flags & FEED_CACHE_FLAG_PREV) != 0;
  const bool next = (flags & FEED_CACHE_FLAG_NEXT) != 0;
  // 翻頁列一定是導覽項，而且兩列不會是同一列（只有一筆卻兩邊都標就是壞檔）
  if ((prev && loaded.front().type != OpdsEntryType::NAVIGATION) ||
      (next && loaded.back().type != OpdsEntryType::NAVIGATION) || (prev && next && loaded.size() < 2)) {
    DiagLog::line("OPDS back: cache miss id=%lu pagerows", static_cast<unsigned long>(id));
    return false;
  }
  setError({});  // 與 fetchFeed 相同（它在連線前清）；setError 自己拿鎖，所以放在下面的鎖外面
  {
    RenderLock lock;  // 與 fetchFeed 相同：entries、翻頁列旗標、selectorIndex 要對 render task 一起換掉
    entries = std::move(loaded);
    hasPrevPageItem = prev;
    hasNextPageItem = next;
    applyPendingRestoreLocked();
  }
  state = BrowserState::BROWSING;
  requestUpdate();
  DiagLog::line("OPDS back: cache hit id=%lu n=%u ms=%lu max=%u free=%u", static_cast<unsigned long>(id),
                static_cast<unsigned>(count), millis() - t0, static_cast<unsigned>(maxBlock),
                static_cast<unsigned>(freeHeap));
  return true;
}

void OpdsBookBrowserActivity::removeFeedCache(const uint32_t id) {
  if (id == 0) return;
  char path[80];
  feedCachePath(path, sizeof(path), id);
  Storage.remove(path);
}

// 快取資料夾整個刪掉。呼叫端都未持 RenderLock：onEnter，以及 loop() 裡兩個回主畫面的出口。
void OpdsBookBrowserActivity::clearFeedCache() {
  char dir[64];
  feedCacheDir(dir, sizeof(dir));
  Storage.removeDir(dir);  // 資料夾不存在就是回 false，沒關係
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  setStatus(book.title);
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
    setError(tr(STR_DOWNLOAD_FAILED));
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

  navigationHistory.push_back(
      {currentPath, selectorIndex, entries.empty() ? std::string() : entries[selectorIndex].href});
  currentPath = url;  // <-- add this

  state = BrowserState::LOADING;
  setStatus(tr(STR_LOADING));
  releaseEntries();
  selectorIndex = 0;
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    setStatus(tr(STR_LOADING));
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
    setStatus(tr(STR_LOADING));
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    setError(tr(STR_WIFI_CONN_FAILED));
    requestUpdate();
  }
}
