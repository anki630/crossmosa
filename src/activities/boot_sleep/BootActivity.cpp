#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/LogoBear240.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int logoX = (pageWidth - LOGO_BEAR_240_SIZE) / 2;
  const int logoY = (pageHeight - LOGO_BEAR_240_SIZE) / 2;
  const int textY = logoY + LOGO_BEAR_240_SIZE + 10;

  // v36: single-pass 1-bit splash (v34/v35's grayscale pipeline added 1-2
  // visible refresh flashes at boot — real-device feedback preferred the
  // plain refresh). drawImageGray in the default BW mode renders every
  // non-white logo pixel black, so the grey design elements degrade to
  // solid black.
  renderer.clearScreen();
  renderer.drawImageGray(LogoBearGray240, logoX, logoY, LOGO_BEAR_240_SIZE, LOGO_BEAR_240_SIZE);
  renderer.drawCenteredText(UI_10_FONT_ID, textY, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, textY + 25, tr(STR_BOOTING));
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);
  renderer.displayBuffer();
}
