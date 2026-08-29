#pragma once

#include <functional>
#include <vector>

#include "MappedInputManager.h"

class ButtonNavigator final {
  using Callback = std::function<void()>;
  using Buttons = std::vector<MappedInputManager::Button>;

  const uint16_t continuousStartMs;
  const uint16_t continuousIntervalMs;
  uint32_t lastContinuousNavTime = 0;
  static const MappedInputManager* mappedInput;

  [[nodiscard]] bool shouldNavigateContinuously() const;

 public:
  explicit ButtonNavigator(const uint16_t continuousIntervalMs = 500, const uint16_t continuousStartMs = 500)
      : continuousStartMs(continuousStartMs), continuousIntervalMs(continuousIntervalMs) {}

  static void setMappedInputManager(const MappedInputManager& mappedInputManager) { mappedInput = &mappedInputManager; }

  void onNext(const Callback& callback);
  void onPrevious(const Callback& callback);
  void onPressAndContinuous(const Buttons& buttons, const Callback& callback);

  void onNextPress(const Callback& callback);
  void onPreviousPress(const Callback& callback);
  void onPress(const Buttons& buttons, const Callback& callback);

  void onNextRelease(const Callback& callback);
  void onPreviousRelease(const Callback& callback);
  void onRelease(const Buttons& buttons, const Callback& callback);

  void onNextContinuous(const Callback& callback);
  void onPreviousContinuous(const Callback& callback);
  void onContinuous(const Buttons& buttons, const Callback& callback);

  [[nodiscard]] static int nextIndex(int currentIndex, int totalItems);
  [[nodiscard]] static int previousIndex(int currentIndex, int totalItems);

  [[nodiscard]] static int nextPageIndex(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndex(int currentIndex, int totalItems, int itemsPerPage);

  // v31/v156：翻頁的【夾限】版 —— 翻過最後一頁停在底、第一頁之前停在頂，不環繞。
  // 長按翻頁在到底之後環繞回頂端 = 使用者在長清單（OPDS feed、書籤、章節）裡
  // 無限繞圈，長按變成賭「放手的時機」。單步選取的環繞（nextIndex）維持不變。
  [[nodiscard]] static int nextPageIndexClamped(int currentIndex, int totalItems, int itemsPerPage);
  [[nodiscard]] static int previousPageIndexClamped(int currentIndex, int totalItems, int itemsPerPage);

  // Navigation uses the logical NavNext / NavPrevious buttons; MappedInputManager::mapButton resolves
  // them to physical buttons and applies any orientation-based direction swap, so this stays settings-free.
  [[nodiscard]] static Buttons getNextButtons() { return {MappedInputManager::Button::NavNext}; }
  [[nodiscard]] static Buttons getPreviousButtons() { return {MappedInputManager::Button::NavPrevious}; }
};
