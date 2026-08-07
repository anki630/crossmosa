#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/book24.h"
#include "components/icons/bookmark.h"
#include "components/icons/cover.h"
#include "components/icons/file24.h"
#include "components/icons/folder.h"
#include "components/icons/folder24.h"
#include "components/icons/hotspot.h"
#include "components/icons/image24.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings2.h"
#include "components/icons/text24.h"
#include "components/icons/transfer.h"
#include "components/icons/wifi.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = LyraMetrics::cornerRadius;
constexpr int titleAccentWidth = 6;  // header 標題左側實黑豎條(頁面身分錨,v35 教訓:實心才看得見)
constexpr int titleAccentGap = 8;
// v44/v45 全機選取語言(維護者拍板+實機回饋修正):2px 圓角外框 + 框左緣【內側】6px 實黑豎條。
// 豎條上下各內縮圓角半徑,只覆蓋框左緣的直線段 → 與框完全融合成單一形狀
// (v44 首版豎條在外側,圓角處會凸出兩截超出弧線的短線,實機一眼可見)。
// 文字座標完全不動;墨量比網點色塊少 → 游標移動翻的像素更少更安靜。
// 註腳清單(EpubReaderFootnotesActivity)與選項彈窗(BaseTheme::drawOptionPopup)有複本,改動要同步
// (OPDS 已於 v48 改用共用 drawList,複本消滅)。
constexpr int selectionBarWidth = 6;
constexpr int selectionFrameThickness = 2;
void drawSelectionMarker(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  // v47(實機回饋):豎條畫全高,再用圓角外遮罩把四角超出弧線的像素修掉 → 頂/底剛好貼合弧線
  // 不超過(v45 內縮直線段的做法會在圓角處留缺口)。框最後畫,弧線不被遮罩咬到。
  renderer.fillRect(x, y, selectionBarWidth, h, true);
  renderer.maskRoundedRectOutsideCorners(x, y, w, h, cornerRadius, Color::White);
  renderer.drawRoundedRect(x, y, w, h, selectionFrameThickness, cornerRadius, true);
}
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 32;
constexpr int listIconSize = 24;
constexpr int mainMenuColumns = 2;
int coverWidth = 0;

