"""PlatformIO pre-build script: take the TCPIP core lock around the framework's
`dns_clear_cache()` in NetworkManager::hostByName (v111).

WHAT IT FIXES
-------------
Sporadic panic/reboot a few seconds after connecting to WiFi (seen as "OPDS
連線後偶發重開機"). The abort comes from lwIP's own core-locking assertion:

    assert failed: udp_new_ip_type ... Required configuration LWIP_ASSERT_CORE_LOCKED

Chain (verified this session by disassembly + source reading, not inferred):

    app task -> NetworkManager::hostByName()            (libraries/Network/src/NetworkManager.cpp)
             -> dns_clear_cache()                        RAW, no core lock held   <-- the bug
             -> dns_call_found(..., NULL)                dns_clear_cache aborts in-flight queries
             -> sntp_dns_found(NULL)                     SNTP's failure path
             -> sntp_try_next_server() -> sntp_request()
             -> dns_gethostbyname() -> dns_enqueue()
             -> udp_new_ip_type()                        LWIP_ASSERT_CORE_LOCKED -> abort()

esp-lwip's `dns_clear_cache()` does not just drop cache entries: it synchronously
invokes the callbacks of *in-flight* queries with a NULL address. SNTP reacts to
that failure by immediately issuing a fresh DNS query, which needs a new UDP pcb
— and every raw-API lwIP entry point asserts that the caller holds the TCPIP core
lock. `hostByName` runs on the Arduino app task and holds nothing, so the assert
fires. It is timing-dependent (needs an SNTP query in flight exactly when the
interface's IP state flips), which is why it presents as "sporadic".

Upstream arduino-esp32 still has the raw call on master as of 2026-08-07. The
framework's OWN idiom for this exact problem is in
`cores/esp32/esp32-hal-time.c:84-109` (configTzTime), which brackets its raw sntp
calls with `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` under
`#ifdef CONFIG_LWIP_TCPIP_CORE_LOCKING`. We apply the same idiom here, with one
improvement: we remember whether *we* took the lock and only unlock in that case.
hal-time.c re-queries holdership before unlocking, which would wrongly release a
lock it did not take if it were ever called while already holding one. Ours
cannot do that.

DEADLOCK ANALYSIS (why holding the lock across dns_clear_cache is safe)
-----------------------------------------------------------------------
Everything reachable under the held lock is raw lwIP API:
    dns_clear_cache -> dns_call_found -> sntp_dns_found -> sntp_try_next_server
                    -> sntp_request -> dns_gethostbyname -> dns_enqueue
                    -> udp_new / udp_bind / udp_sendto
The raw API never takes the core lock itself — it only *asserts* holdership
(that is the whole point of LWIP_ASSERT_CORE_LOCKED). Nothing in that cascade
goes through tcpip_api_call / netconn / the socket layer, which are the only
paths that would try to acquire it. So holding the lock through the call is
exactly what the core-locking design prescribes, and it cannot self-deadlock.
Hold time is µs-scale (a few pcb allocations and one UDP send at most).

WHY PATCH THE SHARED PACKAGE (and why that is acceptable here)
--------------------------------------------------------------
The file lives in the shared ~/.platformio package, not in this repo, so an
in-place hand edit would be silently wiped by any package reinstall/upgrade and
the crash would come back with no diff to show for it. This script therefore:
  * re-applies the patch on EVERY build (idempotent — marker-guarded, a no-op
    once patched, and the "already patched" check matches the WHOLE patched
    block byte-for-byte, never loose substrings — see PATCHED_PARTS),
  * HARD-FAILS the build if the pristine block is not found byte-for-byte, or
    is found more than once (patching one of two sites would silently leave a
    crashing one behind), and
  * writes atomically (tmp + os.replace), so an interrupted build can never
    leave a truncated source in the shared package for the next build to
    misdiagnose as upstream drift.
The hard fail is the contract: if upstream ever edits that block, the build stops
and a human re-derives the patch. Silently skipping would let the reboot return
unnoticed — the failure mode this whole script exists to prevent. Discipline
borrowed from scripts/verify_libsmb2_patch.py ("pin the pristine bytes, fail
loud on drift").

See also: CLAUDE.md 已知問題與排錯 (v111 entry).
"""

import os

Import("env")  # noqa: F821 — injected by PlatformIO

MARKER = "/* crosspoint-tc v111: dns_clear_cache TCPIP core lock (see scripts/patch_network_dns_lock.py) */"

