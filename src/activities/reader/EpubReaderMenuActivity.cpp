#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const bool hasBookmarks)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, hasBookmarks)),
      title(title),
      pendingOrientation(currentOrientation),
      pendingFontSize(SETTINGS.fontSize),
      pendingLineSpacing(SETTINGS.lineSpacing),
      pendingBoldBody(SETTINGS.boldBodyText),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes,
                                                                                     bool hasBookmarks) {
  // v129:依「開這個選單的目的」排序,不依「是不是設定」分類。
  // ⚠️ 直向可見 12 列,最壞情況(有註腳 + 有書籤)正好 12 項——【零餘裕】。
  //    要加任何一項,必須先砍一項。橫向只看得見 7 列 → 前 7 項決定橫向讀者
  //    不捲動能碰到什麼。判準與三條否決見 docs/specs/2026-08-12-reader-menu-ia.md。
  std::vector<MenuItem> items;
  items.reserve(12);
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  // 排版三兄弟必須相鄰:是「邊看邊調」的一叢動作,調完會互相影響
  // (字級調大之後常常就想收緊行距)。v38/v41 加入,v129 改為集中。
  items.push_back({MenuAction::FONT_SIZE, StrId::STR_FONT_SIZE});
  items.push_back({MenuAction::LINE_SPACING, StrId::STR_LINE_SPACING});
  items.push_back({MenuAction::BOLD_TEXT, StrId::STR_BOLD_TEXT});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  // 「一次設定型」與排錯型降位到選單尾。v129 否決了把它們收進子選單:
  // 兩者已各自帶一個 OptionPopup,再包一層就變三層,而清單底部本來就等於免費的子選單。
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  return items;
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                       pendingOrientation, [this](int idx) {
                         pendingOrientation = idx;
                         requestUpdate();
                       });
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::FONT_SIZE) {
      optionPopup.show(StrId::STR_FONT_SIZE, fontSizeLabels.data(), static_cast<int>(fontSizeLabels.size()),
                       pendingFontSize, [this](int idx) {
                         pendingFontSize = static_cast<uint8_t>(idx);
                         requestUpdate();
                       });
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::LINE_SPACING) {
      optionPopup.show(StrId::STR_LINE_SPACING, lineSpacingLabels.data(), static_cast<int>(lineSpacingLabels.size()),
                       pendingLineSpacing, [this](int idx) {
                         pendingLineSpacing = static_cast<uint8_t>(idx);
                         requestUpdate();
                       });
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::BOLD_TEXT) {
      optionPopup.show(StrId::STR_BOLD_TEXT, boldTextLabels.data(), static_cast<int>(boldTextLabels.size()),
                       pendingBoldBody, [this](int idx) {
                         pendingBoldBody = static_cast<uint8_t>(idx);
                         requestUpdate();
                       });
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      optionPopup.show(I18N.get(StrId::STR_AUTO_TURN_PAGES_PER_MIN), pageTurnLabels.data(),
                       static_cast<int>(pageTurnLabels.size()), selectedPageTurnOption, [this](int idx) {
                         selectedPageTurnOption = idx;
                         requestUpdate();
                       });
      requestUpdate();
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption, pendingFontSize,
                         pendingLineSpacing, pendingBoldBody});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data =
        MenuResult{-1, pendingOrientation, selectedPageTurnOption, pendingFontSize, pendingLineSpacing, pendingBoldBody};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuItems.size(), selectedIndex,
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr,
      [this](int index) {
        const auto value = menuItems[index].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          // Render current orientation value on the right edge of the content area.
          return I18N.get(orientationLabels[pendingOrientation]);
        } else if (value == MenuAction::FONT_SIZE) {
          // Render current / pending font size on the right edge of the content area.
          return I18N.get(fontSizeLabels[pendingFontSize]);
        } else if (value == MenuAction::LINE_SPACING) {
          return I18N.get(lineSpacingLabels[pendingLineSpacing]);
        } else if (value == MenuAction::BOLD_TEXT) {
          return I18N.get(boldTextLabels[pendingBoldBody]);
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          // Render current page turn value on the right edge of the content area.
          return pageTurnLabels[selectedPageTurnOption];
        } else {
          return "";
        }
      },
      true);

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
