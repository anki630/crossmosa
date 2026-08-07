#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server)
      : Activity("OpdsBookBrowser", renderer, mappedInput), buttonNavigator(), server(std::move(server)) {}

  // v102: keep the core at 160 MHz for the whole OPDS session, the way
  // CrossPointWebServerActivity already does. Everything this activity does is network work
  // (TLS handshake, feed fetch, book download) and the TLS handshake in particular is ECC-heavy
  // on a chip with no ECC acceleration -- the 10 MHz idle clock is a 16x penalty right where it
  // hurts. main.cpp's power-saving branch keys off exactly this, and its own comment already
  // notes "the web server forces full speed via skipLoopDelay above"; OPDS never did.
  bool skipLoopDelay() override { return true; }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  struct HistoryEntry {
    std::string path;
    int selectorIndex = 0;
    std::vector<OpdsEntry> cachedEntries;  // 空 = 未快取（返回時重抓）
    std::string cachedNextPageUrl;
    std::string cachedPrevPageUrl;
  };
  std::vector<HistoryEntry> navigationHistory;
  // 伺服器頁快取【存 SD 堆疊、不佔 RAM】：這趟看過的頁都留，往回/往前跳幾頁都快。像瀏覽器上/下一頁
  // 堆疊——往前=離開頁 push「back 堆疊」、還原「fwd 堆疊」頂；往回相反。用【方向+深度】配對，不靠 URL
  //（OPDS 的 prev/next 連結字串常≠原始頁 URL）。檔名 b0.bin/b1.bin…(back)、f0.bin…(fwd)。
  // 為何存 SD 不存 RAM：此裝置「載入一頁」的連線需 ~40-55KB 連續塊，握著任何一頁在 RAM 就會
  // failed to fetch（v20 實測）；寫 SD 後真釋放 RAM，連線才不失敗，往回再從 SD 讀(不連網、快)。
  int backServerDepth = 0;  // back 堆疊深度（目前頁「後面」有幾頁在 SD）
  int fwdServerDepth = 0;   // fwd 堆疊深度（backed up 後「前面」有幾頁在 SD）
  std::string currentPath;
  std::string searchTemplate;
  std::string nextPageUrl;  // 目前 feed 的伺服器下一頁（絕對 URL，空 = 無）
  std::string prevPageUrl;  // 目前 feed 的伺服器上一頁
  bool consumeConfirm = false;
  bool consumeBack = false;  // Added missing member
  bool navNextActionDone = false;  // 本次長按 NavNext 已做過一次動作（壓制自動重複；只由放開邊緣清）
  bool navPrevActionDone = false;  // 本次長按 NavPrevious 已做過一次動作
  bool backHoldFired = false;        // 長按 Back 已觸發回主畫面，放開時吞掉單點退層
  unsigned long backPressStartMs = 0;  // Back 自己的按下時間（判長按用，避免全域 getHeldTime 誤觸）
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void pushHistoryLevel();  // 把當層(路徑+游標+可選快取)推入 history，供 navigateToEntry/performSearch 共用
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void loadServerPage(const std::string& pageUrl, bool forward);  // 側向載入伺服器頁（方向+深度 SD 堆疊快取；不 push history）
  bool writePageToSd(const char* path);                          // 序列化目前頁(entries+next/prevUrl)到 SD 檔
  bool readPageFromSd(const char* path);                         // 從 SD 檔反序列化回 entries+next/prevUrl
  void releaseCurrentPageRam();                                  // 真釋放目前頁的 RAM（entries swap + URL swap）
  void clearOneStack(bool back);                                 // 刪掉單一方向堆疊的所有 SD 檔並歸零深度
  void clearServerPageCache();                                   // 刪掉 back+fwd 兩個堆疊（換 feed 情境/進出時）
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override { return true; }
};
