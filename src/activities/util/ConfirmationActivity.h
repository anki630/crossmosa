#pragma once
#include <functional>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "fontIds.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  const int margin = 20;
  // v363：標題與說明改 14px（原本 10px；選項本來就是 14px）。字變高之後，行距 30→8、起點 1/6→1/8 螢幕高，
  //   兩行字（底端＝起點＋76）才不會壓到置中的選項彈窗：最緊的是 X4 橫向（高 480）——起點 60、字底 136，
  //   彈窗頂 Formosa Pro 156／Formosa 151。原本 10px 在 X4 橫向就已經壓到（Pro 2px、Formosa 15px）。
  const int spacing = 8;
  const int fontId = UI_12_FONT_ID;

  std::string safeHeading;
  std::string safeBody;
  OptionPopup confirmPopup;
  int startY = 0;
  int lineHeight = 0;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};