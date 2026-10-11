#pragma once

// Formosa Cover 首頁（2026-10-07；v367 減法版 2026-10-08）。規格：工作區 docs/specs/2026-10-07-formosa-cover-theme.md
// §2.1；
//   像素級畫面 work/home-cover/covers/v2-home-x3-192.png（X3）／v2-home-x4-192.png（X4）。由上到下（v385 起）：
//   ⓪ 書架列 ① 正在閱讀卡（封面 160×240＋書名／作者＋今天／這本＋進度）② 最近閱讀 3
//   本（128×192＋細進度條，不寫書名：封面上就有） ③ 工具列（傳輸／OPDS／設定），貼在按鍵提示上方；② 在 ① 與 ③
//   之間置中。預設選取 ①，往上一格就是書架。 按鍵（v367，維護者：手指固定在一組鍵就走得遍）：⑦⑧ 與 ①④ 同義 ——
//   短按＝照閱讀順序上一格／下一格（走到底繞回第一格），
//   長按＝上一區／下一區（同設定頁的長按換分類；計時是每組鍵自己的，見 HoldRepeat）。⑥ 開啟、⑥ 長按 1
//   秒（書本上）＝書本選單（最愛／從最近閱讀移除）、⑤ 繼續閱讀最近那本。
//   移動選取只擦舊框、畫新框（不重讀封面）；其他所有 render 整頁重畫。

#include <atomic>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/Favorites.h"
#include "util/HoldRepeat.h"

class CoverHomeActivity final : public Activity {
 public:
  explicit CoverHomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             HomeMenuItem initialMenuItem = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem_(initialMenuItem) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

  enum class Zone : uint8_t { Card, Recent, Shelf, Tools };

 private:
  struct Focus {
    Zone zone = Zone::Card;
    int index = 0;
    bool operator==(const Focus& o) const { return zone == o.zone && index == o.index; }
  };
  enum class Tool : uint8_t { Transfer, Opds, Settings };

  void loadRecents();
  bool zoneAvailable(Zone z) const;
  int zoneSize(Zone z) const;
  void moveFocus(const Focus& f);
  void step(int dir);      // 短按：照閱讀順序走一格，繞圈
  void jumpZone(int dir);  // 長按：跳到上一區／下一區的第一格，繞圈
  void activate();
  void openBookMenu(int recentIndex);
  void toggleFavoriteLocked();  // 呼叫端持繪製鎖
  void removeFromRecents();     // 自己拿鎖（SD 在鎖外）
  std::vector<RecentBook> collectRecents() const;
  int toolCount() const { return hasOpds_ ? 3 : 2; }
  Tool toolAt(int i) const;
  // 版面（全部由螢幕寬高與字型行高算出來；render 與局部重畫共用）
  void focusRect(const Focus& f, int& x, int& y, int& w, int& h) const;
  int recentTop() const;
  void drawFocus(const Focus& f, bool on) const;
  void drawShelfRow(bool inverted) const;
  void drawToolCell(int i, bool inverted) const;
  int toolsTop() const;

  const HomeMenuItem initialMenuItem_;
  std::vector<RecentBook> recents_;                                // [0]＝正在閱讀，[1..3]＝最近閱讀
  favorites::FavoriteSet favorites_;                               // 只在首頁期間載入（星號、書本選單）
  favorites::LoadResult favLoad_ = favorites::LoadResult::Absent;  // Newer＝這次唯讀
  bool hasOpds_ = false;
  Focus focus_;
  HoldRepeat navNext_;            // ⑧④：短按下一格、長按下一區
  HoldRepeat navPrev_;            // ⑦①：短按上一格、長按上一區
  OptionPopup menu_;              // 書本選單（長按 ⑥）：蓋在首頁上，關掉之後整頁重畫
  std::string menuPath_;          // 選單是哪一本書的
  int menuChoice_ = -1;           // 回呼記下的選項（-1＝取消）
  bool menuCanToggle_ = false;    // 第一項是不是真的能加／移最愛（已滿、唯讀時不行）
  Favorites::SaveInfo saveInfo_;  // 鎖裡存檔的結果，放開鎖之後才寫 log
  bool backPressSeen_ = false;
  bool confirmPressSeen_ = false;
  bool longPressFired_ = false;
  std::atomic<bool> partialOk_{false};
  uint32_t drawnEpoch_ = 0;          // render 任務：上一次整頁畫的時候的螢幕世代
  unsigned long confirmDownAt_ = 0;  // 確認鍵按下的時間（長按判斷）
  bool drawn_ = false;               // render 任務：整頁畫過了
  Focus drawnFocus_;                 // render 任務：上一次畫選取的位置
  bool thumbsChecked_ = false;
};
