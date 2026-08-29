# SMB2 server desktop test harness

Task 1-4 of the `smb2-server` plan (see
`.superpowers/sdd/2026-07-28-smb2-server/`). Task 1 vendored libsmb2 into
`lib/smb2/`; this harness builds that library plus this project's own SMB
handler code **natively for Linux**, against a POSIX stand-in for the
firmware's storage HAL, so Tasks 4-7 can iterate on and test SMB2 handler
logic in seconds instead of by flashing the X3 and pulling the SD card. The
X3 has no serial port, so this harness is the difference between debuggable
and not.

## What this harness does NOT do

**It does not test iOS/Files-app compatibility.** That risk is being carried
deliberately elsewhere in the plan and is explicitly out of scope here. This
harness's job is: does our handler logic do the right thing given an SMB2
request, decoded by a real SMB2 client library talking a real SMB2 wire
protocol. Whether iOS's specific SMB2 client behaves identically to
`smbprotocol` (the Python client used here) is a separate question this
harness cannot answer and isn't trying to.

As of Task 4 round 5, `SmbFileHandlers.cpp` has real handlers for
authentication, tree_connect, create/close, and the eleven
trivial-but-not-optional handlers, **and a client sending the same dialect
list a real modern client (including, reportedly, iOS) would now
negotiates SMB 3.0.2 and reaches tree_connect/create/close successfully
with zero client-side bypasses** -- see "Baseline (Task 4, round 3,
current)" below. `SmbServer::acceptOneConnection()` pins every accepted
connection's dialect ceiling to 3.0.2 (`smb2_set_version()`), so the
still-unfixed SMB 3.1.1 signing bug found in round 2 is never reachable at
all -- a client offering only 3.1.1 now fails cleanly at NEGOTIATE instead.
Task 5 then added `query_directory` and `query_info`, so a client can now
**list the share** (see "Baseline (Task 5 ...)" below); Task 6 added
`read`/`write`/`flush`, and Task 7 real file timestamps plus `set_info`
(rename / delete / set size) -- the last of which needed a third divergence in
the vendored library to be reachable at all.
Task 3 also found and resolved a
deeper gap along the way (a small patch to `lib/smb2/`, needed for
`SmbServer` to bootstrap accepted connections at all) -- see "Current
state" and `docs/third-party/libsmb2-vendoring.md` for that history.

## Layout

```
test/host/
  main.cpp                  Harness entry point (Task 3 -- see below)
  Makefile                   Builds libsmb2 + this harness for the host
  stub_hal/
    HalStorage.h/.cpp        POSIX stand-in for lib/hal/HalStorage.h, so
                             src/network/SmbFileHandlers.cpp's real
                             HalStorage-backed logic (Task 4) compiles
                             unmodified against either this or the device HAL
    util/DiagLog.h           Header-only host stub for src/util/DiagLog.h
                             (Task 4) -- prints to stderr instead of writing
                             an SD diag.log/walking ESP-IDF heap pools
  host_config/
    config.h                 Host-specific config.h (see "config.h" below)
    Logging.h                 Host shim for lib/Logging/Logging.h (see "Host
                             shims" below)
  smb_smoke_test.py           Python SMB2 client smoke test
  .venv/                      (gitignored) smbprotocol's isolated virtualenv
  sdroot/                     (gitignored) default SMBHOST_ROOT scratch directory
```

`src/util/ProtectedPath.{h,cpp}` (Task 4) is NOT stubbed -- it has no
Arduino/HAL dependency at all, so the host build links the exact same file
the device does (see the Makefile's `-I../../src` comment and
`src/util/ProtectedPath.h`'s own header comment for why it was extracted
there instead of duplicated). `src/util/FatTimestamp.{h,cpp}` (Task 7, FAT
packed date/time -> Unix seconds) is the second such file, extracted for the
same reason plus one more: a date conversion needs a unit test with
hand-computed expected values, which `test/host/fat_time_test.cpp` gives it.
`make` builds both `smbhost` and `fat_time_test`; `make check` runs the
latter, and `smb_smoke_test.py` also invokes it so one command covers both.

### `warm_identity_test` (v110 glyph-prefetch, Task 1)

Unrelated to the SMB2 server -- this harness's Makefile also builds a handful
of standalone, header/logic-only unit tests with no libsmb2 and no stub HAL
(`fat_time_test`, `hid_report_map_test`, `ble_button_latch_test`,
`memory_arena_test`, and now this one), because they need the same fast
host-side iteration loop. `lib/GfxRenderer/WarmIdentity.h` (Task 1 of the
2026-08-07 glyph-prefetch plan) is a pure header -- no Arduino/HAL/Epd
includes -- so `warm_identity_test.cpp` compiles it directly with
`-I../../lib/GfxRenderer`; there is no `.cpp` to link. It asserts that
`WarmIdentity::matches()` rejects a mismatch on each of its 14 fields
independently (one assertion per field, not one combined assertion), that a
`valid=false` on either side never matches regardless of field equality, and
that `invalidate()` is the single choke point that clears `valid`. Per
`test/host/README.md`'s mutation-testing convention (see the two tables
above), each of the following was applied, confirmed to turn exactly the
named checks red, then reverted before commit:

| Test | Mutation | Result |
|---|---|---|
| `warm_identity_test` | `matches()` body reduced to just `pageNumber == cur.pageNumber` (kept `valid && cur.valid`) | 13 field-mismatch checks fail |
| `warm_identity_test` | `valid && cur.valid &&` removed from `matches()` | 3 checks fail: `stored-invalid never matches`, `probe-invalid never matches`, `invalidate clears valid` |
| `warm_identity_test` | `invalidate()` body emptied (no-op) | 1 check fails: `invalidate clears valid` |

## Building and running

```bash
cd test/host
make                      # builds ./smbhost
./smbhost [port]          # default 4450 (unprivileged, no root needed)
```

In another terminal:

```bash
cd test/host
python3 -m venv .venv && .venv/bin/pip install smbprotocol   # first time only
.venv/bin/python3 smb_smoke_test.py [port]                    # default 4450
```

Or in one shot (what Task 2 was actually verified with):

```bash
cd test/host && make && (./smbhost 4450 &) && sleep 1 && .venv/bin/python3 smb_smoke_test.py 4450
```

`make clean` removes `smbhost` and the compiled `.o` files. `make run` (or
`make run PORT=1234`) builds and runs in the foreground.

### `SMBHOST_ROOT`

`stub_hal/HalStorage` resolves every device-style absolute path (the SD card
is `/` on device) under a real host directory named by the `SMBHOST_ROOT`
environment variable, defaulting to `./sdroot` (created on first use). Point
it at any directory to control what "the SD card" contains for a given test
run, e.g.:

```bash
SMBHOST_ROOT=/tmp/my-test-sd ./smbhost 4450
```

As of Task 4, `main.cpp` calls `Storage.begin()` on startup (mirroring
`src/main.cpp`'s own boot sequence, which does this long before any network
service starts) -- `SmbFileHandlers.cpp`'s `create_cmd` now does real
`Storage.*`/`HalFile` I/O, which needs the root directory to exist first.
Running `smbhost` now creates `./sdroot` (or `$SMBHOST_ROOT`) on startup if
it doesn't already exist.

## Current state (Task 3)

`main.cpp` now drives `SmbServer` (`src/network/SmbServer.{h,cpp}`, Task 3)
and the real (stub) `smbGetRequestHandlers()` (`src/network/SmbFileHandlers.{h,cpp}`,
also added by Task 3 to break a circular dependency -- see that file's header
comment) instead of calling libsmb2's own blocking `smb2_serve_port()`
directly the way Task 2's version did.

An earlier revision of this section documented a genuine gap found while
building `SmbServer`: `SmbServer::tick()` could not finish bootstrapping a
freshly-accepted connection into libsmb2's negotiate state machine, because
the two things that step needs (`struct connect_data`, a `static` callback
`smb2_negotiate_request_cb`) have zero visibility outside
`lib/smb2/lib/libsmb2.c`. **That gap is now resolved** — see
`docs/third-party/libsmb2-vendoring.md`'s "The one patch" section and
`.superpowers/sdd/2026-07-28-smb2-server/task-3-report.md` for the full
history (the finding, the decision to patch upstream rather than move
`SmbServer` onto its own FreeRTOS task, and why). Short version: one small
function, `crossmosa_smb2_finish_accept()`, is now appended to the very end
of `lib/smb2/lib/libsmb2.c` (nothing above it touched, verified by
`scripts/verify_libsmb2_patch.py`), and `SmbServer::acceptOneConnection()`
calls it.

## Baseline (Task 7 -- timestamps and set_info, current)

**67 checks, all passing.** Task 7 added 19: 5 for timestamps, 8 for
`set_info`, 3 in fix round 2 and 3 in fix round 3 for the review findings
below.

⚠️ **RUN THE SUITE AGAINST A FRESH `SMBHOST_ROOT`.** It is not idempotent: many
tests seed a known filesystem state and assert on the result, so a second run
against a reused root fails on leftovers from the first. This is
**pre-existing** -- running even the Task 4 base suite twice against one root
fails on the second run -- but it will otherwise cost the next person an
afternoon. `rm -rf` the root, or point `SMBHOST_ROOT` somewhere new, between
runs.

`set_info` needed a **third divergence in the vendored library** before it
could be written at all: upstream refused every SET_INFO payload from a
DECODE function, which tore the connection down instead of failing the one
request. That divergence is now approved, applied and pinned -- see
`docs/third-party/libsmb2-vendoring.md`, "The third patch", and "What blocked
`set_info`, and what it cost" below.

- `fat_time_test` — a standalone unit test for the FAT date conversion
  (`src/util/FatTimestamp.cpp`), run by `make check` and also invoked by
  `smb_smoke_test.py` so one command covers everything. 19 checks against
  hand-computed constants: the FAT epoch, leap days under the 4/100/400 rules
  (2000 yes, 2100 no), both ends of the representable range (1980-01-01 and
  2107-12-31T23:59:58), two-second truncation, and per-field rejection of
  impossible values (month 13, 31 April, hour 24, ...), plus a
  month-by-month monotonicity sweep of all 128 representable years.
- `query_info` timestamps: a file whose mtime is set on disk to a fixed
  instant must report exactly that instant in all four MS-FSCC fields of
  `FILE_BASIC`, `FILE_NETWORK_OPEN` and `FILE_ALL`. Decoded from the raw wire
  bytes as a FILETIME, not via `smbprotocol`'s `DateTimeField`, so the
  assertion is about what was actually sent.
- Directory-listing timestamps: three files with three *different* mtimes in
  one listing. Distinct on purpose — a single-file check would pass against an
  implementation that read one entry's date and reused it for all of them,
  which is exactly what the previous behaviour (one shared value: zero) was.
- A file created and written *through this server* reports a real, recent
  timestamp. This is the case the feature exists for: iOS copies a book over
  and then shows the folder.
- The share root reports "no time information", not a date.

**3 of the 5 were run against the pre-change build and all 3 failed** with
`reported NO timestamp (0)` — every timestamp field was zero before this task,
so every file in the Files app carried the same date. The other two cannot
fail that way and were proved by **mutation** instead, which is worth
recording because "it passed against the old build too" is otherwise
indistinguishable from "it tests nothing":

| Test | Mutation | Result |
|---|---|---|
| `fat_time_test` | `- 719468` → `- 719467` in `daysFromCivil` (one-day epoch shift) | 8 checks fail |
| `fat_time_test` | `isLeapYear` → naive `y % 4 == 0` | 2100-02-29 wrongly accepted, 1 check fails |
| share root | delete the `path == "/"` guard in `modifyTimeOf()` | fails: reports the root's meaningless date |

### Timestamps: three device facts worth carrying forward

