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

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  // v13/v156：history 記「路徑＋游標」。返回時還原游標 —— 使用者從第 47 本進了資料夾
  // 再退出來，游標回到第 47 本，而不是每次都被丟回頂端重捲。
  // 【刻意不快取 entries】（v20 教訓：握著任何一頁在 RAM 就 failed to fetch；
  // v22 的 SD 頁堆疊是完整解，帳本另列）—— 返回時重抓，只是游標不再歸零。
  struct HistoryLevel {
    std::string path;
    int selectorIndex = 0;
    // v159（codex 複查）：偽項目「上一頁」的有無可能在往返之間改變（feed 變動），
    // 純數字座標會位移 ±1 —— 還原時先按 href 找，找不到才退回夾限的數字座標。
    std::string href;
  };
  std::vector<HistoryLevel> navigationHistory;
  // 返回中待還原的游標；feed 載入完成時夾限套用（fetch 是先發後至，不能立刻設）。
  int pendingRestoreIndex = -1;
  std::string pendingRestoreHref;  // v159：優先用 href 定位（見 HistoryLevel::href）
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;  // Added missing member
  bool didUnloadFonts_ = false;  // v185：onEnter 卸了 SD 字型、onExit 沒重啟就得自己重載
  int selectorIndex = 0;
  std::string errorMessage;
  std::string errorDetail;  // v101/v158：fetch 失敗的真因（lastError 快照），其他錯誤路徑保持空
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override { return true; }
};
