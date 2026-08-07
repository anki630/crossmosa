#include "SdDateTime.h"

#include <Arduino.h>
#include <SdFat.h>

#include <ctime>

#include "DiagLog.h"
#include "Logging.h"

namespace {

bool gRegistered = false;
uint32_t gLastCheckMs = 0;

// THE PLAUSIBILITY TEST, and why it needs no stored state.
//
// FS_DEFAULT_DATE is FS_DATE(compileYear(), 1, 1) (SdFatConfig.h:311), so
// "the clock reads at or after the year this firmware was built" is exactly
// "the clock is at or after the date we would otherwise stamp". A device that
// has never synced reads 1970 and fails by 56 years; one that has synced reads
// the real year and passes. No settings key, no migration, and correct on the
// first boot after a flash.
//
// gmtime_r, NOT localtime_r: configTzTime() does setenv("TZ") + tzset() from
// another task, and localtime_r reads that state. Everything here is UTC.
bool clockIsPlausible(struct tm& out) {
  const time_t now = time(nullptr);
  gmtime_r(&now, &out);
  return (out.tm_year + 1900) >= compileYear();
}

// Runs under HalStorage's mutex, from at least the Arduino loop task and the
// render task. NO I2C (Wire is shared with the tilt sensor), no allocation, no
// logging, no Storage re-entry, nothing that blocks. time() takes one newlib
// mutex and is a leaf -- nothing under it takes the storage mutex, so there is
// no inversion -- and gmtime_r takes none.
//
// The "clock unknown" branch is unreachable while maybeRegister() is the only
// thing that installs this (it applies the same test first, and the clock never
// runs backwards). It exists so a future caller who registers earlier cannot
// leak 1970 onto the card -- and note that simply passing 1970 through would
// NOT be harmless: FS_DATE does `year -= 1980` on a uint16_t, so 1970 wraps to
// 65526, fails the internal `year > 127` bound and the macro yields 0, which
// decodes as an invalid 1980-00-00 rather than as 1970.
void dateTimeCallback(uint16_t* date, uint16_t* time_, uint8_t* ms10) {
  struct tm utc {};
  if (!clockIsPlausible(utc)) {
    *date = FS_DEFAULT_DATE;
    *time_ = FS_DEFAULT_TIME;
    *ms10 = 0;
    return;
  }
  *date = FS_DATE(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
  *time_ = FS_TIME(utc.tm_hour, utc.tm_min, utc.tm_sec);
  *ms10 = 0;
}

}  // namespace

bool SdDateTime::isRegistered() { return gRegistered; }

bool SdDateTime::nowUtc(struct tm* out) {
  if (out == nullptr) return false;
  struct tm utc {};
  if (!clockIsPlausible(utc)) return false;
  *out = utc;
  return true;
}

void SdDateTime::maybeRegister() {
  if (gRegistered) return;

  // Throttle: before the clock is known this runs from loop(), and time() takes
  // a mutex. Once per second is far more often than SNTP can land and costs
  // nothing measurable.
  const uint32_t now = millis();
  if (gLastCheckMs != 0 && now - gLastCheckMs < 1000) return;
  gLastCheckMs = now;

  struct tm utc {};
  if (!clockIsPlausible(utc)) return;

  // The 3-argument overload on purpose: the 2-argument one installs a
  // forwarding shim (FsDateTime.cpp:30-33), an extra indirection on a path that
  // runs under the storage mutex.
  FsDateTime::setCallback(dateTimeCallback);
  gRegistered = true;

  LOG_INF("SDDT", "date/time callback registered at %04d-%02d-%02d %02d:%02d:%02d UTC", utc.tm_year + 1900,
          utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
  DiagLog::line("SD date/time callback registered at %04d-%02d-%02d %02d:%02d:%02d UTC (uptime %lu ms)",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                (unsigned long)now);
}
