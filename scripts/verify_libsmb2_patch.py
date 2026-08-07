#!/usr/bin/env python3
"""Verify the ENTIRE vendored lib/smb2/ tree matches pristine upstream,
except for FIVE exact, expected, pinned divergences:

  1. lib/smb2/lib/libsmb2.c                  -- one APPENDED function
  2. lib/smb2/lib/smb2-cmd-query-directory.c -- one IN-LINE expression fix
  3. lib/smb2/lib/smb2-cmd-set-info.c        -- one IN-LINE removed guard
  4. lib/smb2/include/libsmb2-private.h     -- one IN-LINE changed #define

Context: docs/third-party/libsmb2-vendoring.md ("The one patch", "The second
patch", "The third patch"). Task 1 vendored libsmb2 with a hard
"byte-identical to upstream" invariant, proven at the time via `diff -rq`
against a fresh clone -- a tree-wide comparison. Task 3 needed one small piece
of functionality (`crossmosa_smb2_finish_accept()`, appended at the very end
of lib/smb2/lib/libsmb2.c) that is architecturally unreachable from any other
file -- see that function's own banner comment for why. Task 5 then found a
genuine upstream defect that made directory listings silently drop files:
smb2_encode_query_directory_reply() emitted an ABSOLUTE NextEntryOffset where
MS-FSCC 2.4.8 requires a RELATIVE one, which upstream's own client (walking
`offset += fs.next_entry_offset`) cannot follow either. That is patch 2.
Task 7 found that the server path cannot decode a SET_INFO payload at all --
smb2_process_set_info_request_variable() refuses every buffered request
outside passthrough mode, and because that refusal comes from a DECODE
function its -1 tears the connection down instead of producing a clean
per-request error. That is patch 3.

The invariant is now "upstream + exactly one appended function + exactly two
in-line fixes, nothing else touched, anywhere in the tree" -- and this script
checks the WHOLE tree, not just the changed files, because a tree-wide
guarantee was exactly what the patches were authorised under. (Fix round 1,
review finding 2: an earlier version of this script only ever read
lib/libsmb2.c, so a hand-edit to e.g. socket.c or any header would have
passed silently -- the doc's "machine-checked" claim was true of one file,
not the tree it described. Fixed here.)

The two in-line patches share ONE checker (`check_inline_patch()`), driven by
an `InlinePatch` record each. Patch 3 was deliberately not added by copying
patch 2's 60-line checker: two near-identical copies of a security-relevant
check is exactly how one of them ends up quietly weaker than the other.

**Adding a third divergence is a project-level decision, not a code change**:
update the doc's stated invariant in the same commit, or the doc and this
script drift apart -- which has already been an Important review finding once.

Method: **live-fetch the whole pristine tree as a single tarball from GitHub
at the pinned vendored commit, with a pinned per-file manifest fallback**
(scripts/libsmb2_pristine_manifest.json) for network-restricted
environments. One tarball request is used instead of one HTTP request per
file (128 files) for the live path -- both because it's far fewer round
trips and because GitHub's per-commit tarball is itself a single,
content-addressed, immutable artifact, which is a cleaner thing to trust
than assembling 128 independent fetches. The manifest fallback exists
because this script is explicitly NOT wired into any build step (a network
fetch during a build would be hostile) and may run in a network-restricted
environment; the manifest itself was generated once, from the exact same
live tarball fetch, and only needs regenerating if the vendored commit is
deliberately bumped (see the regeneration snippet near the end of this
docstring).

Scope, precisely: every file under lib/smb2/ is checked against its
upstream counterpart at the same relative path, EXCEPT `library.json`
(authored by this project -- PlatformIO build config, has no upstream
counterpart at all; listed in PROJECT_OWNED_FILES) and `lib/libsmb2.c`
(checked specially -- see below). A file present locally with no pristine
counterpart, or a pristine file missing locally, both fail loudly, exactly
like a content mismatch would.

`lib/libsmb2.c` is checked as: (a) its first `len(pristine file)` bytes must
be byte-identical to pristine (the untouched prefix) -- split on a byte
length, not a text search for the banner. An earlier version of this script
searched for the banner's marker text to find the split point, which is
wrong: the marker sits a few dozen bytes *into* the banner comment, not at
the append boundary itself, so that mis-measured the prefix by the width of
the comment's opening delimiter (caught by testing the script against the
real file before trusting it). (b) everything after that offset (the
"suffix") must match a pinned SHA-256 + length of the *entire expected
appended hunk* (EXPECTED_SUFFIX_SHA256 / EXPECTED_SUFFIX_LENGTH below) --
not just "contains these substrings", which is what an earlier version of
this script checked, and which would still have passed if a second,
unrelated function had been appended alongside the expected one. (Fix round
1, review finding 2's second half: pinning the whole hunk's hash makes
"exactly one hunk" true rather than approximate.)

`lib/smb2-cmd-query-directory.c` and `lib/smb2-cmd-set-info.c` are IN-LINE
edits, so the append trick does not apply and they share a different scheme
(`check_inline_patch()`, one `InlinePatch` record each): a pinned byte OFFSET
plus pinned SHA-256 + length for BOTH the pristine region being replaced and
the patched region replacing it, with the region snapped outward to whole-line
boundaries so a failure message names readable source rather than a partial
statement.

* Live-fetch path: everything before the offset and everything after the
  region must be byte-identical to pristine, the pristine region must still
  hash to what the patch was derived against (so an upstream change *inside*
  the replaced lines is caught rather than silently re-patched), and the
  patched region must hash to its pin. Together that is "exactly one in-line
  change, at exactly this place, with exactly this content".
* Manifest path: no pristine bytes exist offline, only a per-file hash -- so
  instead the WHOLE patched file is pinned (`expected_file_sha256`), and the
  manifest's pristine entry is cross-checked against `pristine_file_sha256`.
  That proves the manifest still describes the same upstream file the
  patched-file pin was derived from, which is what closes the gap left by not
  having the bytes themselves.

Both paths catch a *different* edit to this file, which was the explicit
requirement: "a hash of the expected patched region, not just 'the file
differs'".

Usage:
    python3 scripts/verify_libsmb2_patch.py

Exit code 0 and "PASS" on success; exit code 1 and a full list of every
problem found (not just the first) on any deviation. Not wired into pio's
build -- run manually when auditing the vendored tree, or before/after a
libsmb2 version bump.

Regenerating scripts/libsmb2_pristine_manifest.json (only needed when
deliberately bumping the vendored commit -- see the vendoring doc's
"Upgrading libsmb2 later" section):

    python3 -c "
    import tarfile, hashlib, json, urllib.request, io
    commit = '<new commit>'
    prefix = f'libsmb2-{commit}/'
    data = urllib.request.urlopen(f'https://github.com/sahlberg/libsmb2/archive/{commit}.tar.gz').read()
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tf:
        for m in tf.getmembers():
            if not m.isfile() or not m.name.startswith(prefix):
                continue
            rel = m.name[len(prefix):]
            if rel.startswith(('lib/', 'include/', 'libdcerpc/')) or rel in ('COPYING', 'LICENCE-LGPL-2.1.txt'):
                d = tf.extractfile(m).read()
                files[rel] = {'sha256': hashlib.sha256(d).hexdigest(), 'length': len(d)}
    json.dump({'commit': commit, 'excluded_local_files': ['library.json'], 'files': files},
               open('scripts/libsmb2_pristine_manifest.json', 'w'), indent=2, sort_keys=True)
    "

Then re-derive EXPECTED_SUFFIX_SHA256 / EXPECTED_SUFFIX_LENGTH from the
re-applied appended function, and re-derive each in-line patch's six pins as a
set (run this once per entry in INLINE_PATCHES, changing `rel`):

    python3 -c "
    import hashlib, io, tarfile, urllib.request
    from pathlib import Path
    commit = '<new commit>'; rel = 'lib/smb2-cmd-query-directory.c'  # or 'lib/smb2-cmd-set-info.c'
    data = urllib.request.urlopen(f'https://github.com/sahlberg/libsmb2/archive/{commit}.tar.gz').read()
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as tf:
        pristine = tf.extractfile(f'libsmb2-{commit}/' + rel).read()
    local = (Path('lib/smb2') / rel).read_bytes()
    n = min(len(pristine), len(local))
    p = 0
    while p < n and pristine[p] == local[p]: p += 1
    s = 0
    while s < n - p and pristine[len(pristine)-1-s] == local[len(local)-1-s]: s += 1
    start = pristine.rfind(b'\\n', 0, p) + 1                 # snap outward to whole lines
    pe = pristine.find(b'\\n', len(pristine)-s) + 1
    le = local.find(b'\\n', len(local)-s) + 1
    assert pristine[:start] == local[:start] and pristine[pe:] == local[le:]
    h = lambda b: hashlib.sha256(b).hexdigest()
    print('QUERY_DIR_REGION_OFFSET', start)
    print('PRISTINE_QUERY_DIR_REGION_LENGTH', pe-start, h(pristine[start:pe]))
    print('EXPECTED_QUERY_DIR_REGION_LENGTH', le-start, h(local[start:le]))
    print('EXPECTED_QUERY_DIR_FILE', len(local), h(local))
    print('PRISTINE_QUERY_DIR_FILE', len(pristine), h(pristine))
    "

and re-review BOTH patches against the new upstream file by hand -- this
script can only confirm *consistency* of a re-applied patch, never its
*correctness* against a changed upstream. For patch 2 in particular, check
whether upstream has fixed the NextEntryOffset bug itself, in which case the
patch should be dropped rather than re-applied (the pristine-region hash
check will force that question by failing).
"""

