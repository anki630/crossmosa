#include "TxtReaderMenuActivity.h"

#include "ReaderFontSizes.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

TxtReaderMenuActivity::TxtReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::string& title, const float bookProgressPercent,
                                             const uint8_t currentOrientation, const bool hasBookmarks)
    : Activity("TxtReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasBookmarks)),
      title(title),
      pendingOrientation(currentOrientation),
      bookProgressPercent(bookProgressPercent) {}

std::vector<TxtReaderMenuActivity::MenuItem> TxtReaderMenuActivity::buildMenuItems(const bool hasBookmarks) {
  std::vector<MenuItem> items;
  items.reserve(8);
  // 跳到百分比排第一:txt 沒有目錄也沒有書籤,它是唯一的隨機存取手段,
  // 同時也是「進度掉了找得回來」的安全網。
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  // v286：**txt 原本沒有這個入口**，所以閱讀中只能改字級 —— 字型家族、行距、對齊、邊距
  //   都得退出去走「設定 → 閱讀」。那不是刻意的取捨，是漏的（EPUB 從一開始就有）。
  //   ⚠️ 而 v284 起 txt 才第一次吃「行距」設定，沒有入口的代價因此變大。
  //   空間沒有問題：閱讀選單橫向上限 7 列（硬限制第 7 條），txt 連這一項也才 4 項。
  // v290：書籤。順序照 EPUB（清單在前、加／移除在後），而「書籤清單」同樣是**條件式**的
  //   —— 沒有書籤時不佔一列（EPUB 就是這樣，接受「下方項目位置浮動」的代價）。
  if (hasBookmarks) {
    items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
  }
  items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
  items.push_back({MenuAction::TEXT_SETTINGS, StrId::STR_TEXT_SETTINGS});
  // v129:GO_HOME 移除——閱讀中 Back 鍵本來就回得了主畫面(ReaderUtils.h handleBackNavigation:
  // 預設短按回主畫面,開了 backShortToFileBrowser 則改成長按)。註腳開著或自動翻頁進行中時
  // 第一次 Back 會先被那個狀態吃掉,需要按第二次——仍然比「開選單→選項目」便宜。
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  // v289：以下三項是為了**與 EPUB 的選單一致**而補上（維護者立的原則：除非功能真的不能用，
  //   否則兩個閱讀器不該長得不一樣）。順序也照 EPUB 的相對順序排。
  //   ・截圖：⭐ **刻意與全域組合鍵並存**。v130 曾判它「真重複」，但實機回報組合鍵按不準
  //     就截不到圖 —— 兩個入口不一樣好用時，它不是重複，是**更可靠的那一個**。
  //   ・顯示 QR：把當前頁的文字送到手機（維護者有在用；txt 常是長文，需求更強）。
  //   ・刪除本書快取：⚠️ txt 的快取只有封面縮圖與**進度檔**（沒有版面快取，v118 起是串流排版），
  //     所以它的用途比 EPUB 窄很多（主要是封面壞掉時重產）。**進度必須先備份再寫回。**
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
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
    // 沿用既有的 MenuResult(ActivityResult.h),不新增變體型別 —— 呼叫端會依自己的
    // enum 解讀 action。未使用的欄位保持 0。
    if (selectedAction == MenuAction::DELETE_CACHE) {
      // 與 EPUB 同形狀：做成詢問而不是另一列（而且這裡的「保留進度」是**預設**，因為
      // txt 的進度檔就在快取資料夾裡，誤刪的代價是閱讀位置整個不見）。
      optionPopup.show(StrId::STR_DELETE_CACHE, clearCacheLabels.data(), static_cast<int>(clearCacheLabels.size()), 0,
                       [this](int idx) {
                         MenuResult r{.action = static_cast<int>(MenuAction::DELETE_CACHE),
                                      .orientation = pendingOrientation};
                         r.resetProgress = idx == 1 ? 1 : 0;
                         setResult(std::move(r));
                         finish();
                       });
      requestUpdate();
      return;
    }

    setResult(MenuResult{.action = static_cast<int>(selectedAction), .orientation = pendingOrientation});
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{.action = -1, .orientation = pendingOrientation};
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
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
