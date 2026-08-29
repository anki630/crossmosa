#pragma once

#include "components/themes/lyra/Lyra3CoversTheme.h"

class GfxRenderer;

// v179：Formosa Pro —— 蘋果設計語彙（維護者 2026-08-28 拍板）：
//   一個圓角 token（R=16 = X3 面板本身的角，照片量到 ≈1.2mm ≈ 15px）、同心推導、連續曲率 n=4；
//   iOS 分組內縮清單（髮絲線、›、值靠右）、選取＝左側細豎條＋粗體（不塗底，C-33）；
//   狀態列左時間右電池；按鍵提示改懸浮 pill；頁籤改 segmented control。
// 繼承 Formosa Extended（三封面）；Formosa 本體一行不動，失敗可切回。
namespace FormosaProMetrics {
constexpr ThemeMetrics values = [] {
  ThemeMetrics v = Lyra3CoversMetrics::values;
  v.cornerRadius = 16;
  v.cornerSmoothing = 60;  // iOS
  v.batteryWidth = 26;
  v.batteryHeight = 12;
  v.menuRowHeight = 48;  // 主畫面選單變成分組卡：列高同清單，卡片相鄰無溝
  v.menuSpacing = 0;
  v.homeMenuTopOffset = 24;
  v.popupCornerRadius = 16;
  v.keyboardKeyCornerRadius = 10;
  v.optionPopupSelectionRadius = 10;
  v.popupMarginX = 24;  // 彈窗：macOS alert 的留白
  v.popupMarginY = 14;
  v.popupFrameThickness = 1;
  return v;
}();
}  // namespace FormosaProMetrics

class FormosaProTheme : public Lyra3CoversTheme {
 public:
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  int menuRowHeightFor(int availableHeight, int count) const override;
  void drawMenuList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                    const std::function<std::string(int index)>& rowTitle,
                    const std::function<std::string(int index)>& rowValue,
                    const std::function<bool(int index)>& groupBreakBefore) const override;
  Rect drawPopup(const GfxRenderer& renderer, const char* message) const override;
  void drawTextField(const GfxRenderer& renderer, Rect rect, int textWidth, bool cursorMode = false,
                     int contentStartX = 0, int contentWidth = 0) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawOptionPopup(const GfxRenderer& renderer, const char* title, const std::vector<std::string>& options,
                       int selectedIndex) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;

 private:
  static constexpr int R = 16;   // = FormosaProMetrics::values.cornerRadius
  static constexpr int N = 60;   // = cornerSmoothing（百分比，v180 起）
  static int innerR(int inset) { return R - inset < 2 ? 2 : R - inset; }
  void drawChevron(const GfxRenderer& renderer, int x, int cy) const;
  void drawGroupedRows(const GfxRenderer& renderer, int cardX, int cardY, int cardW, int rowH, int rowCount,
                       int selectedRow, int iconSize, const std::function<std::string(int)>& title,
                       const std::function<std::string(int)>& subtitle, const std::function<UIIcon(int)>& icon,
                       const std::function<std::string(int)>& value, const std::function<bool(int)>& dimmed,
                       int firstIndex, bool chevrons = true,
                       const std::function<bool(int)>& groupBreakBefore = nullptr) const;
};
