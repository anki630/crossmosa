#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <common/FsDateTime.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;  // Singleton instance

void HalClock::fatDateTimeCb(uint16_t* date, uint16_t* time) {
  // v194：SdFat 在持鎖改目錄時同步呼叫。這裡不准 I2C／LOG／寫檔，只讀快取立刻回。
  // v194（複查）：讀的是【一次發布的 32 位元】而不是多欄位結構——逐欄寫入會被中途讀到
  // 「新日期配舊時間」，跨日／跨月時甚至產生不合法的 FAT 時戳。0 同時代表無效。
  const uint32_t packed = halClock._fatStamp.load(std::memory_order_acquire);
  if (packed == 0) {
    *date = 0;
    *time = 0;
    return;
  }
  *date = static_cast<uint16_t>(packed >> 16);
  *time = static_cast<uint16_t>(packed & 0xFFFF);
}

// v194（複查）：把 date/time 壓成一個 32 位元一次發布；順帶在快取第一次有效時補註冊
// ——原本只在開機試一次，那一次 I2C 失敗就整次開機都不會有 FAT 時戳。
void HalClock::publishFatStamp(const Rtc::DateTime& dt) const {
  if (dt.year < 2020 || dt.year > 2107) return;
  const uint32_t packed = (static_cast<uint32_t>(FS_DATE(dt.year, dt.month, dt.day)) << 16) |
                          static_cast<uint32_t>(FS_TIME(dt.hour, dt.minute, dt.second));
  if (packed == 0) return;  // 理論上不會，但 0 是無效哨兵，不能發布
  _fatStamp.store(packed, std::memory_order_release);
  if (!_fatCbInstalled) {
    FsDateTime::setCallback(&HalClock::fatDateTimeCb);
    _fatCbInstalled = true;
    LOG_INF("CLK", "FsDateTime callback registered year=%u", dt.year);
  }
}

void HalClock::installFatDateTimeCallbackIfKnown() {
  if (!_available) return;
  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) return;
  // v194：時鐘已知才註冊。年 < 2020 視為沒對過（出廠／震盪器剛醒）。
  publishFatStamp(dt);
}

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
  // v194：開機時 RTC 已有可信時間才掛 FAT 時戳；沒對過就維持編譯年 1 月 1 日。
  installFatDateTimeCallbackIfKnown();
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  _cachedHour = dt.hour;
  _cachedMinute = dt.minute;
  _lastPollMs = now;
  _hasCachedTime = true;
  // v194：FAT callback 只讀快取。真正的 I2C 取時放這裡——狀態列低頻輪詢，不持 SdFat 鎖。
  publishFatStamp(dt);  // v194（複查）：這裡也會補註冊，開機那次 I2C 失敗不再是永久的
  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (_sdkRtc.set(dt)) {
        _lastPollMs = 0;
        _cachedHour = dt.hour;
        _cachedMinute = dt.minute;
        _hasCachedTime = true;
        publishFatStamp(dt);  // v194（複查）：對過時間之後立刻發布並補註冊
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", dt.year, dt.month, dt.day, dt.hour, dt.minute,
                dt.second);
        installFatDateTimeCallbackIfKnown();
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