import hashlib
import io
import json
import sys
import tarfile
import urllib.error
import urllib.request
from pathlib import Path
from typing import NamedTuple

PROJECT_DIR = Path(__file__).resolve().parent.parent
VENDOR_ROOT = PROJECT_DIR / "lib" / "smb2"
MANIFEST_PATH = Path(__file__).resolve().parent / "libsmb2_pristine_manifest.json"

# The exact commit task 1 vendored (docs/third-party/libsmb2-vendoring.md,
# "Upstream" section). Pinning this (not e.g. a branch name) is what makes
# "pristine" well-defined -- upstream's own history moving on must not
# silently change what this script considers a pass.
VENDORED_COMMIT = "dd0bbdee24bb56f7ec95431afc07ce0a666533f7"
PRISTINE_TARBALL_URL = f"https://github.com/sahlberg/libsmb2/archive/{VENDORED_COMMIT}.tar.gz"
TARBALL_PREFIX = f"libsmb2-{VENDORED_COMMIT}/"

# Relative to lib/smb2/. The three files with intentional, pinned divergences
# -- see module docstring. Everything else in the tree must be byte-identical.
LIBSMB2_C_REL = "lib/libsmb2.c"
QUERY_DIR_C_REL = "lib/smb2-cmd-query-directory.c"
SET_INFO_C_REL = "lib/smb2-cmd-set-info.c"
# Patch 4 (v73): the only divergence in a HEADER rather than a .c file, and the
# only one whose effect is a size rather than a behaviour -- see its banner in
# the file itself for the measurement that chose 32.
PRIVATE_H_REL = "include/libsmb2-private.h"
INIT_C_REL = "lib/init.c"

