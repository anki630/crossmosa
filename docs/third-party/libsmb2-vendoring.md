# libsmb2 vendoring

Vendored dependency for the in-progress SMB2 server (Files app / `smb://` support
on the Xteink X3). This document originally covered only the vendoring and
build integration (task 1 of the `smb2-server` plan); it now also covers the
one deliberate patch on top of that vendoring (task 3), added below.

## Vendoring status (updated: task 7)

**Not byte-identical to upstream any more.** The tree is: upstream commit
`dd0bbdee24bb56f7ec95431afc07ce0a666533f7` **plus exactly four divergences**:

| # | File | Kind | What |
|---|---|---|---|
| 1 | `lib/smb2/lib/libsmb2.c` | **appended** | one function, `crossmosa_smb2_finish_accept()`, added after the end of the upstream file (task 3) |
| 2 | `lib/smb2/lib/smb2-cmd-query-directory.c` | **in-line** | one expression, `offset + fs_size` → `fs_size`, fixing an absolute-vs-relative `NextEntryOffset` bug, plus its banner comment (task 5) |
| 3 | `lib/smb2/lib/smb2-cmd-set-info.c` | **in-line** | one four-line `if (!smb2->passthrough) { … return -1; }` guard removed from `smb2_process_set_info_request_variable()`, replaced by its banner comment, so the server hands the SET_INFO buffer to the handler instead of tearing the connection down (task 7) |

Patch 1 touches no existing line at all. Patch 2 changes exactly one
expression on one line. Patch 3 deletes exactly one four-line block and adds
no executable line — the two statements that follow it are upstream's own,
untouched. Nothing else anywhere in `lib/smb2/` is modified.

This is a machine-checked fact, not a promise:
`scripts/verify_libsmb2_patch.py` asserts the vendored tree differs from
pristine upstream in exactly these three places — pinning each one's content
by SHA-256, so a *different* edit to any patched file is caught, not just "the
file differs" — and fails loudly on any other divergence in any file. See
"The one patch", "The second patch" and "The third patch" below for what each
does, why, and what to do when upgrading libsmb2.

**If a fourth divergence is ever added, this table and the script must change
in the same commit.** Task 1's original "byte-identical, do not modify"
invariant (this section used to say so, and the whole of task 1's review
existed to prove it) no longer holds and should not be assumed by anyone
reading old task reports; this section is the current source of truth.

## Upstream

- Repo: https://github.com/sahlberg/libsmb2
- Commit vendored: `dd0bbdee24bb56f7ec95431afc07ce0a666533f7` (cloned 2026-07-28,
  `git describe`: `libsmb2-6.2-289-gdd0bbde`)
- `library.json` `"version": "3.0.1"` is **not** the upstream libsmb2 release
  version (that's 6.1.0/6.2 per `CMakeLists.txt`) — it's copied from upstream's
  own `idf_component.yml`, i.e. the version upstream itself assigns to its
  ESP-IDF component. Kept for consistency with how upstream versions its ESP
  integration.

## What was copied, and what wasn't

Per upstream's own `COPYING` file, licensing is split by directory:

| Path in this vendor tree | Upstream license | Compiled by us? |
|---|---|---|
| `lib/smb2/lib/*.c` (54 top-level files) | LGPL-2.1 | **Yes** |
| `lib/smb2/include/` (except DCE/RPC headers) | LGPL-2.1 | Yes (headers only) |
| `lib/smb2/lib/ps2/`, `lib/smb2/lib/dreamcast/` | LGPL-2.1 | **No** — excluded via `srcFilter` (see below) |
| `lib/smb2/libdcerpc/dcerpc.c`, `dcerpc-srvsvc.c` (2 of 6 files) | 2-Clause BSD | **Yes** — see correction below |
| `lib/smb2/libdcerpc/dcerpc-dtyp.c`, `dcerpc-lsa.c`, `dcerpc-winreg.c`, `dcerpc-wkssvc.c` (remaining 4) | 2-Clause BSD | **No** — never `#include`d by anything we compile |
| `include/smb2/libsmb2-dcerpc*.h` | 2-Clause BSD | Headers only, not built |
| upstream `examples/` | 2-Clause BSD | Not vendored (not copied) |

**Correction (found during the Task 1 fix round while investigating compiler
warnings, see below)**: this table originally said all of `libdcerpc/` was
"outside `library.json`'s `srcDir`, not compiled." That's true of the
*directory as a build target* — `library.json`'s `srcDir` really is scoped to
`lib/smb2/lib` only, and PlatformIO never compiles `lib/smb2/libdcerpc/*.c`
as its own translation units. But two of our 54 "genuine LGPL" files use a
unity-build trick to pull BSD content in anyway:

```c
// lib/smb2/lib/libsmb2-dcerpc.c:17
#include "../libdcerpc/dcerpc.c"

// lib/smb2/lib/libsmb2-dcerpc-srvsvc.c:15
#include "../libdcerpc/dcerpc-srvsvc.c"
```

So `dcerpc.c` and `dcerpc-srvsvc.c` (2-Clause BSD) genuinely end up compiled
into `libsmb2-dcerpc.c.o` / `libsmb2-dcerpc-srvsvc.c.o` and thus into the
firmware binary — this is upstream's own design (`libsmb2-dcerpc-prefix.h`
exists specifically to manage the nested `config.h` re-inclusion this causes),
not something introduced by our vendoring. The other 4 `libdcerpc/*.c` files
are referenced only via their `.h` headers (for type declarations used by
`dcerpc.c`/`dcerpc-srvsvc.c`), never their `.c` bodies, so they are genuinely
not compiled. **This doesn't create a new obligation** — 2-Clause BSD is
permissive and compatible with LGPL-2.1, doesn't require source disclosure,
and is compatible with the static-linking note below — but the earlier "No"
in this table was simply wrong, and is corrected here rather than left
standing.

