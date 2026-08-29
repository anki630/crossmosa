#pragma once

#include <functional>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ClearCacheActivity final : public Activity {
 public:
  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClearCache", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, ASK_PROGRESS, CLEARING, SUCCESS, FAILED };  // v192：ASK_PROGRESS 是動手前的保留／重設詢問

  State state = WARNING;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  OptionPopup confirmPopup;
  void askProgress();
  void beginClear(bool keepProgress);
  void clearCache(bool keepProgress);
};