# Files under lib/smb2/ authored by this project (not vendored from
# upstream, no pristine counterpart at all) -- excluded from the walk
# entirely, not compared against anything.
PROJECT_OWNED_FILES = {"library.json"}

# What Task 1 actually copied from upstream (docs/third-party/libsmb2-vendoring.md,
# "What was copied, and what wasn't") -- upstream's repo root also has
# examples/, tests/, cmake/, packaging/, dcerpc-examples/, utils/, CI config,
# etc. that were never vendored at all. The pristine tarball contains the
# *whole* upstream repo, so both the live-fetch and manifest-regeneration
# paths must scope to exactly this subset, or every one of those genuinely
# never-vendored upstream files shows up as "missing locally" (caught by
# testing this script against the real tree before trusting it: the first
# version of this rewrite reported 129 false "missing" files for exactly
# this reason).
VENDORED_SUBTREE_DIRS = ("lib/", "include/", "libdcerpc/")
VENDORED_SUBTREE_FILES = ("COPYING", "LICENCE-LGPL-2.1.txt")


def _in_vendored_subtree(rel: str) -> bool:
    return rel.startswith(VENDORED_SUBTREE_DIRS) or rel in VENDORED_SUBTREE_FILES

# Sanity checks on the *contents* of the appended suffix -- not used to
# locate the split point (see module docstring).
BANNER_MARKER = "CrossMosa addition -- appended below the upstream file"
EXPECTED_FUNCTION_SIGNATURE = "crossmosa_smb2_finish_accept(struct smb2_context *smb2, struct smb2_server *server)"

