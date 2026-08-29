#pragma once
// Host (Linux) config.h for the desktop smb2-server test harness.
//
// Per the Task 2 plan resolution (see ../README.md and
// .superpowers/sdd/2026-07-28-smb2-server/task-2-report.md): try
// lib/smb2/include/esp/config.h first, and only fall back to a host-specific
// config.h if that genuinely fails. It nearly works unmodified -- it already
// defines the right HAVE_* macros for the POSIX headers Linux also has, and
// ESP_PLATFORM (which gates libsmb2's own ESP-specific branches) is not
// defined on Linux, so those branches correctly stay off even though we're
// including a file called "esp/config.h".
//
// The one place it genuinely fails: HAVE_NETINET_TCP_H is #undef'd there
// (ESP-IDF's lwIP doesn't expose netinet/tcp.h the way upstream's autoconf
// probe expects), but real Linux libc genuinely has <netinet/tcp.h>, and
// lib/smb2/lib/socket.c needs TCP_NODELAY from it in three places
// (connect_async_ai(), smb2_bind_and_listen(), smb2_accept_connection_async()).
// Confirmed by an actual build failure, not a guess:
//   lib/smb2/lib/socket.c:1213:29: error: 'TCP_NODELAY' undeclared
// So: include the esp config as the base, then layer on the one correction.
#include "../../../lib/smb2/include/esp/config.h"

#ifndef HAVE_NETINET_TCP_H
#define HAVE_NETINET_TCP_H 1
#endif
