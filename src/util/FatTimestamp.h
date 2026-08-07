#pragma once

// FAT packed date/time -> seconds since the Unix epoch.
//
// Extracted into its own dependency-free file for the same two reasons
// src/util/ProtectedPath.{h,cpp} was (read that header too): it has no
// Arduino/HAL/SdFat dependency at all, so the desktop SMB2 harness
// (test/host/) links the exact file the device does rather than a stand-in,
// AND it can be exercised by a plain unit test (test/host/fat_time_test.cpp)
// instead of only through an SMB2 wire round trip. A date conversion tested
// only end-to-end is a conversion whose off-by-one-era bugs are invisible:
// every wrong answer still looks like a plausible date.
#include <cstdint>

namespace FatTimestamp {

// `date` and `time` are one FAT directory entry's packed 16-bit fields --
// exactly what SdFat's getModifyDateTime() hands back (FatFile.cpp:310-322
// reads them straight out of DirFat_t), and what HalFile::getModifyDateTime()
// forwards:
//
//   date: bits 15-9 = year - 1980, bits 8-5 = month (1-12), bits 4-0 = day (1-31)
//   time: bits 15-11 = hour (0-23), bits 10-5 = minute (0-59), bits 4-0 = second / 2
//
// Returns seconds since 1970-01-01T00:00:00, or **0 for "no time
// information"** -- which is both what an all-zero directory entry means and
// the answer for any field combination that is not a real calendar instant
// (month 0 or 13, 31 February, hour 24 ...). A corrupt or never-written entry
// must not turn into a confidently-wrong date on screen; MS-FSCC 2.4.7 gives
// 0 the explicit meaning "no time information", so callers can pass it
// straight through.
//
// TIME ZONE: FAT stores LOCAL time with no zone attached, while SMB2's
// FILETIME is UTC. There is nothing on the card to reconcile them with, so
// the value is taken as UTC. A file's displayed time is therefore off by the
// writing machine's UTC offset. Deliberate: the alternative is inventing an
// offset, and the purpose these timestamps serve (sorting a book list, "is
// this the copy I just made?") survives a constant shift, whereas every file
// sharing one date -- what a hardcoded zero produces -- destroys it.
int64_t toUnixSeconds(uint16_t date, uint16_t time);

// The decomposed calendar fields SdFat's setter takes.
struct Fields {
  uint16_t year;
  uint8_t month;   // 1-12
  uint8_t day;     // 1-31
  uint8_t hour;    // 0-23
  uint8_t minute;  // 0-59
  uint8_t second;  // 0-59
};

// The inverse of toUnixSeconds(), in the shape HalFile::setTimestamp() (and
// therefore SdFat's FatFile::timestamp()) wants: separate fields, not a packed
// pair. Returns false, leaving `out` untouched, when `seconds` is not
// representable.
//
// ⚠️ THE ACCEPTED RANGE IS 1980-2099, WHICH IS NARROWER THAN THE GETTER'S.
// FAT's 7-bit year field reaches 2107 and toUnixSeconds() will happily decode
// such a date, but SdFat's setter refuses anything past 2099 outright
// (`year < 1980 || year > 2099` -> `goto fail`, FatLib/FatFile.cpp:1278-1283),
// so a value this function accepted but SdFat rejected would turn into a
// mysterious write failure instead of a clear "out of range". The asymmetry is
// SdFat's, not this file's; it is enforced here so it is visible in one place.
bool fromUnixSeconds(int64_t seconds, Fields& out);

}  // namespace FatTimestamp
