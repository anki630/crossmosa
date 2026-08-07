# Desktop build for the smb2-server test harness (Task 2 of the
# .superpowers/sdd/2026-07-28-smb2-server/ plan). Builds vendored libsmb2
# (../../lib/smb2/) plus this harness's own main.cpp and POSIX stub_hal/
# natively for Linux, so SMB handler logic (Task 4's own
# ../../src/network/SmbFileHandlers.cpp) can be iterated on and tested in
# seconds instead of by flashing the X3 and pulling the SD card.
#
# See README.md for what this harness does and does not prove, and for the
# documented baseline failure this Task 2 version is expected to produce.

SMB2 := ../../lib/smb2

# Mirrors ../../lib/smb2/library.json's flags (see
# ../../docs/third-party/libsmb2-vendoring.md) so the host build sees exactly
# the same preprocessor/include surface the device build does -- with one
# deliberate difference: `-Ihost_config` comes BEFORE `-I$(SMB2)/include/esp`,
# so the quoted `#include "config.h"` (in both lib/smb2/lib/*.c and, via
# CrossPointSmb2.h, in main.cpp) resolves to host_config/config.h instead of
# the device's include/esp/config.h. See host_config/config.h for why a
# host-specific config.h is needed at all (short version: HAVE_NETINET_TCP_H
# is #undef'd in the esp one, and socket.c genuinely needs it on Linux).
#
# -D_U_ only needs a single quote layer here (unlike library.json's
# documented double-layer trick): a Makefile recipe line goes through exactly
# one shell parse, there's no intervening SCons/shlex pass to consume a layer
# of quoting first.
COMMON_FLAGS := -g -O0 -DHAVE_CONFIG_H -D_U_='__attribute__((unused))' \
                -Ihost_config \
                -I$(SMB2)/include -I$(SMB2)/include/smb2 -I$(SMB2)/include/esp -I$(SMB2)/lib \
                -Wno-unused-parameter -Wno-sign-compare

# -Wno-discarded-qualifiers is a C/ObjC-only warning flag (GCC rejects it for
# C++ with "valid for C/ObjC but not for C++"), so it's only added to CFLAGS,
# not CXXFLAGS -- matching library.json's set, which is C-only for the same
# reason (it's only ever applied to lib/smb2/lib/*.c).
CFLAGS   := $(COMMON_FLAGS) -Wno-discarded-qualifiers
# -Ihost_config comes first (see host_config/Logging.h's own comment): it now
# also shadows a bare `#include <Logging.h>` with a host shim, the same
# placement trick already used for config.h. -I../../src/network lets both
# main.cpp and SmbServer.cpp itself resolve "SmbServer.h"/"SmbFileHandlers.h"
# without a relative path.
#
# Task 4: SmbFileHandlers.cpp now does real HalStorage-backed work and needs
# two more headers. -I../../src (LAST, after -Istub_hal) lets
# `#include "util/ProtectedPath.h"` resolve to the real, dependency-free
# src/util/ProtectedPath.h/.cpp -- it has no Arduino/HAL dependency at all
# (see that header), so it is genuinely the same file the device links, not a
# stand-in; ordering it after -Istub_hal matters for the OTHER new include,
# `#include "util/DiagLog.h"`, which must keep resolving to
# stub_hal/util/DiagLog.h (a host shim -- the real DiagLog.cpp calls
# ESP-IDF's heap_caps_walk(), which doesn't exist here) rather than falling
# through to the real src/util/DiagLog.h now newly reachable via -I../../src.
#
# -I../../lib/GfxRenderer (Task 1 of v110 glyph-prefetch) lets
# warm_identity_test.cpp's `#include "WarmIdentity.h"` resolve to the real
# lib/GfxRenderer/WarmIdentity.h -- a pure header (no Arduino/HAL/Epd
# includes), so this is genuinely the same file the device compiles.
CXXFLAGS := $(COMMON_FLAGS) -std=gnu++2a -I../../include -I../../lib/Memory -I../../src/network -Istub_hal -I../../src -I../../lib/GfxRenderer

