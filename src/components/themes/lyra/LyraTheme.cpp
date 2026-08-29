#include "LyraTheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
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
constexpr int cornerRadius = 6;
constexpr int titleAccentWidth = 6;  // header 標題左側實黑豎條（頁面身分錨，v35 教訓：實心才看得見）
constexpr int titleAccentGap = 8;
// v44/v45 全機選取語言（維護者拍板＋實機回饋修正）：2px 圓角外框 + 框左緣【內側】6px 實黑豎條。
// 豎條上下各內縮圓角半徑，只覆蓋框左緣的直線段 → 與框融合成單一形狀
// （v44 首版豎條在外側，圓角處會凸出兩截短線，實機一眼可見）。
// 文字座標完全不動；墨量比網點色塊少 → 游標移動翻的像素更少更安靜。
constexpr int selectionBarWidth = 6;
constexpr int selectionFrameThickness = 2;
void drawSelectionMarker(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  // v47（實機回饋）：豎條畫全高，再用圓角外遮罩把四角超出弧線的像素修掉 → 頂/底貼合弧線。
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

}  // namespace

const uint8_t* LyraTheme::iconForName(UIIcon icon, int size) {
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
      // v168（使用者回報）：最近閱讀（72px 列）用 32px 圖示，txt 走 Text 卻沒有 32px
      // 資產 → nullptr → 整格空白。在閱讀器的語境裡 txt 就是書 —— 回退到書本圖示。
      case UIIcon::Text:
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

namespace {
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
    auto truncatedTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);
    const int titleY = rect.y + LyraMetrics::values.batteryBarHeight + 3;
    // 頁面身分錨：標題左側實黑豎條（1-bit 下以實心元素補 Large Title 字級的缺席，v35）
    renderer.fillRect(rect.x + LyraMetrics::values.contentSidePadding, titleY, titleAccentWidth,
                      renderer.getLineHeight(UI_12_FONT_ID), true);
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

void LyraTheme::drawEmptyCoverPlaceholder(GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  constexpr int kSpineInset = 12;     // 書脊線距左緣
  constexpr int kSpineEndInset = 10;  // 上下留白;必須 > cornerRadius(6),否則線頭會被圓角遮罩咬掉
  constexpr int kIconSize = 32;

  // 太窄或太矮就不畫書脊(三卡版面的封面框比較窄),圖示仍照常置中。
  if (h > 2 * kSpineEndInset && w > kSpineInset + kIconSize) {
    renderer.fillRect(x + kSpineInset, y + kSpineEndInset, 1, h - 2 * kSpineEndInset, true);
    // 置中於書脊【右側】的版面區,而不是整個框 —— 對整個框置中會看起來偏左。
    renderer.drawIcon(CoverIcon, x + kSpineInset + (w - kSpineInset - kIconSize) / 2, y + (h - kIconSize) / 2,
                      kIconSize);
    return;
  }
  renderer.drawIcon(CoverIcon, x + (w - kIconSize) / 2, y + (h - kIconSize) / 2, kIconSize);
}

namespace {
// 剪裁到矩形內的四分之一圓弧（中點圓演算法、1px）。quadrant：0=右下、1=左下、2=左上、3=右上
// （相對圓心）。不用 GfxRenderer::drawArc —— 它不剪裁，弧會畫到隔壁卡片上。
void drawClippedQuarterArc(GfxRenderer& renderer, const int cx, const int cy, const int r, const int quadrant,
                           const int clipX, const int clipY, const int clipW, const int clipH) {
  auto plot = [&](const int dx, const int dy) {
    const int px = cx + dx;
    const int py = cy + dy;
    if (px < clipX || px >= clipX + clipW || py < clipY || py >= clipY + clipH) return;
    renderer.drawPixel(px, py, true);
  };
  const int sx = (quadrant == 0 || quadrant == 3) ? 1 : -1;
  const int sy = (quadrant == 0 || quadrant == 1) ? 1 : -1;
  int x = r;
  int y = 0;
  int err = 1 - r;
  while (x >= y) {
    plot(sx * x, sy * y);
    plot(sx * y, sy * x);
    y++;
    if (err < 0) {
      err += 2 * y + 1;
    } else {
      x--;
      err += 2 * (y - x) + 1;
    }
  }
}

// 點陣格（裝飾）：pitch 像素一點。
void drawDotGrid(GfxRenderer& renderer, const int x0, const int y0, const int cols, const int rows, const int pitch,
                 const int clipX, const int clipY, const int clipW, const int clipH) {
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const int px = x0 + c * pitch;
      const int py = y0 + r * pitch;
      if (px < clipX || px >= clipX + clipW || py < clipY || py >= clipY + clipH) continue;
      renderer.drawPixel(px, py, true);
    }
  }
}
}  // namespace