# Pin of the appended hunk's own exact content, computed from the version
# committed in this task's fix round 1 -- makes "exactly one hunk, and it's
# *this* hunk" a hash comparison instead of "contains these substrings"
# (which would also pass if a second function were appended alongside the
# expected one). Update both values together whenever the appended function
# is deliberately changed, after re-reviewing the new content by hand.
EXPECTED_SUFFIX_SHA256 = "fd9c3bc1f9d0d4d93cb22486d242c40eb44ce4f42f753c35f253095246001621"
EXPECTED_SUFFIX_LENGTH = 4470

# ---------------------------------------------------------------------------
# The IN-LINE patches (2 and 3). One record each, one shared checker.
#
# Each record's six pins were derived together, in one pass, from the live
# pristine tarball at VENDORED_COMMIT and the patched local file -- by taking
# the longest common prefix/suffix and snapping outward to line boundaries.
# They are only meaningful as a SET; regenerate a record's pins as a set (the
# snippet in the module docstring's regeneration section prints all six).


class InlinePatch(NamedTuple):
    """One pinned in-line divergence. `must_contain` / `must_not_contain` are
    content sanity checks that do not locate anything (the offset does that) --
    they exist so a reverted or re-broken patch produces a sentence a human can
    act on instead of only a hash mismatch."""

    rel: str
    what: str  # one line, for the PASS message
    region_offset: int
    pristine_region_length: int
    pristine_region_sha256: str
    expected_region_length: int
    expected_region_sha256: str
    # Used only by the offline/manifest path, where no pristine bytes exist to
    # do the region comparison against -- see module docstring.
    expected_file_length: int
    expected_file_sha256: str
    pristine_file_length: int
    pristine_file_sha256: str
    must_contain: tuple[tuple[str, str], ...]      # (needle, why it matters)
    must_not_contain: tuple[tuple[str, str], ...]  # (needle, why its return is bad)


