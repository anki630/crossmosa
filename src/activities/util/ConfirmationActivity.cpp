#include "ConfirmationActivity.h"

#include <I18n.h>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  headingLineHeight = renderer.getLineHeight(headingFontId);
  bodyLineHeight = renderer.getLineHeight(bodyFontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(headingFontId, heading.c_str(), maxWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    // 多行:長檔名/書名完整顯示(原單行截斷會把「要刪的是哪個檔」吃掉)
    bodyLines = renderer.wrappedText(bodyFontId, body.c_str(), maxWidth, maxBodyLines);
  }

  int totalHeight = 0;
  if (!safeHeading.empty()) totalHeight += headingLineHeight;
  if (!bodyLines.empty()) totalHeight += static_cast<int>(bodyLines.size()) * bodyLineHeight;
  if (!safeHeading.empty() && !bodyLines.empty()) totalHeight += spacing;

  startY = (renderer.getScreenHeight() - totalHeight) / 2;

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  // Draw Heading
  if (!safeHeading.empty()) {
    renderer.drawCenteredText(headingFontId, currentY, safeHeading.c_str(), true, EpdFontFamily::BOLD);
    currentY += headingLineHeight + spacing;
  }

  // Draw Body (multi-line)
  for (const auto& line : bodyLines) {
    renderer.drawCenteredText(bodyFontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    currentY += bodyLineHeight;
  }

  // Draw UI Elements(Confirm/Back = 肯定/否定;左右鍵仍可用但不標——v45 實機回饋:
  // 四格全標會出現兩組「取消/確認」,視覺冗餘)
  const auto labels = mappedInput.mapLabels(I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  // 吞掉進場前就按住的 Confirm/Back(例:長按刪檔進來,放開那一下不能當「確認」;idiom 同 IntervalSelectionActivity)
  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) ignoreConfirmRelease = false;
  }
  if (ignoreBackRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ignoreBackRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) ignoreBackRelease = false;
  }

  const bool confirmed = mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                         mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const bool cancelled = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                         mappedInput.wasReleased(MappedInputManager::Button::Back);

  if (confirmed) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (cancelled) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
  }
}