// v174/v175（使用者的設計稿 Empty_cover_design.PNG）：沒有封面的書（txt、封面缺失或尚未產生）
// → 幾何線條封面：左側書脊線、角落四分之一圓弧、點陣格當裝飾，書名粗體【靠左】排在中段；
// 檔名在「_」處優先斷行（「_」跟著下一行開頭，同設計稿）。全部是線、點與字（C-33 不塗底）。
// 裝飾的擺法由書名雜湊決定三種變體，同一本書永遠一樣、不同書略有差異（同設計稿的三個範例）。
void LyraTheme::drawTitleCoverPlaceholder(GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                                          const std::string& title) {
  constexpr int kSpineInset = 12;
  constexpr int kSpineEndInset = 8;
  constexpr int kTextPad = 8;
  const bool drawSpine = h > 2 * kSpineEndInset && w > kSpineInset + 40;
  if (drawSpine) renderer.fillRect(x + kSpineInset, y + kSpineEndInset, 1, h - 2 * kSpineEndInset, true);
  const int innerX = x + (drawSpine ? kSpineInset + 1 : 0);
  const int innerW = x + w - innerX;

  // --- 裝飾：三種變體 ---
  uint32_t hash = 2166136261u;
  for (const unsigned char ch : title) hash = (hash ^ ch) * 16777619u;
  const int variant = static_cast<int>(hash % 3u);
  const int bigR = innerW * 3 / 5;
  const int smallR = innerW * 3 / 10;
  if (variant == 0) {
    drawClippedQuarterArc(renderer, x + w - 1, y, bigR, 1, innerX, y, innerW, h);                // 右上角，弧朝左下
    drawClippedQuarterArc(renderer, innerX, y + h - 1, smallR, 3, innerX, y, innerW, h);         // 左下角，弧朝右上
    drawDotGrid(renderer, x + w - 6 - 4 * 6, y + bigR + 8, 5, 4, 6, innerX, y, innerW, h);
  } else if (variant == 1) {
    drawClippedQuarterArc(renderer, innerX, y, bigR, 0, innerX, y, innerW, h);                   // 左上角，弧朝右下
    drawClippedQuarterArc(renderer, x + w - 1, y + h - 1, smallR, 2, innerX, y, innerW, h);      // 右下角，弧朝左上
    drawDotGrid(renderer, x + w - 6 - 4 * 6, y + 8, 5, 4, 6, innerX, y, innerW, h);
  } else {
    drawClippedQuarterArc(renderer, x + w - 1, y + h - 1, bigR, 2, innerX, y, innerW, h);        // 右下角，弧朝左上
    drawClippedQuarterArc(renderer, innerX, y, smallR, 0, innerX, y, innerW, h);                 // 左上角，弧朝右下
    drawDotGrid(renderer, x + w - 6 - 4 * 6, y + 8, 5, 4, 6, innerX, y, innerW, h);
  }

  // --- 書名：靠左、粗體、「_」優先斷行、再依寬度折行 ---
  const int textX0 = innerX + kTextPad;
  const int textW = x + w - kTextPad - textX0;
  if (textW < 16) return;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int maxLines = std::max(1, std::min(6, (h - 2 * kTextPad) / std::max(1, lineH)));
  std::vector<std::string> segments;
  {
    size_t start = 0;
    while (start < title.size()) {
      size_t next = title.find('_', start + (start == 0 ? 0 : 1));
      if (next == std::string::npos) next = title.size();
      if (next == start && start == 0) next = title.find('_', 1);  // 開頭就是「_」：跟第一段一起
      if (next == std::string::npos) next = title.size();
      segments.push_back(title.substr(start, next - start));
      start = next;
    }
  }
  std::vector<std::string> lines;
  for (const auto& seg : segments) {
    if (static_cast<int>(lines.size()) >= maxLines) break;
    const auto wrapped = renderer.wrappedText(UI_12_FONT_ID, seg.c_str(), textW,
                                              maxLines - static_cast<int>(lines.size()), EpdFontFamily::BOLD);
    for (const auto& ln : wrapped) lines.push_back(ln);
  }
  if (lines.empty()) return;
  const int blockH = static_cast<int>(lines.size()) * lineH;
  int curY = y + (h - blockH) / 2;
  // 書名底下留白給右下角的進度徽章（由呼叫端畫）；文字塊在中段，不會撞到。
  for (const auto& line : lines) {
    renderer.drawText(UI_12_FONT_ID, textX0, curY, line.c_str(), true, EpdFontFamily::BOLD);
    curY += lineH;
  }
}

