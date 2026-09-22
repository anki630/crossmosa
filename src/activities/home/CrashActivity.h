#pragma once
#include "activities/Activity.h"

class CrashActivity final : public Activity {
  std::string panicMessage;

 public:
  explicit CrashActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Crash", renderer, mappedInput) {}
  void onEnter() override;
  bool supportsLightSleep() const override { return false; }  // v327：這個畫面不淺睡眠（見 Activity.h）
  void loop() override;
  void render(RenderLock&&) override;
};
