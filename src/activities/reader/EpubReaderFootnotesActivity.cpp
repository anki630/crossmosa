#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// v363：註腳清單改用跟目錄、書籤同一種清單（主題的 drawList：14px、同樣的選取框與分頁）。
//   原本自己畫，項目是 10px（v11 把所有清單放大到 14px 時沒包到它），一頁約 20 項；現在跟目錄一樣 X3 直向 13、橫向 8。
//   按鍵也跟目錄一致：放開才移動、按住翻一頁；確認鍵或電源鍵跳到那個註腳（電源鍵是原本就有的行為）。

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderFootnotesActivity::onExit() { Activity::onExit(); }

void EpubReaderFootnotesActivity::loop() {
  const int totalItems = static_cast<int>(footnotes.size());
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

  auto selectFootnote = [this, totalItems] {
    if (selectedIndex >= 0 && selectedIndex < totalItems) {
      setResult(FootnoteResult{footnotes[selectedIndex].href});
      finish();
    }
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    selectFootnote();
    return;
  }

  if (totalItems == 0) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;
  switch (handleListTouch(selectedIndex, totalItems, contentTop, contentHeight, false)) {
    case ListTouchResult::Activated:
      selectFootnote();
      return;
    case ListTouchResult::Consumed:
      return;
    case ListTouchResult::None:
      break;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectedIndex = ButtonNavigator::nextPageIndexClamped(selectedIndex, totalItems, pageItems);
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectedIndex = ButtonNavigator::previousPageIndexClamped(selectedIndex, totalItems, pageItems);
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndexClamped(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndexClamped(selectedIndex, totalItems, pageItems);
    requestUpdate();
  });
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_FOOTNOTES));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  if (footnotes.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 20, tr(STR_NO_FOOTNOTES));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, static_cast<int>(footnotes.size()),
               selectedIndex, [this](int index) {
                 // 註腳的編號（連結上的字，例如「[1]」「註3」）；連結上沒有字就顯示「[連結]」。
                 const char* number = footnotes[index].number;
                 return std::string(number[0] != '\0' ? number : tr(STR_LINK));
               });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
