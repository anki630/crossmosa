#pragma once
// 電腦端替身的共用狀態：假的 millis()、WiFi、SNTP、系統時間、DS3231。測試直接改這些值。
// ⚠️ 替身要照 SDK 的語意（freeink-sdk libs/hardware/Rtc/src/Rtc.cpp，DS3231 那一支）：
//   now()：I2C 失敗或 OSF（振盪器停止）→ false；不看年份。set()：寫入時間，成功後清 OSF。
#include <cstdint>

#include "hal/ClockCache.h"

namespace fake {
inline uint32_t nowMs = 0;
inline bool wifiConnected = true;
inline uint32_t sntpCompleteAtMs = 0xFFFFFFFFu;  // 到這個 millis 之後 SNTP 算完成；0xFFFFFFFF ＝ 永遠不完成
inline int64_t sysTime = 0;                      // time(nullptr)
inline bool tzConfigured = false;

inline bool rtcPresent = true;
inline bool osf = false;
inline bool i2cBroken = false;
inline int i2cFailNext = 0;
inline bool setSticks = true;      // set() 真的存進晶片
inline int32_t setOffsetSec = 0;   // 存進去的 ＝ 寫的 ＋ 這個（讀回不一致）
inline bool garbageMonth = false;  // 讀出 13 月（暫存器壞掉）
// codex v343 第三輪：替身不能比晶片寬鬆 ——
inline bool setFailsHalfway = false;  // set() 寫到一半 I2C 失敗：晶片被改了一半（時分換了、日期沒換）而且回 false
inline int failReadsAfterSet = 0;     // set() 之後緊接著的 N 次 now() 讀失敗（單獨讓「讀回」失敗）
inline bool rawActive = false;        // now() 直接吐出下面這組原始欄位（可以是 2 月 31 日這種不存在的日期）
inline uint16_t rawYear = 2026;
inline uint8_t rawMonth = 1, rawDay = 1, rawHour = 0, rawMinute = 0, rawSecond = 0;
inline uint32_t rtcEpochAtRef = 0;
inline uint32_t rtcRefMs = 0;
inline int reads = 0;
inline int sets = 0;

inline void setRtc(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
  rtcEpochAtRef = clockcache::epochOf(y, mo, d, h, mi, s);
  rtcRefMs = nowMs;
  garbageMonth = false;
}
inline uint32_t rtcNowEpoch() { return rtcEpochAtRef + (nowMs - rtcRefMs) / 1000u; }

inline void reset() {
  nowMs = 1000;
  wifiConnected = true;
  sntpCompleteAtMs = 0xFFFFFFFFu;
  sysTime = 0;
  tzConfigured = false;
  rtcPresent = true;
  osf = false;
  i2cBroken = false;
  i2cFailNext = 0;
  setSticks = true;
  setOffsetSec = 0;
  garbageMonth = false;
  setFailsHalfway = false;
  failReadsAfterSet = 0;
  rawActive = false;
  reads = sets = 0;
  setRtc(2026, 9, 26, 12, 0, 0);
}

inline bool i2cOk() {
  if (i2cBroken) return false;
  if (i2cFailNext > 0) {
    --i2cFailNext;
    return false;
  }
  return true;
}
}  // namespace fake
