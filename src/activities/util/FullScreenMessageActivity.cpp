#include "FullScreenMessageActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  const auto mainHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const auto hintHeight = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int gap = 16;
  const auto top = (renderer.getScreenHeight() - mainHeight - gap - hintHeight) / 2;

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, top, text.c_str(), true, style);
  renderer.drawCenteredText(UI_10_FONT_ID, top + mainHeight + gap, tr(STR_PRESS_ANY_KEY_RESTART));
  renderer.displayBuffer(refreshMode);
}

void FullScreenMessageActivity::loop() {
  // 唯一使用場景 = main.cpp SD 卡初始化失敗:原本不吃任何按鍵(只能硬關機),改任意前面板鍵重開機再試。
  using Btn = MappedInputManager::Button;
  for (const Btn b : {Btn::Confirm, Btn::Back, Btn::Left, Btn::Right, Btn::Up, Btn::Down}) {
    if (mappedInput.wasReleased(b)) {
      ESP.restart();
    }
  }
}
