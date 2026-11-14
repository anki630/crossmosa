#include "AnkiFonts.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "fontIds.h"

namespace anki {

bool CardFonts::available() {
  sdFontSystem.refreshIfDirty();  // 剛用網頁傳上來的字型也算
  return sdFontSystem.registry().findFamily(kCardFontFamily) != nullptr;
}

bool CardFonts::load(GfxRenderer& renderer) {
  if (loaded_) return dedicated_;
  loaded_ = true;
  sdFontSystem.refreshIfDirty();
  const auto* family = sdFontSystem.registry().findFamily(kCardFontFamily);
  if (family) {
    sdFontSystem.unloadForLowMemory(renderer);  // 先卸閱讀字型（見標頭的 ⚠️）
    if (manager_.loadFamily(*family, renderer, kBodyPt)) {
      body_ = manager_.getFontId(kCardFontFamily);
      head_ = manager_.loadFamilyExtraSize(*family, renderer, kHeadPt);
      if (head_ == 0) head_ = body_;
      // 牌組名稱等 UI 字：內建 UI 字型沒有的字（日文假名、韓文）借卡面字型畫
      renderer.setFallbackFont(UI_10_FONT_ID, body_);
      renderer.setFallbackFont(UI_12_FONT_ID, body_);
      dedicated_ = true;
      LOG_INF("ANKI", "Card font %s loaded body=%d head=%d", kCardFontFamily, body_, head_);
      return true;
    }
    LOG_ERR("ANKI", "Failed to load %s, falling back to reader font", kCardFontFamily);
  }
  sdFontSystem.ensureLoaded(renderer);
  body_ = SETTINGS.getReaderFontId();
  head_ = body_;
  dedicated_ = false;
  return false;
}

void CardFonts::unload(GfxRenderer& renderer) {
  if (!loaded_) return;
  loaded_ = false;
  if (dedicated_) {
    manager_.unloadAll(renderer);  // 連 UI 備援一起清
    sdFontSystem.ensureLoaded(renderer);  // 閱讀字型＋UI 備援裝回去
    dedicated_ = false;
  }
  body_ = head_ = 0;
}

}  // namespace anki