INLINE_PATCHES = (
    # Patch 2 -- the NextEntryOffset fix. The pristine region is one line:
    #   "                                smb2_set_uint32(iov, offset + 0, offset + fs_size);\n"
    # The patched region is the CrossMosa banner comment plus the same line
    # with `offset + ` removed.
    InlinePatch(
        rel=QUERY_DIR_C_REL,
        what="pristine plus exactly the one in-line NextEntryOffset fix",
        region_offset=12190,
        pristine_region_length=84,
        pristine_region_sha256="e501a6748f42b9c93683b0a3a2f1719d5c854e8486e88ed70543fd808ad2c9ae",
        expected_region_length=1838,
        expected_region_sha256="0be2aa1628605b743ddb1d34061f0e57d740808ab3227099e9c6dce507ab914c",
        expected_file_length=24954,
        expected_file_sha256="bd8cbf3c1ee1759b8e036af8fb5aaed1df6731408ade3678da44ef861e670220",
        pristine_file_length=23200,
        pristine_file_sha256="c257dde2ba7a77be3ac5c73a7e45d03d6a18a0bfd74a3b36d4cbfcca1c3c100d",
        must_contain=(
            ("CrossMosa in-line fix to an upstream bug", "the CrossMosa banner -- patch 2 absent?"),
            (
                "smb2_set_uint32(iov, offset + 0, fs_size);",
                "the fixed expression -- without it NextEntryOffset is emitted as an absolute "
                "offset again (MS-FSCC 2.4.8 wants relative) and listings silently drop files",
            ),
        ),
        must_not_contain=(
            (
                "smb2_set_uint32(iov, offset + 0, offset + fs_size);",
                "the original buggy absolute-offset expression is back",
            ),
        ),
    ),
    # Patch 3 -- the removed set-info decode guard (Task 7). The pristine
    # region is the four-line
    #   if (!smb2->passthrough) { smb2_set_error(...); return -1; }
    # block; the patched region is the CrossMosa banner comment that replaces
    # it. The two lines that follow (`req->input_data = iov->buf; return 0;`)
    # are upstream's own and are deliberately untouched.
    InlinePatch(
        rel=SET_INFO_C_REL,
        what="pristine plus exactly the one in-line set-info decode guard removal",
        region_offset=14312,
        pristine_region_length=151,
        pristine_region_sha256="c55bfaa21fae18b2ed26a45c012c673754cd44267f43edfe32ed7f042cce2cb1",
        expected_region_length=2448,
        expected_region_sha256="5da15652e6f434585a74d3f6684b61aa5a3729834477b1622c977dceb63d7667",
        expected_file_length=16817,
        expected_file_sha256="65762c9e1e9d509b7eb429bbc7e7bb94babc2810904509b4ed21b3173909f932",
        pristine_file_length=14520,
        pristine_file_sha256="b86a7ebc72a6a190a6cb65c4be37eceaf308eaa7bf697fb9e84bb1f269933fa8",
        must_contain=(
            (
                "CrossMosa divergence from upstream (patch 3 of 3)",
                "the CrossMosa banner -- patch 3 absent?",
            ),
            (
                "req->input_data = iov->buf;",
                "the line that hands the SET_INFO buffer to the handler",
            ),
        ),
        # WHITESPACE-ANCHORED ON PURPOSE. Patch 3's banner deliberately quotes
        # the upstream code it removes (so a reader can see exactly what
        # changed without fetching pristine), which means a bare
        # "can not interpret set-info buffers yet" needle matches the COMMENT
        # and fails on a correctly-patched file -- observed the first time this
        # record was written. Both needles below are the statements as they
        # appear in real code: leading newline + upstream's 8-space indent for
        # the guard, and the single-line call form for the error (the banner
        # wraps that one across two lines behind a ` * ` prefix). The region
        # hash above is the actual proof; these two only make a revert produce
        # a sentence instead of a hash diff.
        must_not_contain=(
            (
                "\n        if (!smb2->passthrough) {",
                "upstream's decode-stage guard is back AS CODE -- every SET_INFO would tear the "
                "connection down again, which on iOS means a disconnect at the end of the first "
                "file copy",
            ),
            (
                'smb2_set_error(smb2, "can not interpret set-info buffers yet");',
                "upstream's decode-stage refusal is back as code",
            ),
        ),
    ),
    # Patch 4 -- SMB2_MAX_VECTORS 256 -> 32 (v73). The pristine region is the
    # single line `#define SMB2_MAX_VECTORS 256`; the patched region is the
    # CrossMosa banner plus the same define with 32. Unlike patches 2 and 3 this
    # one changes no logic at all -- it changes how much memory every in-flight
    # PDU and every connection costs, which on this device is the difference
    # between "about six PDUs fit" and "hundreds do".
    InlinePatch(
        rel=PRIVATE_H_REL,
        what="pristine plus exactly the one in-line SMB2_MAX_VECTORS reduction",
        region_offset=1495,
        pristine_region_length=28,
        pristine_region_sha256="f2f2b2f40f430767f71527cb75597ced7dce37ee1049b6a63028e40f31bbcbe3",
        expected_region_length=2783,
        expected_region_sha256="d26a05cd725f6c44b8946fd2381005b9dca12f5d4be5958d3558f620eb3a8fff",
        expected_file_length=32639,
        expected_file_sha256="eeda5ea1e6fd98a8423c2d7d464c40ea03aab29f584f942bdee8799bbde5fb96",
        pristine_file_length=29884,
        pristine_file_sha256="20c996da1e9b635637895a4f213615f64a16bd6c39236dfb70888cd4780eab25",
        must_contain=(
            ("CrossMosa divergence from upstream (patch 4 of 4)", "the CrossMosa banner -- patch 4 absent?"),
            (
                "#define SMB2_MAX_VECTORS 32",
                "the reduced bound -- without it every in-flight PDU costs ~6.3 KB on this "
                "device and the server runs out of contiguous heap mid-copy",
            ),
        ),
        must_not_contain=(
            (
                "#define SMB2_MAX_VECTORS 256",
                "the upstream 256 is back, and with it the ~6.3 KB-per-PDU cost",
            ),
        ),
    ),
    # Patch 5 -- the double free in smb2_add_iovector()'s overflow branch
    # (v80, approved by the project owner). Upstream frees the caller's buffer
    # and returns NULL; nine call sites then free the same pointer again.
    # MEASURED on the desktop harness: compounding 10 WRITEs into one packet
    # works, 11 aborts with "free(): double free detected". The compound count
    # is the CLIENT's choice, and on the device's TLSF allocator this is heap
    # corruption rather than an abort.
    #
    # Fixed in the callee, not at the nine call sites, because 66 OTHER sites
    # pass a free_cb and rely on this branch to free for them. The trade is
    # 9 double frees for 66 leaks-on-overflow -- bounded, and never damaging
    # someone else's allocation. See the banner in the source.
    InlinePatch(
        rel=INIT_C_REL,
        what="pristine plus exactly the one removed double free in smb2_add_iovector()",
        region_offset=14684,
        pristine_region_length=188,
        pristine_region_sha256="1108de16613b50368bc3e4769f56d50418b3872ea7c2576cac08b1bf94b3e6df",
        expected_region_length=2389,
        expected_region_sha256="5ae278c164c4333df91accef9e874cd7dd982c1ff3cd16abfe27b1caa3fac8d9",
        expected_file_length=26457,
        expected_file_sha256="d51f64662b2dadcad93aa1ece3c4b749241f89f43b3db2557535c95fc736c50c",
        pristine_file_length=24256,
        pristine_file_sha256="3acf8b1246a17bca5079ce2755f1837cd1d4aa5708d23bd07512102ec4eca262",
        must_contain=(
            ("CrossMosa divergence #5", "the CrossMosa banner -- patch 5 absent?"),
        ),
        must_not_contain=(
            (
                # The real statement, at its real indentation. The banner above
                # it QUOTES the upstream line to explain what was removed, so a
                # bare substring would match our own comment -- the same trap
                # patch 4's pin hit.
                "\n                        if (free_cb && buf) {",
                "the upstream double free is back: this branch frees a buffer that nine call "
                "sites free again, which on TLSF is heap corruption chosen by the client",
            ),
        ),
    ),
)