SMB2_SRC := $(wildcard $(SMB2)/lib/*.c)
SMB2_OBJ := $(notdir $(SMB2_SRC:.c=.o))

# Task 3 harness sources: main.cpp now drives SmbServer (../../src/network/)
# instead of calling libsmb2's own smb2_serve_port() directly. SmbFileHandlers.cpp
# is Task 3's stub table; Task 4 replaced its bodies with real HalStorage-backed
# logic, so stub_hal/HalStorage.cpp now actually gets exercised.
# ../../src/util/ProtectedPath.cpp is the real (dependency-free) shared
# path-protection ruleset SmbFileHandlers.cpp's create_cmd and
# src/network/WebDAVHandler.cpp both call -- see src/util/ProtectedPath.h.
#
# ../../src/util/FatTimestamp.cpp (Task 7) is the second such file: FAT packed
# date/time -> Unix seconds, with no Arduino/HAL/SdFat dependency either, so
# the harness links the same conversion the device runs.
HARNESS_SRC := main.cpp stub_hal/HalStorage.cpp \
               ../../src/network/SmbServer.cpp ../../src/network/SmbFileHandlers.cpp \
               ../../src/util/ProtectedPath.cpp ../../src/util/FatTimestamp.cpp

all: smbhost fat_time_test hid_report_map_test ble_button_latch_test memory_arena_test warm_identity_test

smbhost: $(HARNESS_SRC) $(SMB2_SRC)
	$(CC) $(CFLAGS) -c $(SMB2_SRC)
	$(CXX) $(CXXFLAGS) -o smbhost $(HARNESS_SRC) $(SMB2_OBJ)

# Standalone unit test for the FAT date conversion -- no libsmb2, no sockets,
# no server. See fat_time_test.cpp's header for why the wire tests in
# smb_smoke_test.py are not sufficient on their own for a date conversion.
fat_time_test: fat_time_test.cpp ../../src/util/FatTimestamp.cpp
	$(CXX) $(CXXFLAGS) -o fat_time_test fat_time_test.cpp ../../src/util/FatTimestamp.cpp

# Standalone unit tests for the BLE remote's pure-logic units — no libsmb2,
# no sockets, no stub HAL (same pattern as fat_time_test above).
hid_report_map_test: hid_report_map_test.cpp ../../src/util/HidReportMap.cpp
	$(CXX) $(CXXFLAGS) -o hid_report_map_test hid_report_map_test.cpp ../../src/util/HidReportMap.cpp

ble_button_latch_test: ble_button_latch_test.cpp ../../src/util/BleButtonLatch.cpp
	$(CXX) $(CXXFLAGS) -o ble_button_latch_test ble_button_latch_test.cpp ../../src/util/BleButtonLatch.cpp

memory_arena_test: memory_arena_test.cpp ../../lib/Memory/MemoryArena.cpp
	$(CXX) $(CXXFLAGS) -o memory_arena_test memory_arena_test.cpp ../../lib/Memory/MemoryArena.cpp

# Header-only test (Task 1 of v110 glyph-prefetch): WarmIdentity.h has no
# .cpp of its own to link, unlike the other standalone tests above.
warm_identity_test: warm_identity_test.cpp ../../lib/GfxRenderer/WarmIdentity.h
	$(CXX) $(CXXFLAGS) -o warm_identity_test warm_identity_test.cpp

.PHONY: all clean run check
check: fat_time_test hid_report_map_test ble_button_latch_test memory_arena_test warm_identity_test
	./fat_time_test
	./hid_report_map_test
	./ble_button_latch_test
	./memory_arena_test
	./warm_identity_test

clean:
	rm -f smbhost fat_time_test hid_report_map_test ble_button_latch_test memory_arena_test warm_identity_test *.o

# `make run` / `make run PORT=1234` -- runs in the foreground (Ctrl-C to stop).
PORT ?= 4450
run: smbhost
	./smbhost $(PORT)
