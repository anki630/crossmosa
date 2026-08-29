#include "FormosaProTheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int hPaddingInSelection = 8;
constexpr int listIconSize = 24;
constexpr int mainMenuIconSize = 32;
constexpr int maxListValueWidth = 200;
constexpr int hairlineRightInset = 16;
constexpr int chevronSize = 5;
constexpr int chevronRightInset = 22;
constexpr int selectionBarX = 6;   // 卡片左緣到豎條
constexpr int selectionBarW = 3;
constexpr int selectionBarVPad = 10;
}  // namespace

void FormosaProTheme::drawChevron(const GfxRenderer& renderer, const int x, const int cy) const {
  // iOS 的「›」：兩段 2px 斜線
  renderer.drawLine(x, cy - chevronSize, x + chevronSize, cy, 2, true);
  renderer.drawLine(x, cy + chevronSize, x + chevronSize, cy, 2, true);
}

// iOS 電池：膠囊外框（同心圓角）＋右側小突起＋依電量填色。
void FormosaProTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const int bodyW = rect.width - 3;
  renderer.drawSmoothRoundedRect(rect.x, rect.y, bodyW, rect.height, 1, 4, N, true);
  renderer.fillRect(rect.x + bodyW + 1, rect.y + rect.height / 2 - 2, 2, 4, true);
  const int innerW = bodyW - 4;
  const int fillW = std::max(0, std::min(innerW, innerW * static_cast<int>(percentage) / 100));
  if (fillW > 0) {
    renderer.fillSmoothRoundedRect(rect.x + 2, rect.y + 2, fillW, rect.height - 4, 2, N, Color::Black);
  }
}

// 狀態列：左時間（有 RTC 才有）、右電池；標題在第二道，粗體、不畫左豎條與底線（留白就是階層）。
void FormosaProTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                                 const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const auto& m = FormosaProMetrics::values;

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - m.batteryWidth;
  drawBatteryRight(renderer, Rect{batteryX, rect.y + 5, m.batteryWidth, m.batteryHeight}, showBatteryPercentage);

  if (halClock.isAvailable()) {
    const auto sb = SETTINGS.statusBarSpec();
    char timeBuf[16];
    if (halClock.formatTime(timeBuf, sizeof(timeBuf), sb.clockUtcOffsetQ, sb.clock12h)) {
      renderer.drawText(UI_12_FONT_ID, rect.x + m.contentSidePadding, rect.y + 1, timeBuf, true, EpdFontFamily::BOLD);
    }
  }

  if (title) {
    const int maxTitleWidth = rect.width - m.contentSidePadding * 2 -
                              (subtitle ? renderer.getTextWidth(UI_10_FONT_ID, subtitle) + 12 : 0);
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    const int titleY = rect.y + m.batteryBarHeight + 3;
    renderer.drawText(UI_12_FONT_ID, rect.x + m.contentSidePadding, titleY, truncatedTitle.c_str(), true,
                      EpdFontFamily::BOLD);
  }
  if (subtitle) {
    const int w = renderer.getTextWidth(UI_10_FONT_ID, subtitle);
    renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - m.contentSidePadding - w, rect.y + 50, subtitle, true);
  }
}

// 副標列（閱讀選單的「章 x/y ・ 書 z%」等）：10px 文字，左標籤右值，不畫整寬底線 —— 留白就是分隔。
void FormosaProTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                                    const char* rightLabel) const {
  const auto& m = FormosaProMetrics::values;
  int rightSpace = m.contentSidePadding;
  if (rightLabel) {
    const auto r = renderer.truncatedText(UI_10_FONT_ID, rightLabel, maxListValueWidth);
    const int rw = renderer.getTextWidth(UI_10_FONT_ID, r.c_str());
    renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - m.contentSidePadding - rw, rect.y + 7, r.c_str());
    rightSpace += rw + hPaddingInSelection;
  }
  const auto l = renderer.truncatedText(UI_10_FONT_ID, label, rect.width - m.contentSidePadding - rightSpace);
  renderer.drawText(UI_10_FONT_ID, rect.x + m.contentSidePadding, rect.y + 6, l.c_str(), true);
}