def fetch_pristine_tree() -> dict[str, bytes] | None:
    """Live-fetch the whole pristine tree at the vendored commit as
    {relpath: bytes}, scoped to the directories/files
    docs/third-party/libsmb2-vendoring.md records as vendored. Returns None
    (not an exception) on any network failure so the caller can fall back to
    the pinned manifest -- this script's job is to verify a local tree, not
    to require internet access to do it every single time."""
    try:
        with urllib.request.urlopen(PRISTINE_TARBALL_URL, timeout=20) as resp:
            data = resp.read()
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        print(f"[verify_libsmb2_patch] live tarball fetch failed ({exc}); falling back to pinned manifest", file=sys.stderr)
        return None

    tree: dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        for member in tf.getmembers():
            if not member.isfile() or not member.name.startswith(TARBALL_PREFIX):
                continue
            rel = member.name[len(TARBALL_PREFIX) :]
            if not _in_vendored_subtree(rel):
                continue  # upstream repo content Task 1 never vendored (examples/, tests/, cmake/, ...)
            extracted = tf.extractfile(member)
            if extracted is not None:
                tree[rel] = extracted.read()
    return tree


def load_manifest() -> dict:
    return json.loads(MANIFEST_PATH.read_text())


def local_files() -> dict[str, Path]:
    """{relpath: Path} for every file under lib/smb2/, excluding
    PROJECT_OWNED_FILES."""
    result = {}
    for path in sorted(VENDOR_ROOT.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(VENDOR_ROOT).as_posix()
        if rel in PROJECT_OWNED_FILES:
            continue
        result[rel] = path
    return result


def first_diff_offset(a: bytes, b: bytes) -> int:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b))


def check_libsmb2_c(
    local_bytes: bytes,
    pristine_bytes: bytes | None,
    pristine_hash: str | None,
    pristine_len: int,
) -> list[str]:
    """Returns a list of failure messages for lib/libsmb2.c (empty = pass).
    Exactly one of pristine_bytes (live-fetch path) / pristine_hash
    (manifest fallback path) is non-None."""
    failures = []
    if len(local_bytes) <= pristine_len:
        failures.append(
            f"{LIBSMB2_C_REL}: local file is {len(local_bytes)} bytes, not longer than "
            f"pristine's {pristine_len} bytes -- no appended patch present (or content was "
            "removed instead of only appended)"
        )
        return failures

    prefix = local_bytes[:pristine_len]
    suffix = local_bytes[pristine_len:]

    if pristine_bytes is not None:
        if prefix != pristine_bytes:
            offset = first_diff_offset(prefix, pristine_bytes)
            failures.append(f"{LIBSMB2_C_REL}: prefix differs from live-fetched pristine at byte offset {offset}")
    else:
        prefix_hash = hashlib.sha256(prefix).hexdigest()
        if prefix_hash != pristine_hash:
            failures.append(f"{LIBSMB2_C_REL}: prefix SHA-256 {prefix_hash} does not match pinned {pristine_hash}")

    if BANNER_MARKER.encode() not in suffix:
        failures.append(f"{LIBSMB2_C_REL}: appended hunk missing the expected banner marker {BANNER_MARKER!r}")
    if EXPECTED_FUNCTION_SIGNATURE.encode() not in suffix:
        failures.append(f"{LIBSMB2_C_REL}: appended hunk missing the expected function signature")

    suffix_hash = hashlib.sha256(suffix).hexdigest()
    if len(suffix) != EXPECTED_SUFFIX_LENGTH or suffix_hash != EXPECTED_SUFFIX_SHA256:
        failures.append(
            f"{LIBSMB2_C_REL}: appended hunk is {len(suffix)} bytes / sha256={suffix_hash}, expected "
            f"{EXPECTED_SUFFIX_LENGTH} bytes / sha256={EXPECTED_SUFFIX_SHA256} -- the patch content itself "
            "changed (if deliberate, re-review by hand, then update EXPECTED_SUFFIX_SHA256/"
            "EXPECTED_SUFFIX_LENGTH in this script)"
        )
    return failures


