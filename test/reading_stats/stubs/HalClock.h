#pragma once
// 電腦端替身：真正的 trustedUtcNow 在 test/hal_clock 測；這裡只控制「有沒有可信時間、是幾點」。
#include <cstdint>

#include "Arduino.h"
// 跟真的一樣：發布值靠 millis() 往前推、比 maxAgeMs 舊就不給（codex 程式碼複查第二輪：替身不能比裝置寬鬆）
class HalClock {
 public:
  bool available = true;
  bool valid = false;
  uint32_t epoch = 0;  // 在 publishedAtMs 那一刻的 UTC 秒數
  uint32_t publishedAtMs = 0;
  int polls = 0;
  bool isAvailable() const { return available; }
  bool trustedUtcNow(uint32_t& e, uint32_t maxAgeMs) const {
    if (!available || !valid) return false;
    const uint32_t age = millis() - publishedAtMs;
    if (age >= maxAgeMs) return false;
    e = epoch + age / 1000u;
    return true;
  }
  bool getTime(uint8_t&, uint8_t&) {
    ++polls;
    return valid;
  }
};
inline HalClock halClock;