// 鍵盤輸入欄：底線 → macOS 文字框（1px 連續圓角框；游標模式 2px）。文字已由活動畫好，這裡只描框。
// rect = {0, 第一行 y, 螢幕寬, 已畫行數×行高}；contentStartX／contentWidth = 文字欄的左緣與寬度。
void FormosaProTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                                    int contentStartX, int contentWidth) const {
  const auto& m = FormosaProMetrics::values;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  constexpr int padX = 10;
  constexpr int padY = 6;
  int boxX, boxW;
  if (contentWidth > 0) {
    boxX = rect.x + contentStartX - padX;
    boxW = contentWidth + padX * 2;
  } else {
    const int w = textWidth + m.textFieldHorizontalPadding * 2;
    boxX = rect.x + (rect.width - w) / 2 - padX;
    boxW = w + padX * 2;
  }
  const int boxY = rect.y - padY;
  const int boxH = std::max(rect.height, 0) + lineHeight + padY * 2;
  renderer.drawSmoothRoundedRect(boxX, boxY, boxW, boxH, cursorMode ? 2 : 1, innerR(4), N, true);
}

// segmented control：外框一個膠囊，選中的分段反白。x 佈局與 Lyra 完全相同（tabIndexFromPoint 沿用）。
void FormosaProTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                                 bool selected) const {
  const auto& m = FormosaProMetrics::values;
  int currentX = rect.x + m.contentSidePadding;
  const int segY = rect.y + 4;
  const int segH = rect.height - 8;
  const int segR = innerR(4);
  // 先算總寬畫外框
  int totalW = 0;
  for (size_t i = 0; i < tabs.size(); i++) {
    totalW += renderer.getTextWidth(UI_12_FONT_ID, tabs[i].label, EpdFontFamily::REGULAR) + 2 * hPaddingInSelection;
    if (i + 1 < tabs.size()) totalW += m.tabSpacing;
  }
  renderer.drawSmoothRoundedRect(currentX - 4, segY, totalW + 8, segH, 1, segR, N, true);
  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, tab.label, EpdFontFamily::REGULAR);
    const int tabW = textWidth + 2 * hPaddingInSelection;
    const bool inverted = tab.selected && selected;
    if (tab.selected) {
      if (inverted) {
        renderer.fillSmoothRoundedRect(currentX, segY + 2, tabW, segH - 4, innerR(6), N, Color::Black);
      } else {
        renderer.drawSmoothRoundedRect(currentX, segY + 2, tabW, segH - 4, 1, innerR(6), N, true);
      }
    }
    renderer.drawText(UI_12_FONT_ID, currentX + hPaddingInSelection, rect.y + 3, tab.label, !inverted,
                      EpdFontFamily::REGULAR);
    currentX += tabW + m.tabSpacing;
  }
}

