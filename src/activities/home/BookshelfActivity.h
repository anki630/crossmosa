#pragma once

// Formosa Cover 書架（2026-10-07；v367 2026-10-08）：卡上所有的書，攤平、3×2、封面（Kindle 模式）。取代 Formosa Cover
// 主題下的「瀏覽檔案」。
//   規格：工作區 docs/specs/2026-10-07-formosa-cover-theme.md §2.2；像素級畫面
//   work/home-cover/covers/v2-shelf-{x3,x4}.png。
//   上方分頁「最愛｜全部｜資料夾」（v369：有最愛就從最愛開始；卡上沒有最愛就不顯示最愛分頁）：
//   全部＝卡上所有的書；最愛＝我的最愛；資料夾＝原本的瀏覽檔案（看圖、刪檔都在那裡）。
//   v367 減法：每本封面下不寫書名（封面上就有），改成頁碼上方一行「選到的那本」；不寫「N 本」、不寫百分比（留細條）。
//   按鍵：⑦⑧ 與 ①④ 同義 —— 短按＝上一本／下一本（照閱讀順序，到頁尾自動翻頁；分頁列也是一格，走到底繞回分頁列），
//   長按＝上一頁／下一頁（到頭停住，不繞）；選取在分頁列時長按＝往右／往左換分頁（繞圈，v372）。分頁列上
//   ⑥＝換到下一個分頁；書上 ⑥＝開書、長按 1 秒＝書本選單。
//   進場（v373）：從首頁／資料夾進來停在分頁列；從閱讀器回來停在剛讀的那本（placeInitialSelection）。
//   ⑤（v368）：在書上＝跳到分頁列（畫面停在這一頁，⑧ 回到這一頁的第一本）；在分頁列＝回首頁。任何位置換分頁＝⑤ ⑥。

#include <atomic>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "ShelfIndex.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/Favorites.h"
#include "util/HoldRepeat.h"

class BookshelfActivity final : public Activity {
 public:
  explicit BookshelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string returnPath = {},
                             int initialTab = -1)
      : Activity("Bookshelf", renderer, mappedInput), returnPath_(std::move(returnPath)), initialTab_(initialTab) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  static constexpr int kCols = 3;
  static constexpr int kRows = 2;
  static constexpr int kPerPage = kCols * kRows;
  static constexpr int kTabAll = 0;   // 「全部」
  static constexpr int kTabFav = 1;   // 「最愛」（「資料夾」是另一個畫面：瀏覽檔案）
  static constexpr int kOnTabs = -1;  // selected_ 的特殊值：選取在分頁列
  // 瀏覽檔案（「資料夾」分頁）畫同一條分頁列用：上一次書架有沒有「最愛」分頁（v374；只在 RAM）
  static inline bool sHasFavTab = false;
  // 從書架換到「資料夾」分頁：瀏覽檔案進場時選取停在分頁列（v376，維護者：換分頁時選取一直在分頁列；只用一次就清掉）
  static inline bool sFolderOnTabs = false;

 private:
  void scan();
  void resort();
  int pageCount() const;
  void moveSelection(int newIndex);
  void step(int dir);
  void jumpPage(int dir);
  void openBookMenu(int index);
  void toggleFavoriteLocked();  // 呼叫端持繪製鎖
  void resortLocked();          // 呼叫端持繪製鎖
  void removeFromRecents();     // 自己拿鎖（SD 在鎖外）
  void nextTab();
  void switchTab(int dir);
  void longOnTabs(int dir);
  std::string thumbPathFor(const char* path, const RecentBook* recent) const;
  const RecentBook* recentFor(const char* path) const;
  std::string titleOf(int index) const;
  void tileRect(int slot, int& x, int& y, int& w, int& h) const;
  int gridTop() const;
  int titleLineY() const;
  void drawTabs(bool focused) const;
  void drawTitleLine(int sel) const;
  void drawHints(bool onTabs) const;

  void placeInitialSelection();

  const std::string returnPath_;  // 從閱讀器回來：剛讀的那本（空＝從首頁或資料夾進來）
  const int initialTab_;          // 從「資料夾」分頁換過來：要停的分頁（−1＝照預設）
  ShelfIndex index_;
  std::vector<RecentBook> recents_;                                // 最近閱讀（≤10），排序與進度用
  favorites::FavoriteSet favorites_;                               // 只在書架期間載入
  favorites::LoadResult favLoad_ = favorites::LoadResult::Absent;  // Newer＝這次唯讀
  int tab_ = kTabAll;
  bool hasFavTab_ = false;        // 顯示「最愛」分頁嗎（卡上找得到最愛才顯示；v369）
  int selected_ = 0;              // 目前檢視裡的第幾本；kOnTabs＝分頁列
  HoldRepeat navNext_;            // ⑧④：短按下一本、長按下一頁
  HoldRepeat navPrev_;            // ⑦①：短按上一本、長按上一頁
  OptionPopup menu_;              // 書本選單（長按 ⑥）：蓋在書架上，關掉之後整頁重畫
  std::string menuPath_;          // 選單是哪一本書的
  int menuChoice_ = -1;           // 回呼記下的選項（-1＝取消）
  bool menuCanToggle_ = false;    // 第一項是不是真的能加／移最愛（已滿、唯讀時不行）
  Favorites::SaveInfo saveInfo_;  // 鎖裡存檔的結果，放開鎖之後才寫 log
  bool backPressSeen_ = false;
  bool confirmPressSeen_ = false;
  bool longPressFired_ = false;
  // 局部重畫：只有「這一頁內移動選取」那一次 render 可以只擦舊框、畫新框（螢幕上的畫面就是快取，不重讀封面）。
  //   其他所有 render（進場、翻頁、從選單回來、淺睡眠醒來重畫）一律整頁重畫 —— 那時螢幕上可能是彈窗或桌布。
  std::atomic<bool> partialOk_{false};
  bool tabHoldUsed_ = false;         // 這次按住已經換過分頁了（分頁列上長按不連發）
  bool tabsByBack_ = false;          // 選取是用 ⑤ 跳上分頁列的（不是繞圈繞上來的）
  std::atomic<int> shownPage_{0};    // 選取在分頁列時畫面停在哪一頁（主任務寫、render 讀；v368）
  std::atomic<int> drawnPage_{-1};   // render 任務：上一次整頁畫的是哪一頁（-1＝要整頁重畫）；moveSelection 在主任務讀
  int drawnSel_ = -1;                // render 任務：上一次畫選取的位置（可以是 kOnTabs）
  uint32_t drawnEpoch_ = 0;          // render 任務：上一次整頁畫的時候的螢幕世代（ActivityManager::screenEpoch）
  unsigned long confirmDownAt_ = 0;  // 確認鍵按下的時間（長按判斷）
  uint32_t scanMs_ = 0;
  uint32_t scanVisited_ = 0;
  bool scanTruncated_ = false;  // 掃描碰到上限（項目數／路徑長／深度／記憶體）提早結束：頁碼列提示「只顯示前 N 本」
};
