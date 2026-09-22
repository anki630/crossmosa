#pragma once
#include "activities/Activity.h"

class Bitmap;
struct WallCacheSrc;  // v320：桌布平面快取的來源身分（定義在 SleepActivity.cpp）

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sleep", renderer, mappedInput) {}
  void onEnter() override;
  bool supportsLightSleep() const override { return false; }  // v327：這個畫面不淺睡眠（見 Activity.h）
  // v322：enterDeepSleep() 已先畫過「進入休眠」提示 → 這次休眠的【第一個】SleepActivity::onEnter 不再畫
  //   （用完自動清）。淺睡眠沒睡成／超時之後 goToSleep() 建的正式 SleepActivity 仍會再畫一次 —— 那是 v321 以前
  //   就有的雙重繪製（帳本 C-2），不在這裡處理。
  static bool skipEnteringPopup;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, const WallCacheSrc* cacheSrc = nullptr,
                               bool displayPlanes = true) const;
  void renderBlankSleepScreen() const;
};