// 分組內縮卡的列：髮絲線、圖示、標題／副標、值、›，選取＝左豎條＋粗體。
void FormosaProTheme::drawGroupedRows(const GfxRenderer& renderer, const int cardX, const int cardY, const int cardW,
                                      const int rowH, const int rowCount, const int selectedRow, const int iconSize,
                                      const std::function<std::string(int)>& title,
                                      const std::function<std::string(int)>& subtitle,
                                      const std::function<UIIcon(int)>& icon,
                                      const std::function<std::string(int)>& value,
                                      const std::function<bool(int)>& dimmed, const int firstIndex,
                                      const bool chevrons, const std::function<bool(int)>& groupBreakBefore) const {
  renderer.drawSmoothRoundedRect(cardX, cardY, cardW, rowH * rowCount, 1, R, N, true);
  const bool hasIcons = static_cast<bool>(icon);
  const int textX = cardX + 16 + (hasIcons ? iconSize + 12 : 0);
  const int chevronX = cardX + cardW - (chevrons ? chevronRightInset : 8);
  const int textYOff = subtitle ? 8 : (rowH - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  for (int r = 0; r < rowCount; r++) {
    const int idx = firstIndex + r;
    const int rowY = cardY + r * rowH;
    if (r > 0) {
      if (groupBreakBefore && groupBreakBefore(idx)) {
        renderer.drawLine(cardX + 1, rowY, cardX + cardW - 2, rowY, true);  // 群組分隔：整寬髮絲線（macOS 選單語彙）
      } else {
        renderer.drawLine(textX, rowY, cardX + cardW - hairlineRightInset, rowY, true);
      }
    }
    const bool isSel = (r == selectedRow);
    if (isSel) {
      renderer.fillRect(cardX + selectionBarX, rowY + selectionBarVPad, selectionBarW, rowH - 2 * selectionBarVPad,
                        true);
    }
    if (hasIcons) {
      const uint8_t* bmp = LyraTheme::iconForName(icon(idx), iconSize);
      if (bmp) renderer.drawIcon(bmp, cardX + 16, rowY + (rowH - iconSize) / 2, iconSize);
    }
    int rowTextW = chevronX - 8 - textX;
    std::string valueText;
    if (value) {
      valueText = renderer.truncatedText(UI_12_FONT_ID, value(idx).c_str(), maxListValueWidth);
      if (!valueText.empty()) {
        const int vw = renderer.getTextWidth(UI_12_FONT_ID, valueText.c_str());
        renderer.drawText(UI_12_FONT_ID, chevronX - 8 - vw, rowY + textYOff, valueText.c_str(), true);
        rowTextW -= vw + 8;
      }
    }
    const std::string t = title(idx);
    const auto shown = renderer.truncatedText(UI_12_FONT_ID, t.c_str(), rowTextW, isSel ? EpdFontFamily::BOLD
                                                                                          : EpdFontFamily::REGULAR);
    renderer.drawText(UI_12_FONT_ID, textX, rowY + textYOff, shown.c_str(), true,
                      isSel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (dimmed && dimmed(idx) && !isSel) {
      const int tw = renderer.getTextWidth(UI_12_FONT_ID, shown.c_str());
      const int lh = renderer.getLineHeight(UI_12_FONT_ID);
      for (int py = rowY + textYOff; py < rowY + textYOff + lh; py++)
        for (int px = textX; px < textX + tw; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }
    if (subtitle) {
      const auto sub = renderer.truncatedText(UI_10_FONT_ID, subtitle(idx).c_str(), rowTextW);
      renderer.drawText(UI_10_FONT_ID, textX, rowY + 46, sub.c_str(), true);
    }
    if (chevrons) drawChevron(renderer, chevronX, rowY + rowH / 2);
  }
}

void FormosaProTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                               const std::function<std::string(int index)>& rowTitle,
                               const std::function<std::string(int index)>& rowSubtitle,
                               const std::function<UIIcon(int index)>& rowIcon,
                               const std::function<std::string(int index)>& rowValue, bool highlightValue,
                               const std::function<bool(int index)>& rowDimmed) const {
  (void)highlightValue;
  const auto& m = FormosaProMetrics::values;
  const int rowHeight = (rowSubtitle != nullptr) ? m.listWithSubtitleRowHeight : m.listRowHeight;
  const int pageItems = rowHeight > 0 ? std::max(1, rect.height / rowHeight) : 1;
  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  // 捲軸與 Lyra 同位置（右緣），卡片讓出同樣寬度 —— 觸控命中幾何（handleListTouch）不變。
  if (totalPages > 1) {
    const int scrollBarHeight = (rect.height * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((rect.height - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - m.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + rect.height, true);
    renderer.fillRect(scrollBarX - m.scrollBarWidth, scrollBarY, m.scrollBarWidth, scrollBarHeight, true);
  }
  const int contentWidth = rect.width - (totalPages > 1 ? (m.scrollBarWidth + m.scrollBarRightOffset) : 1);
  const int pageStart = (selectedIndex < 0 ? 0 : selectedIndex / pageItems) * pageItems;
  const int rows = std::max(0, std::min(pageItems, itemCount - pageStart));
  if (rows == 0) return;
  drawGroupedRows(renderer, rect.x + m.contentSidePadding, rect.y, contentWidth - m.contentSidePadding * 2, rowHeight,
                  rows, selectedIndex >= 0 ? selectedIndex - pageStart : -1, listIconSize, rowTitle, rowSubtitle,
                  rowIcon, rowValue, rowDimmed, pageStart);
}

// 主畫面選單：同一張分組卡（32px 圖示）。列高 menuRowHeight、間距 0 → HomeActivity 的命中幾何不變。
// v180：把卡片撐到可用高度（48–72px 之間）。318 ppi 上 48px 只有 3.8mm，iOS 的 44pt 列是 5.8mm ——
// 更高的列反而更像蘋果；含 OPDS 五項時底下不再留一大塊空白。
int FormosaProTheme::menuRowHeightFor(int availableHeight, int count) const {
  if (count <= 0) return FormosaProMetrics::values.menuRowHeight;
  return std::max(48, std::min(72, availableHeight / count));
}

void FormosaProTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                     const std::function<std::string(int index)>& buttonLabel,
                                     const std::function<UIIcon(int index)>& rowIcon) const {
  const auto& m = FormosaProMetrics::values;
  if (buttonCount <= 0) return;
  drawGroupedRows(renderer, rect.x + m.contentSidePadding, rect.y, rect.width - m.contentSidePadding * 2,
                  menuRowHeightFor(rect.height, buttonCount), buttonCount, selectedIndex, mainMenuIconSize, buttonLabel,
                  nullptr, rowIcon, nullptr, nullptr, 0);
}

// 按鍵提示（維護者回饋 v179）：不畫整顆按鈕 —— 只圓上兩角、兩側直落到螢幕底緣、不封底，
// 像是從實體按鍵延伸上來的舌片。x 位置同 Lyra（X3 實測的四鍵位置）。
void FormosaProTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                      const char* btn4) const {
  if (gpio.hasTouch()) return;
  const GfxRenderer::Orientation orig = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int tabH = 36;
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* pos = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};
  const int y = pageHeight - tabH;
  const int r = innerR(6);
  for (int i = 0; i < 4; i++) {
    if (labels[i] == nullptr || labels[i][0] == '\0') continue;
    const int x = pos[i];
    renderer.fillRect(x, y, buttonWidth, tabH, false);
    for (int j = 0; j < tabH; j++) {
      const int io = renderer.smoothCornerInset(j, r, N, buttonWidth / 2);
      if (io > 0 || j == 0) {
        // 圓角段：這一列的邊界到相鄰列的邊界之間都畫（8-連通）
        const int prev = j > 0 ? renderer.smoothCornerInset(j - 1, r, N, buttonWidth / 2) : io;
        const int nxt = renderer.smoothCornerInset(j + 1, r, N, buttonWidth / 2);
        int runEnd = std::max({io + 1, prev, nxt});
        if (j == 0) runEnd = buttonWidth - io;  // 最上列：整段頂線
        renderer.fillRect(x + io, y + j, runEnd - io, 1, true);
        renderer.fillRect(x + buttonWidth - runEnd, y + j, runEnd - io, 1, true);
      } else {
        renderer.drawPixel(x, y + j, true);
        renderer.drawPixel(x + buttonWidth - 1, y + j, true);
      }
    }
    const int tw = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
    renderer.drawText(UI_10_FONT_ID, x + (buttonWidth - 1 - tw) / 2, y + 7, labels[i]);
  }
  renderer.setOrientation(orig);
}

// 閱讀選單：macOS 右鍵選單 —— 置中的較窄面板、群組整寬分隔線、無 ›、值靠右。
// 列高＝listRowHeight、y 佈局與 drawList 相同 → handleListTouch 的命中幾何不變（橫向 7 列／直向 12 列不變）。
void FormosaProTheme::drawMenuList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                   const std::function<std::string(int index)>& rowTitle,
                                   const std::function<std::string(int index)>& rowValue,
                                   const std::function<bool(int index)>& groupBreakBefore) const {
  const auto& m = FormosaProMetrics::values;
  const int rowHeight = m.listRowHeight;
  const int pageItems = rowHeight > 0 ? std::max(1, rect.height / rowHeight) : 1;
  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollBarHeight = (rect.height * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((rect.height - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - m.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + rect.height, true);
    renderer.fillRect(scrollBarX - m.scrollBarWidth, scrollBarY, m.scrollBarWidth, scrollBarHeight, true);
  }
  const int pageStart = (selectedIndex < 0 ? 0 : selectedIndex / pageItems) * pageItems;
  const int rows = std::max(0, std::min(pageItems, itemCount - pageStart));
  if (rows == 0) return;
  const int panelW = std::min(rect.width - 2 * m.contentSidePadding, 432);
  const int panelX = rect.x + (rect.width - panelW) / 2;
  drawGroupedRows(renderer, panelX, rect.y, panelW, rowHeight, rows, selectedIndex >= 0 ? selectedIndex - pageStart : -1,
                  0, rowTitle, nullptr, nullptr, rowValue, nullptr, pageStart, /*chevrons=*/false, groupBreakBefore);
}

// 單訊息彈窗：白底、1px 連續圓角框、文字置中（macOS alert 的留白）。回傳的 layout 給 fillPopupProgress 用，
// margin 取自 metrics，進度條位置照舊對齊。
Rect FormosaProTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  const auto& m = FormosaProMetrics::values;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = std::min(textWidth + m.popupMarginX * 2, renderer.getScreenWidth() - 2 * m.contentSidePadding);
  const int h = textHeight + m.popupMarginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  const int y = static_cast<int>(renderer.getScreenHeight() * m.popupTopOffsetRatio);
  renderer.fillSmoothRoundedRect(x - 1, y - 1, w + 2, h + 2, R, N, Color::White);
  renderer.drawSmoothRoundedRect(x, y, w, h, m.popupFrameThickness, R, N, true);
  const auto shown = renderer.truncatedText(UI_12_FONT_ID, message, w - 2 * 12);
  const int tw = renderer.getTextWidth(UI_12_FONT_ID, shown.c_str());
  renderer.drawText(UI_12_FONT_ID, x + (w - tw) / 2, y + m.popupMarginY + m.popupTextBaselineOffsetY, shown.c_str(),
                    true);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}