`libdcerpc/` (as a copied directory) and the license files
(`LICENCE-LGPL-2.1.txt`, `COPYING`) were copied in for completeness /
traceability with the upstream tree, matching the brief's Step 2. Copying the
directory itself adds no *extra* build cost beyond the two files above, since
`library.json`'s `build.srcDir` is scoped to `lib/smb2/lib` and never scans
`lib/smb2/libdcerpc/` directly.

**Why `ps2/` and `dreamcast/` are excluded from the build**: these two
subdirectories under `lib/smb2/lib/` contain platform-specific glue for the
PS2 IOP SDK (`iomanX.h`, `sifman.h`, ...) and Dreamcast KallistiOS (`kos.h`),
neither of which exist in this ESP32 Arduino toolchain. Compiling them would
fail immediately. `library.json`'s `srcFilter` (`+<*> -<ps2/> -<dreamcast/>`)
excludes them; the remaining top-level `lib/*.c` count is exactly the 54 files
referenced below.

## Rule: do not modify upstream source (four narrow, documented exceptions)

Nothing under `lib/smb2/` should ever be hand-edited outside the three
divergences recorded in "Vendoring status" above — and that is
machine-checked, not a convention (see "The one patch", "The second patch" and
"The third patch" below). All configuration needed to build it on this
toolchain lives in `lib/smb2/library.json` (and, for forcing it into the
dependency graph pre-callers, `platformio.ini`).

Adding a fourth exception is a project-level decision. The bar all three
existing ones cleared: the need is real and unavoidable from outside the
library (patch 1 — architecturally unreachable functionality; patch 2 — a
defect that silently loses user data, with the only in-application workaround
costing `ceil(N/2)` network round trips per folder; patch 3 — a decode-stage
refusal that kills the connection, with no callback anywhere between it and a
handler), the change is minimal and pinned by hash, and the doc's stated
invariant is updated in the same commit as the script that enforces it.

**The process has held three times, and that is the point**: in each case an
implementer stopped and reported rather than patching on its own authority.
That is why this table can still be trusted to be complete.

What changed in task 3: this project needed one small piece of
functionality — `struct connect_data` and `smb2_negotiate_request_cb`,
described below — that is architecturally impossible to reach from outside
`lib/smb2/lib/libsmb2.c` (not "private", but literally undeclared anywhere
else, or `static`/zero-linkage). This is not "a genuine upstream bug" in the
sense the patch-script pattern (`scripts/patch_wolfssl.py` /
`scripts/patch_jpegdec.py`) was built for — those patch actual defects in
gitignored, downloaded dependencies at build time. This is different in both
*kind* and *delivery*:

- **Kind**: not a bug fix, but a small amount of new, additive functionality
  this project needs that upstream has no reason to expose (upstream's own
  server reference implementation, `smb2_serve_port()`, owns a whole
  blocking loop and was never designed to be driven piecemeal from another
  translation unit).
- **Delivery**: `patch_wolfssl.py`/`patch_jpegdec.py` patch `.pio/libdeps/`
  — downloaded, gitignored copies rebuilt from scratch on every fresh
  checkout, where a build-time `pre:` script is harmless. `lib/smb2/` is
  **vendored and committed** — a `pre:` script mutating a committed file on
  every build would dirty the tree constantly and make `git status`
  permanently noisy. So this patch is committed directly to the tree, as an
  ordinary, reviewable diff, not applied at build time.

See "The one patch" below for exactly what was added and why, and
`src/network/SmbServer.cpp` / `lib/smb2/lib/libsmb2.c`'s own banner comment
around the appended function for the code-level rationale.

## The one patch

**What**: one function, `crossmosa_smb2_finish_accept()`, appended to the
very end of `lib/smb2/lib/libsmb2.c` (after the closing brace of
`smb2_serve_port()` — nothing above that point in the file was touched).
Declared for C++ callers in `include/CrossPointSmb2.h` (**not** in any
`lib/smb2/include/` header — those stay exactly as upstream ships them, so
the "single funnel" story from the `#include <CrossPointSmb2.h>` wrapper,
described below, still holds for every header a consumer needs).

**Why it was unavoidable**: `SmbServer` (`src/network/SmbServer.{h,cpp}`,
task 3) drives libsmb2 as a server without a dedicated thread — a single
activity loop on an ESP32-C3 with no FreeRTOS task calls `SmbServer::tick()`
once per iteration, and `tick()` must never block (that loop also drives the
e-ink display; this device has no serial port, so a hung/crashed task here
is very hard to debug — this is also why an alternative design, moving
`SmbServer` onto its own FreeRTOS task, was considered and rejected: task
stack overflows and teardown races are exactly the kind of failure mode that
needs a serial port to diagnose, and this device doesn't have one).

Upstream's only server-side reference loop, `smb2_serve_port()` (still
present, completely unmodified, earlier in the same file), owns a blocking
`do { ... } while` with a fixed 100ms `select()` timeout, and finishes
preparing each freshly-accepted connection to receive `SMB2_NEGOTIATE` using
two things with zero visibility outside that one file:

- **`struct connect_data`** — defined nowhere at all outside `libsmb2.c`
  (`lib/smb2/include/libsmb2-private.h:714` only forward-declares it
  opaquely, `struct connect_data; /* defined in libsmb2.c */` — even other
  files inside `lib/smb2/lib/` itself cannot construct one).
- **`smb2_negotiate_request_cb`** — declared `static` in `libsmb2.c`, i.e.
  zero external linkage. This is a C-language fact, not a documentation gap:
  no header, no patch to a *consumer* file, no struct-layout trick recovers
  a callable pointer to a `static` function from another translation unit.

`SmbServer.cpp` already does the accept step itself via the existing public
`smb2_serve_port_async()`. `crossmosa_smb2_finish_accept()` does only the
remaining bootstrap — allocate `connect_data`, register the first PDU with
`smb2_negotiate_request_cb`, copy the three size limits, set
`owning_server` — using the exact same calls `smb2_serve_port()` makes
inline, just factored into a callable, exported function. It does **not**
reimplement any SMB2 protocol logic; every byte of actual negotiate/session
setup/tree-connect handling still runs entirely inside upstream's own,
unmodified code.