def check_inline_patch(
    patch: InlinePatch,
    local_bytes: bytes,
    pristine_bytes: bytes | None,
    pristine_hash: str | None,
    pristine_len: int,
) -> list[str]:
    """Returns a list of failure messages for one pinned in-line divergence
    (empty = pass). Exactly one of pristine_bytes (live-fetch path) /
    pristine_hash (manifest fallback path) is non-None. See module docstring
    for why the two paths check different things.

    Shared by patches 2 and 3 -- see the docstring's note on why patch 3 was
    not added by duplicating this."""
    failures = []
    rel = patch.rel

    # Content sanity first: these produce a comprehensible message when
    # someone has reverted or re-broken the fix, instead of only a hash diff.
    for needle, why in patch.must_contain:
        if needle.encode() not in local_bytes:
            failures.append(f"{rel}: {needle!r} is MISSING -- {why}")
    for needle, why in patch.must_not_contain:
        if needle.encode() in local_bytes:
            failures.append(f"{rel}: {needle!r} is PRESENT -- {why}")

    region_end = patch.region_offset + patch.expected_region_length
    if len(local_bytes) < region_end:
        failures.append(
            f"{rel}: local file is {len(local_bytes)} bytes, too short to contain the pinned patched "
            f"region ending at {region_end}"
        )
        return failures

    patched_region = local_bytes[patch.region_offset : region_end]
    patched_region_hash = hashlib.sha256(patched_region).hexdigest()
    if patched_region_hash != patch.expected_region_sha256:
        failures.append(
            f"{rel}: patched region at byte {patch.region_offset} has sha256={patched_region_hash}, "
            f"expected {patch.expected_region_sha256} -- the patch content itself changed (if "
            "deliberate, re-review by hand, then regenerate all six of this patch's pins together)"
        )

    if pristine_bytes is not None:
        # Live path: prove "exactly one in-line change, at exactly this place".
        pristine_region_end = patch.region_offset + patch.pristine_region_length
        if len(pristine_bytes) < pristine_region_end:
            failures.append(f"{rel}: pristine file is shorter than the pinned pristine region -- upstream moved")
            return failures

        if local_bytes[: patch.region_offset] != pristine_bytes[: patch.region_offset]:
            offset = first_diff_offset(local_bytes, pristine_bytes)
            failures.append(f"{rel}: content BEFORE the patched region differs from pristine at byte offset {offset}")

        pristine_region = pristine_bytes[patch.region_offset : pristine_region_end]
        pristine_region_hash = hashlib.sha256(pristine_region).hexdigest()
        if pristine_region_hash != patch.pristine_region_sha256:
            failures.append(
                f"{rel}: the UPSTREAM lines this patch replaces have sha256={pristine_region_hash}, expected "
                f"{patch.pristine_region_sha256} -- upstream changed the very code being patched, so the "
                "patch must be re-reviewed against the new upstream rather than silently re-applied"
            )

        if local_bytes[region_end:] != pristine_bytes[pristine_region_end:]:
            failures.append(f"{rel}: content AFTER the patched region differs from pristine -- more than one edit")
    else:
        # Manifest path: no pristine bytes, so pin the whole patched file and
        # cross-check that the manifest still describes the upstream file the
        # patched-file pin was derived from.
        local_hash = hashlib.sha256(local_bytes).hexdigest()
        if local_hash != patch.expected_file_sha256 or len(local_bytes) != patch.expected_file_length:
            failures.append(
                f"{rel}: sha256={local_hash} length={len(local_bytes)}, expected "
                f"sha256={patch.expected_file_sha256} length={patch.expected_file_length} -- the file "
                "differs from pristine-plus-exactly-this-patch somewhere"
            )
        if pristine_hash != patch.pristine_file_sha256 or pristine_len != patch.pristine_file_length:
            failures.append(
                f"{rel}: the pinned manifest's pristine entry (sha256={pristine_hash} length={pristine_len}) "
                f"is not the upstream file this patch was derived from (sha256={patch.pristine_file_sha256} "
                f"length={patch.pristine_file_length}) -- manifest and pins are out of step"
            )
    return failures