**1. The share root has no directory entry, and asking anyway SUCCEEDS with
garbage.** `FatFile::openRoot()` memsets the file object
(`FatLib/FatFile.cpp:697-724`), so `m_dirSector` stays 0; `FatFile::dirEntry()`
then calls `sync()` — which returns true for a clean read-only handle — and
reads **sector 0**, the boot sector, as though it were a directory entry
(`FatFile.cpp:200-219`, `:310-322`). So `getModifyDateTime()` on the share root
returns `true` holding a plausible-looking, meaningless date. The handler skips
the root for this reason. **The stub deliberately does NOT special-case the
root** (it returns the host directory's own mtime): same observable shape,
"succeeds, means nothing", and it is what makes the guard testable — refusing
instead would have been tidier and would have let the guard be deleted
silently.

**2. Nothing in this firmware registers an `FsDateTime` callback.** A file the
X3 itself creates therefore gets `FS_DEFAULT_DATE` (`SdFatConfig.h:311`,
"1 January *compile year*") and **no modify-time update on write at all**
(`FatFile::sync()`'s `if (FsDateTime::callback)` at `FatFile.cpp:1254` simply
does not fire). Books copied from a computer carry real dates; anything the
device writes — OPDS downloads especially — will all share one. Not fixed here:
registering a global date/time callback against the device clock changes every
write in the whole firmware, which is a project-level change, not a change to
an SMB handler. Worth knowing before reading much into an on-screen date.

**3. FAT stores LOCAL time; FILETIME is UTC.** There is nothing on the card to
reconcile them with, so the value is taken as UTC and a file's displayed time
is off by the writing machine's UTC offset. Deliberate — the alternative is
inventing an offset, and sorting survives a constant shift.

### The 8 `set_info` checks

- **Rename**: a file, a directory, across directories (contents verified after
  the move), and **Traditional Chinese on both sides** -- this user's library
  is entirely Chinese, so a UTF-16 decode bug here breaks everything. Every
  case inspects the filesystem afterwards, not just "the call did not raise":
  a rename that reports success and leaves the old name in place is the
  failure worth catching.
- **Rename onto an existing name**, both ways: `ReplaceIfExists=0` must be
  refused with **both** files byte-intact, `ReplaceIfExists=1` must actually
  replace (destination bytes checked, source name gone).
- **Rename and path protection in BOTH directions.** Renaming *out of*
  `/.crossmosa` exfiltrates the Wi-Fi credentials exactly as effectively as
  renaming into it -- the file becomes readable the moment it is called
  something else.
- **Delete-on-close**: a file (present until close, gone after -- it is a flag,
  not an immediate delete), an empty directory, a **non-empty** directory
  (refused, contents verified intact), and a protected path.
- **Delete-on-close where the CLOSE itself fails** (SYNCFAIL injector): the
  failure is reported, not swallowed, and the slot is still freed (proved by
  opening all 8 slots afterwards).
- **Set size**: smaller, exactly current, zero, larger, absurdly larger, and
  on a read-only handle. **Bytes are verified, not the reported length** --
  including that the grown region really is zeros and the original prefix is
  untouched.
- **Rejections are per-request**: seven malformed/unsupported requests in a
  row (unknown class, unknown info_type, truncated payload, non-zero
  RootDirectory, FileNameLength past the buffer, `..` traversal, zero-length
  payload) and the session is still fully usable afterwards. That last clause
  is the whole point of the vendored patch.
- **Owner check** (connection B cannot rename or delete connection A's handle,
  with A's file id forged) plus the no-op `FILE_BASIC_INFORMATION` rule.

**All 8 were run against the pre-change build and all 8 failed**, every one of
them with `SMBConnectionClosed: SMB socket was closed` -- not a rejection, the
session dying. The other 53 checks passed unchanged.

Two of the timestamp checks cannot fail that way and were proved by
**mutation** instead (`fat_time_test` tests code that did not exist before;
the share-root check trivially passed when every timestamp was zero):

| Test | Mutation | Result |
|---|---|---|
| `fat_time_test` | `- 719468` → `- 719467` in `daysFromCivil` | 8 checks fail |
| `fat_time_test` | `isLeapYear` → naive `y % 4 == 0` | 2100-02-29 accepted, 1 check fails |
| share root | delete the `isShareRootPath()` guard in `modifyTimeOf()` | fails -- reports the root's meaningless date |

**The rename source-side protection check needed a mutation too, and the
reason is worth recording**: it is unreachable from the wire, because
`createCmd` already refuses to open a protected path, so no handle can exist
for one. It is a backstop, and a backstop that is never exercised is
indistinguishable from a broken one. Proved in two steps: with `createCmd`'s
protection removed the protected file *was* opened and the rename out was
**still refused** (by the source check); with the source check removed as
well, the test reports `UNEXPECTEDLY SUCCEEDED -- credentials exfiltrated`.

### Fix round 2: three findings the harness could not see by itself

Two of these were **new stub divergences**, both in the direction that hides a
device failure, and the reviewer found them with probes rather than by
reasoning about the code.

**1. Delete-on-close re-checked nothing.** `setDisposition()` refuses while a
second handle holds the path, but that is a check at ONE MOMENT: `createCmd`
has no same-path guard, so a second handle can be opened *after* the
disposition and before the close. Reproduced:

```
PROBE1: second open of a delete-pending path was ALLOWED
PROBE1: after A.close(), file deleted=True while B still holds a handle=True
```

On device that runs `Storage.remove()` while a live `FatFile` caches the
directory entry and cluster chain being freed. **POSIX `unlink()` is
refcounted, so the harness structurally cannot see the corruption** -- which is
why the check now lives in `deleteOnClose()` itself (after `releaseSlot()` has
cleared the closing handle's own entry, so `otherHandleOn(nullptr, path)` finds
only genuine second holders) and why the test asserts the REFUSAL, which is
observable on both.

**2. `FILE_BASIC_INFORMATION` was refused far more widely than intended, and
the fix was to stop negotiating and add the setter.** The attribute check
refused whenever `attributes != currentAttributes`, and `currentAttributes` is
only ever `NORMAL` (0x80) or `DIRECTORY` (0x10) -- so `ARCHIVE` (0x20), the
ordinary attribute a client sets on a freshly copied file, was refused **even
when every timestamp field said "do not change"**. The harness only ever sent
`attributes = 0`, so neither direction was tested.

`HalFile::setTimestamp()` now wraps `FatFile::timestamp()` (the third "add it
to the HAL, never reach around it" move in this task) and timestamps are
**applied**, not accepted and dropped -- the explicitly unacceptable option,
and the antipattern Task 6 spent two rounds paying for. `ARCHIVE` and `NORMAL`
are treated as bits this server does not model; `READ_ONLY`, `HIDDEN` and
`SYSTEM` are still refused with a log.

FAT has no ChangeTime field, so ChangeTime maps onto the same single modify
stamp as LastWriteTime -- which is what `query_info` here already reports, so
the result is self-consistent and a following query observes exactly what was
stored. If both are present and differ, LastWriteTime wins and the coalescing
is logged.

**3. The rename divergence row modelled exact-name collision only.**
Reproduced:

```
PROBE2: ReplaceIfExists=0 rename source.txt -> VICTIM.TXT : err=None, dir now ['VICTIM.TXT', 'victim.txt']
```

The harness accepted a rename producing two files that cannot coexist on FAT.
It was also an internal inconsistency: `otherHandleOn()` uses `strcasecmp`
*because FAT is case-insensitive*, and the very next check,
`Storage.exists(newPath)`, relied on the stub's POSIX case-sensitivity. The
stub now case-folds within a directory (`resolveExistingCase()`, used by
`exists()`, `open()`, `remove()`, `rmdir()` and both renames), so both
`ReplaceIfExists` values against a case-variant are now real paths.

**All three new checks fail against `4d5e05a`**, with exactly the reviewer's
symptoms: `THE FILE WAS DELETED while B still held it`; `set_info BASIC`
rejected with `0xc0000002`; and `SUCCEEDED, directory now ['VICTIM.TXT',
'victim.txt']`. The other 61 checks passed unchanged.

**Also in round 2**: `ReplaceIfExists` no longer removes the destination
before renaming. It moves it aside to a holding name first -- `dest -> temp`,
`src -> dest`, `remove temp`, rolling `temp -> dest` back if the middle step
fails -- so **both files are on the card under some name at every intermediate
point**. Deletes are irreversible on this device; the extra steps are worth it.

### Fix round 3: two findings introduced BY the round-2 fixes

Both were in the round-2 diff itself, which is worth stating plainly: fixing
review findings is where new ones come from in this project's history.

**1. `setBasicInfo()` applied part of a request it then reported as rejected.**
Validation happened *inside* the apply loop, so
`{LastAccessTime = valid, LastWriteTime = year 2100}` wrote the access stamp
and then returned `-1`. Measured on the harness: atime moved
`1000000000 → 1715938200` while the request failed with `0xc0000002`.

Two things made it worse than an ordinary bug. It contradicted **the
function's own invariant forty lines above** -- "refusing after having already
written timestamps would leave the request half-applied", which is why the
attribute check is done first; the comment was right and the loop below it was
not. And the log line said `reject` for a partially-applied request: on a
device with no serial port `diag.log` is the only evidence that will ever
exist, and a wrong line ends the investigation in the wrong place.

Fixed by hoisting: all three fields are converted and range-checked first, and
only then applied. `test_set_basic_all_or_nothing` asserts **every** timestamp
is untouched after a mixed request, in **both** orderings -- a loop that
stopped at the first bad field would pass one of them by luck. What remains
possible is a partial *card* failure across three separate directory-entry
writes, which cannot be made atomic; the failure log says so instead of
pretending otherwise.

**2. Seventh stub-looser divergence: `setTimestamp()` on the share root.** The
stub reproduced SdFat's calendar validation but not its object-kind guard:
`FatFile::timestamp()` opens with `isFileOrSubDir()` (`FatFile.cpp:1280`) and
the FAT root is neither, so the device refuses while the stub accepted. The
device behaviour was already safe; what was broken was harness fidelity, in
exactly the direction round 2 existed to close. Guard added, table row added
(the setter counterpart of the getter's share-root row), test added.

### Eighth divergence (v74) — and the first where the stub being LOOSER was fatal

`HalFile::close()` on a handle with no `impl`. The device asserts and aborts;
the stub answered `return false`.

`lib/hal/HalStorage.cpp`'s `HAL_FILE_WRAPPED_CALL` is

```c
StorageLock lock; assert(impl != nullptr); return impl->file.method(...)
```

with **no null branch**, and the assert is compiled into the shipping firmware
(confirmed by disassembling `gh_release`'s `HalStorage.cpp.o`: `bnez a0`, then
`li a1,218; call __assert_func`). The stub's `if (!impl) return false;` was the
exact opposite.

That mattered the moment v74 introduced an `OpenFileEntry` with **no `HalFile`
at all** (a discarded named stream). `releaseSlot()` closes unconditionally, so
the X3 would have aborted and rebooted at the CLOSE of every accepted stream —
the exact step v74 exists to enable — from `closeCmd`, from `destructionEvent`
and from `SmbServer::end()` alike. **The 73-check suite was green throughout.**

The previous seven rows were about a stub that accepted something the device
refuses, or refused something the device accepts: wrong answers. This one let a
**crash** ship as a pass. Both halves were fixed:

* `releaseSlot()` skips `close()` for a null stream (the real bug), and
* **the stub now asserts too**, so the harness reproduces the abort.

Verified by mutation: with the `releaseSlot` guard removed, `finderinfo_test.py`
now kills the harness with
`stub_hal/HalStorage.cpp:667: bool HalFile::close(): Assertion 'impl != nullptr' failed.`
Before the stub was fixed, the same mutation passed.

**The rule this is the eighth instance of:** a stub that is more forgiving than
the device does not merely fail to catch a bug — it certifies one. When a change
creates a storage-layer combination that could not occur before (here: a slot
with no file behind it), the stub's behaviour for that combination is a claim
that has to be checked against `lib/hal/`, not assumed.

**Also in round 3**: the holding name is now **per-operation**
(`crossmosa-replace-<n>.tmp`). A fixed name meant an interrupted replace left a
file that then blocked *every later replace in that directory*, permanently,
with one `diag.log` line as the only notice -- a visible leftover is a
nuisance, a directory that silently stops accepting replaces is undiagnosable
from outside. The handler now steps over leftovers (logging each one it finds)
and only gives up after 8 in a row. `test_replace_not_blocked_by_a_leftover`
plants exactly the file the old code would have chosen and requires the
replace to succeed anyway, with the leftover left untouched.

**All 3 new checks fail against `4123c12`**: `PARTIALLY APPLIED -- atime
1715938200 (was 1000000000)`; `accepted -- the device refuses this outright`;
`blocked by the leftover`. The other 64 passed unchanged.

### What blocked `set_info`, and what it cost

Worth keeping because the shape of the bug is more instructive than the fix.

`smb2_process_set_info_request_variable()`
(`lib/smb2/lib/smb2-cmd-set-info.c:397-410`) refused every SET_INFO carrying a
buffer -- which is every real one -- unless `smb2->passthrough` was set.
Upstream's own message says "can not interpret set-info buffers **yet**": the
feature is simply unimplemented on the server path.

The damage was in **where** the `-1` came from. It is a *decode* function, not
a handler: `smb2_process_payload_variable()` propagates it out of
`smb2_read_from_socket()` and the connection is destroyed, whereas the same
`-1` from `set_info_cmd` produces a clean per-request
`STATUS_NOT_IMPLEMENTED`. There is **no callback between the two points**, so
no handler could intervene, and `set_info_cmd`'s own `return -1` had never
once executed. Observed here before the patch -- one rename, from a client
that had just written a file successfully:

```
client: RENAME FAILED: SMBConnectionClosed: SMB socket was closed
        close failed: SMBConnectionClosed          <- the whole session is gone
server: [SMB] DBG: smb2_service(in) failed: Failed to parse variable part of
        command payload. can not interpret set-info buffers yet
        SMB destruction_event ctx=0x... freed=1 sync_failed=0
```

macOS and iOS send SET_INFO after writing a file, so this was the **primary
use case** failing, not an optional feature missing: an iPhone would very
likely have been disconnected at the end of its first copy.

`smb2_set_passthrough()` was rejected again, and more sharply than in Task 5:
`passthrough` is context-wide, and on the server path the one thing it would
break is `smb2_encode_query_directory_reply()` -- i.e. it would hand this
project the wire encoding of `query_directory`, the very encoder patch 2
exists to fix.

### Three device facts that shaped the handler

All read from the SdFat sources rather than assumed, and all three differ from
POSIX -- which is why the stub needed three new rows in the divergence table.

**1. SdFat's rename is CREATE-EXCLUSIVE.** `FatFile::rename()` builds the new
directory entry with `O_CREAT|O_EXCL` (files) or `mkdir(..., pFlag=false)`
(directories) -- `FatFile.cpp:970-984` -- so it FAILS if the target exists,
where POSIX `rename(2)` replaces silently. MS-FSCC's `ReplaceIfExists`
therefore cannot be one call: the handler removes the destination first, which
makes it **the one non-atomic operation in this file**. That was weighed, not
waved through -- what is destroyed in the bad case is exactly the object the
client asked to have destroyed, the client's source file is untouched, and
both steps are logged (`destination_removed=YES (destination is now gone)`).
Refusing every replace instead would break the write-to-temp-then-rename save
that macOS and iOS both use.

**2. The file must be renamed THROUGH ITS OPEN HANDLE.** `FatVolume::rename()`
says so itself (`FatVolume.h:193-195`): "The file to be renamed must not be
open. The directory entry may be moved and file system corruption could occur
if the file is accessed by a file object that was opened before the rename()
call." In SMB2 the handle *is* open -- that is how the client names the source
-- so `Storage.rename()` is unusable here and `HalFile::rename()` (which moves
the entry and updates the live handle) is the only correct call. The same
sentence is why rename and delete refuse when a *second* handle holds the
path; `setEndOfFile()` deliberately does not check, because truncation
rewrites a file's own extent without moving the directory entry.

**3. `truncate()` CANNOT GROW a file.** Both implementations are
`seekSet(length) && truncate()` (`FatFile.h:957`, `ExFatFile.h:786`) and
seekSet refuses to pass EOF -- the same guard Task 6 found for `seek64()`.
POSIX `ftruncate()` grows and zero-fills. So growth is done the only way this
device can: by writing the zeros, with the same helper and the same 64 KiB
budget `write_cmd` already uses for a write past EOF. That budget is a
**non-blocking** constraint, not a storage one -- this runs inside a network
callback. Beyond it the request is refused and logged, and **the log line is
the point**.

> ⚠️ **v80 REVERSED THE SENTENCE THAT USED TO END THIS PARAGRAPH.** It read:
> *"if a real iPhone is ever seen pre-sizing a whole book, the answer is to make
> the fill incremental across ticks, not to raise the budget."* v80 raised the
> budget, 64 KiB -> 4 MiB + 64 KiB, and this is the record of that.
>
> **What was actually seen was not what that sentence anticipated.** iOS does
> not pre-size; it never sends `FILE_END_OF_FILE` in any of ten device logs.
> It splits a large copy into **parallel streams one megabyte apart** and starts
> them all at once (diag27: writes at 0 / 1M / 2M / 3M / 4M within 168 ms, down
> one handle). Incremental-across-ticks does not address that shape -- the holes
> are not one big pre-size, they are four concurrent ones -- and it is a
> STATUS_PENDING + interim-response rework of the write path, an order of
> magnitude more change than a constant.
>
> **The cost is real and is not waved away.** Filling iOS's stride is 4,177,920
> zero bytes = 8,160 sectors, roughly 8-11 s of blocked UI per copy on the
> multi-sector path (which is why `kZeroFillChunkBytes` went 512 -> 4096 in the
> same version; at 512 it would be 20-25 s). There is **no watchdog to escape
> it**: no task is subscribed to the TWDT, and the `esp_task_wdt_reset()` calls
> in `CrossPointWebServerActivity.cpp` return `ESP_ERR_NOT_FOUND`.
>
> Incremental-across-ticks is still the right end state and is still unbuilt.
> v80 ships the measurement that decides whether it is needed: the fill now logs
> `zero-filled N-byte hole ... in NNN ms`, and that path had **never executed on
> hardware** before (`grep -c zero-fill` over all ten logs = 0).

### Recorded, deliberately NOT fixed: nothing sets an `FsDateTime` callback

**This partly undercuts the timestamps this same task implemented, so it
should not be lost.** Verified by grep across `src/`, `lib/` and
`freeink-sdk/`: no `FsDateTime::setCallback` anywhere. Consequences, from the
SdFat sources:

* a file the X3 creates gets `FS_DEFAULT_DATE` -- `SdFatConfig.h:311`,
  literally "1 January *compile year*";
* its modify time is **never updated on write**, because
  `FatFile::sync()`'s `if (FsDateTime::callback)` (`FatFile.cpp:1254`) never
  fires.

So books copied from a computer carry real dates and sort correctly, while
anything the device writes -- OPDS downloads especially -- all share one date.
The X3 has a DS3231 RTC that could feed the callback. Not fixed here:
registering a global date/time callback changes every write in the whole
firmware and is a project-level change, not a change to an SMB handler.

**This is NOT the same thing as `FILE_BASIC_INFORMATION`**, which fix round 2
implemented properly: `HalFile::setTimestamp()` wraps `FatFile::timestamp()`
and the handler applies what the client sends. A setter takes its value from
the client, so it needs no clock -- the two are independent. What is still
missing is only the *automatic* stamping of files the device writes itself.

## Baseline (Task 8 -- wiring; the accept-failure fixtures)

Task 8 wired the server into the firmware and added **2 checks** (69 total),
both for `SmbServer::acceptOneConnection()`'s two-way split of
`smb2_serve_port_async()`'s single `int` return. That branch guards this
device's #1 documented crash class on a path where the `diag.log` line is the
only evidence that will ever exist, and it is three lines of C++ -- exactly the
shape a later "simplification" collapses back into one boolean.

An earlier revision of this task recorded both failure shapes as
**unreachable in this harness. That was wrong**, and the way it was wrong is
worth keeping: the supporting anecdote ("`smbhost` exits 1 at `ulimit -n 9`")
was an artifact of the probe's own bug -- the port was built by string
concatenation, `"45" + "9"`, giving **port 459**, which is privileged, so the
bind failed. `smbhost` starts fine down to `ulimit -n 4`. The other half of the
reasoning was wrong too: the registry-slot pre-check caps *concurrent contexts*
at 4, but it does not stop `accept()` from hitting `EMFILE` when the process
limit is lower than that.

- **`test_accept_eio_latched_and_rearms`** -- starts a private `smbhost` under
  `RLIMIT_NOFILE = 6` so `accept()` fails with `EMFILE`. That failure leaves the
  connection **queued** (`lib/smb2/lib/socket.c:1483-1494` reaches its `else`
  only when `clientfd < 0`), and `select()` is level-triggered, so it re-fires
  on every tick. Asserts ~390 raw failures produce **exactly one** DiagLog line,
  then releases the pressure so an accept succeeds and requires a **second**
  line from a second storm -- i.e. the latch suppresses *and* re-arms.
- **`test_accept_enomem_never_suppressed`** -- `LD_PRELOAD`s
  `smbfail_calloc.c`, which fails the `calloc(1, sizeof(struct smb2_context))`
  inside `smb2_init_context()` while an arm file exists. Here `accept()` has
  already **succeeded and dequeued** (`lib/smb2/lib/libsmb2.c:4455-4458`), so
  nothing repeats by itself. Asserts **3 of 3** injected failures are logged,
  each carrying the contiguous-heap figure.

**Both were run against deliberately reverted latches**, which is what proves
they guard the right thing rather than merely passing:

| revert | `-EIO` test | `-ENOMEM` test |
|---|---|---|
| round 2's one boolean, cleared on success | FAIL (line does not identify the queued case) | **FAIL: 3 injected failures produced 1 line** |
| round 1's no latch at all | **FAIL: 389 raw failures produced 389 lines, expected 1** | n/a |

Both fixtures **skip loudly** rather than pass quietly if they cannot set up
(no `cc`, `__libc_calloc` absent, `smbhost` will not start, `RLIMIT_NOFILE`
cannot be lowered, `accept()` refuses to fail here, or the traced allocation
shape has drifted). `main()` prints skips as `SKIP`, not `PASS`, and repeats
them in a banner block afterwards -- a test that quietly does nothing is the
same failure shape as fix round 3's log line that said "reject" for a
half-applied request.

### Exit codes

| code | meaning |
|---|---|
| 0 | every check ran and passed |
| 1 | at least one check FAILED |
| 2 | **INCOMPLETE** -- nothing failed, but one or more checks were SKIPPED |

Code 2 exists because for one round this script printed `PASS: all checks
passed` and exited 0 after skipping checks. A skip is not a failure, but it is
emphatically not a pass, and a shell chaining on success must not treat an
unverified run as a verified one -- which is exactly the false green the whole
task is about.

### Two things the fixtures get wrong if you write them the obvious way

**1. `preexec_fn` failures are not `OSError`.** They surface as
`subprocess.SubprocessError`, which is not an `OSError` subclass. An
unhandled one crashed the entire script before `main()`'s results loop --
discarding the record of ~200 checks that had already passed and printing no
verdict at all. Worse than a false green in one respect: it destroys the
suite's own reporting for everything else. `_spawn_smbhost()` catches both and
routes them into the same skip path. Reproduce with a monkeypatched
`resource.setrlimit` that raises; the run then completes with a visible SKIP,
the banner, `INCOMPLETE: 68 of 69 checks passed`, and exit 2.

**2. A size RANGE does not identify `smb2_init_context()`'s allocation.** At
least two `calloc(1, sizeof(...))` sites sit inside the obvious 4 KB-64 KB
window:

| struct | size (x86-64) | site | frequency |
|---|---|---|---|
| `smb2_context` | ~7,256 B | `init.c:303` | once per accepted connection |
| `smb2_pdu` | ~12,568 B | `pdu.c:93` | **once per PDU** |

A range match only avoids misfiring because the armed window carries bare,
protocol-free TCP. Extend the fixture to a test that actually speaks SMB and it
starts eating PDU allocations, with the symptom reading as "the injection
stopped working". So the shim matches an **exact size**, and discovers it at
runtime rather than hardcoding a host-dependent number: it traces in-window
sizes to a file, the test truncates that trace, opens one bare connection, and
takes entry 0 -- which is the context by construction, since `accept_cb()`
(`libsmb2.c:4455`) calls `smb2_init_context()` before anything else on that
connection can allocate. Observed here: `calloc(1, 7256)`. If the shape drifts,
the injection yields no `-ENOMEM` and the test SKIPs with a reason.

### Recorded here, deliberately NOT fixed in Task 8

**Upstream leaks the accepted socket on `-ENOMEM`.** `accept_cb`
(`lib/smb2/lib/libsmb2.c:4443-4468`) receives `fd`, fails at
`smb2_init_context()`, and returns without closing it. Measured: 4 fds at
baseline, 9 after five injected `-ENOMEM` connections -- five leaked sockets,
never reclaimed. Fixing it would be a **fourth divergence in `lib/smb2/`**,
which needs the maintainer's approval, and there is a strong reason not to ask:
`CrossPointWebServerActivity::onExit()` ends in `silentRestart()`, so leaked
descriptors cannot outlive one file-transfer session. The compounding chain
(leak -> descriptor exhaustion -> `-EIO`) is bounded by a reboot the user
triggers simply by leaving the screen. **Do not re-open this without that
mitigation in view.**

**`scripts/verify_libsmb2_patch.py` is not wired into CI.** No reference in
`.github/workflows/`. Three separate arguments in Task 8 rest on "the vendored
tree cannot drift silently" -- the three-divergence guarantee,
`isCleanRemoteClose()`'s string match against `socket.c:401`, and every source
line cited in `SmbServer.cpp`'s failure enumeration. All three currently depend
on a human remembering to run it.

## Baseline (Task 6 -- read / write / flush, superseded by Task 7 above)

**49 checks, all passing.** Task 6 added 15, and every one of them compares a
whole-file SHA-256 (or an exact byte string) rather than "the call did not
raise": the failure this task exists to prevent is a transfer that reports
success and lands the wrong bytes, which no exception-shaped assertion would
catch.

- 200 KB read and 200 KB write round trips, SHA-256 exact. 200 KB is
  deliberately larger than the negotiated 32 KB `max_read_size`/
  `max_write_size`, so each takes seven requests and the per-request offset
  arithmetic is actually exercised.
- Chunk-boundary sizes: 32767, 32768, 32769 and 65536 bytes, each both read
  and written. The exact multiples are where "one request too few" lives;
  32769 is the case whose second request carries a single byte.
- Reads straddling, at, and past end-of-file.
- Non-sequential and overlapping writes, including one that starts past the
  current end of file (see the zero-fill note below), plus an absurd hole that
  must be refused with the file left untouched.
- A Traditional Chinese path carrying a 100 KB write and read-back.
- Two connections writing two different files, interleaved request by request,
  each verified separately.
- The rejection paths: a write to a read-only handle, and connection B trying
  to read/write/flush connection A's handle.
- `flush` on a file handle and on a directory handle.
- `MAXIMUM_ALLOWED` against a file carrying the read-only attribute.
- **A failed write-back** (fault-injected, see below) surfacing as a client
  failure for both `flush` and `WRITE_THROUGH`, with an ordinary write and a
  healthy file unaffected.
- **A read-only request that had to create the file** — `FILE_OPEN_IF` +
  `GENERIC_READ` against a missing name — refusing a subsequent write, while
  the `MAXIMUM_ALLOWED` upload path stays writable.
- **A failed final sync at CLOSE** surfacing as a client failure, with a
  healthy handle still closing cleanly.
- **A failed close still freeing its slot**, on both release paths
  (`close_cmd` and `destruction_event`), so a failing card cannot exhaust the
  eight-slot table.

**All 15 were run against the pre-change build first and all 15 failed**, while
the 34 existing checks still passed. The first 11 failed with `0xc0000002`
(`STATUS_NOT_IMPLEMENTED`) from the Task 3 stubs; the last two are review
fixes, and against the pre-fix handler they failed with, respectively, "flush
reported success despite the sync failing" / "WRITE_THROUGH reported success
despite the sync failing", and "a handle opened GENERIC_READ accepted a write"
/ "bytes landed on disk anyway".

### Fault injection: `SYNCFAIL`

`stub_hal/HalStorage.cpp`'s `sync()` reports failure for any file whose name
contains `SYNCFAIL`. It is the only way the failed-write-back branch can be
reached at all — POSIX `fsync()` on a healthy scratch file on a local
filesystem does not fail, so without it the branch would be present and never
executed. A filename marker rather than an environment variable because the
harness server is a separate process, usually started before the test script,
so a test cannot influence its environment — but it can choose what it asks
the server to open.

Four things from this round are worth carrying forward:

**1. A third stub-vs-device divergence, fixed in `stub_hal/HalStorage.cpp`:
`seek64()` past end-of-file.** SdFat refuses it outright — `FatFile::seekSet()`
`goto fail`s for `pos > m_fileSize` (`FatLib/FatFile.cpp:1184-1188`) and
`ExFatFile::seekSet()` the same against `m_dataLength`
(`ExFatLib/ExFatFile.cpp:715-719`). POSIX `lseek()` allows it and a following
`write()` makes a sparse hole, which happens to be exactly the SMB2 semantics
for a write beyond EOF — so the unfixed stub would have taken the free ride
and certified a path the X3 cannot execute at all. Worse, on the device a
failed seek leaves the file position where it was, so a handler that ignored
the result would write the client's bytes at *the previous request's offset*
and report success. The stub now refuses, which is what makes `write_cmd`'s
explicit zero-fill path get exercised here.

**2. A write that starts past end-of-file is zero-filled, up to 4 MiB + 64 KiB.**
There is no sparse-file mechanism to lean on (see above), so the hole is
written out by hand, inside a network callback that is supposed to be
non-blocking. A larger hole is refused and logged. Refusing is the safe
direction — the alternative is not "write it anyway", it is "write it at the
wrong offset".

The budget was 64 KiB until v80, justified as "enough for a client that
reorders by a chunk or two, far short of a multi-second stall". **The first
half was falsified by a real iPhone and the second half is no longer true**:
see the v80 note in the `truncate()` section above for the measurement, the
reversal, and what it costs. The new value is the largest hole actually
observed (4,190,208) plus the old budget as margin — a measurement, not a round
number, and the one constant to raise if a future log shows a wider stride.

Zeros, not garbage, and that distinction is load-bearing: `allocateCluster()`
writes only the FAT table and `remove()` clears only the directory entry, so
any mechanism that grows a file *without* writing data hands back previously
deleted content — on this device, the user's own books, inside whatever file is
being copied. That is why `preAllocate()` and "let the parallel streams cover
it" are both on the rejected list.

**3. `MAXIMUM_ALLOWED` can now open a read-only-attribute file, read-only.**
This was Task 4's recorded deferred item. `MAXIMUM_ALLOWED` forces write mode
in `needsWriteAccess()`, the write-mode open fails on a read-only file exactly
as it does on a directory (`FatFile.cpp:581-585`), and the retry path used to
discard the downgraded handle unless the target was a directory — so such a
file could not be opened at all, and therefore could not be read, by a client
that sends `MAXIMUM_ALLOWED`, which macOS and iOS do routinely. The retry now
also accepts a *file* when the client asserted no explicit write access, and
records the handle as read-only so a later write is refused with a logged
reason rather than silently attempted. A client that did ask for write access
still fails exactly as before.

**4. `flush` used to report a success it never verified, and a fourth
stub divergence came out of fixing it.** `FsFile::flush()` is
`void flush() { sync(); }` (`FsLib/FsFile.h:262`) — it discards the `bool` at
`:809-810`. `lib/hal/HalStorage.h` gained `bool sync()`, and `flush_cmd` plus
`write_cmd`'s `WRITE_THROUGH` branch now check it; `WRITE_THROUGH` is the one
that matters, because MS-SMB2 2.2.21 makes "on stable storage before the
response" a promise the reply asserts, and a card failing mid-copy would
otherwise hand iOS Files a clean "copied". Adding the stub counterpart
immediately failed `test_flush`: a directory in the stub is an `opendir()`
with no descriptor, so an fd-only `sync()` reports failure for every directory
flush — a failure the device does not have, since `FatFile::sync()` handles
directories (`FatLib/FatFile.cpp:1231-1261`). Fixed in the stub, and the
symmetry is worth noting: the previous three divergences were the stub being
*looser* than the device; this one was the stub being *stricter*, and it is
the same class of bug either way.

**5. `close` was the same discarded bool as `flush`, on the call site where
it matters most.** `HalFile::close()` -> `FsBaseFile::close()`
(`FsFile.cpp:58-63`) -> `FatFile::close()`, and the last of those IS
`bool rtn = sync(); ...; return rtn;` (`FatFile.cpp:128-132`;
`ExFatFile.cpp:75-80` identical). `releaseSlot()` discarded it, so a card that
failed its final write-back still got `close_cmd` returning 0 — and **close is
where the last data of an upload lands**: iOS copies with
write, write, ..., close, so the Files app would report success over a
truncated book. `releaseSlot()` now returns the result and is `[[nodiscard]]`,
`close_cmd` returns `-1` with the handle id and path, and `destruction_event`
— which has no client left to answer — logs instead. The slot is freed
unconditionally either way, which is its own test.

`set_info` and timestamps both landed in Task 7 -- see its baseline above,
including the vendored-library divergence `set_info` turned out to need.

## Baseline (Task 5 -- query_directory / query_info, superseded by Task 6 above)

**34 checks, all passing.** Task 5 added 14: directory listing (names, sizes,
directory flag, both supported info classes), the same with Traditional Chinese
names, protected-path hiding, multi-response enumeration, empty directories,
`SMB2_RESTART_SCANS`, search-pattern filtering, the scan bound, a wire-level
`next_entry_offset` guard, the four `SMB2_0_INFO_FILE` and five
`SMB2_0_INFO_FILESYSTEM` classes, rejection of everything else, and
cross-connection handle isolation for both new handlers.

Three things this round found are worth carrying forward, because each is a
place where "it looked fine" and was not:

**1. libsmb2 frees an uninitialised pointer on every enumeration's last
request.** `smb2_process_query_directory_request_fixed()`
(`smb2-cmd-query-directory.c:505-545`) `malloc`s the request struct and, when
`file_name_length == 0`, returns before the only code that assigns
`req->name` ever runs — then `libsmb2.c:3798` does
`if (req->name) smb2_free_data(...)`. MS-SMB2 3.3.5.18 has clients send the
search pattern only on the *first* query, so every continuation hits this.
Reproduced here: after ordinary traffic had recycled the heap, `req->name`
came back as `0x747874` (`"txt"`, the tail of an earlier request's filename)
and the server process died. `queryDirectoryCmd()` now normalises the field
before anything else; `test_listing_multi_response` is the regression test.

**2. `next_entry_offset` was written as an absolute offset, not a relative
one — FOUND AND FIXED IN THE VENDORED TREE.** `smb2-cmd-query-directory.c`
wrote `offset + fs_size` where MS-FSCC 2.4.8 wants `fs_size`. Clients walking
`p += next_entry_offset` — which is what smbprotocol does, and what libsmb2's
*own* client does at `libsmb2.c:332` — over-shoot on entry 3 onward and
silently skip files. Only entries at offset 0 and the final entry (whose field
the encoder hard-zeroes) were safe.

**The vendored tree is patched.** One expression, pinned by hash, documented in
`docs/third-party/libsmb2-vendoring.md` under **"The second patch"** — read that
before touching `lib/smb2/`, and in particular **do not "restore"
`smb2-cmd-query-directory.c` to pristine**: `scripts/verify_libsmb2_patch.py`
expects exactly two divergences now (the appended `crossmosa_smb2_finish_accept()`
in `libsmb2.c` and this in-line fix), and reverting either fails it. With the
fix in, the handler packs entries up to its own memory budget again
(`kMaxDirEntriesPerResponse = 32`, a memory choice, not a correctness one) —
an 80-file directory went from 41 round trips to 4.
`test_listing_next_entry_offset_chain` checks the chain at the wire level and
is the regression test for the patch; reverting it fails that check plus four
other listing checks.

**3. Two more stub-vs-device divergences, both in the enumeration path, both
fixed in `stub_hal/HalStorage.cpp`.** `HalFile::openNextFile()` used to `skip`
an entry whose `stat`/`open` failed; SdFat instead `goto fail`s
(`FatLib/FatFile.cpp:676-679`) and returns an invalid file, indistinguishable
from end-of-directory — so on the X3 one bad entry truncates the whole
listing, and a skipping stub would have certified a listing the device cuts
short. And `HalFile::getName()` used to truncate into a short buffer, where
SdFat's `getName8()` fails and returns 0 (`FatLib/FatName.cpp:99-160`). Both
now match the device.

Timestamps are zero throughout (Task 7 owns `getModifyDateTime`), and
`FS_SIZE`/`FS_FULL_SIZE`'s capacity numbers are a documented placeholder —
`HalStorage` exposes no capacity accessor. See `queryInfoCmd`'s `kNominal*`
comment.

### Recorded here, deliberately NOT fixed in Task 5 -- BOTH CLOSED IN TASK 8

Two hazards found in review that are real but belong to other tasks. Written
down so they are not lost, since neither is visible from the code Task 5
touched. **Both were closed by Task 8 (wiring)**; how, at the end of this
section.

**1. `DiagLog` is off unless `/diag.on` existed at boot — so a first-contact
iOS failure on a normal card produces ZERO evidence.** Every unsupported info
class, every rejection, every diagnostic this task added routes through
`DiagLog::line`, which returns on its first line unless the sentinel file was
present when `DiagLog::begin()` ran (see `src/util/DiagLog.h`). That is correct
for a shipping device — the instrumentation is meant to cost nothing in normal
use — but it undercuts the whole point of requirement 2 ("every unsupported
info class must be logged, because this device has no serial port") on exactly
the run that matters most: the first time someone points an iPhone at it.
**Task 8 (wiring) or Task 9 (user instructions) needs to close this**, e.g. by
having the SMB activity enable diagnostics for its own lifetime, or by telling
the user to drop `/diag.on` on the card before the first connection attempt.

**2. `HalFile::openNextFile()` allocates with throwing `new` per entry, on the
device.** `lib/hal/HalStorage.cpp:166-170` does
`std::make_unique<Impl>(impl->file.openNextFile())` — and under
`-fno-exceptions` a failed allocation is `abort()`, not `nullptr` (CLAUDE.md
hard limit 2). This is pre-existing HAL behaviour that `Storage.open()` has
always had, but Task 5 is what puts it on a **per-directory-entry** path with
WiFi up, which is the memory situation the device is worst at. A 300-entry
folder is 300 such allocations. Not changed here — the HAL is shared with the
whole firmware and is out of this task's scope — but it is the most likely
place a listing turns into a panic on a fragmented heap.

**How Task 8 closed both.**

*(1) Diagnostics.* `DiagLog` gained `setForced(bool)`, and
`CrossPointWebServerActivity` calls it on `onEnter()` / `onExit()` -- so
diagnostics are on for exactly as long as file-transfer mode is, sentinel file
or not, and off again (or rebooted away) after. The sentinel remains the
switch for everything else, which is the case v57 designed it for: the reading
path really should cost nothing. What changed is the observation that the one
activity a user opens *in order to be connected to* is also the one where the
instrumentation has to be free. The cost is bounded by what actually logs:
almost every `DiagLog` call site in `SmbFileHandlers.cpp` is on a rejection or
failure path, so a healthy transfer is silent, and `MAX_DIAG_BYTES` (192 KB)
still caps the file. The one call site that would NOT have been silent --
`read_cmd`'s per-request reply buffer, ~3,200 lines for a 100 MB book -- is
rate-limited instead; see `diagBigAlloc()` for the argument.

*(2) Throwing `new` in the HAL.* All four `HalFile::Impl` allocations in
`lib/hal/HalStorage.cpp` (`open`, `openNextFile`, `openFileForRead`,
`openFileForWrite`) are now `makeUniqueNoThrow`, with a `LOG_ERR` on failure. A
null `Impl` was already a meaningful result at every call site -- `isOpen()`
returns false, which every caller already reads as "could not open" or "end of
directory" -- so the failure mode goes from `abort()` to a truncated listing.
For `openNextFile()` that is *exactly* the shape SdFat itself produces for an
entry it cannot read (`FatFile.cpp:676-680`), which the listing loop already
documents and handles. `openFileForRead`/`openFileForWrite` additionally force
their `bool` result to `false`, because there the caller reads the bool rather
than the file's truthiness.

**This is a device-only failure path and the harness structurally cannot
reproduce it** -- a POSIX scratch directory does not run out of memory -- so it
is deliberately NOT a new row in the divergence table: there is no observable
difference to test, only a difference in what happens when the device is out of
heap. Recorded here instead.

## Baseline (Task 4, round 5 -- directory-open semantics, superseded by Task 5 above)

Round 4 fixed the access-mode half of the directory problem and left the
disposition half. Re-review found the gap by probing the running server
directly, and it is worth recording the exact shape because round 4's own
code comment asserted the opposite:

```
REFUSED  subdir, FILE_OPEN    + MAXIMUM_ALLOWED, no DIR flag   0xC0000002   (round 4 disclosed this)
REFUSED  subdir, FILE_OPEN_IF + GENERIC_READ,    no DIR flag   0xC0000002   (round 4 did NOT)
OK       subdir, FILE_OPEN    + GENERIC_READ,    no DIR flag
```

Round 4 defined write intent as "any disposition other than `FILE_OPEN`", so
a **pure-read** `FILE_OPEN_IF` against an existing folder still asked for
`O_RDWR` and died on SdFat's subdirectory guard. The residual was a function
of disposition **and** access; round 4 documented only the access half.

**The fix separates two things that were being conflated**, the way POSIX
`open(2)` does:

* **creation bits** (`O_CREAT` / `O_TRUNC` / `O_EXCL`) come from
  `create_disposition`;
* **access mode** (`O_RDONLY` vs `O_RDWR`) comes from `desired_access`.

`FILE_OPEN_IF` then still creates when the target is absent, while a pure-read
open of an existing folder gets `O_RDONLY|O_CREAT`, which the filesystem
accepts. Existing directories are additionally narrowed to `O_RDONLY`
regardless of access, so `MAXIMUM_ALLOWED` probes work too.

**Three filesystem rules constrain the pairing**, read from the vendored
sources rather than assumed, and identical in the FAT and exFAT
implementations:

| Rule | FAT | exFAT |
|---|---|---|
| write-mode open of a subdirectory / read-only file is refused | `FatFile.cpp:581-585` | `ExFatFile.cpp:399-405` |
| `O_TRUNC` without write mode is refused | `FatFile.cpp:552-556` | `ExFatFile.cpp:407-412` |
| `O_CREAT` only creates **in write mode** | `FatFileLFN.cpp:372-373`, `FatFileSFN.cpp:99-100` | `ExFatFile.cpp:432-433` |

The third one is why a creating disposition still forces write access when the
target is **absent** — `O_RDONLY|O_CREAT` silently fails to create on device
even though POSIX would create. **The stub models rules 2 and 3 now too**;
without that, the harness would have been *looser* than the device for a
combination this round newly generates — the same failure mode round 4 fixed,
in the other direction.

The suite is now **19 checks** (the matrix counts as one but covers 24
combinations). Against the pre-round-5 binary (built from `7205819`'s handler
against this round's stub), exactly 4 of those 24 fail:

```
FAIL (dir matrix: subdir, FILE_OPEN    + MAXIMUM_ALLOWED,      no DIR flag): 0xc0000002
FAIL (dir matrix: subdir, FILE_OPEN_IF + GENERIC_READ,         no DIR flag): 0xc0000002
FAIL (dir matrix: subdir, FILE_OPEN_IF + MAXIMUM_ALLOWED,      no DIR flag): 0xc0000002
FAIL (dir matrix: subdir, FILE_OPEN_IF + FILE_READ_ATTRIBUTES, no DIR flag): 0xc0000002
```

The fourth was not on anyone's list before the matrix existed — same root
cause, one more access flag. Every share-root combination passes on **both**
builds, confirming `openRoot()`'s oflag-blindness is modelled and unregressed.

### One previously-passing assertion changed, deliberately

Round 4's *"subdir + `FILE_OPEN` + `GENERIC_WRITE` must be refused"* could not
survive: existing directories are now narrowed to `O_RDONLY`, so that request
succeeds by design. It was **not** weakened away — it was **moved to the shape
that should still fail**, `test_truncating_open_of_directory_refused()`:
`FILE_OVERWRITE_IF` + `GENERIC_WRITE` on a directory. The narrowing retry
deliberately excludes truncating dispositions ("truncate this directory" has
no read-only reading), so that open still reaches the filesystem in write mode
and must still be refused. The stub-fidelity check therefore remains
wire-observable, which was the whole reason round 4 valued it.

## Baseline (Task 4, round 4 -- superseded by round 5 for directory opens)

Round 4 fixed six review findings in `create_cmd`/`close_cmd` and, just as
importantly, **one in this harness itself**. Rounds 1-3 (below) are unchanged
and still in effect; this section only adds what round 4 moved.

**The harness was hiding a device-only failure.** `stub_hal/HalStorage.cpp`'s
`open()` ignored `oflag` entirely for directories, so a write-mode open of a
subdirectory succeeded here. On device, SdFat refuses it outright
(`FatFile::open()` sets `FILE_FLAG_WRITE` for `O_WRONLY`/`O_RDWR` and then
fails when the target `isSubDir()` or `isReadOnly()` --
`.pio/libdeps/default/SdFat/src/FatLib/FatFile.cpp:581-585`). Combined with
`resolveFileOflag()` returning `O_RDWR` even for `FILE_OPEN` + `GENERIC_READ`,
that meant the ordinary "is this a file or a folder?" probe every client makes
**passed in this harness and would have failed on the X3** -- precisely the
class of bug a harness exists to catch. The stub now models SdFat's guard,
including its one genuine exception: a path of just `/` short-circuits to
`FatFile::openRoot()` (`FatFile.cpp:456-461`), which never inspects `oflag`,
so the share root still opens in any mode.

The suite is now **18 checks**. Eight of them are new, and six of those eight
were verified to **fail against the pre-round-4 handler code** (built from the
previous commit against this same fixed stub) before being accepted:

```
[against the PRE-fix binary, new tests only]
FAIL (protected + FILE_OVERWRITE_IF + MAXIMUM_ALLOWED): create UNEXPECTEDLY SUCCEEDED against a protected path
FAIL (protected + FILE_OVERWRITE_IF + MAXIMUM_ALLOWED): the protected file was MODIFIED (0 bytes, expected 60) -- the open was refused too late, after O_TRUNC
FAIL (protected + FILE_OPEN_IF + MAXIMUM_ALLOWED): create UNEXPECTEDLY SUCCEEDED against a protected path
FAIL (protected read open): a READ open of a protected path SUCCEEDED
FAIL (directory probe): SMBResponseException ... 0xc0000002
FAIL (DIRECTORY_FILE vs plain file): SUCCEEDED -- directory-ness is still taken from the client's hint rather than the filesystem
FAIL (cross-connection close): connection B closed connection A's handle
```

The first of those is the whole point: a protected file was not merely opened
but **truncated to zero bytes** by a request an iOS client sends routinely
(`FILE_OVERWRITE_IF` + `MAXIMUM_ALLOWED`, e.g. dropping `.DS_Store`).

Full run against the fixed build (18 checks, all pass):

```
$ SMBHOST_ROOT=... .venv/bin/python3 smb_smoke_test.py 4481
PASS (anonymous rejected): SMBAuthenticationError: ...
PASS (x3/wrongpassword): correctly rejected with LogonFailure: ... 0xc000006d
PASS (tree_connect SD, non-guest, signed): tree_id=4277009102
PASS (tree_connect NOPE rejected): SMBResponseException
PASS (create/close file): smoke_test_file.txt
PASS (create/close directory): share root
PASS (protected path rejected): SMBResponseException
PASS (protected + FILE_OVERWRITE_IF + MAXIMUM_ALLOWED): rejected with SMBResponseException
PASS (protected + FILE_OVERWRITE_IF + MAXIMUM_ALLOWED): protected file still intact (60 bytes)
PASS (protected + FILE_OPEN_IF + MAXIMUM_ALLOWED): rejected with SMBResponseException
PASS (protected + FILE_OPEN_IF + MAXIMUM_ALLOWED): protected file still intact (60 bytes)
PASS (protected read open rejected): SMBResponseException
PASS (.. traversal rejected): SMBResponseException
PASS (CJK filename round-trip): created, found on disk, and re-opened read-only: 測試資料夾/範例書坊-繁體中文檔名.txt
PASS (directory open without FILE_DIRECTORY_FILE): attributes=0x00000010
PASS (write-mode subdirectory open refused, as SdFat would): SMBResponseException
PASS (NON_DIRECTORY_FILE vs directory rejected): SMBResponseException
PASS (DIRECTORY_FILE vs plain file rejected): SMBResponseException
PASS (cross-connection close rejected): SMBResponseException
PASS (cross-connection close): A's handle survived and A closed it itself
PASS (echo keepalive): session still alive after idle + echo
PASS (destruction_event recycle): 12/12 connections opened a file without ever closing it
PASS (3.1.1 not offered, as designed): NEGOTIATE itself rejected a 3.1.1-only client ...
PASS (default client -> SMB 3.0.2, non-guest, signed, tree_connect OK): dialect=770, tree_id=...

PASS: all checks passed
```

Two tests seed or inspect the SD root directly (`SD_ROOT` in
`smb_smoke_test.py`, honouring `SMBHOST_ROOT`): a protected file cannot be
created over SMB -- that is the property under test -- so it has to be planted
on disk, and its contents have to be re-read afterwards to prove nothing
truncated it.

**Protected paths are now refused for reads as well as writes.** WebDAV has
always blocked them on every method including GET
(`src/network/WebDAVHandler.cpp:51,296`); SMB blocked only write-intent
creates, which would have exposed `/.crossmosa/` -- Wi-Fi credentials,
settings, reading progress -- to any client on the network the moment Task 6
lands `read_cmd`. The matching *hiding* rule for `query_directory` (a listing
must not advertise what a read refuses) is **Task 5's**, and is deliberately
not implemented here.

## Baseline (Task 4, round 3 -- superseded by round 4 above for create/close behavior)

Round 2 fixed the guest-flag problem but exposed a second one: SMB 3.1.1's
session-setup reply fails the client's own signature verification (see
round 2's baseline below for the full trace). The coordinator's round-3
fix doesn't touch that bug at all -- it makes the server never offer 3.1.1
in the first place, so no client can ever reach it.

**The mechanism**: `smb2_negotiate_request_cb` (`lib/smb2/lib/libsmb2.c:4222`)
decides which dialect(s) to offer back by switching on `smb2->version`
(`libsmb2.c:4250-4279`) -- `SMB2_VERSION_ANY` (the default) offers all five,
2.0.2 through 3.1.1; any single concrete version instead makes
`dialect_count = 1`. `SmbServer::acceptOneConnection()`
(`src/network/SmbServer.cpp`) now calls
`smb2_set_version(ctx, SMB2_VERSION_0302)` on every freshly accepted
context, before `crossmosa_smb2_finish_accept()` registers the NEGOTIATE
handler -- see that call site's own long comment for why this ordering is
safe (verified by reading the accept path, not assumed: neither
`smb2_serve_port_async()` nor `crossmosa_smb2_finish_accept()` can read or
dispatch any SMB2 PDU bytes; the negotiate callback can only run from a
*later* call to `SmbServer::tick()`, since the context isn't even in
`clients_[]` -- the only thing `tick()`'s `select()` polls -- until
`acceptOneConnection()` finishes). The result: this server now offers
**only** SMB 3.0.2, unconditionally, to every client.

**Why 3.0.2 and not some other single dialect**: it's the highest dialect
that doesn't have the round-2 bug, and real clients are reported to use it
happily. Per Visuality Systems' iOS SMB article (cited in the coordinator's
spec and in `SmbServer.cpp`'s comment), iOS's own SMB2 client is reported to
use the 3.0.2 dialect variant internally, and their own guidance for server
operators is to configure servers for 3.0.2 rather than 3.1.1-only. A
modern client that also speaks 3.1.1 always includes 3.0.2 in its offered
dialect list too (3.0.2 predates 3.1.1), so this pin doesn't lock out
"newer" clients -- it just stops the negotiation from reaching the one
dialect that's currently broken server-side.

Confirmed empirically, dialect-by-dialect, with a throwaway probe script
before updating `smb_smoke_test.py` itself:

```
[3.1.1-only client]                       NEGOTIATE FAILED: SMBConnectionClosed: SMB socket was closed, cannot send or receive any more data
[default client (full modern list, like iOS)] NEGOTIATE OK, dialect=770 (0x0302 = SMB 3.0.2)
[default client (full modern list, like iOS)] SESSION SETUP OK, signing_key=set
[default client (full modern list, like iOS)] TREE_CONNECT(SD) OK, tree_id=...
[3.0.2-only client]                       NEGOTIATE OK, dialect=770; SESSION SETUP OK; TREE_CONNECT(SD) OK
```

**This is the headline result of round 3**: a client that sends the same
dialect list a real, unmodified modern client (including, per the citation
above, iOS) would send -- `dialect=None` in `smbprotocol` terms, which
expands to its own standard list, 2.0.2 through 3.1.1 -- now negotiates SMB
3.0.2, gets a genuinely non-guest session with a real signing key, and
completes `tree_connect("SD")`. Zero client-side bypasses of any kind.
`smb_smoke_test.py`'s `test_default_client_lands_on_302()` is this exact
scenario, now a permanent regression test (not just a throwaway probe).

Full `smb_smoke_test.py` run (9 checks, all pass):

```
$ .venv/bin/python3 smb_smoke_test.py 4530
PASS (anonymous rejected): SMBAuthenticationError: ...
PASS (x3/wrongpassword): correctly rejected with LogonFailure: ... STATUS_LOGON_FAILURE: 0xc000006d
PASS (tree_connect SD, non-guest, signed): tree_id=...
PASS (tree_connect NOPE rejected): SMBResponseException
PASS (create/close file): smoke_test_file.txt
PASS (create/close directory): share root
PASS (protected path rejected): SMBResponseException
PASS (echo keepalive): session still alive after idle + echo
PASS (destruction_event recycle): 12/12 connections opened a file without ever closing it
PASS (3.1.1 not offered, as designed): NEGOTIATE itself rejected a 3.1.1-only client, before any session/signature exchange could happen: SMBConnectionClosed: ...
PASS (default client -> SMB 3.0.2, non-guest, signed, tree_connect OK): dialect=770, tree_id=...

PASS: all checks passed
```

`test_smb311_dialect_not_offered()` (renamed from round 2's
`test_smb311_signature_gap()`) now asserts the OPPOSITE failure mode from
before: a 3.1.1-only client must fail at NEGOTIATE itself (no common
dialect), not later at a signature mismatch. It explicitly checks the
failure is NOT signature-related, specifically so a regression that
somehow let 3.1.1 through again would be caught as a real failure, not
mistaken for "the known gap, still there, nothing to see."

### What would have to change to support SMB 3.1.1

This pin is a workaround, not a fix -- the underlying signing-key-
derivation mismatch in `lib/smb2/`'s SMB 3.1.1 path (round 2's finding) is
still there, just unreachable. To actually support 3.1.1 (e.g. if a future
client requires it, or to stop leaving performance/feature improvements
3.1.1 offers on the table), someone would need to:

1. Root-cause why the server's derived signing key for the SESSION_SETUP
   reply doesn't match the client's independently-derived one under 3.1.1
   specifically -- the leading hypothesis (not yet confirmed) is a preauth-
   integrity-hash accumulation mismatch, since 3.1.1 folds that hash into
   key derivation and 3.0.x doesn't; see round 2's baseline below for the
   reasoning trail.
2. Fix it inside `lib/smb2/` (likely `libsmb2.c`/`ntlmssp.c`/
   `smb2-signing.c`) -- which needs the same explicit sign-off as Task 3's
   one already-committed vendored patch, since `scripts/verify_libsmb2_patch.py`
   currently machine-checks that the *entire* tree matches pristine upstream
   except for exactly one appended hunk.
3. Change `SmbServer::acceptOneConnection()`'s `smb2_set_version()` call to
   stop pinning at 3.0.2 (or make it conditional / offer the full list
   again) once the underlying bug is confirmed fixed -- and re-verify with
   `test_smb311_dialect_not_offered()` and `test_default_client_lands_on_302()`,
   both of which currently assert 3.1.1-specific and 3.0.2-specific
   behavior that would need deliberate updating.

None of this is attempted in round 3 -- the pin is the intended, complete
fix for *this* round's stated goal (get a real client, including one that
would otherwise land on 3.1.1, to a working non-guest signed session), not
a stopgap awaiting a follow-up in the same round.

## Baseline (Task 4, round 2 -- superseded, kept for history)

**Superseded by round 3 above.** The guest-flag fix here (`allow_anonymous
= 0`) is still correct and still in effect -- what's superseded is the
SMB 3.1.1 signature-mismatch *symptom* this section documents: round 3
makes the server stop offering 3.1.1 at all, so that failure mode is no
longer reachable by any client. Kept verbatim because it's the correct,
detailed trace of *why* 3.1.1 fails, which round 3's fix depends on
understanding but does not resolve (see round 3's own "what would have to
change" subsection above).

The coordinator traced all four call sites in `lib/smb2/` that read
`server->allow_anonymous` and found the actual fix for round 1's guest-flag
problem: **set `allow_anonymous = 0`**, not 1 (`SmbServer.cpp`, with a long
comment explaining why -- it looks like the more restrictive choice but it's
strictly better: a guest session can never sign, and signing was mandatory
here at the time, so the "anonymous fallback" the old setting enabled was
never usable in the first place, and it was actively breaking the one session
type that is). **`allow_anonymous = 0` is still right after v82 made signing
adaptive** -- the reason it is right is that we authenticate, not that we sign;
see "Signing is adaptive" below. `authorize_user` (`SmbFileHandlers.cpp`) now explicitly rejects
anonymous too, instead of returning 0 and relying on a downstream library
check to reject it -- the anonymous path is closed by design now, not just
unreachable in practice.

Result, confirmed empirically across every SMB dialect this harness can
force smbprotocol to negotiate:

```
$ .venv/bin/python3 smb_smoke_test.py 4500
PASS (anonymous rejected): SMBAuthenticationError: ...
PASS (x3/wrongpassword): correctly rejected with LogonFailure: ... STATUS_LOGON_FAILURE: 0xc000006d
PASS (tree_connect SD, non-guest, signed): tree_id=...
PASS (tree_connect NOPE rejected): SMBResponseException
PASS (create/close file): smoke_test_file.txt
PASS (create/close directory): share root
PASS (protected path rejected): SMBResponseException
PASS (echo keepalive): session still alive after idle + echo
PASS (destruction_event recycle): 12/12 connections opened a file without ever closing it
PASS (3.1.1 gap, as documented): SMBException: Server message signature could not be verified: ...

PASS: all checks passed
```

**The headline result: for SMB dialects 2.0.2, 2.1.0, 3.0.0, and 3.0.2, a
signing-required client authenticating as `x3` now gets a genuinely
non-guest session (real signing key, `session.signing_key is not None`)
and completes `tree_connect("SD")`/`create`/`close` successfully --
with *zero* client-side bypasses.** No `require_signing=False`, no
`require_secure_negotiate=False`, no forced fallback dialect for the sake of
sidestepping a guest check -- `connect_session()` in `smb_smoke_test.py`
uses the client's own defaults except for pinning the dialect (see next
paragraph for why that pin exists). This is real, complete,
protocol-correct end-to-end success: NEGOTIATE, real NTLMv2 authentication,
session-key/signing-key derivation, a signed and verified SESSION_SETUP
reply, and a signed TREE_CONNECT. `tree_connect("NOPE")` is still correctly
rejected. Compare this to round 1's baseline (below), where every one of
these dialects would have hit the guest-flag rejection at session setup,
before ever reaching tree_connect.

**The dialect pin exists because of a newly-*visible* (not newly
*introduced*) problem at SMB 3.1.1 specifically** -- the dialect
`smbprotocol` (and very likely a real iOS/macOS client) negotiates by
default when both sides advertise support for it, since it's the highest
mutually-supported dialect. At 3.1.1, the client's own verification of the
SESSION_SETUP reply's signature fails outright:
`SMBException: Server message signature could not be verified: X != Y`.
This is a *different* failure from round 1's guest-flag rejection --
proven by the fact that it never appeared until the guest-flag issue was
fixed (guest sessions are never even signature-checked by the client at
this point; see `smbprotocol`'s `session.py` -- the guest-flag branch
raises and returns before the final `verify_signature()` call ever runs).
The most likely explanation, not yet investigated further: libsmb2's SMB
3.1.1 signing-key derivation (which, unlike 3.0.x, folds in a running
preauth-integrity hash accumulated across NEGOTIATE and SESSION_SETUP) ends
up producing a different key server-side than the client independently
derives. **Deliberately not fixed this round** -- same reasoning as round
1's guest-flag finding: this is server-vs-client vendored-library signing
behavior, not something `SmbFileHandlers.cpp` can address, and any real fix
would likely touch `lib/smb2/` itself, which needs the same kind of
explicit sign-off as Task 3's one already-committed patch (see
`scripts/verify_libsmb2_patch.py`'s "append-only, one hunk" invariant,
discussed in round 1's baseline below). `smb_smoke_test.py`'s
`test_smb311_signature_gap()` pins this down precisely (asserts the failure
is specifically a signature mismatch, not any other error) so a future fix
attempt gets an unambiguous, currently-failing regression test to turn
green, and so a completely unrelated new failure at 3.1.1 won't be
mistaken for "the known gap, nothing to see here."

**What this means for "will the iPhone connect" today**: unknown without a
real device test -- if iOS's SMB2 client, like `smbprotocol`, prefers 3.1.1
whenever available, it will very likely hit this same signature-mismatch
wall. If it can be made to negotiate 3.0.2 or lower (not something this
server currently controls, since libsmb2 offers whatever dialects it's
configured to advertise and the client picks the highest it also supports),
the session should work end-to-end per the results above. This is the
single most important open question for whoever picks up the next round.

## Baseline (Task 4, round 1 -- superseded, kept for history)

**Superseded by round 2 above -- the guest-flag problem described in this
section is fixed (`allow_anonymous = 0`), and `smb_smoke_test.py` no longer
contains the bypasses this section describes.** Kept verbatim for the
historical trail: it's the original, correct diagnosis of the guest-flag
mechanism, and the reasoning that led to correctly identifying
`allow_anonymous` as the fix in round 2.

Task 4 replaced `authorize_user`'s stub (which admitted the `x3` identity
without ever checking the password) with a real one that calls
`smb2_set_password(smb2, "x3")`, making libsmb2 perform genuine NTLMv2
verification. Confirmed empirically:

```
$ .venv/bin/python3 smb_smoke_test.py 4460
EXPECTED FAILURE (anonymous): SMBAuthenticationError: ... (unchanged -- client-side limitation)
EXPECTED FAILURE (x3/x3): SMBException: SMB encryption or signing was required but session was authenticated as a guest which does not support encryption or signing
PASS (x3/wrongpassword): correctly rejected with LogonFailure: ... STATUS_LOGON_FAILURE: 0xc000006d
PASS (tree_connect SD): tree_id=...
PASS (tree_connect NOPE rejected): SMBResponseException
PASS (create/close file): smoke_test_file.txt
PASS (create/close directory): share root
PASS (protected path rejected): SMBResponseException
PASS (echo keepalive): session still alive after idle + echo
PASS (destruction_event recycle): 12/12 connections opened a file without ever closing it
```

**A wrong password for `x3` now fails differently from a correct one**
(`STATUS_LOGON_FAILURE` vs. reaching a "guest" rejection) -- proof the
handler does real cryptographic verification now, not just a username
check. That's the good news.

**The bad news, found while verifying this task (not anticipated by the
Task 4 brief, which assumed setting the password would be sufficient): even
with the correct password, the resulting session is still marked
`SMB2_SESSION_FLAG_IS_GUEST` in the wire reply.** Root cause (see
`authorizeUser()`'s own comment in `../../src/network/SmbFileHandlers.cpp`
for the full trace through the library source): `ntlmssp_authenticate_blob()`
(`lib/smb2/lib/ntlmssp.c:1276`) unconditionally wipes `smb2->password` back
to `""` immediately after using it to verify the NTLM proof -- and
`libsmb2.c`'s guest-flag decision (`libsmb2.c:4179-4183`) runs later,
checking that already-wiped value. **No handler callback runs between the
wipe and that check**, so there is nothing `SmbFileHandlers.cpp` can do to
close this gap -- confirmed by direct experiment (setting the password and
observing the client still report a guest session; see
`task-4-report.md`'s "concerns" section for the two raw repro scripts and
their output).

A client that requires signing (`smbprotocol`'s default, and per this
project's own constraints, macOS/iOS) refuses a guest-flagged session
locally, **before ever attempting tree_connect** -- so the tests above that
exercise `tree_connect`/`create`/`close` do so via a `Connection` opened
with `require_signing=False`, dialect forced to `SMB_2_0_2` (sidesteps SMB
3.1.1's own "TREE_CONNECT is always signed" special case,
`lib/smb2/lib/pdu.c:701-702`), and `TreeConnect.connect(require_secure_negotiate=False)`
(`smbprotocol`'s own anti-downgrade check, which also refuses guest sessions
by design and offers this exact opt-out). These are the **client's own**
documented escape hatches for "I know this is a guest/anonymous session and
I accept that" -- nothing server-side changes for these tests; they exist so
the handler logic itself (tree_connect share validation, create/close file
I/O, path protection, destruction_event slot recycling) could still be
verified despite the guest-flag issue blocking the "normal" path.

**Fixing the guest-flag issue for real would mean editing
`ntlmssp.c`/`libsmb2.c` themselves** -- out of Task 4's file scope
(`SmbFileHandlers.{h,cpp}` only) and would need the same kind of explicit
sign-off Task 3's one already-committed vendored-file patch got:
`scripts/verify_libsmb2_patch.py` machine-checks that the *entire*
`lib/smb2/` tree matches pristine upstream except for exactly one appended
hunk in `lib/libsmb2.c` -- a second, in-place edit to an existing function
(not an append) would fail that check by design. This is flagged for the
coordinator to decide, not something Task 4 silently worked around.

## Baseline (Task 3, for additional history)

With the patch in place, a real SMB2 client now reaches **at least** Task
2's original baseline (NEGOTIATE completes, then session setup fails) —
and, for the `x3`/`x3` case, gets further than Task 2's baseline did,
because this task's stub `authorize_user` (per its brief) admits `x3` at
the identity level (`return 0`), unlike Task 2's stub, which denied every
user unconditionally. "Getting further" here means failing for a different,
later reason (signing/session-key material Task 4 hasn't implemented yet),
not "succeeding" — no real file access is possible yet.

Exact output observed running `smb_smoke_test.py` against `smbhost` on this
branch (2026-07-28), after the patch:

```
$ .venv/bin/python3 smb_smoke_test.py 4452
EXPECTED FAILURE (anonymous): SMBAuthenticationError: Failed to authenticate with server: SpnegoError (1): SpnegoError (16): Operation not supported or available, Context: No username or password was specified and the credential cache did not exist or contained no credentials, Context: Unable to negotiate common mechanism
EXPECTED FAILURE (x3/x3): SMBException: SMB encryption or signing was required but session was authenticated as a guest which does not support encryption or signing
PASS: reached session setup over a real socket and both attempts failed as documented in README.md's baseline -- harness, host libsmb2 build, and Python client all work.
```

Server-side (`smbhost`'s stderr) for the same run:

```
[SMB] INF: listening on port 4452
[smbhost] listening on 127.0.0.1:4452 (SmbServer + stub SmbFileHandlers)
[SMB] DBG: smb2_service(in) failed: Read from socket failed, remote closed connection.
[SMB] DBG: smb2_service(in) failed: Read from socket failed, remote closed connection.
```

No "cannot bootstrap" error appears — the earlier failure mode is gone.
Both attempts complete NEGOTIATE (`smb_smoke_test.py`'s `try_login()` only
prints "could not even complete NEGOTIATE" when `connection.connect()`
itself raises, which happens for neither attempt here); both then fail at
session setup, for the two different, expected reasons:

- **Anonymous attempt**: unchanged from Task 2's baseline -- `smbprotocol`'s
  own SPNEGO/NTLM layer refuses to build an anonymous authentication token
  client-side (no credential cache, no username/password given). A
  client-library limitation, not something our server rejects.
- **`x3`/`x3` attempt**: this task's stub `authorize_user` admits the
  identity (`return 0`), so libsmb2 proceeds to establish a session --
  without real credential/session-key handling (Task 4's job), the
  resulting session has no valid signing key, and `smbprotocol` (which
  requires signing by default) rejects it client-side once it realizes the
  session came back as guest-only. This is exactly the kind of "further,
  but still a clean failure" progress the coordinator's verification bar
  anticipated.

If later tasks change handler behavior and this baseline needs to move
(e.g. Task 4 adds real credential/session-key handling that lets a
signing-capable session actually establish), update this section to
describe the new baseline and why it changed.

### Task 2's baseline, for additional history

The below was true when `main.cpp` called `smb2_serve_port()` directly
(Task 2) and, separately, briefly true again while the accept-bootstrap gap
above was still open (an intermediate state of this same task, no longer
current) -- kept for reference since the anonymous-attempt behavior and the
`authorize_user` null-guard note below remain accurate today.

```
EXPECTED FAILURE (anonymous): SMBAuthenticationError: Failed to authenticate with server: SpnegoError (1): SpnegoError (16): Operation not supported or available, Context: No username or password was specified and the credential cache did not exist or contained no credentials, Context: Unable to negotiate common mechanism
EXPECTED FAILURE (x3/x3): LogonFailure: Received unexpected status from the server: The attempted logon is invalid. This is either due to a bad username or authentication information. (3221225581) STATUS_LOGON_FAILURE: 0xc000006d
PASS: reached session setup over a real socket and both attempts failed as documented in README.md's baseline -- harness, host libsmb2 build, and Python client all work.
```

That `x3`/`x3` result (`LogonFailure`/`STATUS_LOGON_FAILURE`) was produced by
Task 2's stub, which denied every user unconditionally; this task's stub
admits `x3` instead (per its brief), which is why the current baseline above
differs for that one case specifically.

### A note on `authorize_user` (still accurate after Task 4)

Unlike every other entry in `smb2_server_request_handlers`,
`lib/smb2/lib/ntlmssp.c:1215-1219` calls
`server->handlers->authorize_user(...)` **unconditionally** whenever
`server->handlers` is non-null -- there is no
`server->handlers && server->handlers->authorize_user` guard, unlike every
`handlers->*_cmd` call site in `lib/smb2/lib/libsmb2.c`. Leaving this field
null while `server.handlers` is set is a null function pointer call (a
crash), not a clean "not implemented" failure. `SmbFileHandlers.cpp` (Task 4)
wires a real, non-null handler specifically because of this. Keep that in
mind if the handler table is ever restructured.

## `config.h` on the host

Per the Task 2 plan: try `lib/smb2/include/esp/config.h` first (it defines
the right `HAVE_*` macros for the POSIX headers Linux also has, and
`ESP_PLATFORM` -- which gates libsmb2's own ESP-specific branches -- is not
defined on Linux, so those branches correctly stay off); only add a
host-specific `config.h` if that genuinely fails.

It genuinely fails in exactly one place: `HAVE_NETINET_TCP_H` is `#undef`'d
in `include/esp/config.h` (ESP-IDF's lwIP doesn't expose `<netinet/tcp.h>`
the way upstream's autoconf probe expects), but real Linux libc has that
header, and `lib/smb2/lib/socket.c` needs `TCP_NODELAY` from it in three
places (`connect_async_ai()`, `smb2_bind_and_listen()`,
`smb2_accept_connection_async()`). Confirmed by an actual build failure, not
a guess:

```
lib/smb2/lib/socket.c:1213:29: error: 'TCP_NODELAY' undeclared (first use in this function); did you mean 'O_NDELAY'?
```

So `test/host/host_config/config.h` exists, `#include`s the esp one as its
base, and `#define`s that one macro on top. The Makefile puts `-Ihost_config`
*before* `-I$(SMB2)/include/esp` so the quoted `#include "config.h"` (used
both by `lib/smb2/lib/*.c` and, via `CrossPointSmb2.h`, by `main.cpp`)
resolves to this file instead. `lib/smb2/` itself is untouched -- see
`host_config/config.h` for the full comment.

## Host shims

`lib/Logging/Logging.h` (the device's `LOG_ERR`/`LOG_INF`/`LOG_DBG` macros)
`#include <HardwareSerial.h>` and is Arduino-only, so it cannot compile in
this desktop harness -- `SmbServer.cpp` uses those macros. Rather than weaken
the device-side header, `test/host/host_config/Logging.h` is a minimal host
shim (prints to stderr) that gets picked up in place of the real one, using
the exact same placement trick as `host_config/config.h`: the Makefile puts
`-Ihost_config` first in the include search order, so a bare
`#include <Logging.h>` resolves there. See that file's own header comment.

## Installing `smbprotocol`

This is Ubuntu with PEP 668 ("externally managed environment") in effect --
a bare `pip install smbprotocol` is refused:

```
error: externally-managed-environment
```

Do not pass `--break-system-packages`, and do not install into
the PlatformIO build virtualenv (the environment used for the
firmware build; keeping this harness's Python dependency out of it is
deliberate). Instead, this harness gets its own isolated virtualenv, created
and used from inside `test/host/`:

```bash
cd test/host
python3 -m venv .venv
.venv/bin/pip install --upgrade pip
.venv/bin/pip install smbprotocol
.venv/bin/python3 smb_smoke_test.py
```

`.venv/` is gitignored (see `.gitignore`). Verified working with
`smbprotocol` 1.17.0 on Python 3.12.3 (Ubuntu 24.04).

## Verifying the firmware build is unaffected

`test/` is PlatformIO's reserved `test_dir` (`platformio.ini` does not
override it). Adding `test/host/` did not require any change to
`platformio.ini` -- `pio run -e gh_release` only scans `test/` for the
`pio test` unit-test runner, not for the firmware build itself. Verified:
`pio run -e gh_release` produces the identical flash usage before and after
this harness was added (`5,095,937` bytes, matching Task 1's recorded
baseline) -- see
`.superpowers/sdd/2026-07-28-smb2-server/task-2-report.md` for the full
before/after build output.

## Stub HAL scope

`stub_hal/HalStorage.h` mirrors every public method of
`lib/hal/HalStorage.h`'s `HalStorage` and `HalFile` classes by name,
parameter types, defaults, and return type -- diffed method by method (see
the task report). It does **not** faithfully reproduce Arduino's `String` or
`Print` classes, or FreeRTOS's `SemaphoreHandle_t` -- see the header comment
in `stub_hal/HalStorage.h` for exactly what's simplified and why (short
version: those are supporting framework types, not part of the storage HAL's
own contract, and this harness is single-threaded so there is nothing to
mutex against).

**Where the stub must be faithful, it must be faithful.** Those
simplifications are all in *supporting framework types*. Behavior that handler
code can observe is a different matter, and round 4 found the stub getting one
wrong: `open()` ignored `oflag` for directories, while SdFat refuses a
write-mode open of a subdirectory (`FatFile.cpp:581-585`) and exempts only the
share root (`openRoot()`, `FatFile.cpp:456-461`). Both halves are modelled
now. Anything that changes whether an operation *succeeds* belongs on the
faithful side of that line -- a divergence there doesn't simplify the harness,
it silently inverts its verdict.

The running list of places where POSIX is looser than SdFat and the stub
therefore has to say no on purpose (each one is commented at its call site):

| Behaviour | SdFat | POSIX | Added in |
|---|---|---|---|
| write-mode open of a subdirectory | refused (`FatFile.cpp:581-585`) | allowed | Task 4 |
| `O_CREAT` without write mode | does not create (`FatFileLFN.cpp:372-373`) | creates | Task 4 |
| `O_TRUNC` without write mode | refused (`FatFile.cpp:552-556`) | allowed | Task 4 |
| `openNextFile()` on an unreadable entry | ends the listing (`FatFile.cpp:676-679`) | (stub used to skip) | Task 5 |
| `getName()` into a short buffer | fails, returns 0 (`FatName.cpp:99-160`) | (stub used to truncate) | Task 5 |
| `seek64()` past end-of-file | refused (`FatFile.cpp:1184-1188`) | allowed, makes a hole | Task 6 |
| `sync()` on a directory handle | succeeds (`FatFile.cpp:1231-1261`) | no descriptor to `fsync` | Task 6 (stub was *stricter*) |
| `close()`'s return value | the final `sync()`'s result (`FatFile.cpp:128-132`) | (stub returned a flat `true`) | Task 6 |
| `getModifyDateTime()` on the **share root** | succeeds, returning a decode of **sector 0** (`openRoot()` leaves `m_dirSector = 0`, `FatFile.cpp:697-724` + `:200-219`) | stub returns the host directory's real mtime — different value, **same observable shape**: succeeds, means nothing | Task 7 |
| `getModifyDateTime()` outside 1980-2107 | not representable (7-bit year field) | stub returns false rather than wrapping into a wrong year | Task 7 |
| `rename()` onto an EXISTING name | refused -- create-exclusive (`O_CREAT\|O_EXCL` for files, `mkdir(..., pFlag=false)` for dirs, `FatFile.cpp:970-984`) | POSIX `rename(2)` replaces silently | Task 7 |
| `truncate(length)` GROWING a file | impossible -- it is `seekSet(length) && truncate()` and seekSet refuses past EOF (`FatFile.h:957`) | `ftruncate()` grows and zero-fills | Task 7 |
| `truncate()`'s effect on file position | leaves it at the new end of file | `ftruncate()` does not move the position | Task 7 |
| **name case** | **case-INSENSITIVE**: `VICTIM.TXT` and `victim.txt` are one file | case-sensitive; both can exist | Task 7 fix 2 |
| **`remove()` while another handle has the file open** | corrupts -- the live `FatFile` caches the freed directory entry and cluster chain (`FatFile.h:570-571`) | `unlink()` is refcounted, so it just works. **The stub CANNOT model this**; the handler refuses instead and the test asserts the refusal | Task 7 fix 2 |
| `setTimestamp()`'s creation stamp | FAT stores one (`T_CREATE`) | POSIX birthtime is not settable -- the stub accepts and drops it. Nothing in the harness can read a creation time back, so no assertion is affected | Task 7 fix 2 |
| `setTimestamp()` year range | 1980-2099 (`FatFile.cpp:1278-1283`) -- **narrower than the getter's 2107** | n/a; the stub reproduces SdFat's validation field for field | Task 7 fix 2 |
| `setTimestamp()` on the **share root** | refused -- `timestamp()` opens with `isFileOrSubDir()` (`FatFile.cpp:1280`) and the FAT root is neither (`FILE_ATTR_ROOT_FIXED`/`ROOT32`, never `FILE_ATTR_SUBDIR`, `FatFile.h:454-460`, `:1014-1022`) | `utimensat()` on the tree root works fine -- the stub had to be taught the guard | Task 7 fix 3 |
| **case-folding depth** | case-insensitive at **every** path component | the stub folds only the LAST component, so it refuses a rename into `SUB/` when `Sub/` exists, where FAT accepts. Known residual, narrower than the device | Task 7 fix 3 (residual) |
| **two live handles on one file** | corrupts for ANY pair of `FatFile` objects, not only deletes (`FatFile.h:570-571`) | refcounted and safe. `createCmd` has no same-path guard, so read / write / `setEndOfFile` remain **unguarded on both sides** -- pre-existing (Task 6 already allowed concurrent handles) and out of scope; only delete and rename, which free or move the directory entry, are guarded | Task 7 fix 3 (residual) |


---

## Signing is adaptive (v82), and how to get the independent clients back

The server no longer calls `smb2_set_sign(ctx, 1)`. libsmb2's own NEGOTIATE
logic decides: `smb2->sign` is raised only when the CLIENT sends
SIGNING_REQUIRED, because the dialect 2.1.0 and >= 3.1.1 clauses cannot fire
while the dialect is pinned to 3.0.2. `server->signing_enabled` stays 1, so a
client that *requires* signing still gets a signed session and is never dropped
before the reply (the v64 bug stays fixed).

**Why.** AES-CMAC runs over every PDU in both directions, and the vendored AES
re-derives its 176-byte key schedule for every 16-byte block
(`aes_reference.c:444-456`) — 326,164 expansions per direction for a 5 MB file,
which on a 160 MHz RV32 is this project's long-recorded ~300-600 KB/s signing
ceiling. The owner's call, made explicitly: a personal device, on a home LAN,
with the server off except during a transfer — and **this server has never
encrypted anything**, so signing only ever bought integrity against an active
on-path attacker, not confidentiality. NTLM authentication is untouched.

**⚠️ What it costs this harness, measured A/B, not assumed.** A client that
advertises SIGNING_ENABLED and then refuses an unsigned reply stops working:

```
signing on   ->  smbclient //127.0.0.1/SD ...  lists the share
adaptive     ->  tree connect failed: NT_STATUS_ACCESS_DENIED
```

Samba `smbclient` and the Linux kernel `cifs` client are **two of the three
independent implementations** this suite exists to use, and every blocker found
in the SMB work came through one of them (see "the harness is a claim that needs
verifying" above). Losing them would cost far more than the seconds signing
saves — so they get an escape hatch rather than a eulogy:

```bash
SMBHOST_SIGN=1 SMBHOST_ROOT=$R ./smbhost 4450     # forces signing on
```

`main.cpp` reads that variable and calls `SmbServer::setForceSigning(true)`.
**The default is adaptive, deliberately** — the suite must test what the device
does, and a harness that quietly signed when the device did not would be
divergence number ten.

Two checks pin both halves: one asserts an ENABLED-only client gets an UNSIGNED
response from a default server, the other spawns its own `SMBHOST_SIGN=1` server
and asserts the same client gets a signed one. If you ever restore
`smb2_set_sign(ctx, 1)` unconditionally, the first goes red.