**Verification**: `scripts/verify_libsmb2_patch.py` (run manually, not wired
into any build step — a network fetch during a build would be hostile).
This is a **tree-wide** check, not a single-file one — every file under
`lib/smb2/` (128 files, excluding only `library.json`, which is this
project's own build config with no upstream counterpart) is compared
byte-for-byte against pristine upstream at the vendored commit, with
`lib/smb2/lib/libsmb2.c` checked specially: its untouched prefix must match
pristine exactly, and the appended suffix must match a pinned SHA-256 +
length of the *entire* expected hunk (not merely "contains the right
banner/function name" — that weaker check would still pass if a second,
unrelated function were appended alongside the expected one). Any other
file differing at all, any file present upstream but missing locally, or
any local file with no upstream counterpart, all fail loudly (non-zero
exit, one line per problem found, not just the first).

(Fix round 1, code review finding 2: an earlier version of this script only
ever read `lib/smb2/lib/libsmb2.c`, so a hand-edit to e.g. `socket.c` or any
header would have passed silently — this section's own "tree-wide" wording
was true of the *intent* the patch was authorised under, but not yet true
of what the script actually checked. It's tree-wide now, checked by testing
the script against deliberately-tampered scratch copies of both an
unrelated file and `libsmb2.c` itself before trusting it.)

Preferred method: a single live fetch of the whole pristine tree as a
tarball from GitHub at the pinned commit (one request, not 128), scoped to
the same directories Task 1's vendoring copied
(`lib/`, `include/`, `libdcerpc/`, `COPYING`, `LICENCE-LGPL-2.1.txt`).
Falls back to `scripts/libsmb2_pristine_manifest.json` (a pinned per-file
SHA-256 manifest, generated once from the same live fetch) when network
access isn't available — this script is explicitly not wired into any build
step, so it may run in a network-restricted environment.

**Upgrading libsmb2 later**: diff the new upstream file against the current
one *before* the appended banner comment (or diff against a fresh clone of
the vendored commit) to isolate genuine upstream changes from this patch;
re-apply `crossmosa_smb2_finish_accept()` (copy the whole banner-to-end
block) onto the new file's end; regenerate
`scripts/libsmb2_pristine_manifest.json` (see the snippet in
`verify_libsmb2_patch.py`'s own module docstring) and re-derive
`EXPECTED_SUFFIX_SHA256`/`EXPECTED_SUFFIX_LENGTH` in that script from the
re-applied function; re-run `scripts/verify_libsmb2_patch.py` against the
new commit before trusting the result. Do not assume the function still
compiles unchanged — `struct connect_data`'s layout,
`smb2_negotiate_request_cb`'s name, or `smb2_server`'s three size-limit
field names could all change upstream; re-read the function against the new
`smb2_serve_port()` rather than assuming it still applies verbatim. This
script can only confirm *consistency* of a re-applied patch, never its
*correctness* against a changed upstream — that step is still a manual,
judgment-requiring read.

## The second patch

**What**: one expression, in `lib/smb2/lib/smb2-cmd-query-directory.c`, inside
`smb2_encode_query_directory_reply()`'s per-entry loop:

```diff
  if (in_remain >= SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE) {
-         smb2_set_uint32(iov, offset + 0, offset + fs_size);
+         smb2_set_uint32(iov, offset + 0, fs_size);
  }
```

plus an unmissable banner comment immediately above it. **Nothing else in the
file is touched** — not the surrounding lines, not formatting.

**Why it is unavoidable**: this is a genuine upstream defect, and it makes
directory listings silently lose files. `offset` is the running byte position
of the **current** entry from the start of the reply buffer, so the original
emitted an **absolute** offset. MS-FSCC 2.4.8 defines `NextEntryOffset` as
"the byte offset from the beginning of **this** entry to the beginning of the
next entry" — a **relative** distance. Entry 1 is correct only by accident
(its `offset` is 0); from entry 2 on, a client walking
`p += NextEntryOffset` overshoots by the accumulated prefix.

Measured on the desktop harness before the fix, four entries in one reply:

```
off=0   next=124  name='beta.epub'
off=124 next=240  name='nested'      <- should be 116; 240 is entry 3's ABSOLUTE offset
off=364 next=0    name='gamma.bin'
                                     <- the entry at 240 ('alpha.txt') never visited
```

One file simply missing, no error anywhere. With Traditional Chinese
filenames the same overshoot lands mid-entry and the client's parse aborts
outright. This is not a house convention we could have adapted to: **libsmb2's
own client walks relatively** (`lib/libsmb2.c`, `offset += fs.next_entry_offset`
in the readdir decode loop), so upstream's server cannot feed upstream's
client. So do `smbprotocol` and every other conformant client.

The alternative considered and rejected was `smb2_set_passthrough()`, which
does make the encoder skip this loop — but `passthrough` is **context-wide**
and is consulted by nine command files including `query_info`, `read`,
`write`, `set_info` and `ioctl`. Enabling it would make this project
responsible for on-the-wire encoding across all of them. That is a different
architecture, not a cheap escape.

The application-side workaround that existed before this patch — capping
replies at two entries, the largest set the bug cannot corrupt — cost
`ceil(N/2)` round trips per folder. With the patch, `src/network/
SmbFileHandlers.cpp` packs entries until its budget is full: an 80-file
directory went from **41 round trips to 4**.

**Kind and delivery**: unlike the first patch this *is* "a genuine upstream
bug" in the sense `scripts/patch_wolfssl.py` / `scripts/patch_jpegdec.py` were
built for — but the delivery argument from "Rule: do not modify upstream
source" applies unchanged: `lib/smb2/` is vendored and **committed**, so a
build-time `pre:` script mutating it would leave `git status` permanently
dirty. It is committed directly, as an ordinary reviewable diff.

**How it is machine-checked**: `scripts/verify_libsmb2_patch.py` pins a byte
offset plus SHA-256 + length for **both** the pristine region being replaced
and the patched region replacing it, with the region snapped outward to whole
lines. On the live-fetch path everything before and after that region must be
byte-identical to pristine, so "exactly one in-line change, at exactly this
place, with exactly this content" is proven rather than asserted; pinning the
*pristine* region additionally forces a re-review if upstream ever edits the
very lines being patched. On the offline manifest path (no pristine bytes
available) the whole patched file is pinned instead, cross-checked against the
manifest's pristine entry. Verified negatively in four ways: tampering an
unrelated file, making a *second, different* edit elsewhere in this same file,
editing *inside* the pinned region, and reverting the fix — all four fail, in
both the live and offline paths.

**Upgrading libsmb2 later**: first check whether upstream has fixed this
itself, in which case **drop the patch rather than re-applying it**. The
pristine-region hash check forces that question by failing. Otherwise
re-apply the one-expression change, then regenerate all six of this patch's
pins **as a set** using the snippet in `verify_libsmb2_patch.py`'s module
docstring — they are derived together and are only meaningful together.

## The third patch

**What**: one four-line guard deleted from
`smb2_process_set_info_request_variable()`, the last function in
`lib/smb2/lib/smb2-cmd-set-info.c`:

```diff
  struct smb2_set_info_request *req = (struct smb2_set_info_request*)pdu->payload;
  struct smb2_iovec *iov = &smb2->in.iov[smb2->in.niov - 1];

- if (!smb2->passthrough) {
-         smb2_set_error(smb2, "can not interpret set-info buffers yet");
-         return -1;
- }
  req->input_data = iov->buf;
  return 0;
```

plus an unmissable banner comment where the block was. **No executable line is
added**: the two statements that remain are upstream's own, previously reached
only in passthrough mode. Nothing else in the file is touched.

**Why it is unavoidable**: upstream's own wording is "can not interpret
set-info buffers **yet**" — SET_INFO is simply unimplemented on the server
path, not refused as invalid. On its own that would be an ordinary missing
feature. What makes it a project-stopping defect is *where* the `-1` comes
from: this is a **decode** function, not a handler.
`smb2_process_payload_variable()` (`pdu.c:1101`) propagates it out of
`smb2_read_from_socket()`, and the connection is torn down — whereas the same
`-1` returned from `handlers->set_info_cmd` produces a clean per-request
`STATUS_NOT_IMPLEMENTED` and leaves the session alive (`libsmb2.c:3883-3893`).
**There is no callback between the two points**, so nothing in
`src/network/SmbFileHandlers.cpp` can intervene, and `set_info_cmd`'s own
`return -1` had in fact never executed once.

Measured on the desktop harness before the patch — one rename request, from a
client that had just successfully written a file:

```
client: RENAME FAILED: SMBConnectionClosed: SMB socket was closed
        close failed: SMBConnectionClosed          <- the whole session is gone
server: [SMB] DBG: smb2_service(in) failed: Failed to parse variable part of
        command payload. can not interpret set-info buffers yet
        SMB destruction_event ctx=0x... freed=1 sync_failed=0
```

And this is the **primary use case**, not an optional extra: macOS and iOS
send SET_INFO after writing a file — `FILE_BASIC_INFORMATION` to stamp
timestamps, `FILE_DISPOSITION_INFORMATION` to delete,
`FILE_END_OF_FILE_INFORMATION` to set size — so an iPhone would very likely be
disconnected at the end of its first copy, with rename and delete impossible
besides.

The change is **server-only by construction**: a client never *receives* a
SET_INFO request, so this function is unreachable in client mode and no client
path can be affected.

The alternative, `smb2_set_passthrough()`, was rejected for the same reason as
in "The second patch", and now more sharply: `passthrough` is context-wide,
and on the server path it would hand this project the **wire encoding of
`query_directory`** — the very encoder patch 2 exists to fix. Traced site by
site: `query_info`'s passthrough branch is only its
`created_output_buffer_length == 0` fallback (unreached by the classes this
server answers), and `read`/`write`'s sites are in the client-side request
*encoders*, so `query_directory` is the sole real conflict — but it is
sufficient. A third option, toggling `passthrough` per handler, is wrong by
construction: the decode happens before the handler and the encode after it,
so no handler can leave the flag correct for both, and a compound request
chain breaks any "restore it next tick" scheme.

**How it is machine-checked**: same scheme as patch 2, and now literally the
same code — `verify_libsmb2_patch.py` grew an `InlinePatch` record per in-line
divergence and one shared `check_inline_patch()`, rather than a second copy of
the checker (two near-identical copies of a security-relevant check is how one
of them ends up quietly weaker). Byte offset plus SHA-256 + length for both
the pristine region being replaced and the patched region replacing it; on the
live path everything before and after must be byte-identical to pristine; on
the offline manifest path the whole patched file is pinned and cross-checked
against the manifest's pristine entry. Verified negatively, in **both** paths:
tampering an unrelated file, making a second different edit elsewhere in this
same file, and reverting the guard — all caught.

One wrinkle worth knowing before editing the banner: it deliberately **quotes
the upstream code it removes**, so the script's "the guard is back" sanity
needles are anchored to the code's real leading newline + 8-space indent and
to the single-line form of the `smb2_set_error()` call. A bare substring
needle matched the banner itself and failed a correctly-patched tree.

**Upgrading libsmb2 later**: first check whether upstream has implemented
server-side set-info decoding, in which case **drop the patch rather than
re-applying it** — the pristine-region hash check forces that question by
failing. Note also that if upstream implements it by *parsing* the buffer into
typed fields (rather than passing it raw), `set_info_cmd` in
`src/network/SmbFileHandlers.cpp` must change with it: it currently decodes
`req->input_data` as raw wire bytes per MS-FSCC. Otherwise re-apply the
four-line deletion and regenerate all six of this patch's pins **as a set**.

## The fourth patch — `SMB2_MAX_VECTORS` 256 → 32

Approved by the maintainer on 2026-07-31, after three versions in a row failed on the
same wall from different directions.

**The file:** `lib/smb2/include/libsmb2-private.h`. The only divergence that is
in a header rather than a `.c`, and the only one whose effect is a *size*
rather than a *behaviour*.

**Why it was needed.** `struct smb2_io_vectors` embeds `iov[SMB2_MAX_VECTORS]`
outright, and it appears twice in every `struct smb2_pdu` and twice more in
every `struct smb2_context`. At 256 that makes a single in-flight PDU cost
~12.5 KB on a 64-bit host and **~6.3 KB on this device's 32-bit RISC-V core** —
against a largest contiguous free block of **40,948 bytes** at the moment the
SMB server starts. Room for about six PDUs, while libsmb2's own server hands
the client **128 credits**.

The device reported it directly, mid-copy from an iPhone (`diag20.log`):

```
SMB alloc FAILED site=error_reply cmd=6 status=0xc0000128, largest free block 4596
SMB service(in) FAILED ...: Read from socket failed, errno:9. Closing socket.
```

The server could not allocate a PDU for a *six-byte error reply*, so that
request received no answer at all and the connection died. The same shortage
produced `accept FAILED: err=-12` on a third connection in `diag19.log`. A
single browse took the largest free block from 40,948 down to 4,596.

**Why 32, and why that is not a guess.** `smb2_add_iovector()` was instrumented
in a throwaway copy of the tree to record the high-water mark of `v->niov`
across the whole workload — the 73-check desktop suite, a 400 KB file copied in
and read back, and the compound `CREATE + QUERY_INFO + CLOSE` an iPhone actually
sends. **The high-water mark was 8.** 32 is four times that.

**Measured effect** (x86-64; the device's 32-bit build halves both columns and
the ratio holds):

| | 256 | 32 |
|---|---|---|
| `smb2_io_vectors` | 6,168 | 792 |
| `smb2_pdu` | 12,568 | **1,816** |
| `smb2_context` | 7,256 | **1,880** |

Re-ran the 73-check suite and the 400 KB round trip at 32: all pass, SHA-256
exact, high-water still 8.

**Failure mode if it is ever exceeded.** `smb2_add_iovector()` bounds-checks
(`init.c`, `if (v->niov >= SMB2_MAX_VECTORS)`), sets `"Too many I/O vectors"`
and returns NULL — a clean refusal. It surfaces in `diag.log` as a failed
command, not as damage. Verified after the fix below: compounding 11, 20 and 40
WRITEs into one packet each closes that connection and leaves the server
listening.

> ⚠️ **This paragraph was false until v80**, and the false half is what became
> divergence #5. It used to read "*frees the caller's buffer and returns NULL —
> a clean refusal, not corruption*". **The free was the corruption.** Nine call
> sites free the same pointer again when this returns NULL
> (`smb2-cmd-query-directory.c:119`, `smb2-cmd-set-info.c:92`, `socket.c:432`,
> `:522`, `:576`, `:624`, `:669`, `:709`, `:758`). MEASURED on the desktop
> harness: 10 compounded WRITEs worked, **11 aborted with `free(): double free
> detected`** — and the compound count is chosen by the client, so this was
> reachable from the network. On TLSF it is heap damage rather than a clean
> abort. See **The fifth modification** below.
>
> The lesson worth keeping: this document asserted a failure mode that had
> never been exercised. The pin proved the *define* was 32; nothing proved what
> happened at 32.

**On upgrading libsmb2: re-measure before assuming 32 still suffices.** A new
encoder that emits more vectors per PDU is exactly the kind of change this pin
cannot see, because the pin only proves the *define* is what we set it to. The
instrumentation is four lines added to `smb2_add_iovector()`:

```c
        v->total_size += len;
        v->niov++;
        { extern int g_niov_hi; if (v->niov > g_niov_hi) { g_niov_hi = v->niov;
            fprintf(stderr, "[NIOV] high-water %d\n", g_niov_hi); fflush(stderr); } }
```

plus `int g_niov_hi = 0;` beside the file's `#include <stdio.h>`. Do it in a
copy of the tree, run the suite and a real file copy, and read the last line.

## The three required build flags

Verified 2026-07-28 by compiling all 54 of libsmb2's `lib/*.c` with this
project's exact toolchain (`riscv32-esp-elf-gcc`) and flag set: 54/54 succeeded.

1. **`-DHAVE_CONFIG_H`** — without it, upstream's own `#include "config.h"`
   guard logic in each `.c` file never fires, so `include/esp/config.h` (the
   ESP-specific config upstream ships) is never loaded. Symptom without this
   flag: `string.h`/`unistd.h` and friends get `#ifdef`-ed out because the
   feature-detection macros they're gated behind are never defined.
2. **`-D_U_=__attribute__((unused))`** — an autoconf convention upstream uses
   throughout (`unsigned char buf[16] _U_;`, `void *command_data _U_`, ...) to
   silence "unused parameter/variable" warnings inline. `include/esp/config.h`
   defines everything else autoconf normally provides but omits this one
   macro, so it must come from the build flags.
3. **Four include paths**: `include`, `include/smb2`, `include/esp`, `lib` —
   upstream's `.c` files mix `#include "config.h"` (found via `include/esp`),
   `#include "smb2.h"` / `"libsmb2.h"` (found via `include/smb2`), and
   `#include "compat.h"` / `"sha.h"` / other lib-private headers (found via
   `lib` itself, alongside `include` for `asprintf.h`/`slist.h`/etc. at the
   include root).

`ESP_PLATFORM` (the macro that gates libsmb2's own ESP support in
`include/esp/config.h`) is **not** something we define — it's already defined
by the Arduino/ESP-IDF build for every source file in this project.

### The `_U_` flag needed non-obvious shell-escaping to actually reach the compiler

This is the one piece of this task worth over-documenting, because it cost
real time to work out and will bite again if anyone "simplifies" it later.

This project's SCons/PlatformIO build ultimately invokes every compile command
via `sh -c "<the whole space-joined argv>"`
(`SCons.Platform.posix.subprocess_spawn`, confirmed by reading
`scons-local-4.8.1/SCons/Platform/posix.py`). That means the *final* rendered
command line has to be valid POSIX shell syntax, not just a valid argv list.
A naive `"-D_U_=__attribute__((unused))"` in `library.json`'s `flags` array
fails with `sh: 1: Syntax error: "(" unexpected`, because:

- PlatformIO's `ParseFlagsExtended` → SCons's `env.ParseFlags` runs
  `shlex.split()` on every flag string from `library.json` before sorting it
  into `CPPDEFINES`/`CCFLAGS`/etc. Any quotes or backslashes we put in the
  JSON string are consumed as shlex quoting syntax at *this* step and do not
  survive into the stored value.
- SCons's own `CPPDEFINES` → command-line reconstruction
  (`SCons.Defaults.processDefines`) does **no** escaping when it re-emits
  `-D<name>=<value>` — the `TODO: do we need to quote value if it contains
  space?` comment in that source file is not rhetorical.
- So whatever raw, unescaped value survives step 1 is exactly what lands in
  the final `sh -c` string in step 2, parens and all → shell syntax error.

The fix that survives **both** passes is to embed a *double-quoted, single-quoted*
value in the JSON flag string:

```json
"-D_U_=\"'__attribute__((unused))'\""
```

Read left to right, the literal (post-JSON-unescaping) string is:

```
-D_U_="'__attribute__((unused))'"
```

- Pass 1 (`shlex.split` inside `ParseFlags`): the outer `"..."` has no special
  meaning to POSIX shell-quoting for the single-quote characters it contains
  (single quotes are literal *inside* double quotes), so shlex strips only the
  outer double quotes and returns one token:
  `-D_U_='__attribute__((unused))'` — note the single quotes are now part of
  the token's actual content, not quoting syntax anymore.
- SCons's `-D` handling splits that on the first `=`, giving `CPPDEFINES`
  entry `("_U_", "'__attribute__((unused))'")` — single quotes preserved as
  literal characters in the stored value.
- Pass 2 (the final `sh -c "..."` at actual compile time): the reconstructed
  flag is `-D_U_='__attribute__((unused))'`. This time the single quotes *are*
  real shell quoting, so `sh` treats `__attribute__((unused))` as protected
  literal text, strips the quotes itself, and hands GCC exactly
  `-D_U_=__attribute__((unused))`. No syntax error, correct macro value.

If this ever needs to move to `platformio.ini`'s `[base] build_flags` instead
(the brief's documented fallback if `$PROJECT_LIBDEPS_DIR` doesn't expand as
expected — it did expand fine here, see below), the exact same double-layer
quoting is required, since `build_flags` goes through the identical
`ProcessFlags` → `ParseFlags` pipeline.

### `$PROJECT_LIBDEPS_DIR` resolution

`$PROJECT_LIBDEPS_DIR/../../lib/smb2/include` (and its three siblings) resolved
correctly — the compiled command line showed plain `-Ilib/smb2/include` etc.,
relative to the project root, confirmed by inspecting `pio run -v` output. The
`platformio.ini`-fallback path described in the brief was not needed.

## Consumer contract: `#include <CrossPointSmb2.h>` — never `smb2.h`/`libsmb2.h` directly

**Every project file that needs libsmb2 must `#include <CrossPointSmb2.h>`**
(`src/include/CrossPointSmb2.h`) and nothing else. Do not `#include <smb2.h>`
or `#include <libsmb2.h>` directly from consumer code — route through the
wrapper so the quirk explained below lives in exactly one file instead of
being a contract every consumer has to remember correctly.

`src/include/` is PlatformIO's default `PROJECT_INCLUDE_DIR`
(`platformio.include_dir`, default `${PROJECT_DIR}/include`) — the build
system adds it to `CPPPATH` unconditionally for every file in every
environment, no `library.json`/LDF wiring required. That's why the wrapper
lives there rather than inside `lib/smb2/` (which must stay byte-for-byte
upstream) or in a bespoke project library.

### Background: why a wrapper, not just "remember the right order"

This was originally documented as a two-line instruction ("include `config.h`
before `smb2.h`"), but a code reviewer flagged that as a fragile contract to
lean on across every future consumer file, and the fix is worth having, so
it was turned into this wrapper instead. The underlying mechanics, verified
with a throwaway consumer file compiled through the real project build (not
committed):

`smb2.h` guards its `#include <stdint.h>` / `#include <time.h>` behind
`#ifdef HAVE_STDINT_H` / `#ifdef HAVE_TIME_H`. Those macros are only ever
defined by `include/esp/config.h`, and libsmb2's own `lib/*.c` files all do
`#include "config.h"` before `#include "smb2.h"` — but the **public** headers
under `include/smb2/` never include `config.h` themselves. Consequence: a
bare `#include <smb2.h>` without first including `config.h` fails with
`'uint32_t'`/`'time_t' does not name a type` (confirmed — this is exactly
what happened on first attempt).

`"config.h"` is about as generic a filename as exists in C. Nothing prevents
a future vendored dependency, or some other project header, from shadowing
it depending on include-path search order — and relying on every future
consumer file to independently get a two-line include order right, using a
filename that generic, is exactly the kind of thing that breaks silently
months later. `src/include/CrossPointSmb2.h` does the
`#include "config.h"` → `#include <smb2.h>` → `#include <libsmb2.h>`
sequence exactly once, so consumers depend on one distinctive, project-owned
name instead:

```cpp
// src/include/CrossPointSmb2.h
#pragma once
#include "config.h" // resolves to lib/smb2/include/esp/config.h — do not reorder
#include <smb2.h>
#include <libsmb2.h>
```

This isn't a defect in the vendoring — the underlying include order mirrors
exactly how upstream's own `lib/*.c` files consume their own public headers,
and `-DHAVE_CONFIG_H` + the `include/esp` search path (both already in
`library.json`) are what make `#include "config.h"` resolve to the ESP config
in the wrapper. It's called out here so task 2+ doesn't waste time
rediscovering it, even though it no longer needs to be repeated per-file.

### Verified twice: direct includes, then the wrapper alone

Both were proven with a temporary consumer `.cpp` in `src/` (not committed),
referencing every symbol in the "Produces" list — `smb2_active_contexts`,
`smb2_get_fd`, `smb2_which_events`, `smb2_service`, `smb2_serve_port_async`,
`struct smb2_server`, `struct smb2_server_request_handlers` — compiled and
linked cleanly through the normal `gh_release` build both times:

1. First, `#include "config.h"` + `<smb2.h>` + `<libsmb2.h>` directly (proving
   the underlying three-header sequence works end-to-end, not just in
   isolation).
2. Then, with the wrapper in place, `#include <CrossPointSmb2.h>` **alone**
   (proving the wrapper is sufficient on its own, from a file with no other
   knowledge of `config.h`/`smb2.h`/`libsmb2.h`).

Flash size was identical (5,095,937 bytes) in both cases — the temporary
consumer function was never called from anywhere reachable, so
`--gc-sections` discarded it along with the rest of the still-unused
library, exactly as expected.

### Proven, not assumed: which `config.h` a consumer actually resolves

**libsmb2 ships seven different `config.h` files, one per platform**
(`include/{amiga_os,picow,esp,ps3,xbox,"xbox 360",apple}/config.h`), all with
the identical filename. A code reviewer correctly pushed back on the claim
above: compiling proves the wrapper *builds*, not *which* `config.h` a
consumer's `#include "config.h"` actually resolves to — the wrong one would
still compile, just silently misconfigure libsmb2 for the wrong platform.

Proven directly rather than reasoned about: recreated the temporary consumer
(`.cpp` in `src/`, `#include <CrossPointSmb2.h>` only), extracted the real
`riscv32-esp-elf-g++` command PlatformIO used to compile it (via `pio run -v`,
forcing a genuine non-cached compile), and re-ran that exact command by hand
with `-H` (GCC's header-inclusion trace) appended, redirecting the object
output to a scratch path so it never touched `.pio/build`. Full trace:

```
. include/CrossPointSmb2.h
.. lib/smb2/include/esp/config.h
.. lib/smb2/include/smb2/smb2.h
... lib/smb2/include/smb2/smb2-errors.h
...
.. lib/smb2/include/smb2/libsmb2.h
... lib/smb2/include/smb2/libsmb2-share-enum.h
```

Resolved absolute path:
**`<repo>/lib/smb2/include/esp/config.h`**. Compile
succeeded (exit 0). The only other file named exactly `config.h` anywhere in
the entire 72-line trace is `.../riscv32-esp-elf/include/sys/config.h` —
newlib's own unrelated internal header, reached via `sys/config.h`, not a
bare `config.h`, and nothing to do with libsmb2. None of the other six
platform `config.h` variants (`amiga_os/`, `picow/`, `ps3/`, `xbox/`,
`"xbox 360"/`, `apple/`) appear anywhere in the trace.

**Why this is guaranteed, not incidental**: a header can only be found if its
containing directory is on the `-I` search list for that translation unit.
The full compile command for the consumer file was inspected directly (not
inferred): its `-I` flags include `lib/smb2/include`, `lib/smb2/include/smb2`,
`lib/smb2/include/esp`, and `lib/smb2/lib` — **and no others under
`lib/smb2/include/`** — because those four paths are the *only* ones
`lib/smb2/library.json`'s `flags` array ever adds, and PlatformIO merges a
library's own `CPPPATH` into any consumer's build only through that single
mechanism (verified in round 1 by reading `piolib.py`'s
`PlatformIOLibBuilder.get_include_dirs()`). Nothing in this repository ever
adds `include/amiga_os`, `include/picow`, `include/ps3`, `include/xbox`,
`include/"xbox 360"`, or `include/apple` to any `-I` list, for any file, in
any environment — so there is no include-path ordering, no LDF mode, no
future consumer file that could cause a different `config.h` to shadow the
ESP one *without* someone first editing `library.json` to add one of those
directories, which would be an explicit, reviewable one-line diff, not a
silent failure mode. The resolution isn't "it happened to work this time" —
it's the only reachable candidate by construction.

## Forcing the library into the build (no callers yet)

Nothing in this firmware includes any smb2 header yet, so PlatformIO's Library
Dependency Finder (chain mode, scans `#include` directives) would not have
pulled `lib/smb2` into the build on its own — a green build would have proven
nothing. `smb2` was added as an explicit line in `platformio.ini`'s `[base]
lib_deps`:

```ini
lib_deps =
  ...
  wolfssl/Arduino-wolfSSL @ 5.7.2
  ; smb2: no callers yet (task 1 of smb2-server plan is vendoring-only). Listing it
  ; explicitly forces the library into the build despite no #include referencing it,
  ; so we can prove it compiles ahead of the server code landing in later tasks.
  smb2
```

Per PlatformIO's LDF docs, a dependency named explicitly in `lib_deps` is
always included regardless of LDF mode. This is how `find .../.pio/build -path
"*smb2*" -name "*.o"` was confirmed to return 54 object files for both
`gh_release` and `default`, instead of an empty (falsely "successful") build.

Once a later task adds real `#include <smb2.h>` (or similar) callers, this
explicit `lib_deps` entry becomes redundant (chain-mode LDF would find it via
the include) but is harmless to leave in place — it costs nothing and
documents that libsmb2 is an intentional, permanent project dependency rather
than an optional extra.

## Flash impact of vendoring alone

With no callers, `--gc-sections` discards the entire library. Verified by
diffing a `gh_release` build with `lib/smb2/` + the `platformio.ini` `lib_deps`
line stashed out (`git stash push -u`) against the same build with them
restored:

- Without smb2 vendored: Flash 5,095,937 / 6,553,600 bytes (77.8%)
- With smb2 vendored (54/54 `.o` produced, unused): Flash 5,095,937 / 6,553,600
  bytes (77.8%)

**Delta: 0 bytes.** Exactly the expected outcome per the task brief.

## Compiler warnings from the vendored code

Checked during the Task 1 fix round (initial verification only captured
tail-truncated build output, which a code reviewer correctly flagged as no
evidence either way). Full compiler output was captured for a genuine,
non-cached recompile of all 54 files (verification steps in the fix report,
`task-1-report.md`). Result: **113 warnings, all identical**
(`-Wdiscarded-qualifiers`), **all confined to 2 of the 54 files**:
`libsmb2-dcerpc.c.o` (62) and `libsmb2-dcerpc-srvsvc.c.o` (51) — i.e., all 113
originate in the textually-included BSD `dcerpc.c`/`dcerpc-srvsvc.c` content
described above, not in libsmb2's own client/server protocol code. The
remaining 52 of 54 compiled files produce **zero** warnings.

`-Wdiscarded-qualifiers` fires when a `const`-qualified pointer is passed
where a non-const one is expected, silently dropping the qualifier. Here it's
structural: this DCE/RPC NDR marshalling code uses one shared function-pointer
signature (`coder`) for both encoding (needs `const` source data) and
decoding (needs non-const destination data) — one of the two directions is
always going to trip this warning given that design, upstream-wide, not a
one-off mistake in a specific call site. It is not elevated to a hard error
by this project's build (`platformio.ini`'s `[base]` only hard-errors
`-Werror=return-type`, nothing else), so it does not block or fail the build.

**Decision (round 2, coordinator): suppress, scoped to the vendored library.**
`-Wno-discarded-qualifiers` was added to `lib/smb2/library.json`'s `flags`
array, alongside the existing `-Wno-unused-parameter` / `-Wno-sign-compare`.
Reasoning: these are upstream BSD `libdcerpc` sources under a firm
never-modify-upstream commitment, the warning isn't actionable for us, and
113 warnings on every single build would drown out warnings from this
project's own code in Tasks 2-9 — precisely the point where readable build
output starts to matter. Scoping the suppression to `library.json` means it
applies only when compiling the vendored library, never to project code.
Re-verified with the same method as above (delete only the 54 `.o` files,
rebuild `gh_release`, grep the full log): **warning count is now 0** (54/54
real recompiles, 0 cache hits, flash size unchanged at 5,095,937 bytes).

## License and static-linking obligations

- `lib/smb2/LICENCE-LGPL-2.1.txt` and `lib/smb2/COPYING` are the vendored
  license texts (copied verbatim from upstream, unmodified).
- The code we actually compile (`lib/smb2/lib/*.c`, minus `ps2/`/`dreamcast/`,
  plus the non-DCE/RPC headers under `lib/smb2/include/`) is **LGPL-2.1-or-later**.
- Two files under `lib/smb2/libdcerpc/` — `dcerpc.c` and `dcerpc-srvsvc.c` —
  are **2-Clause BSD** and genuinely get compiled into the firmware (pulled in
  via `#include` from two of our 54 LGPL files; see the correction under "What
  was copied, and what wasn't" above). This is not a problem: 2-Clause BSD is
  permissive, doesn't require source disclosure, and imposes no obligation
  beyond attribution (`lib/smb2/libdcerpc/dcerpc.c`'s own file header carries
  that). The remaining 4 `libdcerpc/*.c` files and the DCE/RPC headers are
  vendored but not compiled — not currently relevant since nothing builds or
  links against them.
- **This firmware links libsmb2 statically** (it's a PlatformIO/SCons static
  library, baked into one monolithic `firmware.bin` for an ESP32-C3 — there is
  no dynamic linking on this target). LGPL-2.1 §6 conditions static linking on
  either (a) shipping the linking application's own object files / source so a
  user can recombine with a modified libsmb2 and relink, or (b) another
  §6-compliant mechanism (e.g. providing a written offer for those materials).
  **This has no practical effect while CrossMosa stays a private fork** (no
  binaries are distributed to third parties). **If CrossMosa is ever open-sourced
  or firmware binaries are distributed publicly** (see the `crossmosa-fork-plan`
  memory note), this obligation becomes live and needs a concrete answer before
  release — most straightforwardly satisfied by the fact that this whole
  firmware, including `lib/smb2/` (upstream plus this doc's one documented,
  appended, source-available patch — see "The one patch" above) and this
  repo's own build system, is already public source, but this should be
  re-checked explicitly at that time rather than assumed. If anything, having
  our one modification to libsmb2 itself already committed as visible source
  makes the §6 "user can recombine with a modified libsmb2" case *more*
  straightforward, not less — there is no closed-source delta to account for.


---

## The fifth modification — the double free in `smb2_add_iovector()`

**File:** `lib/init.c`, the `v->niov >= SMB2_MAX_VECTORS` branch.
**Approved by the project owner (v80).**

Upstream frees the caller's buffer on overflow and returns NULL:

```c
if (free_cb && buf) { free_cb(buf); }
```

Nine call sites free the same pointer again. **Measured:** 10 compounded WRITEs
in one packet work; 11 aborts with `free(): double free detected`. The compound
count is the client's choice, so a remote peer picks it. On the device's TLSF
allocator this is heap corruption rather than a clean abort — damage to an
allocation belonging to something else, at a moment chosen by the network.

**Why the callee and not the nine call sites.** The other **66** call sites pass
a `free_cb` and do *not* free on NULL — they rely on this branch. So the choice
is 9 double frees or 66 leaks-on-overflow. The leak wins: it is bounded (the
request fails, and the file-transfer activity ends in `silentRestart()`), it
never touches memory owned by anything else, and it restores the ordinary C
contract that on failure ownership stays with the caller.

**If you upstream this**, the real fix is to make all 75 sites agree on one
contract. This is the safe half of it.

**Verification.** `scripts/verify_libsmb2_patch.py` pins the region with six
values and a `must_not_contain`. Negative-tested: putting the upstream lines
back makes the check FAIL and name the reason. Note the pin pattern includes the
leading newline and indentation — the banner *quotes* the removed statement to
explain it, so a bare substring would match our own comment (the same trap
patch 4's pin hit).
