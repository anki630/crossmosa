#include "FatTimestamp.h"

namespace FatTimestamp {

namespace {

// Days from 1970-01-01 to y-m-d, proleptic Gregorian. This is Howard
// Hinnant's `days_from_civil` (public domain, "chrono-Compatible Low-Level
// Date Algorithms"), used unmodified rather than reinvented: the era
// arithmetic is exactly where a hand-rolled leap-year loop gets 2100 wrong,
// and a wrong answer here is still a plausible-looking date, so it would not
// announce itself.
//
// No loop over years, so cost does not depend on how far the date is from the
// epoch -- this runs once per directory entry in a listing.
//
// Preconditions (all guaranteed by the caller's validation below): m in
// [1,12], d in [1, last-day-of-month].
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);             // [0, 399]
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// The exact inverse of daysFromCivil, from the same source. Used unmodified
// for the same reason: hand-rolling the reverse mapping is where "one day out
// in March of a leap year" lives, and the answer still looks like a date.
void civilFromDays(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);            // [0, 146096]
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);            // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                                 // [0, 11]
  *d = doy - (153 * mp + 2) / 5 + 1;                                       // [1, 31]
  *m = mp < 10 ? mp + 3 : mp - 9;                                          // [1, 12]
  *y = yy + (*m <= 2 ? 1 : 0);
}

bool isLeapYear(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned daysInMonth(unsigned year, unsigned month) {
  // static constexpr => flash, not RAM (CLAUDE.md's `constexpr` First rule).
  static constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) return 29;
  return kDays[month - 1];
}

}  // namespace

int64_t toUnixSeconds(uint16_t date, uint16_t time) {
  // An all-zero date field is what an entry with no timestamp actually holds;
  // it is not a valid date either (month 0), so the range checks below would
  // reject it anyway -- this is just the common case answered first.
  if (date == 0) return 0;

  const unsigned year = 1980u + ((date >> 9) & 0x7Fu);  // 1980-2107, the whole representable range
  const unsigned month = (date >> 5) & 0x0Fu;
  const unsigned day = date & 0x1Fu;
  const unsigned hour = (time >> 11) & 0x1Fu;
  const unsigned minute = (time >> 5) & 0x3Fu;
  const unsigned second = (time & 0x1Fu) * 2u;  // FAT stores two-second units; odd seconds do not exist

  // Every field is checked, not just the ones that can overflow their bit
  // width. month and day have more bits than legal values (4 bits for 1-12,
  // 5 for 1-31), and hour/minute likewise, so a garbage entry -- or, on this
  // device, a directory-entry read that landed on something that is not a
  // directory entry at all -- reaches here with values no calendar has.
  if (month < 1 || month > 12) return 0;
  if (day < 1 || day > daysInMonth(year, month)) return 0;
  if (hour > 23 || minute > 59 || second > 58) return 0;

  return daysFromCivil(static_cast<int64_t>(year), month, day) * 86400 +
         static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(minute) * 60 +
         static_cast<int64_t>(second);
}

bool fromUnixSeconds(int64_t seconds, Fields& out) {
  // Range-check FIRST, on the input, so the arithmetic below never runs on a
  // value that could make the day count absurd. The bounds are the SETTER's
  // (see the header): 1980-01-01T00:00:00Z .. 2099-12-31T23:59:59Z.
  constexpr int64_t kMinSeconds = 315532800;   // 1980-01-01T00:00:00Z
  constexpr int64_t kMaxSeconds = 4102444799;  // 2099-12-31T23:59:59Z
  if (seconds < kMinSeconds || seconds > kMaxSeconds) return false;

  // Floor division is not needed: the range check above makes `seconds`
  // positive, so / and % are exact.
  const int64_t days = seconds / 86400;
  const int64_t rem = seconds % 86400;

  int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civilFromDays(days, &year, &month, &day);

  // Belt and braces: the range check makes this unreachable, but writing a
  // year that does not fit uint16_t (or that SdFat would reject) into the
  // out-param would be a silent corruption rather than a refusal.
  if (year < 1980 || year > 2099) return false;

  out.year = static_cast<uint16_t>(year);
  out.month = static_cast<uint8_t>(month);
  out.day = static_cast<uint8_t>(day);
  out.hour = static_cast<uint8_t>(rem / 3600);
  out.minute = static_cast<uint8_t>((rem % 3600) / 60);
  out.second = static_cast<uint8_t>(rem % 60);
  return true;
}

}  // namespace FatTimestamp
