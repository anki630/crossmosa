#pragma once
#include "activities/Activity.h"

class BootActivity final : public Activity {
 public:
  explicit BootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Boot", renderer, mappedInput) {}
  void onEnter() override;
  bool supportsLightSleep() const override { return false; }  // v327：這個畫面不淺睡眠（見 Activity.h）
};