const uint8_t* iconForName(UIIcon icon, int size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return Folder24Icon;
      case UIIcon::Text:
        return Text24Icon;
      case UIIcon::Image:
        return Image24Icon;
      case UIIcon::Book:
        return Book24Icon;
      case UIIcon::File:
        return File24Icon;
      default:
        return nullptr;
    }
  } else if (size == 32) {
    switch (icon) {
      case UIIcon::Folder:
        return FolderIcon;
      case UIIcon::Book:
        return BookIcon;
      case UIIcon::Recent:
        return RecentIcon;
      case UIIcon::Settings:
        return Settings2Icon;
      case UIIcon::Transfer:
        return TransferIcon;
      case UIIcon::Library:
        return LibraryIcon;
      case UIIcon::Wifi:
        return WifiIcon;
      case UIIcon::Hotspot:
        return HotspotIcon;
      case UIIcon::Bookmark:
        return BookmarkIcon;
      default:
        return nullptr;
    }
  }
  return nullptr;
}
}  // namespace

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4);
    }
  }
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - LyraMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight},
                   showBatteryPercentage);

  int maxTitleWidth = title != nullptr ? renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD) : 0;
  int maxSubtitleWidth =
      subtitle != nullptr ? renderer.getTextWidth(UI_10_FONT_ID, subtitle, EpdFontFamily::REGULAR) : 0;

  // Available space is the distance between the side paddings, and a with side padding between title and subtitle.
  const int availableSpace =
      rect.width - LyraMetrics::values.contentSidePadding * 3 - titleAccentWidth - titleAccentGap;

  if (maxTitleWidth + maxSubtitleWidth > availableSpace) {
    if ((maxTitleWidth > availableSpace / 2) && (maxSubtitleWidth > availableSpace / 2)) {
      // Both are wider then half the space, truncate both.
      maxTitleWidth = availableSpace / 2;
      maxSubtitleWidth = availableSpace / 2;
    } else {
      // Truncate the the longest one
      if (maxTitleWidth > maxSubtitleWidth) {
        maxTitleWidth = availableSpace - maxSubtitleWidth;
      } else {
        maxSubtitleWidth = availableSpace - maxTitleWidth;
      }
    }
  }

  if (title) {
    const int titleY = rect.y + LyraMetrics::values.batteryBarHeight + 3;
    // 頁面身分錨:標題左側實黑豎條(1-bit 下以實心元素補 Large Title 字級的缺席)
    renderer.fillRect(rect.x + LyraMetrics::values.contentSidePadding, titleY, titleAccentWidth,
                      renderer.getLineHeight(UI_12_FONT_ID), true);
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID,
                      rect.x + LyraMetrics::values.contentSidePadding + titleAccentWidth + titleAccentGap, titleY,
                      truncatedTitle.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
  }

  if (subtitle) {
    auto truncatedSubtitle = renderer.truncatedText(UI_10_FONT_ID, subtitle, maxSubtitleWidth, EpdFontFamily::REGULAR);
    int truncatedSubtitleWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedSubtitle.c_str());
    renderer.drawText(UI_10_FONT_ID,
                      rect.x + rect.width - LyraMetrics::values.contentSidePadding - truncatedSubtitleWidth,
                      rect.y + 50, truncatedSubtitle.c_str(), true);
  }
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;
  int rightSpace = LyraMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(UI_10_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(UI_10_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(UI_10_FONT_ID, rect.x + rect.width - LyraMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, label, rect.width - LyraMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;

  // v51(Formosa 頁籤形,維護者拍板):現行分頁=真頁籤——上緣圓角、兩側落到底、
  // 【底邊不畫】且整條分隔線在開口處斷開(與內容相連的頁籤語意);底線標示退役。
  // 游標在分頁列時,籤內再加左緣豎條(全機游標語言);文字恆黑、無任何填色背景(v50 原則)。
  // 分隔線先畫,頁籤開口用 erase 在其上打洞。
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);

  for (const auto& tab : tabs) {
    // v83:量寬與繪字必須同一個 font id——這裡算出的 textWidth 決定頁籤外框寬與下一個
    // 頁籤的起點,兩處分開就會外框與文字對不上。
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, tab.label, EpdFontFamily::REGULAR);
    const int tabW = textWidth + 2 * hPaddingInSelection;

    if (tab.selected) {
      const int tabY = rect.y + 1;
      const int tabH = rect.height - 1;  // 底列 = 分隔線那一行,側邊與分隔線相接
      if (selected) {
        // 游標豎條:頂端沿左上弧線收邊(單角版遮罩;共用 mask 會把底部也圓掉,頁籤底要開口);
        // 底端止於開口線(tabH-2),與 erase 對齊成一直線(否則側線列殘留成 L 形缺口)
        renderer.fillRect(currentX, tabY, selectionBarWidth, tabH - 2, true);
        for (int dy = 0; dy < cornerRadius; dy++) {
          for (int dx = 0; dx < cornerRadius; dx++) {
            const int tx = cornerRadius - 1 - dx;
            const int ty = cornerRadius - 1 - dy;
            if (tx * tx + ty * ty > (cornerRadius - 1) * (cornerRadius - 1)) {
              renderer.drawPixel(currentX + dx, tabY + dy, false);
            }
          }
        }
      }
      // 頁籤外形:上圓角、側邊到底;先畫框,再抹掉底邊內側段(=開口,含分隔線那一行)。
      // 四角版 drawRoundedRect 的側邊只畫「弧到弧」中段,bl/br=false 時不補下段(複查抓到)
      // → 兩側自行補畫到分隔線。
      renderer.drawRoundedRect(currentX, tabY, tabW, tabH, 2, cornerRadius, true, true, false, false, true);
      renderer.fillRect(currentX + 2, tabY + tabH - 2, tabW - 4, 2, false);
      renderer.fillRect(currentX, tabY + cornerRadius, 2, tabH - cornerRadius, true);
      renderer.fillRect(currentX + tabW - 2, tabY + cornerRadius, 2, tabH - cornerRadius, true);
    }

    // v83:頁籤文字 10px → 14px。這是 Lyra 裡最後一處停在 UI_10 的「主層級」文字——
    // v11 把所有清單主文字升到 UI_12(=ubuntu_14)、v63 再升值欄之後就只剩這裡,旁邊的
    // 清單是 14px 而分頁名是 10px。上游 BaseTheme::drawTabBar 用的本來就是 UI_12。
    // y 偏移 6 → 3:UI_12 行框 34,帶高 40(tabBarHeight,刻意不動——它同時是六個畫面
    // drawSubHeader 的帶高)。+3 讓行框佔 [3,37),漢字墨跡落在第 6-34 列(上下 6/5px,
    // 對中文置中);拉丁降部最低到第 36 列,離分隔線(rect.y+39)還有 3px。
    // 寬度已逐字量過(解 ubuntu_14 glyph 表):繁中四頁籤右緣 370px、英文 508px,
    // 直向寬 528px → 英文右邊留 20px,與左內距對稱。⚠️ 目前只有 English 與 繁中 兩種
    // 語言(v26 移除 29 個),故無截斷邏輯;日後加語言或改分類名稱必須重算這條。
    renderer.drawText(UI_12_FONT_ID, currentX + hPaddingInSelection, rect.y + 3, tab.label, true,
                      EpdFontFamily::REGULAR);

    currentX += textWidth + LyraMetrics::values.tabSpacing + 2 * hPaddingInSelection;
  }
}

int LyraTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  return contentHeight / rowHeight;
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  int pageItems = rect.height / rowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY, LyraMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  // Draw selection
  int contentWidth =
      rect.width -
      (totalPages > 1 ? (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    drawSelectionMarker(renderer, rect.x + LyraMetrics::values.contentSidePadding,
                        rect.y + selectedIndex % pageItems * rowHeight,
                        contentWidth - LyraMetrics::values.contentSidePadding * 2, rowHeight);
  }

  int textX = rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - LyraMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2;
  int iconSize;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  int iconY = (rowSubtitle != nullptr) ? 20 : 14;  // 配合放大的列高下移(原 16/10)
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int rowTextWidth = textWidth;

    // Draw name
    int valueWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      // 值欄字級 10px→14px(跟主文字同級 UI_12):實機回報「開」/「關」在 10px 下筆畫模糊、
      // 兩字看起來很像,放大後兩字輪廓分得開。maxListValueWidth 維持 200px 不變——同一個
      // 預算下 14px 文字比 10px 佔位更寬,較長的值(字型名稱等)會更早被截斷,是既有 v11
      // 清單標題放大時就接受過的同類取捨。
      valueText = renderer.truncatedText(UI_12_FONT_ID, valueText.c_str(), maxListValueWidth);
      valueWidth = renderer.getTextWidth(UI_12_FONT_ID, valueText.c_str()) + hPaddingInSelection;
      rowTextWidth -= valueWidth;
    }

    auto itemName = rowTitle(i);
    // 清單主文字用 UI_12（現承載 14px）放大;副標維持 UI_10（10px）。
    auto item = renderer.truncatedText(UI_12_FONT_ID, itemName.c_str(), rowTextWidth);
    renderer.drawText(UI_12_FONT_ID, textX, itemY + 8, item.c_str(), true);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, item.c_str());
      const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
      for (int py = itemY + 8; py < itemY + 8 + lineH; py++)
        for (int px = textX; px < textX + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection,
                          itemY + iconY, iconSize);
      }
    }

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(UI_10_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(UI_10_FONT_ID, textX, itemY + 46, subtitle.c_str(), true);
    }

    // Draw value(v50 Formosa 原則:選中列值欄不再反白黑 pill——列本身已有選取記號,純黑字即可)
    if (!valueText.empty()) {
      (void)highlightValue;
      const int valueY = itemY + 8;  // 值欄現與主文字同為 14px,對齊主文字基線(itemY+8,同上）
      renderer.drawText(UI_12_FONT_ID, rect.x + contentWidth - LyraMetrics::values.contentSidePadding - valueWidth,
                        valueY, valueText.c_str(), true);
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = LyraMetrics::values.buttonHintsHeight;
  constexpr int buttonY = LyraMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // X3 has wider screen in portrait (528 vs 480), use more spacing
  constexpr int x4ButtonPositions[] = {58, 146, 254, 342};
  constexpr int x3ButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = gpio.deviceIsX3() ? x3ButtonPositions : x4ButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Draw the filled background and border for a FULL-sized button
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    } else {
      // Draw the filled background and border for a SMALL-sized button
      renderer.fillRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, cornerRadius,
                               Color::White);
      renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius, true,
                               true, false, false, true);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
      }
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = rect.width - 2 * LyraMetrics::values.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = LyraMetrics::values.homeCoverHeight * 0.6;
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      int tileX = LyraMetrics::values.contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, LyraMetrics::values.homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        // v57:開檔失敗也要退回佔位圖。原本只有 parseHeaders 失敗那條會清 hasCover,
        // 開檔失敗(縮圖還沒產出來——generateThumbBmp 全樹只有 HomeActivity 會呼叫,
        // 所以每本新書第一次回主畫面必然缺檔)則兩者皆無:既不畫封面也不畫 CoverIcon,
        // 只剩一個空的圓角框,而且那個空框還會被當成有效快照鎖住 coverRendered。
        if (!Storage.openFileForRead("HOME", coverBmpPath, file)) {
          hasCover = false;
        } else {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            // v63: coverWidth stays fixed at the placeholder estimate set above -- it used to be
            // overwritten with bitmap.getWidth() here, but generateThumbBmp's crop=true target-fit
            // only guarantees the thumbnail *covers* the nominal box (it picks the larger of the
            // two fit scales and never clips the overshoot dimension back down), so the real
            // bitmap width varies per book's cover aspect ratio -- it does not reliably equal the
            // nominal estimate. That made the placeholder (drawn before a new book's thumbnail
            // exists) visibly resize once the thumbnail was generated. Crop the loaded bitmap into
            // the fixed box instead, exactly like Lyra3CoversTheme already does for its three-cover
            // layout, so the box size never changes once a book's card first renders.
            const float bitmapRatio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
            const float tileRatio =
                static_cast<float>(coverWidth) / static_cast<float>(LyraMetrics::values.homeCoverHeight);
            const float cropX = std::max(0.0f, 1.0f - (tileRatio / bitmapRatio));
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                LyraMetrics::values.homeCoverHeight, cropX);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way(先把方角內容 mask 成圓角,再畫圓角外框;仿 RoundedRaffTheme 封面慣例)
      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + hPaddingInSelection,
                          tileY + hPaddingInSelection + (LyraMetrics::values.homeCoverHeight / 3), coverWidth,
                          2 * LyraMetrics::values.homeCoverHeight / 3, true);
        renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32);
      }
      renderer.maskRoundedRectOutsideCorners(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                             LyraMetrics::values.homeCoverHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                               LyraMetrics::values.homeCoverHeight, 1, cornerRadius, true);

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    bool bookSelected = (selectorIndex == 0);

    int tileX = LyraMetrics::values.contentSidePadding;
    int textWidth = tileWidth - 2 * hPaddingInSelection - LyraMetrics::values.verticalSpacing - coverWidth;

    if (bookSelected) {
      // v44:整卡外框+左豎條(原四片網點包框);封面 bitmap 內縮 8px,2px 框不壓到封面
      drawSelectionMarker(renderer, tileX, tileY, tileWidth, tileHeight);
    }

    // v42(實機回饋):長書名/長作者改折行——書名最多 4 行、作者最多 2 行(原 3 行/單行截斷)。
    // 高度預算:4×34(UI_12 行框)+ 12 + 2×24(UI_10)= 196px ≤ 卡高 242px,置中不溢出。
    auto titleLines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, 4, EpdFontFamily::BOLD);

    // Author line doubles as the progress display: "author (45%)".
    std::string authorLine = book.author;
    if (book.progressPercent > 0) {
      if (!authorLine.empty()) authorLine += " ";
      authorLine += "(" + std::to_string(book.progressPercent) + "%)";
    }
    std::vector<std::string> authorLines;
    if (!authorLine.empty()) {
      authorLines = renderer.wrappedText(UI_10_FONT_ID, authorLine.c_str(), textWidth, 2);
    }
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
    const int authorBlockHeight =
        authorLines.empty() ? 0 : (authorLineHeight / 2 + authorLineHeight * static_cast<int>(authorLines.size()));
    const int totalBlockHeight = titleBlockHeight + authorBlockHeight;
    int titleY = tileY + tileHeight / 2 - totalBlockHeight / 2;
    const int textX = tileX + hPaddingInSelection + coverWidth + LyraMetrics::values.verticalSpacing;
    for (const auto& line : titleLines) {
      renderer.drawText(UI_12_FONT_ID, textX, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += titleLineHeight;
    }
    if (!authorLines.empty()) {
      titleY += authorLineHeight / 2;
      for (const auto& line : authorLines) {
        renderer.drawText(UI_10_FONT_ID, textX, titleY, line.c_str(), true);
        titleY += authorLineHeight;
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void LyraTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = rect.width - LyraMetrics::values.contentSidePadding * 2;
    Rect tileRect = Rect{rect.x + LyraMetrics::values.contentSidePadding,
                         rect.y + i * (LyraMetrics::values.menuRowHeight + LyraMetrics::values.menuSpacing), tileWidth,
                         LyraMetrics::values.menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      drawSelectionMarker(renderer, tileRect.x, tileRect.y, tileRect.width, tileRect.height);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (LyraMetrics::values.menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
      if (iconBitmap != nullptr) {
        renderer.drawIcon(iconBitmap, textX, textY, mainMenuIconSize);
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }
}
