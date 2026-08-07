#pragma once

// Gives files this device writes a real date -- but only when the device
// actually knows one.
//
// THE PROBLEM. Nothing in this firmware ever registered SdFat's FsDateTime
// callback, so every file the X3 created was stamped FS_DEFAULT_DATE
// ("1 January <build year>") for create, modify AND access, and its modify time
// was never updated on write. Files arriving from a computer carried real
// stamps; files this device wrote did not. A user asked why, having noticed it
// through the iOS Files app.
//
// THE CONSTRAINT THAT SHAPES EVERYTHING. The device usually does NOT know the
// date. Five measured samples across four sessions, every one of them taken
// with WiFi already connected:
//
//   SMB clock at start: epoch=74/196/277/483/685 -> 1970-01-01 00:0x:xx UTC
//
// Those epochs are seconds since boot: time() was returning uptime. So the
// failure mode to design against is not "no date", it is "a WRONG date that
// looks real". Today every device-written file shares ONE wrong date and they
// cluster together in a date-sorted listing, which reads as "these came from
// the machine". A clock that is merely running would give each file a
// DIFFERENT wrong timestamp and silently re-sort a real book library. Obviously
// fake beats plausibly wrong, and that is the user's own view as well.
//
// WHY REGISTRATION IS THE SWITCH, NOT THE RETURN VALUE. The obvious design --
// always register, return FS_DEFAULT_DATE when the clock is unknown -- is NOT
// equivalent to today, and the difference was measured on a host build of this
// exact vendored SdFat: 10 differing bytes on FAT and 10 on exFAT for an
// identical workload. SdFat's create path branches on whether a callback
// EXISTS: with one it writes only createDate/createTime/createTimeMs, without
// one it writes create AND modify AND access (FatFileLFN.cpp:426-441). So the
// only way to reproduce today exactly is to leave the pointer null.
//
// Hence: check first, register once, never unregister. That also makes the
// plain non-atomic global `FsDateTime::callback` safe against the render task
// -- SdFat tests the pointer and then calls it, so a null->set flip observed
// between the two loads simply takes the old branch. set->null would be the
// racy direction, and we never do it.

#include <ctime>

namespace SdDateTime {

// Registers SdFat's date/time callback iff the system clock is plausible.
// Cheap to call repeatedly: one bool test once it has succeeded, and at most
// one time() call per second before that. Safe from any task.
//
// Call it from anywhere the clock might have just become known -- SNTP lands
// asynchronously, seconds after the connect path has already returned, so a
// one-shot check at connect time would miss it.
void maybeRegister();

// For diagnostics only: was the callback live for this session?
bool isRegistered();

// Fills UTC now, and says whether the clock is trustworthy enough to use it.
// False leaves *out untouched -- callers must not stamp anything.
//
// v79: exists so createCmd can repair the one asymmetry a registered callback
// introduces. SdFat's create path branches on whether a callback exists: WITH
// one it writes only createDate/createTime, WITHOUT one it writes create AND
// modify AND access (FatFileLFN.cpp:426-441). So a file created and closed
// before any byte is written -- which is exactly SMB2_FILE_CREATE, mapped to
// O_CREAT|O_EXCL with no O_TRUNC -- ends up with modifyDate == 0 once the
// callback is live, and our CREATE reply then reports "no timestamp" for a file
// that plainly has one. Better to stamp it than to hand a client a zero.
bool nowUtc(struct tm* out);

}  // namespace SdDateTime
