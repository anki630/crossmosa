#include "TextSettingsPreview.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace textsettings {

namespace {

// Map the paragraph-alignment setting to the engine's CssTextAlign (BOOK_STYLE = justified)
CssTextAlign toCssAlign(uint8_t align) {
  if (align == CrossPointSettings::BOOK_STYLE) return CssTextAlign::Justify;
  return static_cast<CssTextAlign>(align);
}

// Lay the sample text out through the reader engine into layout.lines
bool relayout(PreviewLayout& layout, const GfxRenderer& renderer, int fontId, int textWidth) {
  layout.lines.clear();

  BlockStyle style;
  style.alignment = toCssAlign(SETTINGS.paragraphAlignment);
  style.textAlignDefined = true;  // honor the user's choice; RTL auto-detected from text

  ParsedText parsed(SETTINGS.extraParagraphSpacing != 0, SETTINGS.hyphenationEnabled != 0,
                    SETTINGS.focusReadingEnabled != 0, style);

  // Feed one space-separated word at a time; addWord handles NFC/CJK/RTL/focus splitting
  const char* text = I18N.get(StrId::STR_FONT_PREVIEW_TEXT);
  std::string word;
  for (const char* p = text;; p++) {
    if (*p == ' ' || *p == '\0') {
      if (!word.empty()) {
        parsed.addWord(word, EpdFontFamily::REGULAR);
        word.clear();
      }
      if (*p == '\0') break;
    } else {
      word.push_back(*p);
    }
  }

  parsed.layoutAndExtractLines(
      renderer, fontId, static_cast<uint16_t>(textWidth),
      [&layout](std::shared_ptr<TextBlock> line, uint32_t) { layout.lines.push_back(std::move(line)); });
  // v149：低記憶體時丟過字 -> 回報 false，呼叫端不快取 key（下一輪重試）
  return !parsed.hasOom();
}

}  // namespace

void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, int top,
                   int height, const char* familyName, const char* sizeName) {
  const int left = previewPadding;
  const int width = renderer.getScreenWidth() - (previewPadding * 2);
  if (width <= 0 || height <= 0) return;

  const int labelH = renderer.getTextHeight(UI_10_FONT_ID);
  // v286：正文與標籤之間的留白【不再】沿用通用的 verticalSpacing（16px）。
  // ⚠️ 實機回報：22pt 選「寬」時預覽只畫得出**一行**，下面一大片空白 —— 而預覽的
  //    全部意義就是讓你看見行距，一行看不出任何東西。逐列量測後差距是 **3px**：
  //    第二行的基線落在 251，而限制線是 248。那 3px 是被這 16px 的通用留白吃掉的，
  //    但第二行的**墨水**下緣其實只到 254，離標籤還有 10px —— 留白開得比需要的大。
  //    改成 previewPadding/2；漢字的降部只有約 3px，拉丁最多約 14px，仍不會壓到標籤。
  // ℹ️ 更大的字級（28pt 的「寬」行距 102px）仍然只放得下一行 —— 那是**預覽框本身不夠高**，
  //    而加高預覽框會讓下面的設定清單少一列（CLAUDE.md 硬限制第 7 條），是另一個決定。
  const int textLabelGap = previewPadding / 2;
  (void)labelGap;
  const int labelReserved = labelH + textLabelGap + previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s, %s\"", tr(STR_PREVIEW), familyName, sizeName);
  const int labelY = top + height - previewPadding - labelH;
  renderer.drawText(UI_10_FONT_ID, left, labelY, labelBuf);

  const int fontId = SETTINGS.getReaderFontId();
  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int textLeft = left + SETTINGS.screenMargin;
  const int textWidth = width - 2 * SETTINGS.screenMargin;
  if (textWidth <= 0) return;

  const float pitchEm = SETTINGS.getReaderLinePitchEm();
  const int lineAdvance = std::max(1, renderer.getReaderLineHeight(fontId, pitchEm));
  const int paragraphGap = SETTINGS.extraParagraphSpacing ? lineAdvance / 2 : 0;

  // Re-lay-out (and re-prewarm glyphs) only when a layout-affecting setting or the
  // geometry changed; else reuse the cache. The prewarm inputs are (fontId, constant
  // sample text, styleMask<-focusReading), all of which are key fields, so a matching
  // key means an identical prewarm call. This relies on nothing else evicting the SD
  // glyph cache while this activity is up — true today: the only evictor is
  // FontCacheManager::PrewarmScope, used solely by the reader/dictionary activities.
  const PreviewKey key{.fontId = fontId,
                       .fontPointSize = SETTINGS.fontPointSize,
                       .screenMargin = SETTINGS.screenMargin,
                       .textWidth = textWidth,
                       .lineHeightEm = static_cast<float>(lineAdvance),
                       .alignment = SETTINGS.paragraphAlignment,
                       .extraParagraphSpacing = SETTINGS.extraParagraphSpacing != 0,
                       .focusReading = SETTINGS.focusReadingEnabled != 0,
                       .hyphenation = SETTINGS.hyphenationEnabled != 0,
                       .boldBodyText = SETTINGS.boldBodyText != 0};
  if (key != layout.key) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->prewarmCache(fontId, I18N.get(StrId::STR_FONT_PREVIEW_TEXT), (SETTINGS.focusReadingEnabled || SETTINGS.boldBodyText) ? 0x03 : 0x01);
    }
    // v149：低記憶體時 ParsedText 丟過字 —— 截斷的預覽【不要】以這個 key 快取，
    // 否則同一設定下永遠停在殘缺畫面。不更新 key = 下一輪重試。
    if (!relayout(layout, renderer, fontId, textWidth)) return;
    layout.key = key;
  }

  // Draw the sample twice so the paragraph gap is visible
  int y = top + previewPadding;
  const int textBottomLimit = top + height - labelReserved;
  for (int paragraph = 0; paragraph < 2; paragraph++) {
    for (const auto& line : layout.lines) {
      if (y + lineH > textBottomLimit) return;
      // 預覽的區塊【永遠】是橫排排版路徑產生的 —— 明寫，不要沿用閱讀器留下的旗標。
      const GfxRenderer::VerticalScope horizontal(renderer, false);
      line->render(renderer, fontId, textLeft, y);
      y += lineAdvance;
    }
    y += paragraphGap;
  }
}

}  // namespace textsettings
