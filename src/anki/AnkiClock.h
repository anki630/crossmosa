#pragma once

// 單字卡用的「現在幾點」：本地 epoch 秒（UTC＋時區設定），跟閱讀統計（util/ReadingStats.cpp today()）同一套。
// 時鐘不可信（沒校時、RTC 沒電）時回 false，呼叫端用進度檔記的上一次時間往前推。

#include <HalClock.h>

#include <cstdint>

#include "CrossPointSettings.h"

namespace anki {

inline bool trustedLocalNow(uint32_t& out) {
  constexpr uint32_t kClockMaxAgeMs = 10u * 60u * 1000u;
  uint32_t epoch = 0;
  if (!halClock.trustedUtcNow(epoch, kClockMaxAgeMs)) return false;
  uint8_t q = SETTINGS.clockUtcOffsetQ;
  if (q > 104) q = 104;  // 跟狀態列同一個夾限（HalClock::formatTime）
  const int64_t local = static_cast<int64_t>(epoch) + (static_cast<int32_t>(q) - 48) * 900;
  if (local <= 0) return false;
  out = static_cast<uint32_t>(local);
  return true;
}

}  // namespace anki
