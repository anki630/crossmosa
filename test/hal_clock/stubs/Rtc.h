#pragma once
// 電腦端替身：SDK 的 freeink::Rtc（DS3231 那一支的語意，見 FakeClockHw.h）。
#include <cstdint>

#include "FakeClockHw.h"

class Rtc {
 public:
  struct DateTime {
    uint16_t year = 2000;
    uint8_t month = 1;
    uint8_t day = 1;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t weekday = 0;
  };
  bool begin() {
    begun_ = fake::rtcPresent && fake::i2cOk();
    return begun_;
  }
  bool present() const { return begun_; }
  bool now(DateTime& out) {
    ++fake::reads;
    if (fake::failReadsAfterSet > 0) {
      --fake::failReadsAfterSet;
      return false;
    }
    if (!begun_ || !fake::i2cOk() || fake::osf) return false;  // SDK：先讀狀態暫存器，OSF 亮就回 false
    if (fake::rawActive) {  // 暫存器裡的原始 BCD 解出來是什麼就給什麼（SDK 不驗日期）
      out.year = fake::rawYear;
      out.month = fake::rawMonth;
      out.day = fake::rawDay;
      out.hour = fake::rawHour;
      out.minute = fake::rawMinute;
      out.second = fake::rawSecond;
      return true;
    }
    clockcache::civilFromEpoch(fake::rtcNowEpoch(), out.year, out.month, out.day, out.hour, out.minute, out.second);
    if (fake::garbageMonth) out.month = 13;
    return true;
  }
  bool set(const DateTime& dt) {
    ++fake::sets;
    if (!begun_ || !fake::i2cOk()) return false;
    if (fake::setFailsHalfway) {  // 秒、分、時寫進去了，後面的位元組 I2C 失敗：晶片裡是「新的時分配舊的日期」
      uint16_t y;
      uint8_t mo, d, h, mi, sec;
      clockcache::civilFromEpoch(fake::rtcNowEpoch(), y, mo, d, h, mi, sec);
      fake::rtcEpochAtRef = clockcache::epochOf(y, mo, d, dt.hour, dt.minute, dt.second);
      fake::rtcRefMs = fake::nowMs;
      return false;
    }
    if (fake::setSticks) {
      fake::rtcEpochAtRef =
          clockcache::epochOf(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second) + fake::setOffsetSec;
      fake::rtcRefMs = fake::nowMs;
      fake::garbageMonth = false;
    }
    fake::osf = false;  // SDK：寫入成功後清 OSF
    fake::rawActive = false;
    return true;
  }

 private:
  bool begun_ = false;
};
