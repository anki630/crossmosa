#pragma once
#include <functional>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "fontIds.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  const int margin = 20;
  const int spacing = 30;
  static constexpr int headingFontId = UI_12_FONT_ID;  // 14px:最需要看清楚的畫面用最大的字
  static constexpr int bodyFontId = UI_10_FONT_ID;
  static constexpr int maxBodyLines = 6;

  std::string safeHeading;
  std::vector<std::string> bodyLines;
  int startY = 0;
  int headingLineHeight = 0;
  int bodyLineHeight = 0;
  // 進場吞殘留按鍵:從長按(刪檔/移除)進來時 Confirm 可能仍被按住,放開不能當「確認」
  bool ignoreConfirmRelease = true;
  bool ignoreBackRelease = true;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};