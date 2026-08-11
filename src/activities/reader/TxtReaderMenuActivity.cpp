#include "TxtReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

TxtReaderMenuActivity::TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::string& title, const float bookProgressPercent,
                                             const uint8_t currentOrientation)
    : Activity("TxtReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems()),
      title(title),
      pendingOrientation(currentOrientation),
      pendingFontSize(SETTINGS.fontSize),
      bookProgressPercent(bookProgressPercent) {}

std::vector<TxtReaderMenuActivity::MenuItem> TxtReaderMenuActivity::buildMenuItems() {
  std::vector<MenuItem> items;
  items.reserve(4);
  // 跳到百分比排第一:txt 沒有目錄也沒有書籤,它是唯一的隨機存取手段,
  // 同時也是「進度掉了找得回來」的安全網。
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  items.push_back({MenuAction::FONT_SIZE, StrId::STR_FONT_SIZE});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  return items;
}

void TxtReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void TxtReaderMenuActivity::onExit() { Activity::onExit(); }

void TxtReaderMenuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

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

    // 方向與字級走 pending 模式:彈窗選定即生效(即使整個選單被取消),與 EPUB 同語意。
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                       pendingOrientation, [this](int idx) {
                         pendingOrientation = static_cast<uint8_t>(idx);
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

    // 沿用既有的 MenuResult(ActivityResult.h),不新增變體型別 —— 呼叫端會依自己的
    // enum 解讀 action。未使用的欄位保持 0。
    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, 0, pendingFontSize, 0, 0});
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, 0, pendingFontSize, 0, 0};
    setResult(std::move(result));
    finish();
  }
}

void TxtReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str());

  // 副標只放全書百分比 —— txt 沒有章節,而百分比取兩位小數的理由同狀態列:
  // 長文的整數百分比幾乎不動。
  char progressLine[48];
  snprintf(progressLine, sizeof(progressLine), "%s%.2f%%", tr(STR_BOOK_PREFIX), bookProgressPercent);
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine);

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, menuItems.size(), selectedIndex,
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr,
      [this](int index) {
        const auto value = menuItems[index].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          return I18N.get(orientationLabels[pendingOrientation]);
        }
        if (value == MenuAction::FONT_SIZE) {
          return I18N.get(fontSizeLabels[pendingFontSize]);
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