def main() -> int:
    if not VENDOR_ROOT.is_dir():
        print(f"FAIL: {VENDOR_ROOT} not found", file=sys.stderr)
        return 1

    locals_ = local_files()
    pristine_tree = fetch_pristine_tree()

    manifest = None
    if pristine_tree is not None:
        expected_relpaths = set(pristine_tree.keys())
        print(f"[verify_libsmb2_patch] live-fetched pristine tree: {len(expected_relpaths)} files")
    else:
        manifest = load_manifest()
        expected_relpaths = set(manifest["files"].keys())
        print(f"[verify_libsmb2_patch] no network access -- using pinned manifest: {len(expected_relpaths)} files")

    failures: list[str] = []

    missing = sorted(expected_relpaths - set(locals_.keys()))
    extra = sorted(set(locals_.keys()) - expected_relpaths)
    if missing:
        failures.append(f"{len(missing)} file(s) present upstream but missing locally: {missing}")
    if extra:
        failures.append(f"{len(extra)} local file(s) with no upstream counterpart (not in PROJECT_OWNED_FILES): {extra}")

    inline_by_rel = {p.rel: p for p in INLINE_PATCHES}

    for rel in sorted(expected_relpaths & set(locals_.keys())):
        local_bytes = locals_[rel].read_bytes()

        # The three files with pinned divergences get their own checkers; every
        # other file must be byte-identical to pristine (below).
        if rel == LIBSMB2_C_REL or rel in inline_by_rel:
            if pristine_tree is not None:
                pristine_bytes, pristine_hash = pristine_tree[rel], None
                pristine_len = len(pristine_bytes)
            else:
                entry = manifest["files"][rel]
                pristine_bytes, pristine_hash, pristine_len = None, entry["sha256"], entry["length"]

            if rel == LIBSMB2_C_REL:
                failures.extend(check_libsmb2_c(local_bytes, pristine_bytes, pristine_hash, pristine_len))
            else:
                failures.extend(
                    check_inline_patch(inline_by_rel[rel], local_bytes, pristine_bytes, pristine_hash, pristine_len)
                )
            continue

        if pristine_tree is not None:
            if local_bytes != pristine_tree[rel]:
                offset = first_diff_offset(local_bytes, pristine_tree[rel])
                failures.append(f"{rel}: differs from live-fetched pristine at byte offset {offset}")
        else:
            entry = manifest["files"][rel]
            local_hash = hashlib.sha256(local_bytes).hexdigest()
            if local_hash != entry["sha256"] or len(local_bytes) != entry["length"]:
                failures.append(
                    f"{rel}: sha256={local_hash} length={len(local_bytes)}, expected "
                    f"sha256={entry['sha256']} length={entry['length']}"
                )

    if failures:
        print(f"FAIL: {len(failures)} problem(s) found in the lib/smb2/ tree:", file=sys.stderr)
        for msg in failures:
            print(f"  - {msg}", file=sys.stderr)
        return 1

    print(f"[verify_libsmb2_patch] checked {len(locals_)} files under lib/smb2/ (excluding {sorted(PROJECT_OWNED_FILES)})")
    expected = [f"{LIBSMB2_C_REL} (pristine plus exactly the one appended hunk)"]
    expected += [f"{p.rel} ({p.what})" for p in INLINE_PATCHES]
    print(
        f"PASS: lib/smb2/ matches pristine upstream file-for-file, except the {len(expected)} expected "
        "divergences -- " + "; ".join(expected)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
