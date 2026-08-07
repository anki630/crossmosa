#pragma once
// Desktop stand-in for src/util/SdDateTime.h (read that header for what the
// real one does and why). Picked up via test/host/Makefile's `-Istub_hal`, the
// same placement trick as stub_hal/util/DiagLog.h.
//
// SmbFileHandlers.cpp uses this for exactly one thing: the `cb=` field on the
// "SMB clock at start" diag line, which reports whether SdFat's date/time
// callback is live for the session. There is no SdFat on this harness -- the
// POSIX stub gets timestamps free from the kernel -- so the honest answer here
// is "no callback", and it is a constant.
//
// ⚠️ DO NOT WRITE A HARNESS TEST FOR FILE TIMESTAMPS. stub_hal/HalStorage.cpp
// implements getModifyDateTime() with ::stat() + gmtime_r(&st.st_mtime, ...),
// so the kernel supplies a real mtime whether or not the firmware registers a
// callback: such a test is green with the feature removed. That is this
// project's stub/device divergence number nine, and v74 is the standing
// reminder of what a suite with zero real coverage lets ship. Timestamp
// behaviour is verified on hardware, via the diag.log line.

#include <ctime>

namespace SdDateTime {

inline void maybeRegister() {}
inline bool isRegistered() { return false; }
// No SdFat here, so the clock is never "trustworthy enough to stamp with" --
// the POSIX stub gets timestamps from the kernel for free.
inline bool nowUtc(struct tm*) { return false; }

}  // namespace SdDateTime