std::string LyraTheme::displayTitleFor(const std::string& title, const std::string& path) {
  if (!FsHelpers::hasTxtExtension(path)) return title;
  std::string t = title;
  if (FsHelpers::hasTxtExtension(t) && t.size() > 4) t.resize(t.size() - 4);
  // v184（維護者）：檔名裡的「_」視為空格 —— 書名不會有底線，去掉才像一本書；連續底線合併。
  std::string out;
  out.reserve(t.size());
  bool lastSpace = false;
  for (const char ch : t) {
    const bool sp = (ch == '_' || ch == ' ');
    if (sp && lastSpace) continue;
    out.push_back(sp ? ' ' : ch);
    lastSpace = sp;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

bool LyraTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                  const int x, const int y, int& index) const {
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height) {
    return false;
  }

  int currentX = rect.x + LyraMetrics::values.contentSidePadding;
  for (size_t i = 0; i < tabs.size(); i++) {
    // ⚠️ v156：與 drawTabBar 同一個 font id（UI_12）—— 頁籤畫多寬、點擊區就多寬，
    //    兩處分開就會點 A 選到 B（v83 教訓的觸控版）。
    const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, tabs[i].label, EpdFontFamily::REGULAR);
    const int tabWidth = textWidth + 2 * hPaddingInSelection;
    const int left = (i == 0) ? rect.x : currentX - LyraMetrics::values.tabSpacing / 2;
    const int right = currentX + tabWidth + LyraMetrics::values.tabSpacing / 2;
    if (x >= left && x < right) {
      index = static_cast<int>(i);
      return true;
    }
    currentX += tabWidth + LyraMetrics::values.tabSpacing;
  }

  return false;
}

int LyraTheme::getListRowStep(bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  return rowHeight;
}

int LyraTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  const int rowStep = getListRowStep(hasSubtitle);
  if (rowStep <= 0) return 1;
  return std::max(1, contentHeight / rowStep);
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  int pageItems = rowHeight > 0 ? std::max(1, rect.height / rowHeight) : 1;

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
    // v44/v45/v50：選取＝圓角外框＋左緣豎條（不塗滿背景 —— e-ink 原則，維護者拍板）
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
  int iconY = (rowSubtitle != nullptr) ? 20 : 14;  // 配合放大的列高下移（原 16/10）
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    int rowTextWidth = textWidth;

    // Draw name
    int valueWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      // v63：值欄 10px→14px（同主文字 UI_12）：實機回報「開」/「關」在 10px 下筆畫模糊難分。
      valueText = renderer.truncatedText(UI_12_FONT_ID, valueText.c_str(), maxListValueWidth);
      valueWidth = renderer.getTextWidth(UI_12_FONT_ID, valueText.c_str()) + hPaddingInSelection;
      rowTextWidth -= valueWidth;
    }

    auto itemName = rowTitle(i);
    // v11：清單主文字 UI_12（=14px）；副標維持 UI_10。
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
      renderer.drawText(UI_10_FONT_ID, textX, itemY + 46, subtitle.c_str(), true);  // 列高 72 下的副標位置
    }

    // Draw value
    if (!valueText.empty()) {
      // v50 Formosa 原則：選中列值欄不再反白黑 pill —— 列本身已有選取記號，純黑字即可。
      (void)highlightValue;
      const int valueY = itemY + 8;  // 值欄與主文字同為 14px，對齊主文字基線
      renderer.drawText(UI_12_FONT_ID, rect.x + contentWidth - LyraMetrics::values.contentSidePadding - valueWidth,
                        valueY, valueText.c_str(), true);
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  if (gpio.hasTouch()) {
    return;
  }

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
  if (gpio.hasTouch()) {
    return;
  }

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
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(UI_10_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(UI_10_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
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
        const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(UI_10_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
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
    coverWidth = (LyraMetrics::values.homeCoverHeight * 2 + 1) / 3;  // v174：2:3，與縮圖目標同步
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
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            // v63：coverWidth 維持上面那個佔位估計值，【不要】拿 bitmap.getWidth() 覆寫它。
            // generateThumbBmp 的 crop=true 只保證縮圖【覆蓋】名目框（取兩個 fit scale 的較大者，
            // 而且從不把溢出的那一維裁回來），所以實際位元圖寬度會隨每本書封面的長寬比而變，
            // 不等於名目估計值。後果有兩層：
            //   ① coverWidth 是【TU 作用域全域】，會把上一本書的寬度帶進下一本的版面計算
            //      （textWidth / textX / 網點底都吃它）-> 封面框與書名欄每本書平移；
            //   ② 新書還沒有縮圖時先畫佔位圖，縮圖產生後框會當場改變大小。
            // 改成把載入的位元圖裁進固定框，與 Lyra3CoversTheme 的三封面版面同一作法。
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
        } else {
          // v57：開檔失敗時 hasCover 原本【停在 true】-> 佔位圖不畫、封面也沒有，只剩一個空框。
          // ⚠️ 這一行必須與 HomeActivity 產完縮圖後的 freeCoverBuffer() 同批進（本版已一起做）：
          //    單獨補它會讓 render#1 畫出佔位圖的實心黑，而 1-bit 縮圖只畫黑像素、
          //    白像素留原背景 -> 舊快照的黑經 OR 語意穿透 -> 新封面下三分之二整片黑。
          hasCover = false;
        }
      }

      // Draw either way（先把方角內容 mask 成圓角，再畫圓角外框；仿 RoundedRaffTheme 封面慣例）
      if (!hasCover) {
        // v175（使用者的設計稿）：沒有封面一律畫「書名幾何封面」（txt、封面缺失、尚未產生）。
        drawTitleCoverPlaceholder(renderer, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                  LyraMetrics::values.homeCoverHeight, displayTitleFor(book.title, book.path));
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
      // v44：整卡外框+左豎條（原四片網點包框）；封面 bitmap 內縮 8px，2px 框不壓到封面
      drawSelectionMarker(renderer, tileX, tileY, tileWidth, tileHeight);
    }

    // v42（實機回饋）：長書名/長作者改折行——書名最多 4 行、作者最多 2 行（原 3 行/單行截斷）。
    // 高度預算：4×34（UI_12 行框）+ 12 + 2×24（UI_10）= 196px ≤ 卡高 242px，置中不溢出。
    const std::string displayTitle = displayTitleFor(book.title, book.path);
    auto titleLines = renderer.wrappedText(UI_12_FONT_ID, displayTitle.c_str(), textWidth, 4, EpdFontFamily::BOLD);

    // v31/v155：作者行兼作進度顯示：「作者 (45%)」。
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
      // v44/v50：選取＝圓角外框＋左緣豎條，不塗滿背景
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