// 選項彈窗：macOS alert 版 —— 粗體標題置中、內縮髮絲線、選項靠左 40px 列、選取＝左豎條＋粗體。
void FormosaProTheme::drawOptionPopup(const GfxRenderer& renderer, const char* title,
                                      const std::vector<std::string>& options, int selectedIndex) const {
  const auto& m = FormosaProMetrics::values;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  constexpr int rowH = 40;
  constexpr int pad = 20;
  const int titleH = renderer.getLineHeight(UI_12_FONT_ID);
  int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
  for (const auto& opt : options) {
    maxTextWidth = std::max(maxTextWidth, renderer.getTextWidth(UI_12_FONT_ID, opt.c_str(), EpdFontFamily::BOLD));
  }
  const int optionCount = static_cast<int>(options.size());
  const int dialogW =
      std::max(260, std::min(maxTextWidth + pad * 2 + 24, pageWidth - 2 * m.optionPopupDialogSideMargin));
  const int dialogH = pad + titleH + 12 + 1 + 8 + rowH * optionCount + pad - 8;
  const int dialogX = (pageWidth - dialogW) / 2;
  const int dialogY = (pageHeight - dialogH) / 2;
  renderer.fillSmoothRoundedRect(dialogX - 1, dialogY - 1, dialogW + 2, dialogH + 2, R, N, Color::White);
  renderer.drawSmoothRoundedRect(dialogX, dialogY, dialogW, dialogH, 1, R, N, true);
  int y = dialogY + pad;
  {
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, dialogX + (dialogW - tw) / 2, y, title, true, EpdFontFamily::BOLD);
  }
  y += titleH + 12;
  renderer.drawLine(dialogX + pad, y, dialogX + dialogW - pad, y, true);
  y += 1 + 8;
  const int textX = dialogX + pad + 8;
  const int textYOff = (rowH - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
  for (int i = 0; i < optionCount; i++) {
    const int rowY = y + i * rowH;
    const bool sel = (i == selectedIndex);
    if (sel) renderer.fillRect(dialogX + 8, rowY + 8, selectionBarW, rowH - 16, true);
    const auto shown = renderer.truncatedText(UI_12_FONT_ID, options[i].c_str(), dialogW - pad * 2 - 8,
                                              sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    renderer.drawText(UI_12_FONT_ID, textX, rowY + textYOff, shown.c_str(), true,
                      sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}

void FormosaProTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * FormosaProMetrics::values.contentSidePadding) / 3;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0;
           i < std::min(static_cast<int>(recentBooks.size()), FormosaProMetrics::values.homeRecentBooksCount); i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        int tileX = FormosaProMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath =
              UITheme::getCoverThumbPath(coverPath, FormosaProMetrics::values.homeCoverHeight);

          // First time: load cover from SD and render
          HalFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              float coverHeight = static_cast<float>(bitmap.getHeight());
              float coverWidth = static_cast<float>(bitmap.getWidth());
              float ratio = coverWidth / coverHeight;
              const float tileRatio = static_cast<float>(tileWidth - 2 * hPaddingInSelection) /
                                      static_cast<float>(FormosaProMetrics::values.homeCoverHeight);
              float cropX = 1.0f - (tileRatio / ratio);

              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection, FormosaProMetrics::values.homeCoverHeight,
                                  cropX);
            } else {
              hasCover = false;
            }
            file.close();
          }
        }
        // Draw either way
        // Pro：封面框連續曲率 R−4（與選取框 R 同心）；先把位元圖角落遮白再描框。
        renderer.maskSmoothRoundedRectOutsideCorners(tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                                     tileWidth - 2 * hPaddingInSelection,
                                                     FormosaProMetrics::values.homeCoverHeight, innerR(4), N,
                                                     Color::White);
        renderer.drawSmoothRoundedRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                       tileWidth - 2 * hPaddingInSelection, FormosaProMetrics::values.homeCoverHeight,
                                       1, innerR(4), N, true);

        if (!hasCover) {
          // v175（使用者的設計稿）：沒有封面（txt、封面缺失、尚未產生）一律畫「書名幾何封面」。
          LyraTheme::drawTitleCoverPlaceholder(
              renderer, tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
              FormosaProMetrics::values.homeCoverHeight,
              LyraTheme::displayTitleFor(recentBooks[i].title, recentBooks[i].path));
        }

        // v168（使用者拍板）：進度徽章畫在封面右下角 —— 白底黑字＋1px 框（蓋在封面上
        // 要可讀；小面積收編例外，同值欄類）。畫在快照之前，跟著 cover buffer 一起保存。
        if (recentBooks[i].progressPercent > 0) {
          const std::string pct = std::to_string(recentBooks[i].progressPercent) + "%";
          const int tw = renderer.getTextWidth(UI_10_FONT_ID, pct.c_str());
          const int bh = renderer.getLineHeight(UI_10_FONT_ID);
          const int bw = tw + 10;
          const int bx = tileX + hPaddingInSelection + (tileWidth - 2 * hPaddingInSelection) - bw - 4;
          const int by = tileY + hPaddingInSelection + FormosaProMetrics::values.homeCoverHeight - bh - 4;
          renderer.fillSmoothRoundedRect(bx, by, bw, bh, innerR(8), N, Color::White);
          renderer.drawSmoothRoundedRect(bx, by, bw, bh, 1, innerR(8), N, true);
          renderer.drawText(UI_10_FONT_ID, bx + 5, by, pct.c_str(), true);
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), FormosaProMetrics::values.homeRecentBooksCount);
         i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = FormosaProMetrics::values.contentSidePadding + tileWidth * i;

      const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;

      // v168（使用者拍板）：百分比改畫在封面右下角小徽章 —— 書名保有完整 3 行，
      // 徽章白底黑字帶 1px 框（蓋在封面上要可讀，屬 v50 原則的小面積收編例外，同值欄類）。
      const std::string displayTitle = LyraTheme::displayTitleFor(recentBooks[i].title, recentBooks[i].path);
      auto titleLines = renderer.wrappedText(UI_10_FONT_ID, displayTitle.c_str(), maxLineWidth, 3);

      const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int dynamicBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight;
      // Add a little padding below the text inside the selection box just like the top padding (5 + hPaddingSelection)
      (void)dynamicBlockHeight;
      // v180（維護者回饋）：選取框高度固定（封面＋3 行書名的位置），不隨書名行數變 —— 三張卡一致。
      const int dynamicTitleBoxHeight = 3 * titleLineHeight + hPaddingInSelection + 5;

      if (bookSelected) {
        // Draw selection box（v44：整卡 2px 圓角外框；三卡相鄰無溝槽，全機語言的左豎條在此省略）
        renderer.drawSmoothRoundedRect(
            tileX, tileY, tileWidth,
            hPaddingInSelection + FormosaProMetrics::values.homeCoverHeight + dynamicTitleBoxHeight, 2, R, N, true);
      }

      int currentY = tileY + FormosaProMetrics::values.homeCoverHeight + hPaddingInSelection + 5;
      for (const auto& line : titleLines) {
        renderer.drawText(UI_10_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
        currentY += titleLineHeight;
      }

    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}
