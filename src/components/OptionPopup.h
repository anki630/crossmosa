#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

class OptionPopup {
 public:
  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    confirmArmed = backArmed = false;
    active = true;
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect) {
    title = titleStr;
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    confirmArmed = backArmed = false;
    active = true;
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    title = I18N.get(titleId);
    ownedStrings = options;
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    confirmArmed = backArmed = false;
    active = true;
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const int count = static_cast<int>(ownedStrings.size());
    if (input.wasPressed(MappedInputManager::Button::Up) || input.wasPressed(MappedInputManager::Button::Left)) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::Down) ||
               input.wasPressed(MappedInputManager::Button::Right)) {
      selectedIndex = (selectedIndex + 1) % count;
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::Confirm)) {
      // v46:確認/返回改「放開」才動作、且須先在彈窗活躍期間看過同鍵的「按下」(armed)。
      // 兩個都必要:①只看按下 → 放開邊緣漏給 release 型宿主(閱讀選單),出現「選定後彈窗重開」
      // 「Back 穿透回閱讀」(實機回報);②只看放開 → press 型宿主(設定頁)按下當 tick 開彈窗,
      // 「開啟那次按鍵」的放開會被當成選定,彈窗閃現即自關(對抗驗證抓到)。armed 兩型皆正確,
      // 「按住進彈窗」的首個放開也自然成為 no-op。導覽鍵維持按下即動。
      confirmArmed = true;
      return true;
    } else if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      if (confirmArmed) {
        active = false;
        if (onSelectCallback) onSelectCallback(selectedIndex);
        requestUpdate();
      }
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::Back)) {
      backArmed = true;
      return true;
    } else if (input.wasReleased(MappedInputManager::Button::Back)) {
      if (backArmed) {
        active = false;
        requestUpdate();
      }
      return true;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    GUI.drawOptionPopup(renderer, title.c_str(), ownedStrings, selectedIndex);
  }

  bool isActive() const { return active; }

 private:
  bool active = false;
  bool confirmArmed = false;
  bool backArmed = false;
  std::string title;
  std::vector<std::string> ownedStrings;
  int selectedIndex = 0;
  std::function<void(int)> onSelectCallback;
};
