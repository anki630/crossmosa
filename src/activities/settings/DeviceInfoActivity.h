#pragma once

#include <string>

#include "activities/Activity.h"

// v196：唯讀裝置資訊畫面（晶片型號＋完整序號）。由設定頁 DynamicString Confirm 進入。
class DeviceInfoActivity final : public Activity {
 public:
  explicit DeviceInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DeviceInfo", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // v196（複查）：固定緩衝而非 std::string —— 這個畫面可能在記憶體吃緊時被打開，
  // 而 -fno-exceptions 下配置失敗＝abort。序號最長 32 bytes。
  char serialNumber[33] = {0};
  bool serialOk = false;
  const char* controllerName = "";
};
