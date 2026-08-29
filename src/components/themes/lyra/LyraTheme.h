#pragma once

#include "components/themes/BaseTheme.h"

#include <string>

class GfxRenderer;

// Lyra theme metrics (zero runtime cost)
namespace LyraMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 40,
                                 .headerHeight = 84,
                                 .verticalSpacing = 16,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 48,  // v11/v155：清單主文字已是 14px（v132 別名），列高配合 40→48
                                 .listWithSubtitleRowHeight = 72,  // v11/v155：60→72
                                 .menuRowHeight = 64,
                                 .menuSpacing = 8,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 56,
                                 .homeCoverHeight = 226,
                                 .homeCoverTileHeight = 242,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 16,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 48,
                                 .keyboardKeySpacing = 0,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .keyboardKeyCornerRadius = 6,
                                 // v49（實機回饋）：0.165 偏上 → 0.46。單行彈窗高 58px，直向恰置中；
                                 // 橫向低於中心約 8px 可接受。影響所有 drawPopup 站點。
                                 .popupTopOffsetRatio = 0.46f,
                                 .popupMarginX = 16,
                                 .popupMarginY = 12,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 6,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .optionPopupItemSpacing = 8,
                                 .optionPopupInnerPadding = 20,
                                 .optionPopupSelectionHPadding = 16,
                                 .optionPopupSelectionVPadding = 12,
                                 .optionPopupTitleGap = 16,
                                 .optionPopupUseSmallFont = true,
                                 .optionPopupOptionFontBold = false,
                                 .optionPopupSelectionRadius = 6,
                                 .optionPopupSelectionLight = true,
                                 .optionPopupDrawAllRows = false,
                                 .optionPopupDialogSideMargin = 20,
                                 .optionPopupTitleSeparator = true,
                                 .optionPopupSelectionOutline = true,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class LyraTheme : public BaseTheme {
 public:
  // Component drawing methods
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  bool tabIndexFromPoint(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, int x, int y,
                         int& index) const override;

  // v124/v156：無封面書籍的佔位封面（書脊線＋置中圖示）。取代上游「下三分之二實心黑」——
  // 那違反「e-ink 的 UI 不塗滿背景」（v50 維護者拍板），實機抱怨「真的很醜」的正是那塊。
  // Lyra 與 Lyra3Covers 共用（v44 已為漏網的自繪站點付過一次代價）。
  static void drawEmptyCoverPlaceholder(GfxRenderer& renderer, int x, int y, int w, int h);
  // v174（使用者要求）：沒有封面的 txt → 書名畫進封面框，做成一本書的樣子（書脊線＋置中粗體書名＋上下短橫線）。
  static void drawTitleCoverPlaceholder(GfxRenderer& renderer, int x, int y, int w, int h, const std::string& title);
  // v174：卡片顯示用的書名 —— txt 的「書名」是檔名，去掉副檔名再顯示（store 內容不動，舊條目一樣受惠）。
  static std::string displayTitleFor(const std::string& title, const std::string& path);
  int getListRowStep(bool hasSubtitle) const override;
  int getListPageItems(int contentHeight, bool hasSubtitle) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  void drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const;
  bool showsFileIcons() const override { return true; }
  // v179：圖示查表開放給子主題（Formosa Pro）。
  static const uint8_t* iconForName(UIIcon icon, int size);
};