REL_PATH = os.path.join("libraries", "Network", "src", "NetworkManager.cpp")

# --- the include -------------------------------------------------------------
# Anchored on the existing lwip/dns.h include (which is what declares
# dns_clear_cache). Guarded exactly like cores/esp32/esp32-hal-time.c:20-22 so
# targets built without core locking never pull in the private header.
ANCHOR_INCLUDE = '#include "lwip/dns.h"\n'
PATCHED_INCLUDE = (
    '#include "lwip/dns.h"\n'
    "#ifdef CONFIG_LWIP_TCPIP_CORE_LOCKING\n"
    '#include "lwip/priv/tcpip_priv.h"  // crosspoint-tc v111: LOCK_TCPIP_CORE / sys_thread_tcpip\n'
    "#endif\n"
)
INCLUDE_SENTINEL = '#include "lwip/priv/tcpip_priv.h"'

# --- the block ---------------------------------------------------------------
# Pinned byte-for-byte. If upstream touches a single character here the build
# fails loudly rather than shipping an unpatched (crashing) framework.
PRISTINE_BLOCK = """  // If the state of IP addresses has changed, clear the DNS cache
  if (hasGlobalV6 != hasGlobalV6Now || hasGlobalV4 != hasGlobalV4Now) {
    hasGlobalV6 = hasGlobalV6Now;
    hasGlobalV4 = hasGlobalV4Now;
    dns_clear_cache();
    log_d("Clearing DNS cache");
  }
"""

PATCHED_BLOCK = """  // If the state of IP addresses has changed, clear the DNS cache
  if (hasGlobalV6 != hasGlobalV6Now || hasGlobalV4 != hasGlobalV4Now) {
    hasGlobalV6 = hasGlobalV6Now;
    hasGlobalV4 = hasGlobalV4Now;
    %s
    // dns_clear_cache() synchronously fires in-flight DNS callbacks with NULL;
    // SNTP's failure path then allocates a UDP pcb, which asserts core-lock
    // holdership. hostByName runs on the app task and holds nothing -> abort().
    // Only unlock what we locked (unlike the re-query in esp32-hal-time.c).
#ifdef CONFIG_LWIP_TCPIP_CORE_LOCKING
    const bool crosspointLockedHere = !sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER);
    if (crosspointLockedHere) {
      LOCK_TCPIP_CORE();
    }
#endif
    dns_clear_cache();
#ifdef CONFIG_LWIP_TCPIP_CORE_LOCKING
    if (crosspointLockedHere) {
      UNLOCK_TCPIP_CORE();
    }
#endif
    log_d("Clearing DNS cache");
  }
""" % (
    MARKER,
)

# Shape assertions for the "already patched" fast path. The marker alone is not
# enough evidence that the patch is intact, and NEITHER ARE LOOSE SUBSTRINGS:
# "LOCK_TCPIP_CORE();" is a substring of "UNLOCK_TCPIP_CORE();", so a file with
# the acquisition deleted and only the release left would pass a naive check and
# ship firmware that unlocks a mutex it never took. The primary check is
# therefore the WHOLE patched block, matched exactly and exactly once; the named
# sub-blocks below exist only to say *which* part is missing in the error.
ACQUIRE_BLOCK = "\n    if (crosspointLockedHere) {\n      LOCK_TCPIP_CORE();\n    }\n"
RELEASE_BLOCK = "\n    if (crosspointLockedHere) {\n      UNLOCK_TCPIP_CORE();\n    }\n"
QUERY_LINE = "\n    const bool crosspointLockedHere = !sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER);\n"
PATCHED_PARTS = (
    ("holder query", QUERY_LINE),
    ("LOCK_TCPIP_CORE acquisition", ACQUIRE_BLOCK),
    ("dns_clear_cache call", "\n    dns_clear_cache();\n"),
    ("UNLOCK_TCPIP_CORE release", RELEASE_BLOCK),
)


