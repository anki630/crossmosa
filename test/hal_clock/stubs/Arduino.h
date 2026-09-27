#pragma once
// 電腦端替身：HalClock 只用到 millis／delay／configTzTime。
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "FakeClockHw.h"

inline uint32_t millis() { return fake::nowMs; }
inline void delay(uint32_t ms) { fake::nowMs += ms; }
inline void configTzTime(const char*, const char*, const char* = nullptr, const char* = nullptr) {
  fake::tzConfigured = true;
}
// 裝置上 RTC_NOINIT_ATTR 把變數放進撐得過重開機的 RTC 記憶體；電腦端就是一般的全域變數 ——
//   測試用「換一個新的 HalClock 再 begin()」模擬重開機，這些值照樣留著（跟裝置一樣）。
#define RTC_NOINIT_ATTR
