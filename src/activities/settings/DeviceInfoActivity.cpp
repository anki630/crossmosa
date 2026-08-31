#include "DeviceInfoActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DeviceInfo.h"

void DeviceInfoActivity::onEnter() {
  Activity::onEnter();
  controllerName = displayControllerName();
  serialOk = deviceSerial(serialNumber, sizeof(serialNumber));
  requestUpdate();
}

void DeviceInfoActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void DeviceInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DEVICE_INFO));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_DISPLAY_CHIP), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, x, y, controllerName);
  y += lineHeight + metrics.verticalSpacing * 2;

  renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_SERIAL_NUMBER), true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;
  // 維護者拍板：顯示完整 SN；讀不到就顯示占位，不寫進 diag.log。
  renderer.drawText(UI_10_FONT_ID, x, y, serialOk ? serialNumber : tr(STR_SERIAL_UNAVAILABLE));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
