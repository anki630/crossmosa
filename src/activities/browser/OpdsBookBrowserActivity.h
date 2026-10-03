#pragma once
#include <OpdsParser.h>

#include <cstdint>
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
  // 【不在 RAM 快取 entries】（v20 教訓：握著任何一頁在 RAM 就 failed to fetch）。
  // v347：改成 v22/v23 的作法 —— 離開一層時把它的清單寫到 SD、RAM 照樣釋放；按「退出」回來先讀 SD，
  // 讀不到才重新連線。配對靠每一層自己的快取編號（身分），不靠堆疊位置（v23 的錯位教訓）。
  struct HistoryLevel {
    // 從這一層是怎麼走到下一層的：按了清單尾的「下一頁」列、清單頭的「上一頁」列，或一般項目。
    // 在下一層按【反方向】的翻頁列＝回到這一層 → 走「退出」（讀快取、游標還原、history 不變長），
    // 不當成再往前走一頁（v17 的方向配對：OPDS 的上一頁／下一頁網址常跟當初載入的網址不同，不能比網址）。
    enum class Via : uint8_t { Other, NextPage, PrevPage };
    std::string path;
    int selectorIndex = 0;
    // v159（codex 複查）：偽項目「上一頁」的有無可能在往返之間改變（feed 變動），
    // 純數字座標會位移 ±1 —— 還原時先按 href 找，找不到才退回夾限的數字座標。
    std::string href;
    uint32_t cacheId = 0;  // v347：這一層清單在 SD 上的快取編號；0＝沒存（存失敗、清單空）
    Via via = Via::Other;
  };
  uint32_t feedCacheSeq = 0;    // v347：配出去的快取編號（只增不減；資料夾每次進書庫清空）
  uint32_t feedCacheNonce = 0;  // v347：這次瀏覽的身分，寫進每個快取檔頭（onEnter 取亂數）
  std::vector<HistoryLevel> navigationHistory;
  // 返回中待還原的游標；feed 載入完成時夾限套用（fetch 是先發後至，不能立刻設）。
  int pendingRestoreIndex = -1;
  std::string pendingRestoreHref;  // v159：優先用 href 定位（見 HistoryLevel::href）
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;      // Added missing member
  bool didUnloadFonts_ = false;  // v185：onEnter 卸了 SD 字型、onExit 沒重啟就得自己重載
  int selectorIndex = 0;
  // v346：清單頭尾是不是書庫翻頁的偽項目（fetchFeed 把「上一頁」插在最前、「下一頁」放在最後）。
  // 記在旗標裡，不靠比對標題文字 —— 書庫自己的項目也可能叫「下一頁」。
  bool hasPrevPageItem = false;
  bool hasNextPageItem = false;
  // v346（v13③ 搬回）：長按「退出」回主畫面。用 Back【自己的】按下時間 —— 全域 getHeldTime()
  // 算的是最早按住的那顆鍵，按住翻頁鍵時再按 Back 會被誤判成長按（memory x3-opds-nav-gotchas）。
  unsigned long backPressStartMs = 0;
  bool backHoldFired = false;
  // v346（v13④／v15 搬回）：停在清單最底（最頂）的翻頁列上再長按下一頁（上一頁）→ 載入書庫的下一頁（上一頁）。
  // AtEdge＝這次按下時游標就在那一列（按住途中才翻到底的不算，要放開再按）；Done＝這次按住已經換過頁，
  // 放開前不再有任何動作。兩者都只在【放開邊緣】清 —— 換頁的連線是同步的，會打斷按鍵的去彈跳，
  // 用 isPressed 判斷會把「一直按著」誤讀成「放開又按」而連續換頁（v14 的「翻不停」）。
  bool nextHoldAtEdge = false;
  bool nextHoldDone = false;
  bool prevHoldAtEdge = false;
  bool prevHoldDone = false;
  // 翻頁鍵【自己的】按下時間：ButtonNavigator 的長按門檻用全域 getHeldTime()，先按住別的鍵再按翻頁鍵
  // 會立刻過門檻（帳本 B7）。換書庫頁這個動作另外要求翻頁鍵本身按滿 NAV_HOLD_MS（codex 複查）。
  unsigned long nextPressStartMs = 0;
  unsigned long prevPressStartMs = 0;
  bool pagedThisLoop = false;  // 這一輪 loop 換過書庫頁（entries 已換掉），後面的按鍵處理要跳過
  // history 超過上限丟過最舊的層：退到剩下的盡頭時先回書庫最上層，而不是直接離開書庫（codex 第二輪）
  bool historyTruncated = false;
  std::string errorMessage;
  std::string errorDetail;  // v101/v158：fetch 失敗的真因（lastError 快照），其他錯誤路徑保持空
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  // v200：render 在另一個 task 讀這些字串（對它們呼叫 .c_str()），主任務重新配置就是
  // 與 entries 同一類的 UAF。集中成 helper，比在 16 個地方各開一個作用域不容易漏。
  // ⚠️ 只能從【未持鎖】的路徑呼叫（onEnter／loop／fetchFeed／result handler 都是，已逐一查證）；
  //    renderingMutex 非遞迴，從持鎖的 onExit 呼叫會永久死鎖。
  void setStatus(std::string v);
  void setError(std::string msg, std::string detail = {});

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry, HistoryLevel::Via via = HistoryLevel::Via::Other);
  void navigateBack();
  void openPageItem(bool next);  // 翻頁列（確認或長按）：能跟上一層配對就退回去，否則往前載入
  void applyPendingRestoreLocked();
  // v347：SD 頁快取（/.crossmosa/opdscache/<編號>.bin）。任何一步失敗都退回原本的重新連線。
  uint32_t saveFeedCache();
  bool loadFeedCache(uint32_t id);
  void removeFeedCache(uint32_t id);
  void clearFeedCache();
  bool handleBackHold();          // 長按「退出」→ 主畫面；true＝已處理，呼叫端直接 return
  bool consumeBackHoldRelease();  // Back 放開：這次是長按的收尾就吞掉（true），否則交給短按
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override;
};
