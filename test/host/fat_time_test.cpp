// Direct unit test for src/util/FatTimestamp.cpp -- the FAT packed date/time
// -> Unix seconds conversion behind every timestamp the SMB2 server reports.
//
// WHY THIS EXISTS SEPARATELY from smb_smoke_test.py, which also checks
// timestamps: a date conversion tested only end-to-end is a conversion whose
// bugs are invisible. Every wrong answer still looks like a date, and a wire
// test can only compare against whatever the other side of the same code
// produced. The cases below are hand-computed constants -- leap days, century
// rules, the ends of FAT's representable range -- so they fail if the era
// arithmetic is wrong even when the round trip agrees with itself.
//
// Built and run by `make` / `make check` in this directory (see Makefile);
// smb_smoke_test.py also runs the binary so a single command covers both.

#include "util/FatTimestamp.h"

#include <cstdint>
#include <cstdio>

namespace {

int gFailures = 0;

uint16_t fatDate(unsigned year, unsigned month, unsigned day) {
  return static_cast<uint16_t>(((year - 1980) << 9) | (month << 5) | day);
}
uint16_t fatTime(unsigned hour, unsigned minute, unsigned second) {
  return static_cast<uint16_t>((hour << 11) | (minute << 5) | (second / 2));
}

void expectEq(const char* what, int64_t got, int64_t want) {
  if (got == want) {
    std::printf("PASS (%s): %lld\n", what, (long long)got);
    return;
  }
  std::printf("FAIL (%s): got %lld, want %lld\n", what, (long long)got, (long long)want);
  gFailures++;
}

}  // namespace

