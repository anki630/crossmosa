#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// v119:純文字閱讀器的閱讀選單(公開 repo 的 issue #1)。
//
// 刻意【複製】而不是共用 EpubReaderMenuActivity。理由不是潔癖:那個類別的
// buildMenuItems() 十四項裡只有註腳與書籤有旗標控制,共用就得改它,而它是我們每次
// 跟上游同步都要重解衝突的檔案。複製四十行,換掉一筆長期的維護稅。
//
// 只放【真的接線】的四項。行距與粗體在 txt 是死的(行高只取字型 advanceY,繪製處
// 字重寫死 REGULAR),放進去會「看起來有反應但畫面不變」,比死選項更糟(v27 教訓)。
// 截圖不放:main.cpp 的電源+下鍵組合鍵已經是全域的,再放一個是冗餘。
//
// 字級與螢幕方向在 v118 之前不敢放 —— 它們會讓整本索引作廢、重建要 73 分鐘而且
// 進度會漂掉。串流之後兩者都只是「用同一個位元組位移重排當前頁」,約一秒。
class TxtReaderMenuActivity final : public Activity {
 public:
  enum class MenuAction { GO_TO_PERCENT, FONT_SIZE, ROTATE_SCREEN };

  explicit TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                 float bookProgressPercent, uint8_t currentOrientation);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  static std::vector<MenuItem> buildMenuItems();

  const std::vector<MenuItem> menuItems;
  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  std::string title;
  uint8_t pendingOrientation = 0;
  uint8_t pendingFontSize = 0;
  float bookProgressPercent = 0.0f;

  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                                StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
  const std::vector<StrId> fontSizeLabels = {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE,
                                             StrId::STR_X_LARGE};
};