def _fail(path, reason, expected_text=None, expected_label=None):
    banner = "*" * 78
    print(banner)
    print("patch_network_dns_lock: BUILD STOPPED — framework patch could not be applied")
    print("")
    print("  file   : %s" % path)
    print("  reason : %s" % reason)
    print("")
    # Show the text that actually failed to match, not always the pristine block:
    # a damaged marker or a missing include anchor is a different problem and
    # printing the wrong "expected" sends the next reader down the wrong path.
    if expected_text:
        print("  %s" % (expected_label or "Expected to find this exact text:"))
        for line in expected_text.strip("\n").split("\n"):
            print("    | %s" % line)
        print("")
    print("  The v111 fix is NOT applied. Without it, dns_clear_cache() runs from the")
    print("  app task without the TCPIP core lock and the device reboots sporadically")
    print("  after connecting to WiFi (udp_new_ip_type -> LWIP_ASSERT_CORE_LOCKED).")
    print("")
    print("  Most likely cause: the arduino-esp32 package was upgraded and upstream")
    print("  edited this block. Re-derive the patch against the new source, update")
    print("  PRISTINE_BLOCK/PATCHED_BLOCK in scripts/patch_network_dns_lock.py, and")
    print("  re-check that dns_clear_cache is still called without the lock upstream.")
    print(banner)
    env.Exit(1)  # noqa: F821
    # Belt and braces: if a future SCons/PlatformIO ever makes env.Exit() a
    # no-op during the script phase, this guarantees the build still stops.
    raise SystemExit(1)


def _write_atomically(path, content):
    """Write via a temp file in the same directory + os.replace.

    The target lives in the SHARED ~/.platformio package. A write interrupted
    halfway (Ctrl-C, full disk, power loss) would leave a truncated framework
    source there, and the next build would report it as "upstream drift" —
    sending the reader after a package upgrade that never happened. os.replace
    is atomic within a filesystem, so the file is either the old one or the new
    one, never a fragment. Same convention as PersistableStoreBase::writeDocAtomic.
    """
    tmp_path = path + ".crosspoint-tc.tmp"
    try:
        with open(tmp_path, "w") as f:
            f.write(content)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, path)
    except BaseException:
        if os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass
        raise


def main():
    package = env.PioPlatform().get_package("framework-arduinoespressif32")  # noqa: F821
    if not package:
        _fail("(framework-arduinoespressif32 not installed)", "platform package not found")
        return

    path = os.path.join(package.path, REL_PATH)
    if not os.path.isfile(path):
        _fail(path, "file does not exist (framework layout changed?)")
        return

    with open(path, "r") as f:
        content = f.read()

    if MARKER in content:
        # Exact whole-block match, exactly once — see the PATCHED_PARTS comment
        # for why substring sentinels are not sufficient here.
        if content.count(PATCHED_BLOCK) == 1:
            print("patch_network_dns_lock: already patched (%s)" % path)
            return
        missing = [name for name, text in PATCHED_PARTS if text not in content]
        if missing:
            reason = "marker present but the patch has been altered (missing: %s)" % ", ".join(missing)
        elif content.count(PATCHED_BLOCK) > 1:
            reason = "marker present but the patched block appears %d times (expected 1)" % content.count(PATCHED_BLOCK)
        else:
            reason = "marker present but the patched block no longer matches byte-for-byte"
        _fail(
            path,
            reason,
            PATCHED_BLOCK,
            "Expected the patched block to appear exactly once, byte-for-byte:",
        )
        return

    # Uniqueness matters as much as presence: if the block ever appeared twice,
    # replace(..., 1) would patch the first and silently leave a second crashing
    # call site behind. Same "exactly N sites" discipline as verify_libsmb2_patch.py.
    occurrences = content.count(PRISTINE_BLOCK)
    if occurrences != 1:
        reason = (
            "pristine dns_clear_cache block not found (upstream drift)"
            if occurrences == 0
            else "pristine dns_clear_cache block found %d times (expected exactly 1) — "
            "patching only the first would leave a crashing call site" % occurrences
        )
        _fail(path, reason, PRISTINE_BLOCK, "Expected to find this exact block, exactly once, in NetworkManager::hostByName:")
        return

    content = content.replace(PRISTINE_BLOCK, PATCHED_BLOCK, 1)

    if INCLUDE_SENTINEL not in content:
        if content.count(ANCHOR_INCLUDE) != 1:
            _fail(
                path,
                "include anchor not found exactly once (found %d)" % content.count(ANCHOR_INCLUDE),
                ANCHOR_INCLUDE,
                "Expected to find this include line, exactly once:",
            )
            return
        content = content.replace(ANCHOR_INCLUDE, PATCHED_INCLUDE, 1)

    _write_atomically(path, content)
    print("patch_network_dns_lock: applied TCPIP core lock around dns_clear_cache (%s)" % path)


main()
