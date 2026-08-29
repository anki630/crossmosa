#pragma once

// Host build shim for lib/Logging/Logging.h. The real header
// (lib/Logging/Logging.h) `#include <HardwareSerial.h>` and is Arduino-only,
// so it cannot compile in this desktop harness -- see SmbServer.cpp's use of
// LOG_ERR/LOG_INF/LOG_DBG and test/host/README.md's "Host shims" section.
//
// This is a placement trick, not a code change to the device-side header:
// the Makefile puts `-Ihost_config` ahead of any path that could resolve a
// bare `#include <Logging.h>` to the real one (mirroring exactly how
// host_config/config.h already shadows lib/smb2/include/esp/config.h for
// the same reason -- see that file's own comment). The real
// lib/Logging/Logging.h is never touched, weakened, or built for the host.
//
// Deliberately minimal: just the three logging macros SmbServer.cpp uses,
// printed to stderr so `./smbhost`'s console output shows them inline with
// the harness's own fprintf diagnostics. No RTC ring buffer, no
// MySerialImpl, no getLastLogs()/clearLastLogs() -- those back crash-report
// persistence on the device and have no host equivalent to shim.
#include <cstdio>

#define LOG_ERR(origin, format, ...) std::fprintf(stderr, "[%s] ERR: " format "\n", origin, ##__VA_ARGS__)
#define LOG_INF(origin, format, ...) std::fprintf(stderr, "[%s] INF: " format "\n", origin, ##__VA_ARGS__)
#define LOG_DBG(origin, format, ...) std::fprintf(stderr, "[%s] DBG: " format "\n", origin, ##__VA_ARGS__)
