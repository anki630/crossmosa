#pragma once

// 單字卡：牌組清單。列出 SD 卡 /anki/ 的 .xdk，每副顯示今天的「新卡／學習中／到期複習」張數
// （只讀檔頭，不掃描進度）。
// 按鍵：左右（或側鍵）選牌組、確認開始複習、長按確認＝牌組設定（每日新卡數、每日複習上限）、返回＝主畫面。

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "anki/AnkiDeck.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class AnkiDeckListActivity final : public Activity {
 public:
  explicit AnkiDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string selectPath = {})
      : Activity("AnkiDecks", renderer, mappedInput), selectPath_(std::move(selectPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Option : uint8_t { None, NewPerDay, MaxReviews };

  void load();
  void openDeck();
  void openOptions();
  void openValuePicker(Option which);
  void applyValue(Option which, uint16_t value);
  std::string subtitleFor(const anki::DeckInfo& d) const;

  std::string selectPath_;
  std::vector<anki::DeckInfo> decks_;
  int selected_ = 0;
  uint32_t today_ = 0;
  bool clockTrusted_ = false;
  bool hasCardFont_ = false;
  ButtonNavigator nav_;
  OptionPopup popup_;
  Option pendingOption_ = Option::None;  // 第一層選單選了哪一項（回呼裡設）
  int pendingValue_ = -1;                // 第二層選了第幾個值（回呼裡設）
  Option pickerFor_ = Option::None;      // 第二層選單是在改哪一項
  bool confirmPressSeen_ = false;
  bool longPressFired_ = false;
  unsigned long confirmDownAt_ = 0;
};