int main() {
  using FatTimestamp::toUnixSeconds;

  // --- The epoch itself. FAT's zero point is 1980-01-01, which is
  // 3652 days after the Unix epoch (1970 and 1974 through 1979 are 365-day
  // years; 1972 and 1976 are leap). 3652 * 86400 = 315532800 -- the constant
  // every "off by one era" bug moves.
  expectEq("1980-01-01 00:00:00 (FAT epoch)", toUnixSeconds(fatDate(1980, 1, 1), fatTime(0, 0, 0)), 315532800LL);

  // --- A known-good anchor, cross-checkable by hand:
  // 2000-01-01T00:00:00Z = 946684800.
  expectEq("2000-01-01 00:00:00", toUnixSeconds(fatDate(2000, 1, 1), fatTime(0, 0, 0)), 946684800LL);

  // --- Time-of-day packing, all three fields non-zero at once. Same day plus
  // 13:45:30 -> 13*3600 + 45*60 + 30 = 49530.
  expectEq("2000-01-01 13:45:30", toUnixSeconds(fatDate(2000, 1, 1), fatTime(13, 45, 30)), 946684800LL + 49530);

  // --- FAT's two-second resolution: an odd second is not representable and
  // truncates DOWN, never up (rounding up would report a file as modified
  // after it was).
  expectEq("odd seconds truncate down", toUnixSeconds(fatDate(2000, 1, 1), fatTime(13, 45, 31)),
           946684800LL + 49530);

  // --- Century rule. 2000 IS a leap year (divisible by 400) and 1900 is not,
  // which is the case a naive `year % 4` gets wrong -- and 2100 (divisible by
  // 100, not 400) is the one it gets wrong in the other direction, inside
  // FAT's range.
  expectEq("2000-02-29 exists (400-year rule)", toUnixSeconds(fatDate(2000, 2, 29), fatTime(0, 0, 0)),
           946684800LL + 59LL * 86400);
  expectEq("2100-02-29 rejected (100-year rule)", toUnixSeconds(fatDate(2100, 2, 29), fatTime(0, 0, 0)), 0);
  // 2100-03-01T00:00:00Z = 4107542400. If the 2100 leap rule were wrong this
  // lands a day out, which the previous check alone would not catch.
  expectEq("2100-03-01 (past the skipped leap day)", toUnixSeconds(fatDate(2100, 3, 1), fatTime(0, 0, 0)),
           4107542400LL);

  // --- Both ends of the representable range. 2107-12-31T23:59:58Z =
  // 4354819198; the year field is 7 bits, so 2107 is the last year and 2108
  // simply cannot be encoded.
  expectEq("2107-12-31 23:59:58 (last FAT instant)", toUnixSeconds(fatDate(2107, 12, 31), fatTime(23, 59, 58)),
           4354819198LL);

  // --- "No time information". Zero is what an entry that was never stamped
  // holds, and it must not become 1980.
  expectEq("all-zero entry", toUnixSeconds(0, 0), 0);
  expectEq("zero date, non-zero time", toUnixSeconds(0, fatTime(12, 0, 0)), 0);

  // --- Field-by-field rejection of impossible values. Each of these fits its
  // bit width but is not a calendar instant; the point is that garbage in a
  // directory entry becomes "no date" rather than a confident wrong one.
  expectEq("month 0", toUnixSeconds(static_cast<uint16_t>((20u << 9) | (0u << 5) | 1u), 0), 0);
  expectEq("month 13", toUnixSeconds(static_cast<uint16_t>((20u << 9) | (13u << 5) | 1u), 0), 0);
  expectEq("day 0", toUnixSeconds(static_cast<uint16_t>((20u << 9) | (1u << 5) | 0u), 0), 0);
  expectEq("31 April", toUnixSeconds(fatDate(2000, 4, 31), 0), 0);
  expectEq("30 February", toUnixSeconds(fatDate(2000, 2, 30), 0), 0);
  expectEq("29 February in a common year", toUnixSeconds(fatDate(2001, 2, 29), 0), 0);
  expectEq("hour 24", toUnixSeconds(fatDate(2000, 1, 1), static_cast<uint16_t>(24u << 11)), 0);
  expectEq("minute 60", toUnixSeconds(fatDate(2000, 1, 1), static_cast<uint16_t>(60u << 5)), 0);

  // --- Monotonicity across a month and a year boundary. Cheap, and it catches
  // a whole class of "the answer is plausible but the arithmetic is wrong"
  // errors the fixed anchors above cannot see on their own.
  //
  // The sweep tracks its OWN failure count. It used to print its PASS line
  // only when `gFailures == 0`, i.e. an unrelated earlier failure silently
  // suppressed this check's result -- and since smb_smoke_test.py reports the
  // number of "PASS (" lines, the reported check total shifted with it.
  int sweepFailures = 0;
  for (unsigned year = 1980; year <= 2107; ++year) {
    int64_t prev = 0;
    for (unsigned month = 1; month <= 12; ++month) {
      const int64_t s = toUnixSeconds(fatDate(year, month, 1), fatTime(0, 0, 0));
      if (s <= prev) {
        std::printf("FAIL (monotonic): %u-%02u-01 gave %lld, not greater than %lld\n", year, month,
                    (long long)s, (long long)prev);
        sweepFailures++;
        gFailures++;
      }
      prev = s;
    }
  }
  if (sweepFailures == 0) std::printf("PASS (monotonic across 1980-2107, month by month)\n");

  // --- fromUnixSeconds(): the inverse, used by set_info's FILE_BASIC handler.
  // A ROUND TRIP over every hour of the representable range is the strongest
  // cheap statement available -- it fails if either direction drifts, and it
  // covers every leap day and every month boundary without listing them.
  {
    int roundTripFailures = 0;
    int64_t checked = 0;
    // Every 3607 seconds: coprime with 86400 and with 2, so it walks all
    // times of day and lands on odd seconds (which must truncate down).
    for (int64_t t = 315532800; t <= 4102444799LL; t += 3607) {
      FatTimestamp::Fields f{};
      if (!FatTimestamp::fromUnixSeconds(t, f)) {
        std::printf("FAIL (round trip): fromUnixSeconds(%lld) refused an in-range value\n", (long long)t);
        roundTripFailures++;
        break;
      }
      const int64_t back = toUnixSeconds(fatDate(f.year, f.month, f.day), fatTime(f.hour, f.minute, f.second));
      // FAT's two-second resolution is applied by fatTime() here, exactly as
      // the filesystem would, so an odd input second comes back one lower.
      const int64_t want = t - (t % 2);
      if (back != want) {
        std::printf("FAIL (round trip): %lld -> %04u-%02u-%02u %02u:%02u:%02u -> %lld, want %lld\n",
                    (long long)t, f.year, f.month, f.day, f.hour, f.minute, f.second, (long long)back,
                    (long long)want);
        roundTripFailures++;
        break;
      }
      checked++;
    }
    gFailures += roundTripFailures;
    if (roundTripFailures == 0) {
      std::printf("PASS (round trip over %lld instants, 1980-2099, every 3607 s)\n", (long long)checked);
    }
  }

  // --- fromUnixSeconds()'s range is the SETTER's (1980-2099), deliberately
  // narrower than the getter's 2107: SdFat's FatFile::timestamp() refuses
  // anything past 2099 outright, so accepting it here would turn a clear
  // refusal into a mysterious write failure.
  {
    FatTimestamp::Fields f{};
    expectEq("fromUnixSeconds refuses 1979", FatTimestamp::fromUnixSeconds(315532799, f) ? 1 : 0, 0);
    expectEq("fromUnixSeconds accepts the 1980 epoch", FatTimestamp::fromUnixSeconds(315532800, f) ? 1 : 0, 1);
    expectEq("fromUnixSeconds accepts 2099-12-31 23:59:59", FatTimestamp::fromUnixSeconds(4102444799LL, f) ? 1 : 0, 1);
    expectEq("fromUnixSeconds refuses 2100 (setter range, not getter range)",
             FatTimestamp::fromUnixSeconds(4102444800LL, f) ? 1 : 0, 0);
    // A hand-checked decomposition, so the round trip above cannot be passing
    // by two cancelling errors: 2000-02-29T13:45:31Z.
    // 2000-02-29T13:45:31Z, cross-checked against Python's calendar.timegm.
    FatTimestamp::fromUnixSeconds(951831931LL, f);
    expectEq("2000-02-29 13:45:31 -> year", f.year, 2000);
    expectEq("2000-02-29 13:45:31 -> month", f.month, 2);
    expectEq("2000-02-29 13:45:31 -> day", f.day, 29);
    expectEq("2000-02-29 13:45:31 -> hour", f.hour, 13);
    expectEq("2000-02-29 13:45:31 -> minute", f.minute, 45);
    expectEq("2000-02-29 13:45:31 -> second", f.second, 31);
  }

  if (gFailures != 0) {
    std::printf("\nFAIL: %d check(s) failed\n", gFailures);
    return 1;
  }
  std::printf("\nPASS: all FatTimestamp checks passed\n");
  return 0;
}
