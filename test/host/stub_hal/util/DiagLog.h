#pragma once
// Desktop stand-in for src/util/DiagLog.h (see that header). The real
// DiagLog writes to a diagnostic log file on the SD card and walks the
// device's heap pools via ESP-IDF's heap_caps_walk() -- neither of which
// exists on this POSIX harness. Task 4's SmbFileHandlers.cpp calls
// DiagLog::line() only (never mem()/dumpPools()), to log rare/rejected
// requests (auth failures, protected-path denials, handlers Task 4
// intentionally still refuses) -- exactly the kind of thing this harness's
// whole point is to let a human watching stderr see. This stub prints
// straight to stderr instead of the SD card; begin()/mem()/dumpPools() are
// here only so this header type-checks identically to the real one (see
// SmbFileHandlers.cpp's own includes), and do nothing.
//
// Picked up via test/host/Makefile's `-Istub_hal` (already used for
// HalStorage.h) -- SmbFileHandlers.cpp's `#include "util/DiagLog.h"`
// resolves to stub_hal/util/DiagLog.h here instead of the real
// src/util/DiagLog.h/.cpp pair, the same placement trick host_config/
// already uses for config.h and Logging.h.
#include <cstdarg>
#include <cstdio>

namespace DiagLog {

inline void begin() {}
inline bool setForced(bool, const char* = "forced") { return false; }  // host stub is always on (stderr)
inline void mem(const char*) {}

inline void line(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

inline void dumpPools(unsigned, const char*) {}

}  // namespace DiagLog
