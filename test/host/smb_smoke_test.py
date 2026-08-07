#!/usr/bin/env python3
"""Desktop smoke test for the smb2-server harness (Task 4 of the plan --
see ../../.superpowers/sdd/2026-07-28-smb2-server/task-4-report.md for the
full verification record across all three fix rounds this file's current
shape reflects).

Task 4 replaced SmbFileHandlers.cpp's stub bodies with real logic:
authentication (real NTLMv2 password verification, not just a username
check), tree_connect (share validation), create/close (real HalStorage-
backed file/directory I/O with path protection), destruction_event
(open-file table recycling), and the eleven trivial-but-not-optional
handlers (echo_cmd, etc.). query_directory/query_info/read/write/flush/
set_info remain deliberate stubs -- Tasks 5-7's job, not tested here.

HISTORY, briefly (see task-4-report.md for the full trace of each):
round 1 found that `allow_anonymous = 1` made even a correctly-
authenticated x3 session get marked SMB2_SESSION_FLAG_IS_GUEST (ntlmssp.c
wipes the password right after verifying it; libsmb2.c's guest-flag
decision reads that already-wiped value afterward). Round 2's fix,
`allow_anonymous = 0` (see SmbServer.cpp), closed that path -- but exposed
a second, narrower problem only reachable once sessions stopped being
guest-flagged: SMB 3.1.1's session-setup reply fails the client's own
signature verification (never observable before, since the guest-flag
rejection always fired first, before that check could run).

CURRENT STATE (round 3, read before assuming "the iPhone will connect"):
`SmbServer::acceptOneConnection()` now calls
`smb2_set_version(ctx, SMB2_VERSION_0302)` on every freshly accepted
context, which makes `smb2_negotiate_request_cb` offer **only** SMB 3.0.2 --
see that call site's own long comment for exactly why this is safe
(ordering) and correct (this is what real clients, reportedly including
iOS, actually land on when 3.0.2 is available; see that comment for the
Visuality Systems citation). This does not fix the underlying SMB 3.1.1
signing-key-derivation bug -- it makes the server never offer 3.1.1 at
all, so a client can never reach that broken path. **Confirmed
empirically**: a client that sends the same dialect list a real modern
client (including iOS) would -- 2.0.2 through 3.1.1, i.e. `dialect=None`,
this file's `test_default_client_lands_on_302()` -- now negotiates SMB 3.0.2
and completes authentication, a real non-guest signing-verified session,
and `tree_connect("SD")`, all with zero client-side bypasses. A client that
offers *only* 3.1.1 (nothing else) now fails cleanly at NEGOTIATE itself
("no common dialect"), not at the old signature-mismatch point --
`test_smb311_dialect_not_offered()` pins down that this is the new,
intended failure mode, not a regression to something else.

v65 (2026-07-30): this file now pins the v65 server, not the v64 one. Five
behaviours CHANGED deliberately, each because a real client -- Samba's
smbclient, the Linux kernel client, or a real iPhone -- was measured failing
against v64, and the checks that pinned the old behaviour were pinning the bug:

  * QUERY_DIRECTORY emits '.' and '..' first on a RESTARTING enumeration. See
    DOT_NAMES / _dots_present() for the mechanism and the measurement; this is
    what makes an empty directory readable as empty rather than as an error, and
    test_listing_empty_directory()'s premise was inverted by it.
  * a READ starting at or past EOF answers STATUS_END_OF_FILE instead of a
    successful zero-byte read -- test_read_at_and_past_eof().
  * failures carry real NT statuses rather than one blanket NOT_IMPLEMENTED. Only
    one existing check depended on the old status, and it depended on it by
    accident: test_cross_connection_close_rejected() had silently stopped being
    able to fail, because the new (correct) STATUS_FILE_CLOSED is one
    smbprotocol's Open absorbs. Read that docstring before touching it.
  * QUERY_INFO's IndexNumber and the listing's file_id carry a real stable hash
    instead of 0 -- which is also what exposed a latent wrong offset in
    test_query_info_file_classes() that had been passing for four tasks only
    because those fields were zero.
  * responses are SIGNED for a client that advertises SIGNING_ENABLED without
    SIGNING_REQUIRED. This suite could NOT see that fix and still cannot through
    any of its other checks -- see test_signing_enabled_only_client().

Two checks are new: test_signing_enabled_only_client() and
test_compound_related_file_id(). Both cover paths that had never been exercised
here even once, which is why v64 shipped green and then failed on real hardware.

v72 (2026-07-31): two deliberate server changes, both from a measured failure of
a real iPhone copy, and in both cases the checks that pinned the old behaviour
were pinning the bug.

  * THE TRANSFER SIZE IS NO LONGER A LITERAL HERE. max_read/write/transact_size
    went 32768 -> 8192 (SmbServer.cpp), because signing concatenates the entire
    PDU into ONE contiguous malloc in both directions
    (lib/smb2/lib/smb2-signing.c:186) and this device's log shows a largest free
    block of 3188 and 2036 in ordinary use. Six checks hardcoded 32768/32767/9000
    and failed with "The requested write length 32768 is greater than the maximum
    negotiated write size 8192" -- a red that says nothing about the server. Every
    size is now derived from what NEGOTIATE actually returned; see
    _negotiated_chunk(). This number is expected to be retuned AGAIN once the
    device reports its real write-time headroom, and this file must not need
    editing when it is.
  * A TIMESTAMP FAT CANNOT REPRESENT IS SKIPPED, NOT AN ERROR. setBasicInfo()
    used to refuse the whole request; iOS sends CreationTime = 1904-01-01 (the
    Mac/HFS epoch, its "there isn't one" sentinel) and the refusal is what
    stopped the first real copy dead -- the phone created the file, opened
    it for write, and then never sent a single WRITE. Validation still happens
    entirely before the first write, so nothing is ever half-applied; what
    changed is that an unrepresentable field is DROPPED rather than fatal. The
    invariant that mattered -- the reported result matches what happened -- is
    unchanged, and is what test_set_basic_skips_unrepresentable_dates() and
    test_set_basic_structural_failure_writes_nothing() now pin. "All or nothing"
    was the mechanism, not the goal.

One check is new: test_set_basic_ios_1904_creation_time(), which replays the
exact FILETIME from the field log. It is the regression test for the bug this
version exists to fix.

Also in v72, SET_INFO handler failures answer STATUS_INVALID_PARAMETER instead of
libsmb2's blanket STATUS_NOT_IMPLEMENTED (setInfoCmd). No check here asserted the
old status -- every set_info refusal check reports `type(e).__name__` rather than
requiring a particular one -- so this changed the printed type name in several
PASS lines (SMBResponseException / InvalidParameter) and nothing else.

v82 (2026-08-02): SIGNING IS ADAPTIVE, WHICH REVERSES THE v65 BULLET ABOVE. The
unconditional `smb2_set_sign(ctx, 1)` is gone from SmbServer::acceptOneConnection().
`smb2->sign` now starts at 0 and libsmb2's own NEGOTIATE logic raises it only for
a client that sends SIGNING_REQUIRED, so a client advertising SIGNING_ENABLED
only -- Samba's shape -- now gets an UNSIGNED session. The owner decided this
twice, explicitly: AES-CMAC over every PDU in both directions, with a vendored
AES that re-derives its key schedule per 16-byte block, is tens of seconds on one
book, on a personal device that is off except during a transfer, on a home LAN,
whose bytes this server has never encrypted anyway. `server->signing_enabled`
stays 1, so a client that REQUIRES signing is still never dropped before the
reply -- adaptive, not off.

  * test_signing_enabled_only_client() now asserts the OPPOSITE of what it
    asserted under v65: SIGNING_ENABLED advertised WITHOUT SIGNING_REQUIRED, and
    an UNSIGNED TREE_CONNECT response. Its docstring records the reversal so the
    next reader does not restore the old assertion as a "fix".
  * One check is new: test_forced_signing_escape_hatch(). The cost of adaptive
    signing is NOT security, it is that smbclient and the Linux kernel cifs
    client -- two of the three INDEPENDENT implementations this suite runs on --
    refuse an unsigned reply. SmbServer::setForceSigning() exists for them
    (SMBHOST_SIGN=1, wired in test/host/main.cpp); nothing on the device path
    calls it, so without a check it would rot unnoticed until some later task hit
    an inexplicable connection failure. It spawns its own server, since the
    switch is read once at startup.
  * Nothing else needed changing, and this was verified rather than assumed:
    `require_signing=False` appears in exactly one place in this file, the check
    above. Every other client takes smbprotocol's require_signing=True default,
    which sends SIGNING_REQUIRED and so raises `smb2->sign` on the server by
    itself -- those checks ran against a signed session before v82 and still do.

Usage: python3 smb_smoke_test.py [port]   (default 4450, matches smbhost's default)
"""
import hashlib
import os
import resource
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid

from smbprotocol.connection import Connection, Dialects, SecurityMode
from smbprotocol.exceptions import EndOfFile, FileClosed, LogonFailure, NoMoreFiles, ObjectNameNotFound
from smbprotocol.file_info import FileAttributes, FileInformationClass, InfoType
from smbprotocol.header import Smb2Flags
from smbprotocol.open import (
    CreateDisposition,
    CreateOptions,
    DirectoryAccessMask,
    FilePipePrinterAccessMask,
    ImpersonationLevel,
    Open,
    QueryDirectoryFlags,
    SMB2CloseRequest,
    SMB2CreateRequest,
    SMB2CreateResponse,
    SMB2QueryDirectoryRequest,
    SMB2QueryDirectoryResponse,
    SMB2QueryInfoRequest,
    SMB2QueryInfoResponse,
    SMB2SetInfoRequest,
    ShareAccess,
)
from smbprotocol.session import Session
from smbprotocol.tree import SMB2TreeConnectRequest, TreeConnect

# The ONLY dialect the server offers as of round 3 (SmbServer.cpp pins
# smb2->version to this via smb2_set_version() on every accepted
# connection -- see that call site's comment). Forcing it here in most
# tests below just means "match what the server will negotiate to
# regardless" -- test_default_client_lands_on_302() is the one that instead
# sends the full modern dialect list (dialect=None) and asserts negotiation
# still lands here, which is the scenario that actually matters for a real
# client like iOS.
WORKING_DIALECT = Dialects.SMB_3_0_2

# Where stub_hal/HalStorage resolves device-absolute paths (see README's
# SMBHOST_ROOT section). A few tests below need to seed or inspect "the SD
# card" directly -- e.g. planting a protected file the SMB server must refuse
# to touch, which by definition cannot be created over SMB.
SD_ROOT = os.environ.get("SMBHOST_ROOT") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "sdroot")

# A protected path, by ProtectedPath's rules (any '.'-prefixed segment) -- and
# not an arbitrary one: /.crossmosa is the real data directory holding Wi-Fi
# credentials, settings and reading progress.
PROTECTED_DIR = ".crossmosa"
PROTECTED_FILE = "wifi.json"
PROTECTED_SMB_PATH = f"{PROTECTED_DIR}\\{PROTECTED_FILE}"
PROTECTED_SENTINEL = b"SENTINEL: if this file was truncated, path protection failed"


def _seed_protected_file() -> str:
    """Plants a protected file with known contents directly on the harness's
    SD root and returns its host path. Deliberately not created over SMB --
    the whole point is that the server refuses to create/open/truncate it."""
    directory = os.path.join(SD_ROOT, PROTECTED_DIR)
    os.makedirs(directory, exist_ok=True)
    host_path = os.path.join(directory, PROTECTED_FILE)
    with open(host_path, "wb") as fh:
        fh.write(PROTECTED_SENTINEL)
    return host_path


def _protected_open_must_fail(port: int, label: str, disposition: int, access: int) -> bool:
    """Shared body for the protected-path tests: seed the file, attempt one
    create, require rejection, and require the file to still be byte-identical
    afterwards. That last assertion is the one that actually matters for
    FILE_OVERWRITE_IF -- a create that is 'rejected' only after O_TRUNC has
    already fired would still have destroyed the data."""
    host_path = _seed_protected_file()
    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = Open(tree, PROTECTED_SMB_PATH)
        op.create(
            ImpersonationLevel.Impersonation,
            access,
            FileAttributes.FILE_ATTRIBUTE_NORMAL,
            ShareAccess.FILE_SHARE_READ,
            disposition,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        print(f"FAIL ({label}): create UNEXPECTEDLY SUCCEEDED against a protected path")
        ok = False
        try:
            op.close()
        except Exception:  # noqa: BLE001
            pass
    except Exception as e:  # noqa: BLE001
        print(f"PASS ({label}): rejected with {type(e).__name__}")
    finally:
        tree.disconnect()
        connection.disconnect()

    with open(host_path, "rb") as fh:
        after = fh.read()
    if after != PROTECTED_SENTINEL:
        print(f"FAIL ({label}): the protected file was MODIFIED ({len(after)} bytes, expected "
              f"{len(PROTECTED_SENTINEL)}) -- the open was refused too late, after O_TRUNC")
        return False
    print(f"PASS ({label}): protected file still intact ({len(after)} bytes)")
    return ok


def test_protected_overwrite_if_maximum_allowed(port: int) -> bool:
    """THE regression test for the create_cmd bypass. FILE_OVERWRITE_IF (0x05)
    was absent from isWriteIntentCreate()'s disposition list and
    SMB2_MAXIMUM_ALLOWED (0x02000000) was absent from its access mask -- yet
    resolveFileOflag opened FILE_OVERWRITE_IF as O_RDWR|O_CREAT|O_TRUNC
    regardless. So this exact request skipped the protection check and then
    truncated the file. MAXIMUM_ALLOWED is not an exotic choice: macOS/iOS
    send it routinely, and an iOS client dropping .DS_Store / ._ files at the
    share root is the likeliest real-world trigger.

    The pre-existing test_protected_path_rejected() below cannot catch this:
    it uses FILE_CREATE *and* GENERIC_WRITE, both of which were already on the
    lists, so it passes whether or not the hole exists."""
    return _protected_open_must_fail(
        port,
        "protected + FILE_OVERWRITE_IF + MAXIMUM_ALLOWED",
        CreateDisposition.FILE_OVERWRITE_IF,
        FilePipePrinterAccessMask.MAXIMUM_ALLOWED,
    )


def test_protected_open_if_maximum_allowed(port: int) -> bool:
    """The same hole via FILE_OPEN_IF (0x03) -- also absent from the old
    disposition list, also opened O_RDWR|O_CREAT by resolveFileOflag."""
    return _protected_open_must_fail(
        port,
        "protected + FILE_OPEN_IF + MAXIMUM_ALLOWED",
        CreateDisposition.FILE_OPEN_IF,
        FilePipePrinterAccessMask.MAXIMUM_ALLOWED,
    )


def test_protected_read_open_rejected(port: int) -> bool:
    """A plain READ open of a protected path must be refused too. WebDAV has
    always blocked protected paths on every method including GET
    (WebDAVHandler.cpp:51,296); SMB used to block only write-intent creates,
    which would have made /.crossmosa/ -- Wi-Fi credentials, settings,
    reading progress -- readable over the network the moment Task 6 lands
    read_cmd."""
    _seed_protected_file()
    connection, session, tree = connect_session(port)
    try:
        op = Open(tree, PROTECTED_SMB_PATH)
        op.create(
            ImpersonationLevel.Impersonation,
            FilePipePrinterAccessMask.GENERIC_READ,
            FileAttributes.FILE_ATTRIBUTE_NORMAL,
            ShareAccess.FILE_SHARE_READ,
            CreateDisposition.FILE_OPEN,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        print("FAIL (protected read open): a READ open of a protected path SUCCEEDED")
        try:
            op.close()
        except Exception:  # noqa: BLE001
            pass
        return False
    except Exception as e:  # noqa: BLE001
        print(f"PASS (protected read open rejected): {type(e).__name__}")
        return True
    finally:
        tree.disconnect()
        connection.disconnect()


def test_dotdot_traversal_rejected(port: int) -> bool:
    """smbNormalizeUtf8Path() rejects any path containing "..". The rule has
    existed since round 1 but nothing exercised it, so nothing would have
    noticed it being refactored away."""
    connection, session, tree = connect_session(port)
    try:
        op = Open(tree, r"..\escaped.txt")
        op.create(
            ImpersonationLevel.Impersonation,
            FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
            FileAttributes.FILE_ATTRIBUTE_NORMAL,
            ShareAccess.FILE_SHARE_READ,
            CreateDisposition.FILE_OVERWRITE_IF,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        print("FAIL (.. traversal): a path containing '..' was ACCEPTED")
        try:
            op.close()
        except Exception:  # noqa: BLE001
            pass
        return False
    except Exception as e:  # noqa: BLE001
        print(f"PASS (.. traversal rejected): {type(e).__name__}")
        return True
    finally:
        tree.disconnect()
        connection.disconnect()


def test_cjk_filename_roundtrip(port: int) -> bool:
    """Create and then re-open a file whose name is Traditional Chinese, and
    confirm the bytes that landed on "the SD card" are the expected UTF-8.
    This device's entire library is Chinese and nothing tested it: every
    filename in every earlier test was pure ASCII, so a UTF-16 -> UTF-8
    conversion or path-buffer bug would have gone unseen. It is also the
    reason the path buffer is 512 bytes rather than 256 -- CJK costs 3 UTF-8
    bytes per character."""
    name = "測試資料夾/範例書坊-繁體中文檔名.txt"
    smb_name = name.replace("/", "\\")
    host_path = os.path.join(SD_ROOT, *name.split("/"))
    connection, session, tree = connect_session(port)
    try:
        d = Open(tree, "測試資料夾")
        d.create(
            ImpersonationLevel.Impersonation,
            FilePipePrinterAccessMask.GENERIC_READ,
            FileAttributes.FILE_ATTRIBUTE_DIRECTORY,
            ShareAccess.FILE_SHARE_READ,
            CreateDisposition.FILE_OPEN_IF,
            CreateOptions.FILE_DIRECTORY_FILE,
        )
        d.close()

        f = _create_file(
            tree,
            smb_name,
            CreateDisposition.FILE_OVERWRITE_IF,
            FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        f.close()

        if not os.path.exists(host_path):
            print(f"FAIL (CJK filename): create succeeded but {host_path!r} is not on disk -- "
                  f"the name was mangled somewhere in the UTF-16 -> UTF-8 -> HalStorage path")
            return False

        reopened = _create_file(
            tree,
            smb_name,
            CreateDisposition.FILE_OPEN,
            FilePipePrinterAccessMask.GENERIC_READ,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        reopened.close()
        print(f"PASS (CJK filename round-trip): created, found on disk, and re-opened read-only: {name}")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (CJK filename): {type(e).__name__}: {e}")
        return False
    finally:
        tree.disconnect()
        connection.disconnect()


def _ensure_dir(tree, name: str) -> None:
    d = Open(tree, name)
    d.create(
        ImpersonationLevel.Impersonation,
        FilePipePrinterAccessMask.GENERIC_READ,
        FileAttributes.FILE_ATTRIBUTE_DIRECTORY,
        ShareAccess.FILE_SHARE_READ,
        CreateDisposition.FILE_OPEN_IF,
        CreateOptions.FILE_DIRECTORY_FILE,
    )
    d.close()


def test_directory_open_matrix(port: int) -> bool:
    """The grid where iOS actually lives: an EXISTING directory, opened with
    every plausible combination of non-destructive disposition, read-ish
    access, and directory hint. Every one must succeed and report
    FILE_ATTRIBUTE_DIRECTORY.

    Two separate bugs live in this grid, and the first review round only found
    one of them:

      * FILE_OPEN + GENERIC_READ used to get O_RDWR, because resolveFileOflag
        ignored the access mask entirely. Fixed in round 4.
      * FILE_OPEN_IF + GENERIC_READ *still* got O_RDWR after round 4, because
        write intent was then defined as "any disposition except FILE_OPEN".
        Creation bits and access mode are two different questions; conflating
        them made a pure-read open of an existing folder ask for write and die
        on the subdirectory guard. Fixed in round 5 -- this is the shape the
        matrix exists to pin down.

    The share root is included in every combination too: SdFat's openRoot()
    (FatFile.cpp:456-461) ignores oflag entirely, and the stub models that, so
    the root must keep working even where a subdirectory would be narrowed."""
    dir_name = "smoke_test_probe_dir"
    dispositions = [
        ("FILE_OPEN", CreateDisposition.FILE_OPEN),
        ("FILE_OPEN_IF", CreateDisposition.FILE_OPEN_IF),
    ]
    accesses = [
        ("GENERIC_READ", FilePipePrinterAccessMask.GENERIC_READ),
        ("MAXIMUM_ALLOWED", FilePipePrinterAccessMask.MAXIMUM_ALLOWED),
        ("FILE_READ_ATTRIBUTES", FilePipePrinterAccessMask.FILE_READ_ATTRIBUTES),
    ]
    options = [
        ("with DIR flag", CreateOptions.FILE_DIRECTORY_FILE),
        ("no DIR flag", 0),
    ]
    targets = [("subdir", dir_name), ("share root", "")]

    ok = True
    connection, session, tree = connect_session(port)
    try:
        _ensure_dir(tree, dir_name)
        for target_label, target in targets:
            for disp_label, disposition in dispositions:
                for acc_label, access in accesses:
                    for opt_label, create_options in options:
                        label = f"{target_label}, {disp_label} + {acc_label}, {opt_label}"
                        try:
                            op = Open(tree, target)
                            op.create(
                                ImpersonationLevel.Impersonation,
                                access,
                                FileAttributes.FILE_ATTRIBUTE_NORMAL,
                                ShareAccess.FILE_SHARE_READ,
                                disposition,
                                create_options,
                            )
                            attrs = op.file_attributes
                            op.close()
                            if not attrs & FileAttributes.FILE_ATTRIBUTE_DIRECTORY:
                                print(f"FAIL (dir matrix: {label}): opened but reported 0x{attrs:08x} "
                                      f"without FILE_ATTRIBUTE_DIRECTORY")
                                ok = False
                        except Exception as e:  # noqa: BLE001
                            print(f"FAIL (dir matrix: {label}): {type(e).__name__}: {e}")
                            ok = False
        if ok:
            print(f"PASS (directory open matrix): all {len(targets) * len(dispositions) * len(accesses) * len(options)} "
                  f"combinations opened and reported FILE_ATTRIBUTE_DIRECTORY")
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (dir matrix): setup failed: {type(e).__name__}: {e}")
        ok = False
    finally:
        tree.disconnect()
        connection.disconnect()
    return ok


def test_truncating_open_of_directory_refused(port: int) -> bool:
    """The stub-fidelity assertion, and the one place a write-mode
    subdirectory open must still be observable as a failure.

    This REPLACES round 4's "FILE_OPEN + GENERIC_WRITE on a subdirectory must
    fail". That assertion could not survive round 5: existing directories are
    now narrowed to O_RDONLY, so that request succeeds by design. See the
    report -- the change is deliberate, not a weakened assertion.

    A truncating disposition is the right home for the check, because it is
    the one shape that SHOULD fail: "truncate this directory" has no read-only
    reading, so createCmd deliberately skips the narrowing retry for it. The
    open therefore still reaches the filesystem in write mode and must still
    be refused -- which is exactly what proves the stub models SdFat
    (FatFile.cpp:581-585 / ExFatFile.cpp:399-405) rather than papering over
    it. If this starts passing, the harness is hiding the device again."""
    dir_name = "smoke_test_probe_dir"
    connection, session, tree = connect_session(port)
    try:
        _ensure_dir(tree, dir_name)
        op = Open(tree, dir_name)
        op.create(
            ImpersonationLevel.Impersonation,
            FilePipePrinterAccessMask.GENERIC_WRITE,
            FileAttributes.FILE_ATTRIBUTE_NORMAL,
            ShareAccess.FILE_SHARE_READ,
            CreateDisposition.FILE_OVERWRITE_IF,
            0,
        )
        print("FAIL (truncating open of a directory): SUCCEEDED -- either the narrowing retry stopped "
              "excluding truncating dispositions, or the stub HAL is ignoring oflag for directories "
              "again (SdFat FatFile.cpp:581-585)")
        try:
            op.close()
        except Exception:  # noqa: BLE001
            pass
        return False
    except Exception as e:  # noqa: BLE001
        print(f"PASS (truncating open of a directory refused, as SdFat would): {type(e).__name__}")
        return True
    finally:
        tree.disconnect()
        connection.disconnect()


def test_non_directory_flag_against_directory(port: int) -> bool:
    """The mirror of the directory-hint bug: FILE_NON_DIRECTORY_FILE against
    an existing directory must be rejected, not quietly served."""
    dir_name = "smoke_test_probe_dir"
    connection, session, tree = connect_session(port)
    try:
        d = Open(tree, dir_name)
        d.create(
            ImpersonationLevel.Impersonation,
            FilePipePrinterAccessMask.GENERIC_READ,
            FileAttributes.FILE_ATTRIBUTE_DIRECTORY,
            ShareAccess.FILE_SHARE_READ,
            CreateDisposition.FILE_OPEN_IF,
            CreateOptions.FILE_DIRECTORY_FILE,
        )
        d.close()

        op = Open(tree, dir_name)
        op.create(
            ImpersonationLevel.Impersonation,
            FilePipePrinterAccessMask.GENERIC_READ,
            FileAttributes.FILE_ATTRIBUTE_NORMAL,
            ShareAccess.FILE_SHARE_READ,
            CreateDisposition.FILE_OPEN,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        print("FAIL (NON_DIRECTORY_FILE vs directory): SUCCEEDED -- the mismatch is not rejected")
        try:
            op.close()
        except Exception:  # noqa: BLE001
            pass
        return False
    except Exception as e:  # noqa: BLE001
        print(f"PASS (NON_DIRECTORY_FILE vs directory rejected): {type(e).__name__}")
        return True
    finally:
        tree.disconnect()
        connection.disconnect()


def test_directory_flag_against_file(port: int) -> bool:
    """FILE_DIRECTORY_FILE pointed at an existing plain file must be rejected.
    The old code skipped mkdir (the target existed), opened the file, and set
    isDirectory = true unconditionally -- reporting FILE_ATTRIBUTE_DIRECTORY
    for a file and handing Tasks 5-7 a slot with the wrong flag."""
    connection, session, tree = connect_session(port)
    try:
        f = _create_file(
            tree,
            "smoke_test_plain_file.txt",
            CreateDisposition.FILE_OVERWRITE_IF,
            FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        f.close()

        op = Open(tree, "smoke_test_plain_file.txt")
        op.create(
            ImpersonationLevel.Impersonation,
            FilePipePrinterAccessMask.GENERIC_READ,
            FileAttributes.FILE_ATTRIBUTE_DIRECTORY,
            ShareAccess.FILE_SHARE_READ,
            CreateDisposition.FILE_OPEN,
            CreateOptions.FILE_DIRECTORY_FILE,
        )
        print("FAIL (DIRECTORY_FILE vs plain file): SUCCEEDED -- directory-ness is still taken from "
              "the client's hint rather than the filesystem")
        try:
            op.close()
        except Exception:  # noqa: BLE001
            pass
        return False
    except Exception as e:  # noqa: BLE001
        print(f"PASS (DIRECTORY_FILE vs plain file rejected): {type(e).__name__}")
        return True
    finally:
        tree.disconnect()
        connection.disconnect()


def test_anonymous_rejected(port: int) -> bool:
    """Anonymous must fail. Before round 2 this was framed as "a client-side
    limitation unrelated to our server" (smbprotocol's own SPNEGO layer
    refuses to build an anonymous token client-side, before ever reaching
    our server, for either of the two allow_anonymous settings). That
    framing is still literally true for *this specific client library* --
    but it undersold the real change: with allow_anonymous=0, ANY anonymous
    attempt that did reach our server (a different client, or a lower-level
    tool) would now also be rejected there, deliberately and by design (see
    SmbServer.cpp's allow_anonymous comment) -- not just tolerated as an
    unreachable corner case."""
    connection = Connection(uuid.uuid4(), "127.0.0.1", port)
    try:
        connection.connect(timeout=5)
    except Exception as e:  # noqa: BLE001 -- deliberately broad, this is a smoke test
        print(f"FAIL (anonymous): could not even complete NEGOTIATE: {type(e).__name__}: {e}")
        return False
    try:
        session = Session(connection, "", "", require_encryption=False)
        session.connect()
        print("FAIL (anonymous): session setup UNEXPECTEDLY SUCCEEDED")
        return False
    except Exception as e:  # noqa: BLE001
        print(f"PASS (anonymous rejected): {type(e).__name__}: {e}")
        return True
    finally:
        try:
            connection.disconnect()
        except Exception:  # noqa: BLE001 -- best-effort cleanup only
            pass


def test_wrong_password_rejected(port: int) -> bool:
    """x3 with the WRONG password must fail with a real logon failure -- the
    proof that authorize_user's smb2_set_password() call makes libsmb2 do
    genuine NTLMv2 verification, not just an identity check. Uses
    WORKING_DIALECT so this failure is unambiguously about the password, not
    entangled with the separate 3.1.1 signature issue."""
    connection = Connection(uuid.uuid4(), "127.0.0.1", port)
    connection.connect(dialect=WORKING_DIALECT, timeout=5)
    try:
        session = Session(connection, "x3", "definitely-not-the-password", require_encryption=False)
        session.connect()
        print("FAIL (x3/wrongpassword): session setup SUCCEEDED with a wrong password")
        return False
    except LogonFailure as e:
        print(f"PASS (x3/wrongpassword): correctly rejected with LogonFailure: {e}")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (x3/wrongpassword): wrong exception type {type(e).__name__}: {e} (expected LogonFailure)")
        return False
    finally:
        connection.disconnect()


def test_smb311_dialect_not_offered(port: int) -> bool:
    """Round 3: the server pins its dialect ceiling to SMB 3.0.2
    (SmbServer::acceptOneConnection() calls
    smb2_set_version(ctx, SMB2_VERSION_0302) on every accepted connection --
    see that call site's comment for the full rationale). A client that
    offers ONLY 3.1.1 (nothing else in its dialect list) must now fail to
    negotiate a common dialect AT ALL -- cleanly, at NEGOTIATE itself --
    rather than reaching session setup and hitting the old signature-
    mismatch error from round 2. This test's whole point is telling those
    two failure modes apart: if this starts failing with a signature-
    related message instead of failing during connect() itself, that is
    NOT this test passing for the "same reason", it's the dialect pin
    having silently stopped working -- treat it as a regression, not
    "close enough"."""
    connection = Connection(uuid.uuid4(), "127.0.0.1", port)
    try:
        connection.connect(dialect=Dialects.SMB_3_1_1, timeout=5)
        print("FAIL (3.1.1 not offered): NEGOTIATE unexpectedly SUCCEEDED for a 3.1.1-only "
              "client -- the dialect pin in SmbServer::acceptOneConnection() appears to not be "
              "taking effect; check smb2_set_version() is still called before crossmosa_smb2_finish_accept()")
        return False
    except Exception as e:  # noqa: BLE001
        msg = str(e)
        if "signature" in msg.lower():
            print(f"FAIL (3.1.1 not offered): failed with the OLD round-2 signature-mismatch error "
                  f"instead of a clean negotiate rejection -- the dialect pin doesn't seem to be in "
                  f"effect: {type(e).__name__}: {e}")
            return False
        print(f"PASS (3.1.1 not offered, as designed): NEGOTIATE itself rejected a 3.1.1-only "
              f"client, before any session/signature exchange could happen: {type(e).__name__}: {e}")
        return True
    finally:
        # Best-effort cleanup only: the server closes the connection at
        # NEGOTIATE (no common dialect found, libsmb2.c's "No common
        # dialects for protocol" path) without ever sending a reply, so a
        # normal disconnect() here would itself raise on an already-dead
        # socket -- that's this test's own expected scenario, not a new
        # failure to report.
        try:
            connection.disconnect()
        except Exception:  # noqa: BLE001
            pass


def test_default_client_lands_on_302(port: int) -> bool:
    """THE headline test for round 3: a client that sends the same dialect
    list a real, modern, unmodified client would -- `dialect=None`, which
    smbprotocol expands to its own standard list, 2.0.2 through 3.1.1 (see
    Connection._send_smb2_negotiate() -- this is not a special-cased list
    Task 4 invented, it's smbprotocol's own default negotiate behavior) --
    must negotiate SMB 3.0.2 (not 3.1.1, since the server no longer offers
    it), get a genuinely non-guest, signing-verified session, and complete
    tree_connect("SD"). This is the scenario that stands in for "will an
    iPhone's Files app actually connect": per Visuality Systems' iOS SMB
    article (cited in SmbServer.cpp's dialect-pin comment), iOS is reported
    to use the SMB 3.0.2 dialect variant internally, so as long as 3.0.2 is
    somewhere in whatever list it sends -- true for any client that also
    speaks 3.1.1, since 3.0.2 predates it -- this is the path it takes."""
    connection = Connection(uuid.uuid4(), "127.0.0.1", port)
    try:
        connection.connect(timeout=5)  # no `dialect=` -- sends the client's own full standard list
        if connection.dialect != Dialects.SMB_3_0_2:
            print(f"FAIL (default client dialect): expected SMB 3.0.2, negotiated {connection.dialect} instead")
            return False
        session = Session(connection, "x3", "x3", require_encryption=False)
        session.connect()
        if session.signing_key is None:
            print("FAIL (default client dialect): session came back guest-flagged (no signing_key) -- "
                  "this would mean round 2's allow_anonymous=0 fix regressed")
            return False
        tree = TreeConnect(session, r"\\127.0.0.1\SD")
        tree.connect()
        print(f"PASS (default client -> SMB 3.0.2, non-guest, signed, tree_connect OK): "
              f"dialect={connection.dialect}, tree_id={tree.tree_connect_id}")
        tree.disconnect()
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (default client dialect): {type(e).__name__}: {e}")
        return False
    finally:
        try:
            connection.disconnect()
        except Exception:  # noqa: BLE001
            pass


def test_signing_enabled_only_client(port: int) -> bool:
    """A client that advertises SIGNING_ENABLED **without** SIGNING_REQUIRED gets
    an UNSIGNED session: the server advertises SIGNING_ENABLED without
    SIGNING_REQUIRED, and its TREE_CONNECT response is not signed.

    ⚠️ v82 REVERSES v65, DELIBERATELY. THIS IS NOT A BUG TO "FIX" BACK. v65 added
    `smb2_set_sign(ctx, 1)` to SmbServer::acceptOneConnection() and this check
    asserted the exact opposite of what it asserts now. That call is gone from
    the firmware path. Signing is ADAPTIVE: `smb2->sign` starts at 0 and
    libsmb2's own NEGOTIATE logic raises it only when the CLIENT sends
    SIGNING_REQUIRED, because the dialect 2.1.0 and >= 3.1.1 clauses in that
    logic cannot fire while this server pins 3.0.2 (libsmb2.c:4357-4385). The
    reasons are recorded in full at the site of the absent call; in short,
    signing is AES-CMAC over every PDU in both directions and the vendored AES
    re-derives its 176-byte key schedule per 16-byte block, which is tens of
    seconds on one book, on a personal device that is off except during a
    transfer, on a home LAN, and whose bytes were never encrypted anyway.

    WHAT DID **NOT** CHANGE, and is asserted here so it cannot drift:
    `server->signing_enabled` is still 1. That field feeds exactly one
    expression, the advertised SecurityMode's SIGNING_ENABLED bit, and it is
    what keeps a client that REQUIRES signing from being dropped after NEGOTIATE
    without a reply -- the v64 bug. "Adaptive" must not decay into "signing off":
    a REQUIRED-client must still get a signed session (that is
    test_forced_signing_escape_hatch()'s sibling property, and every other check
    in this file exercises it, see below). So assertion 2 requires SIGNING_ENABLED
    to be SET and SIGNING_REQUIRED to be CLEAR -- both halves.

    WHY THIS STILL HAS TO BUILD ITS CLIENT BY HAND, unchanged from v65 and the
    single most important thing on this page. What gates key derivation
    (libsmb2.c:4172-4177, `if (smb2->sign) smb2_create_signing_key()`) and
    per-PDU signing (pdu.c:701-707, `if (smb2->sign) smb2_pdu_add_signature()`)
    is the per-context `smb2->sign`, which the server raises when the CLIENT
    sends SMB2_NEGOTIATE_SIGNING_REQUIRED -- and smbprotocol defaults to
    require_signing=True, so EVERY other check in this file sends
    SIGNING_REQUIRED and raises `smb2->sign` by accident. Those checks therefore
    all run against a SIGNED session and are blind to this axis in both
    directions: they were green before v65, green after it, and green after v82
    removed it again. Only a hand-built ENABLED-only client can see it at all.

    Three assertions, and the FIRST is still the one that keeps this honest:
      1. BEFORE connecting, that this client really is the ENABLED-only shape.
         smbprotocol derives client_security_mode from require_signing in
         Connection.__init__ and then OVERWRITES it to SIGNING_REQUIRED on any
         negotiate response that advertised SIGNING_REQUIRED, so it is only
         readable before connect(). (Against the adaptive server that overwrite
         no longer happens -- but it still does against the forced server in
         test_forced_signing_escape_hatch(), which shares this client shape, and
         relying on that would be relying on the very behaviour under test.)
         Without this assertion a changed smbprotocol default would silently turn
         this check into a duplicate of all the others -- and, now that the
         expected outcome is "unsigned", a duplicate that PASSES for the wrong
         reason is no longer a possibility to be reasoned about: a
         require_signing=True client would make the server sign, this check would
         go red, and the red would be blamed on the firmware.
      2. that the server's advertised SecurityMode has SIGNING_ENABLED set and
         SIGNING_REQUIRED clear. The REQUIRED bit is computed straight from the
         flag under test (libsmb2.c:4389, `(smb2->sign ?
         SMB2_NEGOTIATE_SIGNING_REQUIRED : 0)`), so it is a direct readout of it.
      3. that the TREE_CONNECT RESPONSE HEADER carries neither SMB2_FLAGS_SIGNED
         nor a non-zero signature -- the wire fact, and the one an independent
         client acts on. TREE_CONNECT is built by hand because
         TreeConnect.connect() keeps the parsed body and discards the header.

    WHAT THIS COSTS, so the next reader is not surprised by it: this wire shape
    is exactly what Samba's smbclient 4.19.5 refuses -- "tree connect failed:
    NT_STATUS_ACCESS_DENIED", while this server's own log says `tree_connect ok`
    (MS-SMB2 3.2.5.1.3: a client that required signing discards an unsigned
    response). smbclient and the Linux kernel cifs client are two of the three
    INDEPENDENT implementations this suite depends on, so the firmware keeps an
    escape hatch for them -- SmbServer::setForceSigning(), reached here via
    SMBHOST_SIGN=1 -- and test_forced_signing_escape_hatch() is the check that
    keeps that hatch from rotting. iOS does not enforce signed responses (the v64
    field log shows an iPhone getting past TREE_CONNECT to IOCTL/QUERY_INFO
    against an unsigned server), which is why the device can afford this."""
    connection = Connection(uuid.uuid4(), "127.0.0.1", port, require_signing=False)
    enabled_only = SecurityMode.SMB2_NEGOTIATE_SIGNING_ENABLED
    if int(connection.client_security_mode) != int(enabled_only):
        print(f"FAIL (signing, ENABLED-only client): this client would advertise SecurityMode "
              f"0x{int(connection.client_security_mode):02x}, not SIGNING_ENABLED "
              f"(0x{int(enabled_only):02x}). With SIGNING_REQUIRED in there libsmb2 raises "
              f"smb2->sign by itself, the server signs, and this check goes red for a reason that "
              f"has nothing to do with the firmware -- fix the client setup, do not relax the "
              f"assertion")
        return False
    try:
        connection.connect(dialect=WORKING_DIALECT, timeout=5)
        server_mode = int(connection.server_security_mode)
        if not server_mode & SecurityMode.SMB2_NEGOTIATE_SIGNING_ENABLED:
            print(f"FAIL (signing, ENABLED-only client): server advertised SecurityMode "
                  f"0x{server_mode:02x} WITHOUT SIGNING_ENABLED. Adaptive signing must not become "
                  f"signing-off: server->signing_enabled must stay 1 (SmbServer.cpp), or a client "
                  f"that REQUIRES signing is dropped after NEGOTIATE with no reply at all -- the "
                  f"v64 bug")
            return False
        if server_mode & SecurityMode.SMB2_NEGOTIATE_SIGNING_REQUIRED:
            print(f"FAIL (signing, ENABLED-only client): server advertised SecurityMode "
                  f"0x{server_mode:02x} WITH SIGNING_REQUIRED, i.e. smb2->sign is 1 for a client "
                  f"that never asked for signing. v82 made signing adaptive: the unconditional "
                  f"smb2_set_sign(ctx, 1) is gone from SmbServer::acceptOneConnection() and must "
                  f"not come back -- read the note at that (absent) call before changing this")
            return False

        session = Session(connection, "x3", "x3", require_encryption=False)
        session.connect()
        if session.signing_key is None:
            print("FAIL (signing, ENABLED-only client): no session signing key -- a guest session "
                  "has none, so this is the allow_anonymous regression, not a signing one. (The key "
                  "is derived from the NTLM session key regardless of whether anything is signed; "
                  "it is asserted here purely as the guest-flag guard.)")
            return False

        request = SMB2TreeConnectRequest()
        request["buffer"] = r"\\127.0.0.1\SD".encode("utf-16-le")
        sent = connection.send(request, sid=session.session_id)
        response = connection.receive(sent)
        flags = response["flags"].get_value()
        signature = response["signature"].get_value()
        if flags & Smb2Flags.SMB2_FLAGS_SIGNED:
            print(f"FAIL (signing, ENABLED-only client): TREE_CONNECT response header flags "
                  f"0x{flags:02x} carries SMB2_FLAGS_SIGNED (0x08) for a client that only advertised "
                  f"SIGNING_ENABLED. The server is signing when it should not be -- see assertion 2")
            return False
        if signature != b"\x00" * 16:
            print(f"FAIL (signing, ENABLED-only client): TREE_CONNECT response is unsigned "
                  f"(flags 0x{flags:02x}) yet the signature field is not sixteen zero bytes: "
                  f"{signature.hex()}. libsmb2 memsets that field on an unsigned PDU, so this is "
                  f"either stale buffer contents leaking onto the wire or a half-applied signature")
            return False
        print(f"PASS (signing, ENABLED-only client): client advertised SIGNING_ENABLED only, server "
              f"answered SecurityMode 0x{server_mode:02x} (ENABLED, not REQUIRED) and an UNSIGNED "
              f"TREE_CONNECT response -- adaptive signing, as v82 intends")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (signing, ENABLED-only client): {type(e).__name__}: {e}")
        return False
    finally:
        # No TreeConnect object to disconnect (the request was hand-built), so
        # this borrows _teardown()'s last resort: the tree stays connected until
        # the socket dies, and a leaked socket fills the server's fixed
        # four-slot connection registry and times out the NEXT check instead.
        try:
            connection.disconnect()
        except Exception:  # noqa: BLE001
            pass
        try:
            connection.transport.close()
        except Exception:  # noqa: BLE001
            pass


def connect_session(port: int, share: str = "SD"):
    """Establishes a fully real, non-guest, signing-enforced session + tree
    connect -- NO client-side bypasses of any kind (unlike round 1's
    version of this helper, which needed require_signing=False,
    require_secure_negotiate=False, and a forced old dialect specifically
    to route around the guest-flag bug that no longer exists). Returns
    (connection, session, tree) -- caller is responsible for
    tree.disconnect() and connection.disconnect()."""
    connection = Connection(uuid.uuid4(), "127.0.0.1", port)
    connection.connect(dialect=WORKING_DIALECT, timeout=5)
    session = Session(connection, "x3", "x3", require_encryption=False)
    session.connect()
    assert session.signing_key is not None, "session should NOT be guest-flagged (signing_key must be set)"
    tree = TreeConnect(session, rf"\\127.0.0.1\{share}")
    tree.connect()
    return connection, session, tree


def test_share_validation(port: int) -> bool:
    """tree_connect("SD") must succeed; tree_connect("NOPE") must fail."""
    ok = True
    connection, session, tree = connect_session(port, "SD")
    try:
        print(f"PASS (tree_connect SD, non-guest, signed): tree_id={tree.tree_connect_id}")
    finally:
        tree.disconnect()

    try:
        bad_tree = TreeConnect(session, r"\\127.0.0.1\NOPE")
        bad_tree.connect()
        print("FAIL (tree_connect NOPE): unexpectedly succeeded")
        ok = False
        bad_tree.disconnect()
    except Exception as e:  # noqa: BLE001
        print(f"PASS (tree_connect NOPE rejected): {type(e).__name__}")
    finally:
        connection.disconnect()
    return ok


def _create_file(tree, name: str, disposition: int, access: int, options: int, attributes=FileAttributes.FILE_ATTRIBUTE_NORMAL):
    op = Open(tree, name)
    op.create(ImpersonationLevel.Impersonation, access, attributes, ShareAccess.FILE_SHARE_READ, disposition, options)
    return op


def test_create_close(port: int) -> bool:
    """A plain file create/close and a directory open/close, both backed by
    real HalStorage I/O (see stub_hal/HalStorage.cpp -- this actually writes
    to test/host/sdroot/ on this machine), over a fully signed, non-guest
    session with no bypasses."""
    ok = True
    connection, session, tree = connect_session(port)
    try:
        f = _create_file(
            tree,
            "smoke_test_file.txt",
            CreateDisposition.FILE_OVERWRITE_IF,
            FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        f.close()
        print("PASS (create/close file): smoke_test_file.txt")

        d = _create_file(
            tree,
            "",
            CreateDisposition.FILE_OPEN,
            FilePipePrinterAccessMask.GENERIC_READ,
            CreateOptions.FILE_DIRECTORY_FILE,
            attributes=FileAttributes.FILE_ATTRIBUTE_DIRECTORY,
        )
        d.close()
        print("PASS (create/close directory): share root")
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (create/close): {type(e).__name__}: {e}")
        ok = False
    finally:
        tree.disconnect()
        connection.disconnect()
    return ok


def test_protected_path_rejected(port: int) -> bool:
    """create_cmd must enforce path protection for write-intent opens (Task 4
    brief, Step 5) -- reuses WebDAVHandler's exact rules (any dotfile/dot-
    directory segment, or a reserved directory name). Not just at
    delete/rename time."""
    connection, session, tree = connect_session(port)
    try:
        op = Open(tree, ".hidden/secret.txt")
        op.create(
            ImpersonationLevel.Impersonation,
            FilePipePrinterAccessMask.GENERIC_WRITE,
            FileAttributes.FILE_ATTRIBUTE_NORMAL,
            ShareAccess.FILE_SHARE_READ,
            CreateDisposition.FILE_CREATE,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        print("FAIL (protected path): CREATE unexpectedly succeeded")
        return False
    except Exception as e:  # noqa: BLE001
        print(f"PASS (protected path rejected): {type(e).__name__}")
        return True
    finally:
        tree.disconnect()
        connection.disconnect()


# MS-SMB2 2.2.1.2: the reserved FileId a chained request carries to mean "use
# the FileId the PREVIOUS operation in this chain produced". See
# test_compound_related_file_id().
COMPOUND_FILE_ID = b"\xff" * 16


def _close_request(file_id: bytes):
    """A bare SMB2 CLOSE naming `file_id`.

    Built by hand, and sent/received through Connection rather than Open,
    wherever the RESPONSE matters: Open._close_response swallows FILE_CLOSED
    (`except FileClosed: return`), which since v65 is the status an unowned or
    unknown FileId comes back with -- so a check that goes through Open cannot
    tell a refusal from a success."""
    request = SMB2CloseRequest()
    request["flags"] = 0
    request["file_id"] = file_id
    return request


def _query_info_request(file_id: bytes):
    """A QUERY_INFO(FileAllInformation) naming `file_id` -- info_type 1,
    file_info_class 18, verbatim the pair the iPhone's diag.log line names
    ("query_info reject: ... type=1 class=18")."""
    request = SMB2QueryInfoRequest()
    request["info_type"] = InfoType.SMB2_0_INFO_FILE
    request["file_info_class"] = FileInformationClass.FILE_ALL_INFORMATION
    request["output_buffer_length"] = 65536
    request["additional_information"] = 0
    request["flags"] = 0
    request["file_id"] = file_id
    request["buffer"] = b""
    return request


def test_cross_connection_close_rejected(port: int) -> bool:
    """Connection B must not be able to close connection A's handle.
    findOpenFile() used to match on file_id ALONE, and the ids are trivially
    predictable (makeFileId(): a small monotonic counter plus the slot index),
    so B could simply guess A's -- and after Tasks 6-7 the same lookup backs
    read and write, not just close. This test cheats by *knowing* A's file_id
    rather than guessing it, which is fair: guessing it is arithmetic, and the
    property under test is the ownership check, not the id's entropy.

    Afterwards A must still be able to USE and close its own handle -- proving
    the handle really survived B's attempt rather than being closed with an
    error reply.

    THE ASSERTION SHAPE CHANGED IN v65, and the change belongs to the test, not
    to the server: the refusal is unchanged and still logs
    `SMB close reject: no such handle for ctx=<B> id=<A's id>`. What changed is
    the STATUS. close_cmd now answers STATUS_FILE_CLOSED -- the MS-SMB2 answer
    for an invalid FileId, and what Windows returns -- instead of libsmb2's
    blanket NOT_IMPLEMENTED. smbprotocol's Open treats that status as "already
    closed, nothing to do" and returns normally (Open._close_response:
    `try: self.connection.receive(request) / except FileClosed: return`), so the
    old `thief.close()` in a try/except stopped raising and this check read the
    silence as a pass. A security property that cannot fail is worse than no
    check, so two things changed here:

      * B's CLOSE goes out RAW and is received with `connection.receive()`,
        which does raise FileClosed -- the swallow is in Open, not in the
        transport. The status is asserted specifically, because FILE_CLOSED is
        now the answer we mean to give and a drift back to NOT_IMPLEMENTED is a
        regression worth seeing.
      * A then WRITES and READS through its handle before closing it. That is
        what proves the handle was untouched. "Still closable" was too weak --
        a slot that B had actually released would satisfy it too, since a close
        of an unknown id is exactly what now returns success-shaped silence."""
    conn_a, sess_a, tree_a = connect_session(port)
    conn_b, sess_b, tree_b = connect_session(port)
    payload = b"A's bytes, untouched"
    try:
        victim = _create_file(
            tree_a,
            "smoke_test_victim.txt",
            CreateDisposition.FILE_OVERWRITE_IF,
            FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        victim.write(payload, 0)

        # B naming A's handle, on B's own session and tree. Raw, so the reply
        # reaches this test instead of being absorbed as "already closed" -- see
        # the docstring and _close_request().
        request = conn_b.send(_close_request(victim.file_id), sess_b.session_id, tree_b.tree_connect_id)
        try:
            conn_b.receive(request)
            print("FAIL (cross-connection close): connection B closed connection A's handle")
            return False
        except FileClosed:
            print("PASS (cross-connection close rejected): STATUS_FILE_CLOSED -- B was told the "
                  "FileId is not one of its own, not handed A's handle")
        except Exception as e:  # noqa: BLE001
            print(f"FAIL (cross-connection close): refused, but with {type(e).__name__} rather than "
                  f"FileClosed: {e} -- the refusal is right and the status is not")
            return False

        readback = victim.read(0, len(payload))
        if readback != payload:
            print(f"FAIL (cross-connection close): A's handle read back {readback!r} after B's "
                  f"attempt, expected {payload!r}")
            return False
        victim.write(b"!", len(payload))
        try:
            victim.close()
        except Exception as e:  # noqa: BLE001
            print(f"FAIL (cross-connection close): A's own handle no longer closable afterwards: "
                  f"{type(e).__name__}: {e}")
            return False
        print("PASS (cross-connection close): A's handle survived B's attempt -- still read and "
              "wrote correctly, and A closed it itself")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (cross-connection close): {type(e).__name__}: {e}")
        return False
    finally:
        for tree, conn in ((tree_a, conn_a), (tree_b, conn_b)):
            try:
                tree.disconnect()
            except Exception:  # noqa: BLE001
                pass
            try:
                conn.disconnect()
            except Exception:  # noqa: BLE001
                pass


def test_compound_related_file_id(port: int) -> bool:
    """ONE packet holding CREATE(share root) + QUERY_INFO(FileAllInformation) +
    CLOSE, with the 2nd and 3rd flagged SMB2_FLAGS_RELATED_OPERATIONS and their
    FileId set to the all-0xFF compound placeholder. All three must answer
    STATUS_SUCCESS.

    THIS IS THE SHAPE THAT ACTUALLY BROKE A REAL IPHONE. Copied from the
    device's own diag.log (output/v64-smb-server, diag11), which recorded this
    three times before iOS gave up with "could not get contents":
        SMB tree_connect ok: SD (disk share) path=/crossmosa.local/SD
        SMB ioctl rejected (not implemented)
        SMB query_info reject: no such handle for ctx=... type=1 class=18
        SMB close  reject: no such handle for ctx=... id=ffffffffffffffffffffffff
    type=1 is SMB2_0_INFO_FILE, class=18 is FileAllInformation, and 0xFF..FF is
    the MS-SMB2 2.2.1.2 placeholder for "the FileId the previous operation in
    this chain produced". macOS and iOS do all path-based metadata this way, so
    this compound is the FIRST thing an iPhone sends after TREE_CONNECT.
    libsmb2 defines the constant (libsmb2.c:138 `compound_file_id`) but every
    reference is a client-side `memcpy` INTO a request; nothing server-side
    substitutes it, so findOpenFile() looked 0xFF..FF up literally and missed
    both times. v65 resolves it through gLastCreatedId (SmbFileHandlers.cpp).

    Note what this catches that nothing else here can. Every other check in this
    file opens a handle, reads the real FileId out of the CREATE response, and
    uses that -- so the placeholder path was never sent even once, and iOS's very
    first request was the first thing ever to touch it. A green suite meant
    nothing about it.

    The NEGATIVE half lives here rather than in its own check because it is the
    same mechanism from the other side: resolving the placeholder must not become
    a way to NAME someone else's handle. Connection A opens a file, arming
    gLastCreatedId with a live handle; connection B, which has created nothing,
    sends QUERY_INFO with 0xFF..FF and must be refused. gLastCreatedOwner is
    compared by smb2_context pointer for exactly this reason. A's handle must
    still work afterwards, so the refusal is a refusal and not a side effect."""
    ok = True
    connection, session, tree = connect_session(port)
    try:
        create = SMB2CreateRequest()
        create["impersonation_level"] = ImpersonationLevel.Impersonation
        create["desired_access"] = (DirectoryAccessMask.FILE_READ_ATTRIBUTES
                                    | DirectoryAccessMask.FILE_LIST_DIRECTORY
                                    | DirectoryAccessMask.SYNCHRONIZE)
        create["file_attributes"] = 0
        create["share_access"] = ShareAccess.FILE_SHARE_READ | ShareAccess.FILE_SHARE_WRITE
        create["create_disposition"] = CreateDisposition.FILE_OPEN
        create["create_options"] = CreateOptions.FILE_DIRECTORY_FILE
        # The SHARE ROOT, which is what iOS opens here -- and the target matters,
        # because the root is the one path createCmd handles through openRoot()
        # rather than a normal open. smbprotocol encodes it as the sentinel
        # b"\x00\x00", which its own _buffer_path_size() reports as name_length 0.
        create["buffer_path"] = b"\x00\x00"

        requests = connection.send_compound(
            [create, _query_info_request(COMPOUND_FILE_ID), _close_request(COMPOUND_FILE_ID)],
            session.session_id, tree.tree_connect_id, related=True)

        labels = ("CREATE(share root)", "QUERY_INFO(FileAllInformation)", "CLOSE")
        created_id = None
        for label, request in zip(labels, requests):
            try:
                response = connection.receive(request)
                if label.startswith("CREATE"):
                    parsed = SMB2CreateResponse()
                    parsed.unpack(response["data"].get_value())
                    created_id = parsed["file_id"].get_value()
                print(f"PASS (compound related file id): {label} -> STATUS_SUCCESS")
            except Exception as e:  # noqa: BLE001
                print(f"FAIL (compound related file id): {label} -> {type(e).__name__}: {e} -- "
                      f"against v64 the 2nd and 3rd failed exactly here, and this is the whole of "
                      f"what an iPhone sends before it decides the share is unusable")
                ok = False
        if created_id == COMPOUND_FILE_ID:
            print("FAIL (compound related file id): CREATE echoed the 0xFF placeholder back as the "
                  "handle's own FileId; the chain would 'work' by accident and nothing would ever "
                  "resolve the placeholder")
            ok = False

        # --- the negative half ---
        conn_b, sess_b, tree_b = connect_session(port)
        try:
            owned = _create_file(
                tree,
                "smoke_test_compound_owned.txt",
                CreateDisposition.FILE_OVERWRITE_IF,
                FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
                CreateOptions.FILE_NON_DIRECTORY_FILE,
            )
            # A has just created, so the placeholder is armed and points at a
            # live handle. B has created nothing on its own context.
            sent = conn_b.send(_query_info_request(COMPOUND_FILE_ID),
                               sess_b.session_id, tree_b.tree_connect_id)
            try:
                conn_b.receive(sent)
                print("FAIL (compound placeholder, unarmed connection): B resolved 0xFF..FF to A's "
                      "handle -- the placeholder became a way to name another connection's file")
                ok = False
            except Exception as e:  # noqa: BLE001
                print(f"PASS (compound placeholder, unarmed connection): refused with "
                      f"{type(e).__name__}")

            after = _query_info(tree, owned, InfoType.SMB2_0_INFO_FILE,
                                FileInformationClass.FILE_ALL_INFORMATION)
            if not after:
                print("FAIL (compound placeholder): A's own handle stopped answering query_info "
                      "after B's probe")
                ok = False
            owned.close()
        finally:
            _teardown(tree_b, conn_b)
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (compound related file id): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_echo_keepalive(port: int) -> bool:
    """Connect, idle briefly, send an SMB2 ECHO, and confirm the session is
    still usable afterward -- echo_cmd returning anything other than 0 would
    make the client conclude the server is dead."""
    connection, session, tree = connect_session(port)
    try:
        time.sleep(0.3)
        connection.echo(session.session_id)
        # Prove the session is still alive by doing one more real operation.
        f = _create_file(
            tree,
            "smoke_test_after_echo.txt",
            CreateDisposition.FILE_OVERWRITE_IF,
            FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
            CreateOptions.FILE_NON_DIRECTORY_FILE,
        )
        f.close()
        print("PASS (echo keepalive): session still alive after idle + echo")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (echo keepalive): {type(e).__name__}: {e}")
        return False
    finally:
        tree.disconnect()
        connection.disconnect()


def test_destruction_event_recycles_slots(port: int) -> bool:
    """Opens a file on 12 separate connections in a row (the open-file table
    is 8 fixed slots) WITHOUT ever closing it -- only the raw TCP socket is
    dropped each time. If destruction_event didn't release that connection's
    slot(s), the 9th connection onward would fail with "open-file table
    full". All 12 succeeding is the proof destruction_event actually runs
    and reclaims."""
    for i in range(12):
        connection, session, tree = connect_session(port)
        try:
            op = Open(tree, f"smoke_test_abandoned_{i}.txt")
            op.create(
                ImpersonationLevel.Impersonation,
                FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
                FileAttributes.FILE_ATTRIBUTE_NORMAL,
                ShareAccess.FILE_SHARE_READ,
                CreateDisposition.FILE_OVERWRITE_IF,
                CreateOptions.FILE_NON_DIRECTORY_FILE,
            )
        except Exception as e:  # noqa: BLE001
            print(f"FAIL (destruction_event recycle): connection {i}/12 could not open a file: {type(e).__name__}: {e}")
            connection.disconnect()
            return False
        # Deliberately no op.close()/tree.disconnect() -- just drop the
        # socket, forcing destruction_event (not close_cmd) to reclaim.
        connection.transport.close()
        time.sleep(0.1)  # give the server a tick to notice the dead fd and cull it
    print("PASS (destruction_event recycle): 12/12 connections opened a file without ever closing it")
    return True


# ---------------------------------------------------------------------------
# Task 5: query_directory / query_info.

def _open_dir(tree, name: str):
    """Opens a directory handle suitable for enumeration."""
    d = Open(tree, name)
    d.create(
        ImpersonationLevel.Impersonation,
        FilePipePrinterAccessMask.GENERIC_READ,
        FileAttributes.FILE_ATTRIBUTE_DIRECTORY,
        ShareAccess.FILE_SHARE_READ,
        CreateDisposition.FILE_OPEN,
        CreateOptions.FILE_DIRECTORY_FILE,
    )
    return d


def _teardown(tree, connection) -> None:
    """Best-effort cleanup. Every Task 5 test routes its `finally` through this
    instead of calling disconnect() directly: when a listing test fails part
    way it leaves an Open handle untracked-closed, and smbprotocol's
    connection.disconnect() then raises out of the `finally` block itself --
    which aborts the whole suite at the first failure instead of reporting it
    and moving on. Found while negative-testing the next_entry_offset guard."""
    try:
        tree.disconnect()
    except Exception:  # noqa: BLE001
        pass
    try:
        connection.disconnect()
    except Exception:  # noqa: BLE001
        pass
    # Last resort, added in Task 6. Swallowing disconnect()'s exception is not
    # enough: it walks the session's still-open handles FIRST and can raise
    # part way through, before it ever reaches transport.close() -- so the TCP
    # socket stays open. That is invisible on the client side but fills the
    # server's fixed four-slot connection registry (SmbServer.cpp), and the
    # next test then times out during NEGOTIATE instead of reporting its own
    # result. Found running the Task 6 tests against the pre-change build,
    # where four handler failures in a row leaked four sockets and took the
    # rest of the suite down with them.
    try:
        connection.transport.close()
    except Exception:  # noqa: BLE001
        pass


def _seed_tree(relative: str, files: dict, subdirs=()) -> None:
    """Plants a directory tree directly on the harness's SD root. Done outside
    SMB on purpose: the point is to enumerate what is really on the card, not
    only what this same server just created."""
    base = os.path.join(SD_ROOT, *relative.split("/")) if relative else SD_ROOT
    os.makedirs(base, exist_ok=True)
    for name, size in files.items():
        with open(os.path.join(base, name), "wb") as fh:
            fh.write(b"x" * size)
    for name in subdirs:
        os.makedirs(os.path.join(base, name), exist_ok=True)


def _drain_listing(handle, info_class=FileInformationClass.FILE_ID_BOTH_DIRECTORY_INFORMATION,
                   pattern_first="*", pattern_rest="", flags=None):
    """Enumerates a directory to exhaustion the way a real client does, and
    returns (entries, round_trips).

    `pattern_rest=""` is the part that matters and is not incidental: MS-SMB2
    3.3.5.18 has the client send FileName only on the FIRST query of an
    enumeration, so every continuation and every termination arrives with
    file_name_length == 0. That is precisely the request shape that made
    libsmb2 free an uninitialised pointer (see queryDirectoryCmd's first
    statement), so any listing that needs more than one round trip is also the
    regression test for that crash."""
    entries = []
    trips = 0
    pattern = pattern_first
    while True:
        trips += 1
        try:
            batch = handle.query_directory(pattern, info_class, flags=flags if trips == 1 else None)
        except NoMoreFiles:
            return entries, trips
        for e in batch:
            entries.append(e)
        pattern = pattern_rest


def _entry_names(entries) -> dict:
    """{name: (end_of_file, is_directory)} from decoded directory entries."""
    out = {}
    for e in entries:
        name = e["file_name"].get_value().decode("utf-16-le")
        attrs = e["file_attributes"].get_value()
        out[name] = (e["end_of_file"].get_value(), bool(attrs & FileAttributes.FILE_ATTRIBUTE_DIRECTORY))
    return out


# v65: every RESTARTING enumeration now begins with '.' and '..'.
#
# v64 emitted neither, because the underlying openNextFile() returns neither on
# either backend. That looked harmless and was not: an EMPTY directory answered
# its very first QUERY_DIRECTORY with zero entries, libsmb2 turns an empty reply
# into STATUS_NO_MORE_FILES (libsmb2.c:3785-3788, `else if (!ret) { if
# (rep.output_buffer_length == 0) ... SMB2_STATUS_NO_MORE_FILES`), and a client
# reports THAT as an error rather than as an empty folder -- measured with
# smbclient, `cd emptydir; ls` -> "NT_STATUS_NO_SUCH_FILE listing \emptydir\*".
# The two dots are how a client learns "present, and empty". See
# queryDirectoryCmd's `if (restart)` block in src/network/SmbFileHandlers.cpp
# (immediately before the scan loop) for the server side, including why they are
# emitted on a restart ONLY.
#
# Two consequences the checks below are built around:
#   * a pattern-less listing carries them, so an exact set comparison has to
#     account for them -- hence _real_only() rather than loosening the
#     comparisons into subset tests, which would stop noticing a dropped book;
#   * they go through the SAME wildcardMatch() as any other name, so a
#     pattern-filtered listing must NOT contain them (test_listing_pattern_filter
#     and the needle half of test_listing_scan_bound are what pin that).
DOT_NAMES = (".", "..")


def _dots_present(got: dict, label: str) -> bool:
    """Requires both '.' and '..' in a decoded listing, each shaped like the
    directory it names: end_of_file 0 and FILE_ATTRIBUTE_DIRECTORY.

    Checked, not merely tolerated. A '..' that arrived flagged as a plain file
    is a real bug -- clients walk back up through it -- and it is exactly the
    kind of bug a "just ignore the dots" subset check would hide."""
    for dot in DOT_NAMES:
        if dot not in got:
            print(f"FAIL ({label}): {dot!r} missing -- a restarting enumeration must emit '.' and "
                  f"'..' first, or an empty directory reads to the client as an error")
            return False
        if got[dot] != (0, True):
            print(f"FAIL ({label}): {dot!r} reported {got[dot]!r}, expected (0, True) -- both dot "
                  f"entries name directories and carry size 0")
            return False
    return True


def _real_only(got: dict) -> dict:
    """`got` minus the two dot entries, so every check keeps comparing an EXACT
    set of real names and sizes."""
    return {n: v for n, v in got.items() if n not in DOT_NAMES}


def test_directory_listing(port: int) -> bool:
    """Three files with distinct sizes plus a subdirectory: every real entry
    present exactly once, sizes exact, the directory flagged as one -- plus the
    '.' and '..' entries v65 prepends to a restarting enumeration (DOT_NAMES)."""
    rel = "smoke_test_listing"
    sizes = {"alpha.txt": 11, "beta.epub": 2048, "gamma.bin": 0}
    _seed_tree(rel, sizes, subdirs=("nested",))
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel)
        entries, trips = _drain_listing(d)
        d.close()
        got = _entry_names(entries)
        expected = {n: (s, False) for n, s in sizes.items()}
        expected["nested"] = (0, True)
        if not _dots_present(got, "directory listing"):
            return False
        if _real_only(got) != expected:
            print(f"FAIL (directory listing): got {_real_only(got)!r} (besides '.' and '..'), "
                  f"expected {expected!r}")
            return False

        # The OTHER info class the handler supports. Both 0x25 and 0x26 flow
        # through the same encoder line the vendored NextEntryOffset fix
        # touches, and 0x26's entries have a different fixed size (80 vs 104) --
        # so a listing that is correct for one is not automatically correct for
        # the other, and 0x26 was previously never exercised at all.
        d2 = _open_dir(tree, rel)
        full_entries, full_trips = _drain_listing(
            d2, info_class=FileInformationClass.FILE_ID_FULL_DIRECTORY_INFORMATION)
        d2.close()
        full_got = _entry_names(full_entries)
        # A fresh handle, so this query is a restart too and must carry the dots
        # as well -- they are emitted by queryDirectoryCmd before the info class
        # is used to size anything, so a class-specific dot bug would show here.
        if not _dots_present(full_got, "directory listing, ID_FULL"):
            return False
        if _real_only(full_got) != expected:
            print(f"FAIL (directory listing, ID_FULL): got {_real_only(full_got)!r}, expected {expected!r}")
            return False

        print(f"PASS (directory listing): 4/4 real entries plus '.' and '..', sizes and directory "
              f"flag correct -- ID_BOTH in {trips} round trips, ID_FULL in {full_trips}")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (directory listing): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_directory_listing_cjk(port: int) -> bool:
    """The same, with Traditional Chinese names -- this device's library is
    essentially all Chinese, so an ASCII-only listing suite would stay green
    through a UTF-16LE <-> UTF-8 bug that broke every real folder on the card.
    Also checks the reported name LENGTH is in bytes (the protocol's unit),
    which is where a CJK-specific off-by-3 would surface even when the decoded
    string happens to look right.

    The byte-length loop deliberately covers the '.' and '..' entries too (2 and
    4 bytes in UTF-16LE): v65 fills their name_length through the same
    `2 * strlen(name)` arithmetic as the real names, so a one-byte-per-character
    slip there would show up on the shortest names in the reply."""
    rel = "列舉測試"
    sizes = {"範例書坊-繁體中文檔名.epub": 1234, "紅樓夢.txt": 7}
    _seed_tree(rel, sizes, subdirs=("子資料夾",))
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel)
        entries, _ = _drain_listing(d)
        d.close()
        got = _entry_names(entries)
        expected = {n: (s, False) for n, s in sizes.items()}
        expected["子資料夾"] = (0, True)
        if not _dots_present(got, "CJK listing"):
            return False
        if _real_only(got) != expected:
            print(f"FAIL (CJK listing): got {_real_only(got)!r} (besides '.' and '..'), "
                  f"expected {expected!r}")
            return False
        for e in entries:
            name_bytes = e["file_name"].get_value()
            declared = e["file_name_length"].get_value()
            if declared != len(name_bytes):
                print(f"FAIL (CJK listing): file_name_length={declared} but the name is "
                      f"{len(name_bytes)} bytes ({name_bytes.decode('utf-16-le')!r}) -- the length "
                      f"field is being filled with a character count, not a byte count")
                return False
        print(f"PASS (CJK listing): {len(_real_only(got))} Chinese-named entries round-tripped, byte "
              f"lengths correct (including '.' and '..')")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (CJK listing): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_listing_hides_protected(port: int) -> bool:
    """THE listing-side half of path protection. create_cmd refuses to open a
    protected path at all, read included -- so a listing that still enumerates
    them advertises exactly what the server then refuses, and /.crossmosa/ is
    where the Wi-Fi credentials, settings and reading progress live. Mirrors
    WebDAV's PROPFIND hide rule via the shared ProtectedPath ruleset.

    v65's '.' and '..' are not a hole in that rule and are not counted as
    visible content: they name this directory and its parent, which the client
    already has, and they carry no name a client could ask for. Escaping the
    share via '..' is a PATH-resolution question, not a listing one, and is
    pinned separately by test_dotdot_traversal_rejected()."""
    rel = "smoke_test_hide"
    _seed_tree(rel, {"visible.epub": 3})
    _seed_tree(f"{rel}/.crossmosa", {"wifi.json": 5})
    _seed_tree(f"{rel}/.hidden_dotdir", {})
    _seed_tree(f"{rel}/System Volume Information", {})
    _seed_tree(f"{rel}/XTCache", {})
    with open(os.path.join(SD_ROOT, rel, ".DS_Store"), "wb") as fh:
        fh.write(b"junk")
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel)
        entries, _ = _drain_listing(d)
        d.close()
        got = _entry_names(entries)
        if not _dots_present(got, "protected hiding"):
            return False
        real = set(_real_only(got))
        if real != {"visible.epub"}:
            print(f"FAIL (protected hiding): listing returned {sorted(real)!r} besides '.' and '..'; "
                  f"only ['visible.epub'] should be visible")
            return False
        print("PASS (protected hiding): .crossmosa / .hidden_dotdir / .DS_Store / "
              "'System Volume Information' / XTCache all absent from the listing")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (protected hiding): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_listing_multi_response(port: int) -> bool:
    """A directory with more files than fit in one response (the handler caps
    entries per reply and continues on the next request). Proves three things
    at once: the per-handle cursor really advances, nothing is dropped or
    duplicated across the boundary, and the continuation requests -- which
    carry NO search pattern, exactly like macOS/iOS -- do not kill the server
    on libsmb2's uninitialised req->name."""
    rel = "smoke_test_many"
    count = 80
    _seed_tree(rel, {f"book_{i:03d}.epub": i for i in range(count)})
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel)
        entries, trips = _drain_listing(d)
        d.close()
        got = _entry_names(entries)
        expected = {f"book_{i:03d}.epub": (i, False) for i in range(count)}
        if not _dots_present(got, "multi-response listing"):
            return False
        # count + 2, and comparing the LIST length rather than the dict's is the
        # part that catches a duplicate (a dict would collapse it). The 2 is
        # exact, not a floor: the dots are emitted on the restarting request
        # only, so a server that re-emitted them on every continuation -- which
        # is the obvious way to get this wrong, and would make a client show the
        # same folder several times over -- lands here as count + trips.
        if len(entries) != count + 2:
            print(f"FAIL (multi-response listing): {len(entries)} entries returned for {count} files "
                  f"plus '.' and '..' across {trips} round trips -- either a real file was dropped or "
                  f"duplicated at a response boundary, or the dot entries were repeated on a "
                  f"continuation instead of only on the restart")
            return False
        if _real_only(got) != expected:
            print(f"FAIL (multi-response listing): entry set/sizes wrong across {trips} round trips")
            return False
        if trips < 3:
            print(f"FAIL (multi-response listing): finished in {trips} round trips -- the test is no "
                  f"longer exercising continuation at all; raise `count`")
            return False
        # The server must still be alive: a continuation request is where the
        # uninitialised-req->name free used to take the whole process down.
        connection.echo(session.session_id)
        print(f"PASS (multi-response listing): {count} files plus '.' and '..' over {trips} round "
              f"trips, no drops or duplicates, server alive after pattern-less continuations")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (multi-response listing): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_listing_empty_directory(port: int) -> bool:
    """An empty directory must list exactly '.' and '..', and then end.

    THIS CHECK'S PREMISE CHANGED IN v65, and the old one -- "answer
    STATUS_NO_MORE_FILES on the very first query, the protocol's way of saying
    'nothing here', not an error" -- was the bug, stated as the contract. It is
    not how a client reads that reply: an empty reply is turned into
    STATUS_NO_MORE_FILES by libsmb2 (libsmb2.c:3785-3788) and surfaces as a
    failure, measured with smbclient as `cd emptydir; ls` ->
    "NT_STATUS_NO_SUCH_FILE listing \\emptydir\\*". Emitting the two dots is how
    every real server distinguishes "present, and empty" from "gone".

    So both round trips are load-bearing and the count is asserted: trip 1
    carries '.' and '..', trip 2 is a CONTINUATION (not a restart, hence no
    dots) and is what legitimately terminates the enumeration. One round trip
    would mean the dots were never sent; three would mean they were re-sent on
    the continuation, and an enumeration that keeps producing entries never
    terminates."""
    rel = "smoke_test_empty"
    _seed_tree(rel, {})
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel)
        entries, trips = _drain_listing(d)
        d.close()
        got = _entry_names(entries)
        if not _dots_present(got, "empty directory"):
            return False
        if _real_only(got):
            print(f"FAIL (empty directory): listing returned {sorted(_real_only(got))!r} besides "
                  f"'.' and '..' in a directory with nothing in it")
            return False
        if trips != 2:
            print(f"FAIL (empty directory): {len(entries)} entries in {trips} round trips, expected "
                  f"'.' and '..' in the first and NO_MORE_FILES in the second")
            return False
        print("PASS (empty directory): exactly '.' and '..' on the first query, then NO_MORE_FILES -- "
              "'present but empty', which is what a client can actually tell apart from an error")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (empty directory): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_listing_restart_scans(port: int) -> bool:
    """SMB2_RESTART_SCANS must rewind an already-drained handle and hand back
    the whole directory again.

    Both passes carry '.' and '..', and that is the point rather than noise:
    queryDirectoryCmd computes `restart` as
    `(flags & (SMB2_RESTART_SCANS | SMB2_REOPEN)) != 0 || !slot->enumStarted`,
    so the first query on a fresh handle and an explicit RESTART_SCANS take the
    identical path. The two passes therefore have to be byte-for-byte the same
    set -- an implementation that emitted the dots only for one of the two
    reasons would break this equality."""
    rel = "smoke_test_restart"
    _seed_tree(rel, {"one.txt": 1, "two.txt": 2, "three.txt": 3})
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel)
        first, _ = _drain_listing(d)
        # Handle is now exhausted: without the rewind this would raise
        # NoMoreFiles immediately and `second` would be empty.
        second, _ = _drain_listing(d, flags=QueryDirectoryFlags.SMB2_RESTART_SCANS)
        d.close()
        got = _entry_names(second)
        if _entry_names(first) != got:
            print(f"FAIL (RESTART_SCANS): first pass {sorted(_entry_names(first))!r}, "
                  f"restarted pass {sorted(got)!r}")
            return False
        if not _dots_present(got, "RESTART_SCANS"):
            return False
        if len(_real_only(got)) != 3 or len(second) != 5:
            print(f"FAIL (RESTART_SCANS): restarted pass returned {len(second)} entries "
                  f"({sorted(got)!r}), expected 3 files plus '.' and '..'")
            return False
        print("PASS (RESTART_SCANS): exhausted handle rewound and re-enumerated all 3 entries "
              "plus '.' and '..', identically to the first pass")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (RESTART_SCANS): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_listing_pattern_filter(port: int) -> bool:
    """A non-'*' search pattern must actually filter. macOS uses a specific
    name here as an existence probe; answering it with the whole folder would
    be wrong (and, for a big library, expensive).

    Since v65 these EXACT set comparisons carry a second job, which is why they
    are deliberately not relaxed to allow the dot entries: '.' and '..' go
    through the same wildcardMatch() as every other name, so neither 'b.epub'
    nor '*.epub' may return them. A client probing for one specific filename
    that was handed '.' instead would read the probe as a hit on the wrong
    object."""
    rel = "smoke_test_pattern"
    _seed_tree(rel, {"a.epub": 1, "b.epub": 2, "c.txt": 3})
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel)
        exact, _ = _drain_listing(d, pattern_first="b.epub", pattern_rest="b.epub")
        d.close()
        d2 = _open_dir(tree, rel)
        glob, _ = _drain_listing(d2, pattern_first="*.epub", pattern_rest="*.epub")
        d2.close()
        if set(_entry_names(exact)) != {"b.epub"}:
            print(f"FAIL (pattern filter): exact-name query returned {sorted(_entry_names(exact))!r}")
            return False
        if set(_entry_names(glob)) != {"a.epub", "b.epub"}:
            print(f"FAIL (pattern filter): '*.epub' returned {sorted(_entry_names(glob))!r}")
            return False
        print("PASS (pattern filter): exact name and '*.epub' both filtered correctly")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (pattern filter): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_listing_scan_bound(port: int) -> bool:
    """The scan bound must never cost a file, in either of the two shapes that
    exercise it.

    The handler bounds entries EXAMINED per request (hidden and non-matching
    ones cost an SD read each but fill no buffer), so that one network callback
    cannot walk an arbitrarily long run of skipped entries -- tick() is
    supposed to be non-blocking. Two cases, and they pull in opposite
    directions:

      * TRAILING JUNK: a few real files followed by a long run of hidden ones
        (the macOS "._" sidecar shape). The bound fires here, and the listing
        must still be complete -- the enumeration just continues on the next
        request.
      * NOTHING MATCHED YET: a narrow pattern whose only match sits after a
        long run of non-matching entries. Here the bound must NOT fire, because
        stopping with zero accepted entries is indistinguishable on the wire
        from "end of directory" (libsmb2 turns an empty reply into
        STATUS_NO_MORE_FILES) -- the client would be told the file does not
        exist. This is the case that makes the bound's `count > 0` condition a
        correctness requirement rather than caution, so it is tested
        explicitly."""
    ok = True
    rel_trailing = "smoke_test_scan_trailing"
    real = {f"real_{i:02d}.epub": i + 1 for i in range(5)}
    _seed_tree(rel_trailing, real)
    # 300 hidden entries after the real ones: more than the 128-entry scan
    # bound, so a single request cannot wade through them all.
    _seed_tree(rel_trailing, {f"._hidden_{i:03d}": 1 for i in range(300)})

    rel_needle = "smoke_test_scan_needle"
    _seed_tree(rel_needle, {f"chaff_{i:03d}.txt": 1 for i in range(300)})
    _seed_tree(rel_needle, {"needle.epub": 42})

    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel_trailing)
        entries, trips = _drain_listing(d)
        d.close()
        got = _entry_names(entries)
        expected = {n: (s, False) for n, s in real.items()}
        if not _dots_present(got, "scan bound, trailing junk"):
            ok = False
        elif _real_only(got) != expected:
            print(f"FAIL (scan bound, trailing junk): got {sorted(_real_only(got))!r}, expected "
                  f"{sorted(expected)!r} -- the scan bound dropped or duplicated a real file")
            ok = False

        d2 = _open_dir(tree, rel_needle)
        found, needle_trips = _drain_listing(d2, pattern_first="needle.epub", pattern_rest="needle.epub")
        d2.close()
        # No dot entries here, and that is asserted rather than tolerated: the
        # pattern is a literal filename and v65 runs '.' and '..' through the
        # same wildcardMatch() as everything else.
        #
        # That is also why this half still tests what it was written to test.
        # The scan bound is `count > 0 && scanned >= kMaxDirEntriesScannedPerResponse`
        # (SmbFileHandlers.cpp:1908), and a dot the pattern rejects is skipped by
        # a `continue` that increments NEITHER counter -- so `count` is still 0
        # when the scan loop starts, and the "nothing matched yet" case is as
        # unbounded as it was before v65. If a future change made the dots slip
        # past the pattern filter, `count` would be 2 here, the bound would start
        # firing on a search that has matched nothing, and the shape this check
        # exists to forbid would be back.
        if set(_entry_names(found)) != {"needle.epub"}:
            print(f"FAIL (scan bound, needle after 300 non-matching): got {sorted(_entry_names(found))!r}, "
                  f"expected ['needle.epub'] -- the scan bound fired with nothing accepted, so the "
                  f"empty reply was read as STATUS_NO_MORE_FILES and the file was reported missing")
            ok = False

        if ok:
            print(f"PASS (scan bound): 5 real files (plus '.' and '..') behind 300 hidden ones listed "
                  f"completely in {trips} round trips, and a needle after 300 non-matching entries "
                  f"still found in {needle_trips} with no dot entries in the filtered reply")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (scan bound): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_listing_next_entry_offset_chain(port: int) -> bool:
    """Wire-level guard on the one invariant that makes this listing readable
    at all, checked here rather than only via decoded names so that the reason
    is visible when it breaks.

    THIS GUARDS AN APPLIED FIX. libsmb2's reply encoder used to write an
    ABSOLUTE next_entry_offset where MS-FSCC 2.4.8 requires a RELATIVE one
    (`smb2_set_uint32(iov, offset + 0, offset + fs_size)`). The two agree only
    for an entry at offset 0, and the final entry's field is hard-zeroed by the
    same encoder, so entry 3 onward was silently skipped by any conformant
    client. **That expression is now patched in the vendored tree** -- see
    docs/third-party/libsmb2-vendoring.md, "The second patch", and
    scripts/verify_libsmb2_patch.py, which pins it. The handler therefore packs
    entries up to its own memory budget again (kMaxDirEntriesPerResponse = 32,
    a memory choice, not a correctness one).

    This asserts that a single raw response's chain, walked the way every real
    client walks it (`p += next_entry_offset`), visits every entry and lands
    exactly on the end of the declared buffer. Revert the vendored patch and
    this fails immediately, instead of quietly dropping a book from someone's
    library."""
    rel = "smoke_test_chain"
    names = {f"chain_{i}.epub": i + 1 for i in range(6)}
    _seed_tree(rel, names)
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel)
        query = SMB2QueryDirectoryRequest()
        query["file_information_class"] = FileInformationClass.FILE_ID_BOTH_DIRECTORY_INFORMATION
        query["flags"] = None
        query["file_index"] = 0
        query["file_id"] = d.file_id
        query["output_buffer_length"] = 65536
        query["buffer"] = "*".encode("utf-16-le")
        request = connection.send(query, session.session_id, tree.tree_connect_id)
        response = connection.receive(request)
        parsed = SMB2QueryDirectoryResponse()
        parsed.unpack(response["data"].get_value())
        buf = parsed["buffer"].get_value()
        d.close()

        offset = 0
        seen = 0
        while True:
            if offset + 104 > len(buf):
                print(f"FAIL (next_entry_offset chain): walked to offset {offset} but the buffer is only "
                      f"{len(buf)} bytes -- next_entry_offset is being written as an absolute offset")
                return False
            next_off = struct.unpack("<I", buf[offset:offset + 4])[0]
            name_len = struct.unpack("<I", buf[offset + 60:offset + 64])[0]
            entry_end = offset + 104 + name_len
            if entry_end > len(buf):
                print(f"FAIL (next_entry_offset chain): entry at {offset} claims a {name_len}-byte name "
                      f"that runs past the {len(buf)}-byte buffer")
                return False
            seen += 1
            if next_off == 0:
                if (len(buf) - entry_end) >= 4:
                    print(f"FAIL (next_entry_offset chain): chain ended at {entry_end} but the buffer is "
                          f"{len(buf)} bytes -- {len(buf) - entry_end} bytes of entries were skipped")
                    return False
                break
            if next_off < 104:
                print(f"FAIL (next_entry_offset chain): entry at {offset} declares next_entry_offset "
                      f"{next_off}, smaller than one fixed-size entry")
                return False
            offset += next_off
        print(f"PASS (next_entry_offset chain): relative walk visited all {seen} entries in the response "
              f"and ended exactly at the {len(buf)}-byte buffer end")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (next_entry_offset chain): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_listing_unsupported_class_rejected(port: int) -> bool:
    """The two directory info classes libsmb2's reply encoder cannot produce
    (FILE_DIRECTORY_INFORMATION 0x01 and FILE_FULL_DIRECTORY_INFORMATION 0x02 --
    its switch only handles 0x25/0x26 and yields a zero-length body for
    anything else) must come back as an ERROR, not as a successful reply
    describing an empty folder. "This folder is empty" is the worst possible
    answer: it is indistinguishable from the truth. See queryDirectoryCmd's
    header comment for why supporting them is not available without either
    editing the machine-checked vendored tree or turning on context-wide
    passthrough."""
    rel = "smoke_test_listing"
    _seed_tree(rel, {"alpha.txt": 11})
    ok = True
    connection, session, tree = connect_session(port)
    try:
        for label, info_class in (
            ("FILE_DIRECTORY_INFORMATION", FileInformationClass.FILE_DIRECTORY_INFORMATION),
            ("FILE_FULL_DIRECTORY_INFORMATION", FileInformationClass.FILE_FULL_DIRECTORY_INFORMATION),
        ):
            d = _open_dir(tree, rel)
            try:
                got = d.query_directory("*", info_class)
                print(f"FAIL (unsupported dir class {label}): returned {len(got)} entries instead of an "
                      f"error -- a zero-length success here reads as 'this folder is empty'")
                ok = False
            except NoMoreFiles:
                print(f"FAIL (unsupported dir class {label}): answered NO_MORE_FILES, which a client "
                      f"cannot tell apart from a genuinely empty directory")
                ok = False
            except Exception as e:  # noqa: BLE001
                print(f"PASS (unsupported dir class {label} rejected): {type(e).__name__}")
            finally:
                d.close()
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (unsupported dir class): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def _query_info(tree, handle, info_type: int, info_class: int, output_buffer_length: int = 4096) -> bytes:
    """Sends a raw SMB2 QUERY_INFO and returns the response's output buffer.
    smbprotocol's Open has no query_info() wrapper, so the request is built
    directly -- which is also what lets the filesystem info classes (not part
    of its FileInformationClass enum) be exercised at all."""
    req = SMB2QueryInfoRequest()
    req["info_type"] = info_type
    req["file_info_class"] = info_class
    req["output_buffer_length"] = output_buffer_length
    req["file_id"] = handle.file_id
    request = tree.session.connection.send(req, tree.session.session_id, tree.tree_connect_id)
    response = tree.session.connection.receive(request)
    query_response = SMB2QueryInfoResponse()
    query_response.unpack(response["data"].get_value())
    return query_response["buffer"].get_value()


def test_query_info_file_classes(port: int) -> bool:
    """The four SMB2_0_INFO_FILE classes the Files app asks for, against both a
    file of known size and a directory. Decoded field by field rather than
    "did it not raise": a reply of the right length carrying the wrong
    attributes or a zero size is exactly what makes a client show an empty or
    unopenable file."""
    rel = "smoke_test_info"
    _seed_tree(rel, {"sized.bin": 4321})
    ok = True
    connection, session, tree = connect_session(port)
    try:
        f = _create_file(tree, f"{rel}\\sized.bin", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.GENERIC_READ, CreateOptions.FILE_NON_DIRECTORY_FILE)
        d = _open_dir(tree, rel)

        basic = _query_info(tree, f, InfoType.SMB2_0_INFO_FILE, FileInformationClass.FILE_BASIC_INFORMATION)
        attrs = struct.unpack("<I", basic[32:36])[0]
        if len(basic) != 40 or attrs & FileAttributes.FILE_ATTRIBUTE_DIRECTORY:
            print(f"FAIL (query_info BASIC on file): {len(basic)} bytes, attributes 0x{attrs:08x}")
            ok = False

        std = _query_info(tree, f, InfoType.SMB2_0_INFO_FILE, FileInformationClass.FILE_STANDARD_INFORMATION)
        # "<QQIBB" == MS-FSCC 2.4.41 FileStandardInformation: AllocationSize 0-7,
        # EndOfFile 8-15, NumberOfLinks 16-19, DeletePending 20, Directory 21.
        # Re-verified against the wire while fixing the FILE_ALL_INFORMATION
        # offset below (this one was already right): a 4321-byte file answers
        # (4321, 4321, 1, 0, 0) and its parent directory (0, 0, 1, 0, 1).
        alloc, eof, links, delete_pending, is_dir = struct.unpack("<QQIBB", std[:22])
        if (eof, links, delete_pending, is_dir) != (4321, 1, 0, 0) or alloc < eof:
            print(f"FAIL (query_info STANDARD on file): alloc={alloc} eof={eof} links={links} "
                  f"delete_pending={delete_pending} dir={is_dir} (expected eof=4321, links=1, dir=0)")
            ok = False

        nopen = _query_info(tree, f, InfoType.SMB2_0_INFO_FILE,
                            FileInformationClass.FILE_NETWORK_OPEN_INFORMATION)
        n_alloc, n_eof, n_attrs = struct.unpack("<QQI", nopen[32:52])
        if len(nopen) != 56 or n_eof != 4321 or n_alloc < n_eof or (n_attrs & FileAttributes.FILE_ATTRIBUTE_DIRECTORY):
            print(f"FAIL (query_info NETWORK_OPEN): {len(nopen)} bytes, alloc={n_alloc} eof={n_eof} "
                  f"attrs=0x{n_attrs:08x}")
            ok = False

        allinfo = _query_info(tree, f, InfoType.SMB2_0_INFO_FILE, FileInformationClass.FILE_ALL_INFORMATION)
        # MS-FSCC 2.4.2 FileAllInformation, block by block: Basic 0-39
        # (FileAttributes 32-35), Standard 40-63 (EndOfFile 48-55,
        # DeletePending 60, Directory 61), Internal 64-71 (IndexNumber),
        # Ea 72-75, Access 76-79, Position 80-87, Mode 88-91, Alignment 92-95,
        # Name 96+ (FileNameLength 96-99, then the UTF-16LE name).
        #
        # The Directory flag read used to be `allinfo[69]`, which is wrong and
        # was wrong from the day it was written -- byte 69 sits in the middle of
        # InternalInformation's IndexNumber. It passed for four tasks only
        # because v64 sent IndexNumber = 0, so bytes 64-71 were all zero and 69
        # happened to agree with 61. v65 sends a real hash there, byte 69 became
        # 0x51, and the latent bug surfaced. Verified on the wire: for this file
        # byte 61 is 0 and byte 69 is 38; for the directory below, byte 61 is 1
        # and byte 69 is 233.
        a_attrs = struct.unpack("<I", allinfo[32:36])[0]
        a_eof = struct.unpack("<Q", allinfo[48:56])[0]
        a_isdir = allinfo[61]
        a_index = struct.unpack("<Q", allinfo[64:72])[0]
        a_namelen = struct.unpack("<I", allinfo[96:100])[0]
        a_name = allinfo[100:100 + a_namelen].decode("utf-16-le")
        if a_eof != 4321 or a_isdir != 0 or (a_attrs & FileAttributes.FILE_ATTRIBUTE_DIRECTORY):
            print(f"FAIL (query_info ALL on file): eof={a_eof} dir={a_isdir} attrs=0x{a_attrs:08x}")
            ok = False
        # v65 property, worth pinning on its own: IndexNumber must be NON-ZERO.
        # v64 sent 0 -- defensible in the abstract, since FAT has no per-file id
        # and MS-FSCC permits 0 for such volumes, but measured to be expensive:
        # the Linux kernel client logs "Autodisabling the use of server inode
        # numbers", re-asks for the directory with FileFullDirectoryInformation
        # (0x02, which this server cannot encode) and `ls` then fails outright
        # with "Operation not supported". So a zero here is not a cosmetic
        # regression, it costs the whole listing.
        if a_index == 0:
            print("FAIL (query_info ALL on file): IndexNumber is 0 -- the Linux kernel client reacts "
                  "by disabling server inode numbers and then falling back to a directory info class "
                  "this server cannot encode, which fails the listing entirely")
            ok = False
        if a_name != rf"\{rel}\sized.bin":
            print(f"FAIL (query_info ALL on file): FileNameInformation is {a_name!r}, expected "
                  f"{chr(92) + rel + chr(92)}sized.bin (share-relative, backslash-separated)")
            ok = False

        d_std = _query_info(tree, d, InfoType.SMB2_0_INFO_FILE,
                            FileInformationClass.FILE_STANDARD_INFORMATION)
        if d_std[21] != 1:
            print(f"FAIL (query_info STANDARD on directory): directory byte is {d_std[21]}, expected 1")
            ok = False
        d_basic = _query_info(tree, d, InfoType.SMB2_0_INFO_FILE, FileInformationClass.FILE_BASIC_INFORMATION)
        if not struct.unpack("<I", d_basic[32:36])[0] & FileAttributes.FILE_ATTRIBUTE_DIRECTORY:
            print("FAIL (query_info BASIC on directory): FILE_ATTRIBUTE_DIRECTORY not set")
            ok = False

        f.close()
        d.close()
        if ok:
            print(f"PASS (query_info file classes): BASIC / STANDARD / NETWORK_OPEN / ALL correct for "
                  f"a 4321-byte file and for a directory, ALL's Directory flag read at its real offset "
                  f"(61) and IndexNumber non-zero (0x{a_index:016x})")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (query_info file classes): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


# --------------------------------------------------------------------------
# Task 7: timestamps.
#
# FAT's modify time has TWO-SECOND resolution (the seconds field stores
# seconds/2), so every comparison below allows 2 s -- and expects the value to
# be rounded DOWN, never up: a file must not report having been modified after
# it was.
FAT_RESOLUTION_SECONDS = 2

# 2024-05-17T09:30:00Z. A fixed instant, not "now": a test whose expected value
# is computed the same way the code under test computes it proves only that the
# code agrees with itself. This number was cross-checked against Python's own
# calendar.timegm, independently of the C++ conversion.
KNOWN_MTIME = 1715938200

# FILETIME is 100 ns ticks since 1601-01-01; the Unix epoch is 11644473600 s
# later, i.e. 116444736000000000 ticks. Decoded from raw bytes here rather than
# via smbprotocol's DateTimeField so that the assertion is about what is
# actually on the wire, not about the client library's interpretation of it.
FILETIME_EPOCH_DELTA = 116444736000000000


def _filetime_to_unix(raw8: bytes):
    """8 wire bytes -> seconds since the Unix epoch, or None for 'no time
    information' (MS-FSCC 2.4.7's literal 0)."""
    value = struct.unpack("<Q", raw8)[0]
    if value == 0:
        return None
    return (value - FILETIME_EPOCH_DELTA) / 10_000_000.0


def _check_timestamp(label: str, got, expected_unix: float) -> bool:
    """One timestamp assertion. `got` is None when the server reported no time
    at all, which is a distinct failure worth naming: it is what the pre-Task-7
    build did for every file."""
    if got is None:
        print(f"FAIL ({label}): reported NO timestamp (0), expected {expected_unix}")
        return False
    delta = got - expected_unix
    if -FAT_RESOLUTION_SECONDS < delta <= 0:
        return True
    print(f"FAIL ({label}): {got} vs expected {expected_unix} (delta {delta:+.1f}s; FAT's 2-second "
          f"resolution must truncate DOWN, so -2 < delta <= 0)")
    return False


def test_fat_timestamp_unit_test(port: int) -> bool:
    """Runs test/host/fat_time_test -- the FAT-date conversion's own unit test,
    which checks leap-year and century rules and both ends of FAT's
    representable range against hand-computed constants. Run from here so one
    command covers the whole suite; see fat_time_test.cpp for why the wire
    checks below are not sufficient on their own (every wrong date still looks
    like a date)."""
    import subprocess

    binary = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fat_time_test")
    if not os.path.exists(binary):
        print("FAIL (FatTimestamp unit test): fat_time_test not built -- run `make` in test/host/")
        return False
    proc = subprocess.run([binary], capture_output=True, text=True)
    if proc.returncode != 0:
        print("FAIL (FatTimestamp unit test):")
        print(proc.stdout)
        return False
    checks = proc.stdout.count("PASS (")
    print(f"PASS (FatTimestamp unit test): {checks} direct conversion checks")
    return True


def test_timestamps_query_info(port: int) -> bool:
    """A file whose modification time is set on disk to a known instant must
    come back as that instant through every query_info class that carries
    timestamps -- decoded as a real FILETIME, not just 'non-zero'.

    All four MS-FSCC fields are checked, because FAT has one time and this
    server reports it for all four (see modifyTimeOf()); a client that sorts by
    creation date rather than modification date has to get an answer too."""
    rel = "smoke_test_times"
    _seed_tree(rel, {"dated.bin": 128})
    host_path = os.path.join(SD_ROOT, rel, "dated.bin")
    os.utime(host_path, (KNOWN_MTIME, KNOWN_MTIME))
    expected = float(KNOWN_MTIME)

    ok = True
    connection, session, tree = connect_session(port)
    try:
        f = _create_file(tree, f"{rel}\\dated.bin", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.GENERIC_READ, CreateOptions.FILE_NON_DIRECTORY_FILE)

        # FILE_BASIC_INFORMATION: creation / access / write / change at 0/8/16/24.
        basic = _query_info(tree, f, InfoType.SMB2_0_INFO_FILE, FileInformationClass.FILE_BASIC_INFORMATION)
        for name, off in (("creation", 0), ("access", 8), ("write", 16), ("change", 24)):
            ok = _check_timestamp(f"BASIC {name}_time", _filetime_to_unix(basic[off:off + 8]), expected) and ok

        # FILE_NETWORK_OPEN_INFORMATION: same four, same offsets.
        nopen = _query_info(tree, f, InfoType.SMB2_0_INFO_FILE,
                            FileInformationClass.FILE_NETWORK_OPEN_INFORMATION)
        for name, off in (("creation", 0), ("access", 8), ("write", 16), ("change", 24)):
            ok = _check_timestamp(f"NETWORK_OPEN {name}_time", _filetime_to_unix(nopen[off:off + 8]),
                                  expected) and ok

        # FILE_ALL_INFORMATION embeds FILE_BASIC at offset 0.
        allinfo = _query_info(tree, f, InfoType.SMB2_0_INFO_FILE, FileInformationClass.FILE_ALL_INFORMATION)
        ok = _check_timestamp("ALL basic.write_time", _filetime_to_unix(allinfo[16:24]), expected) and ok

        f.close()
        if ok:
            print(f"PASS (query_info timestamps): BASIC/NETWORK_OPEN/ALL all report {KNOWN_MTIME} "
                  f"(2024-05-17T09:30:00Z) as a correct FILETIME")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (query_info timestamps): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_timestamps_directory_listing(port: int) -> bool:
    """Three files with three DIFFERENT modification times, listed in one go.

    Distinct times on purpose: a listing where every entry carries the same
    timestamp is exactly the pre-Task-7 behaviour, and a single-file test would
    pass against an implementation that read one entry's date and reused it."""
    rel = "smoke_test_times_dir"
    _seed_tree(rel, {"old.epub": 10, "mid.epub": 20, "new.epub": 30})
    wanted = {
        "old.epub": KNOWN_MTIME - 86400 * 365,
        "mid.epub": KNOWN_MTIME,
        "new.epub": KNOWN_MTIME + 86400 * 30,
    }
    for name, when in wanted.items():
        os.utime(os.path.join(SD_ROOT, rel, name), (when, when))

    ok = True
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, rel)
        entries, _ = _drain_listing(d)
        d.close()

        seen = {}
        for e in entries:
            name = e["file_name"].get_value().decode("utf-16-le")
            # pack() gives the raw 8 wire bytes, so this checks the FILETIME
            # the server actually sent rather than smbprotocol's datetime view.
            seen[name] = e["last_write_time"].pack()

        for name, when in wanted.items():
            if name not in seen:
                print(f"FAIL (listing timestamps): {name} missing from the listing")
                ok = False
                continue
            ok = _check_timestamp(f"listing {name} last_write_time", _filetime_to_unix(seen[name]),
                                  float(when)) and ok

        if ok:
            print("PASS (listing timestamps): three entries, three distinct correct dates")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (listing timestamps): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_timestamp_of_freshly_written_file(port: int) -> bool:
    """A file created and written THROUGH THIS SERVER must report a real,
    recent timestamp -- not zero.

    This is the case the feature exists for: iOS copies a book over, then shows
    the folder. A zero here is what makes every file in the Files app share one
    date. Checked against wall-clock 'now' rather than a fixed instant because
    nothing else knows when the write happened."""
    rel = "smoke_test_times_fresh"
    _seed_tree(rel, {})
    ok = True
    connection, session, tree = connect_session(port)
    try:
        before = time.time()
        f = _create_file(tree, f"{rel}\\just_written.bin", CreateDisposition.FILE_OVERWRITE_IF,
                         FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
                         CreateOptions.FILE_NON_DIRECTORY_FILE)
        f.write(b"fresh bytes", 0)
        f.close()

        g = _create_file(tree, f"{rel}\\just_written.bin", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.GENERIC_READ, CreateOptions.FILE_NON_DIRECTORY_FILE)
        basic = _query_info(tree, g, InfoType.SMB2_0_INFO_FILE, FileInformationClass.FILE_BASIC_INFORMATION)
        g.close()
        after = time.time()

        got = _filetime_to_unix(basic[16:24])
        if got is None:
            print("FAIL (fresh file timestamp): reported NO timestamp (0) for a file it just wrote")
            ok = False
        elif not (before - FAT_RESOLUTION_SECONDS <= got <= after + FAT_RESOLUTION_SECONDS):
            print(f"FAIL (fresh file timestamp): {got} is outside the write window "
                  f"[{before:.1f}, {after:.1f}] (+/- FAT's 2 s)")
            ok = False
        else:
            print(f"PASS (fresh file timestamp): non-zero and inside the write window ({got:.0f})")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (fresh file timestamp): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_share_root_reports_no_timestamp(port: int) -> bool:
    """The share root must report 0, not a date.

    Not a nicety: on device the FAT root directory has no directory entry, and
    asking SdFat for one returns TRUE holding a decode of SECTOR 0 -- the boot
    sector read as though it were a directory entry (FatFile::openRoot() leaves
    m_dirSector = 0; see HalFile::getModifyDateTime()'s header comment). So the
    only correct answer is 'none', and this check is what holds the handler's
    skip-the-root guard in place. The harness's stub deliberately DOES answer
    for the root (with the host directory's own mtime -- same shape as the
    device's garbage: 'succeeds, means nothing'), so deleting the guard makes
    this check fail rather than silently pass."""
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, "")
        basic = _query_info(tree, d, InfoType.SMB2_0_INFO_FILE, FileInformationClass.FILE_BASIC_INFORMATION)
        d.close()
        got = _filetime_to_unix(basic[16:24])
        if got is not None:
            print(f"FAIL (share root timestamp): reported {got}, expected 'no time information' (0) -- "
                  f"the root has no directory entry to read one from")
            return False
        print("PASS (share root timestamp): reported as 'no time information', not a decode of sector 0")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (share root timestamp): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


# --------------------------------------------------------------------------
# Task 7: set_info (rename / delete-on-close / set size).
#
# EVERY ONE OF THESE WAS UNREACHABLE before the vendored-library patch (see
# docs/third-party/libsmb2-vendoring.md, "The third patch"): upstream refused
# the payload from a DECODE function, so a set_info request did not fail, it
# killed the CONNECTION. Several assertions below therefore check that the
# session is still usable afterwards -- not decoration, that was the actual
# symptom.


def _set_info(tree, handle, info_class: int, buffer: bytes, info_type: int = InfoType.SMB2_0_INFO_FILE):
    """Sends one raw SET_INFO. Built by hand rather than via smbprotocol's
    typed helpers so the test controls the exact bytes on the wire -- the
    handler decodes raw MS-FSCC structures, so that is the thing under test."""
    req = SMB2SetInfoRequest()
    req["info_type"] = info_type
    req["file_info_class"] = info_class
    req["buffer"] = buffer
    req["file_id"] = handle.file_id
    request = tree.session.connection.send(req, tree.session.session_id, tree.tree_connect_id)
    return tree.session.connection.receive(request)


def _rename_buffer(new_name: str, replace: bool = False, root_directory: int = 0) -> bytes:
    """MS-FSCC 2.4.34.2 FileRenameInformation for SMB2."""
    name = new_name.encode("utf-16-le")
    return (
        struct.pack("<B7x", 1 if replace else 0)
        + struct.pack("<Q", root_directory)
        + struct.pack("<I", len(name))
        + name
    )


def _try_rename(tree, handle, new_name: str, replace: bool = False) -> str | None:
    """Returns None on success, or the exception's type name on rejection."""
    try:
        _set_info(tree, handle, FileInformationClass.FILE_RENAME_INFORMATION, _rename_buffer(new_name, replace))
        return None
    except Exception as e:  # noqa: BLE001
        return type(e).__name__


def _open_rw(tree, name: str, disposition=CreateDisposition.FILE_OPEN):
    """A handle with the access a client uses for rename/delete/set-size."""
    return _create_file(
        tree,
        name,
        disposition,
        FilePipePrinterAccessMask.GENERIC_READ
        | FilePipePrinterAccessMask.GENERIC_WRITE
        | FilePipePrinterAccessMask.DELETE,
        CreateOptions.FILE_NON_DIRECTORY_FILE,
    )


def _host(*parts: str) -> str:
    return os.path.join(SD_ROOT, *parts)


def test_rename_file_and_directory(port: int) -> bool:
    """The four ordinary shapes: a file, a directory, a move across
    directories, and Traditional Chinese on BOTH sides.

    Every case checks the FILESYSTEM afterwards, not just that the call did not
    raise -- a rename that reports success and leaves the old name in place is
    the failure worth catching."""
    rel = "smoke_test_rename"
    _seed_tree(rel, {"before.epub": 11}, subdirs=("olddir", "target"))
    _seed_tree(f"{rel}/移動來源", {"範例書坊-舊檔名.txt": 22})
    ok = True
    connection, session, tree = connect_session(port)
    try:
        # 1. a plain file
        f = _open_rw(tree, f"{rel}\\before.epub")
        err = _try_rename(tree, f, f"{rel}\\after.epub")
        f.close()
        if err is not None:
            print(f"FAIL (rename file): rejected with {err}")
            ok = False
        elif os.path.exists(_host(rel, "before.epub")) or not os.path.exists(_host(rel, "after.epub")):
            print("FAIL (rename file): the filesystem still shows the old name")
            ok = False
        else:
            print("PASS (rename file): before.epub -> after.epub, old name gone")

        # 2. a directory
        d = _open_dir(tree, f"{rel}\\olddir")
        err = _try_rename(tree, d, f"{rel}\\newdir")
        d.close()
        if err is not None:
            print(f"FAIL (rename directory): rejected with {err}")
            ok = False
        elif os.path.isdir(_host(rel, "olddir")) or not os.path.isdir(_host(rel, "newdir")):
            print("FAIL (rename directory): the filesystem still shows the old name")
            ok = False
        else:
            print("PASS (rename directory): olddir -> newdir")

        # 3. across directories, with the file's bytes verified after the move
        with open(_host(rel, "after.epub"), "rb") as fh:
            content_before = fh.read()
        f2 = _open_rw(tree, f"{rel}\\after.epub")
        err = _try_rename(tree, f2, f"{rel}\\target\\moved.epub")
        f2.close()
        if err is not None:
            print(f"FAIL (rename across directories): rejected with {err}")
            ok = False
        elif not os.path.exists(_host(rel, "target", "moved.epub")):
            print("FAIL (rename across directories): not at the destination")
            ok = False
        else:
            with open(_host(rel, "target", "moved.epub"), "rb") as fh:
                if fh.read() != content_before:
                    print("FAIL (rename across directories): contents changed")
                    ok = False
                else:
                    print("PASS (rename across directories): moved with contents intact")

        # 4. Traditional Chinese on BOTH sides -- this user's library is
        # entirely Chinese, so a UTF-16 decode bug here breaks everything.
        c = _open_rw(tree, f"{rel}\\移動來源\\範例書坊-舊檔名.txt")
        err = _try_rename(tree, c, f"{rel}\\移動來源\\範例書坊-新檔名【繁體】.txt")
        c.close()
        if err is not None:
            print(f"FAIL (rename CJK): rejected with {err}")
            ok = False
        elif not os.path.exists(_host(rel, "移動來源", "範例書坊-新檔名【繁體】.txt")):
            print("FAIL (rename CJK): destination not on disk (UTF-16 decode?)")
            ok = False
        else:
            print("PASS (rename CJK): 範例書坊-舊檔名.txt -> 範例書坊-新檔名【繁體】.txt")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (rename): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_rename_onto_existing(port: int) -> bool:
    """Renaming onto a name that already exists.

    ReplaceIfExists=0 must be refused with BOTH files intact. ReplaceIfExists=1
    must replace -- and this is the one non-atomic operation in the handler
    (SdFat's rename is create-exclusive, so the destination is removed first),
    so the bytes are checked, not just the names."""
    rel = "smoke_test_rename_exists"
    _seed_tree(rel, {})
    with open(_host(rel, "source.txt"), "wb") as fh:
        fh.write(b"SOURCE")
    with open(_host(rel, "victim.txt"), "wb") as fh:
        fh.write(b"VICTIM-MUST-SURVIVE")
    ok = True
    connection, session, tree = connect_session(port)
    try:
        f = _open_rw(tree, f"{rel}\\source.txt")
        err = _try_rename(tree, f, f"{rel}\\victim.txt", replace=False)
        f.close()
        if err is None:
            print("FAIL (rename onto existing, no replace): UNEXPECTEDLY SUCCEEDED")
            ok = False
        else:
            print(f"PASS (rename onto existing, no replace): rejected with {err}")
        with open(_host(rel, "victim.txt"), "rb") as fh:
            if fh.read() != b"VICTIM-MUST-SURVIVE":
                print("FAIL (rename onto existing, no replace): the destination was modified anyway")
                ok = False
        if not os.path.exists(_host(rel, "source.txt")):
            print("FAIL (rename onto existing, no replace): the source disappeared")
            ok = False

        # ReplaceIfExists=1
        f2 = _open_rw(tree, f"{rel}\\source.txt")
        err = _try_rename(tree, f2, f"{rel}\\victim.txt", replace=True)
        f2.close()
        if err is not None:
            print(f"FAIL (rename onto existing, replace): rejected with {err}")
            ok = False
        else:
            with open(_host(rel, "victim.txt"), "rb") as fh:
                landed = fh.read()
            if landed != b"SOURCE":
                print(f"FAIL (rename onto existing, replace): destination holds {landed!r}, expected b'SOURCE'")
                ok = False
            elif os.path.exists(_host(rel, "source.txt")):
                print("FAIL (rename onto existing, replace): the source name still exists")
                ok = False
            else:
                print("PASS (rename onto existing, replace): destination replaced byte-exact, source name gone")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (rename onto existing): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_rename_protected_both_directions(port: int) -> bool:
    """Protection applies to rename in BOTH directions.

    Renaming a file INTO /.crossmosa is the obvious half. Renaming one OUT of
    it is the half that matters more and is easy to forget: the Wi-Fi
    credentials become readable the moment they are called something else --
    every later read would be of an unprotected path and would be allowed.

    The out-of-protection direction cannot be driven through create() (which
    refuses to open a protected path at all), so it is driven the only way an
    attacker could: rename a handle that is legitimately open, then check that
    the protected file is untouched. The handler's source-side check is what is
    under test."""
    rel = "smoke_test_rename_protected"
    _seed_tree(rel, {"innocent.txt": 5})
    protected_dir = os.path.join(SD_ROOT, PROTECTED_DIR)
    os.makedirs(protected_dir, exist_ok=True)
    with open(os.path.join(protected_dir, "secrets.json"), "wb") as fh:
        fh.write(b"WIFI-PASSWORD")
    ok = True
    connection, session, tree = connect_session(port)
    try:
        # INTO a protected path
        f = _open_rw(tree, f"{rel}\\innocent.txt")
        err = _try_rename(tree, f, f"{PROTECTED_DIR}\\stolen.txt")
        f.close()
        if err is None:
            print("FAIL (rename INTO protected): UNEXPECTEDLY SUCCEEDED")
            ok = False
        elif os.path.exists(os.path.join(protected_dir, "stolen.txt")):
            print("FAIL (rename INTO protected): the file landed in the protected directory anyway")
            ok = False
        else:
            print(f"PASS (rename INTO protected): rejected with {err}, nothing landed")

        # OUT OF a protected path -- the open itself must already be refused,
        # and if that ever regresses the source-side check is the backstop.
        opened = None
        try:
            opened = _open_rw(tree, f"{PROTECTED_DIR}\\secrets.json")
        except Exception:  # noqa: BLE001
            print("PASS (rename OUT of protected): the protected path cannot even be opened")
        if opened is not None:
            err = _try_rename(tree, opened, f"{rel}\\exfiltrated.json")
            opened.close()
            if err is None:
                print("FAIL (rename OUT of protected): UNEXPECTEDLY SUCCEEDED -- credentials exfiltrated")
                ok = False
            else:
                print(f"PASS (rename OUT of protected): rejected with {err}")
        if os.path.exists(_host(rel, "exfiltrated.json")):
            print("FAIL (rename OUT of protected): the protected file was moved out")
            ok = False
        with open(os.path.join(protected_dir, "secrets.json"), "rb") as fh:
            if fh.read() != b"WIFI-PASSWORD":
                print("FAIL (rename OUT of protected): the protected file was modified")
                ok = False
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (rename protected): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_delete_on_close(port: int) -> bool:
    """FILE_DISPOSITION_INFORMATION: a file, an EMPTY directory, a NON-EMPTY
    directory (must fail, and must not take the contents with it), and a
    protected path.

    The non-empty case is the dangerous one: HalStorage also exposes a
    RECURSIVE removeDir(), and reaching for it here would silently destroy a
    whole book folder on a request that is supposed to fail."""
    rel = "smoke_test_delete"
    _seed_tree(rel, {"doomed.txt": 7}, subdirs=("emptydir",))
    _seed_tree(f"{rel}/fulldir", {"keepme.epub": 9})
    ok = True
    connection, session, tree = connect_session(port)
    try:
        # A file: still present while the handle is open, gone after close.
        f = _open_rw(tree, f"{rel}\\doomed.txt")
        _set_info(tree, f, FileInformationClass.FILE_DISPOSITION_INFORMATION, b"\x01")
        if not os.path.exists(_host(rel, "doomed.txt")):
            print("FAIL (delete-on-close file): deleted immediately, not at close")
            ok = False
        f.close()
        if os.path.exists(_host(rel, "doomed.txt")):
            print("FAIL (delete-on-close file): still on disk after close")
            ok = False
        else:
            print("PASS (delete-on-close file): present until close, gone after")

        # An empty directory.
        d = _open_dir(tree, f"{rel}\\emptydir")
        _set_info(tree, d, FileInformationClass.FILE_DISPOSITION_INFORMATION, b"\x01")
        d.close()
        if os.path.isdir(_host(rel, "emptydir")):
            print("FAIL (delete-on-close empty dir): still on disk")
            ok = False
        else:
            print("PASS (delete-on-close empty dir): removed")

        # A NON-empty directory: close must report failure and the contents
        # must survive.
        d2 = _open_dir(tree, f"{rel}\\fulldir")
        _set_info(tree, d2, FileInformationClass.FILE_DISPOSITION_INFORMATION, b"\x01")
        closed_ok = True
        try:
            d2.close()
        except Exception:  # noqa: BLE001
            closed_ok = False
        if closed_ok:
            print("FAIL (delete-on-close non-empty dir): close reported SUCCESS for a delete that cannot work")
            ok = False
        if not os.path.exists(_host(rel, "fulldir", "keepme.epub")):
            print("FAIL (delete-on-close non-empty dir): THE CONTENTS WERE DESTROYED (recursive remove?)")
            ok = False
        else:
            print("PASS (delete-on-close non-empty dir): refused, contents intact")

        # A protected path cannot be opened at all, so the disposition can
        # never be set on one; assert the opening step still refuses.
        try:
            p = _open_rw(tree, PROTECTED_SMB_PATH)
            p.close()
            print("FAIL (delete-on-close protected): a protected path was opened")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (delete-on-close protected): the protected path cannot be opened ({type(e).__name__})")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (delete-on-close): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_delete_on_close_where_close_fails(port: int) -> bool:
    """Delete-on-close where the CLOSE itself fails.

    Uses the SYNCFAIL injector (see README): the final write-back fails while
    the delete succeeds. The two results disagree, and the handler reports
    failure -- Task 6 spent two rounds establishing that close must not report
    unverified success, and a delete path is no place to reintroduce it. The
    slot must still be freed, which is checked by opening eight more handles
    afterwards."""
    rel = "smoke_test_delete_syncfail"
    _seed_tree(rel, {"SYNCFAIL_doomed.txt": 3})
    ok = True
    connection, session, tree = connect_session(port)
    try:
        f = _open_rw(tree, f"{rel}\\SYNCFAIL_doomed.txt")
        _set_info(tree, f, FileInformationClass.FILE_DISPOSITION_INFORMATION, b"\x01")
        reported_ok = True
        try:
            f.close()
        except Exception:  # noqa: BLE001
            reported_ok = False
        if reported_ok:
            print("FAIL (delete-on-close, close fails): close reported success despite the failed sync")
            ok = False
        else:
            print("PASS (delete-on-close, close fails): reported as a failure")
        # The delete itself still happened -- the flag is consumed after the
        # close regardless of the close's result.
        if os.path.exists(_host(rel, "SYNCFAIL_doomed.txt")):
            print("FAIL (delete-on-close, close fails): the file survived, so nothing was reported honestly")
            ok = False
        else:
            print("PASS (delete-on-close, close fails): the delete still happened")

        # The slot must not have leaked: eight more opens must all succeed.
        _seed_tree(rel, {f"slot_{i}.txt": 1 for i in range(8)})
        handles = []
        for i in range(8):
            handles.append(_open_rw(tree, f"{rel}\\slot_{i}.txt"))
        for h in handles:
            h.close()
        print("PASS (delete-on-close, close fails): the table still has all 8 slots")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (delete-on-close, close fails): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_set_end_of_file(port: int) -> bool:
    """FILE_END_OF_FILE_INFORMATION: smaller, exactly current, zero, and larger.

    THE BYTES ARE VERIFIED, not the reported length: a truncate that reports
    success and leaves the tail behind, or one that zeroes the wrong region, is
    the failure this exists to catch. The growth case is where the device and
    POSIX genuinely differ -- SdFat's truncate() cannot grow at all -- so the
    grown region is checked to be zeros and the original prefix to be intact."""
    rel = "smoke_test_eof"
    _seed_tree(rel, {})
    ok = True
    connection, session, tree = connect_session(port)
    try:
        payload = bytes(range(256)) * 8  # 2048 bytes, every value distinct-ish

        def fresh(name: str, data: bytes):
            with open(_host(rel, name), "wb") as fh:
                fh.write(data)
            return _open_rw(tree, f"{rel}\\{name}")

        # 1. smaller
        h = fresh("shrink.bin", payload)
        _set_info(tree, h, FileInformationClass.FILE_END_OF_FILE_INFORMATION, struct.pack("<Q", 700))
        h.close()
        with open(_host(rel, "shrink.bin"), "rb") as fh:
            got = fh.read()
        if got != payload[:700]:
            print(f"FAIL (set EOF smaller): {len(got)} bytes, contents do not match the first 700 of the original")
            ok = False
        else:
            print("PASS (set EOF smaller): 2048 -> 700, surviving bytes byte-exact")

        # 2. exactly the current size -- a legal no-op that must change nothing
        h = fresh("same.bin", payload)
        _set_info(tree, h, FileInformationClass.FILE_END_OF_FILE_INFORMATION, struct.pack("<Q", len(payload)))
        h.close()
        with open(_host(rel, "same.bin"), "rb") as fh:
            if fh.read() != payload:
                print("FAIL (set EOF same size): the file changed")
                ok = False
            else:
                print("PASS (set EOF same size): unchanged, byte-exact")

        # 3. zero
        h = fresh("zero.bin", payload)
        _set_info(tree, h, FileInformationClass.FILE_END_OF_FILE_INFORMATION, struct.pack("<Q", 0))
        h.close()
        if os.path.getsize(_host(rel, "zero.bin")) != 0:
            print(f"FAIL (set EOF zero): {os.path.getsize(_host(rel, 'zero.bin'))} bytes, expected 0")
            ok = False
        else:
            print("PASS (set EOF zero): truncated to 0")

        # 4. larger -- SdFat cannot grow via truncate, so this exercises the
        # handler's explicit zero-fill.
        h = fresh("grow.bin", payload)
        _set_info(tree, h, FileInformationClass.FILE_END_OF_FILE_INFORMATION, struct.pack("<Q", 5000))
        h.close()
        with open(_host(rel, "grow.bin"), "rb") as fh:
            got = fh.read()
        if len(got) != 5000:
            print(f"FAIL (set EOF larger): {len(got)} bytes, expected 5000")
            ok = False
        elif got[: len(payload)] != payload:
            print("FAIL (set EOF larger): the original bytes were disturbed")
            ok = False
        elif got[len(payload) :] != b"\x00" * (5000 - len(payload)):
            print("FAIL (set EOF larger): the grown region is not zeros")
            ok = False
        else:
            print("PASS (set EOF larger): 2048 -> 5000, prefix intact and the new region is zeros")

        # 5. absurdly larger -- must be refused, and must leave the file alone.
        h = fresh("huge.bin", payload)
        refused = False
        try:
            _set_info(tree, h, FileInformationClass.FILE_END_OF_FILE_INFORMATION, struct.pack("<Q", 8 * 1024 * 1024))
        except Exception:  # noqa: BLE001
            refused = True
        h.close()
        if not refused:
            print("FAIL (set EOF absurd): an 8 MB zero-fill was accepted inside a network callback")
            ok = False
        elif os.path.getsize(_host(rel, "huge.bin")) != len(payload):
            print("FAIL (set EOF absurd): the file was modified anyway")
            ok = False
        else:
            print("PASS (set EOF absurd): refused, file untouched")

        # 6. a read-only handle must not be able to resize anything.
        with open(_host(rel, "ro.bin"), "wb") as fh:
            fh.write(payload)
        r = _create_file(tree, f"{rel}\\ro.bin", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.GENERIC_READ, CreateOptions.FILE_NON_DIRECTORY_FILE)
        refused = False
        try:
            _set_info(tree, r, FileInformationClass.FILE_END_OF_FILE_INFORMATION, struct.pack("<Q", 10))
        except Exception:  # noqa: BLE001
            refused = True
        r.close()
        if not refused or os.path.getsize(_host(rel, "ro.bin")) != len(payload):
            print("FAIL (set EOF on a read-only handle): accepted or the file changed")
            ok = False
        else:
            print("PASS (set EOF on a read-only handle): refused, file untouched")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (set EOF): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_set_info_rejections_and_survival(port: int) -> bool:
    """The rejection paths, and the property the vendored patch exists for:
    A REJECTED set_info MUST NOT KILL THE SESSION.

    Before the patch every one of these tore the connection down. Each case
    below therefore does real work on the same session afterwards."""
    rel = "smoke_test_setinfo_reject"
    _seed_tree(rel, {"probe.txt": 4, "other.txt": 4})
    ok = True
    connection, session, tree = connect_session(port)
    try:
        f = _open_rw(tree, f"{rel}\\probe.txt")

        cases = [
            ("unsupported class (FILE_ALLOCATION_INFORMATION)", 0x13, struct.pack("<Q", 4096), InfoType.SMB2_0_INFO_FILE),
            ("unsupported info_type (FILESYSTEM)", 0x04, b"\x00" * 40, InfoType.SMB2_0_INFO_FILESYSTEM),
            ("truncated rename payload", FileInformationClass.FILE_RENAME_INFORMATION, b"\x00" * 8,
             InfoType.SMB2_0_INFO_FILE),
            ("rename with a non-zero RootDirectory", FileInformationClass.FILE_RENAME_INFORMATION,
             struct.pack("<B7x", 0) + struct.pack("<Q", 0x1234) + struct.pack("<I", 2) + "x".encode("utf-16-le"),
             InfoType.SMB2_0_INFO_FILE),
            ("rename with FileNameLength past the buffer", FileInformationClass.FILE_RENAME_INFORMATION,
             struct.pack("<B7x", 0) + struct.pack("<Q", 0) + struct.pack("<I", 4096) + b"ab",
             InfoType.SMB2_0_INFO_FILE),
            ("rename to a '..' traversal", FileInformationClass.FILE_RENAME_INFORMATION,
             _rename_buffer("..\\..\\escaped.txt"), InfoType.SMB2_0_INFO_FILE),
            ("zero-length payload", FileInformationClass.FILE_DISPOSITION_INFORMATION, b"",
             InfoType.SMB2_0_INFO_FILE),
        ]
        for label, info_class, buf, info_type in cases:
            try:
                _set_info(tree, f, info_class, buf, info_type)
                print(f"FAIL (set_info {label}): UNEXPECTEDLY SUCCEEDED")
                ok = False
            except Exception as e:  # noqa: BLE001
                print(f"PASS (set_info {label}): rejected with {type(e).__name__}")

        # THE POINT: the session survived all of that.
        g = _open_rw(tree, f"{rel}\\other.txt")
        g.write(b"alive", 0)
        g.close()
        f.close()
        if os.path.exists(_host(rel, "escaped.txt")) or os.path.exists(os.path.join(SD_ROOT, "..", "escaped.txt")):
            print("FAIL (set_info rejections): a '..' rename escaped the share")
            ok = False
        print("PASS (set_info rejections): the session was still fully usable after 7 rejected requests")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (set_info rejections): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_set_info_cross_connection_and_no_op_basic(port: int) -> bool:
    """Two properties that share a session pair.

    1. Connection B must not be able to rename or delete connection A's handle.
       File ids here are a small counter plus a slot index, so an id-only
       lookup would be trivially guessable -- this is the owner check.
    2. A FILE_BASIC_INFORMATION that asks for NOTHING (all timestamps 0 or
       all-ones, attributes 0) is accepted, because every field the client sent
       has been honoured; one that would really change something is refused,
       because setting FAT timestamps is not implemented and reporting success
       would be a lie."""
    rel = "smoke_test_setinfo_owner"
    _seed_tree(rel, {"a.txt": 6})
    ok = True
    ca, sa, ta = connect_session(port)
    cb, sb, tb = connect_session(port)
    try:
        victim = _open_rw(ta, f"{rel}\\a.txt")

        stolen = Open(tb, f"{rel}\\a.txt")
        stolen.file_id = victim.file_id  # forge B's handle to be A's
        for label, info_class, buf in (
            ("rename", FileInformationClass.FILE_RENAME_INFORMATION, _rename_buffer(f"{rel}\\hijacked.txt")),
            ("delete", FileInformationClass.FILE_DISPOSITION_INFORMATION, b"\x01"),
        ):
            try:
                _set_info(tb, stolen, info_class, buf)
                print(f"FAIL (cross-connection set_info {label}): B operated on A's handle")
                ok = False
            except Exception as e:  # noqa: BLE001
                print(f"PASS (cross-connection set_info {label}): rejected with {type(e).__name__}")
        if os.path.exists(_host(rel, "hijacked.txt")):
            print("FAIL (cross-connection set_info): the file was renamed anyway")
            ok = False

        # A no-op FILE_BASIC_INFORMATION: 4 timestamps + attributes + reserved.
        noop = struct.pack("<QQQQII", 0, 0, 0, 0, 0, 0)
        try:
            _set_info(ta, victim, FileInformationClass.FILE_BASIC_INFORMATION, noop)
            print("PASS (set_info BASIC no-op): accepted, because it asked for nothing")
        except Exception as e:  # noqa: BLE001
            print(f"FAIL (set_info BASIC no-op): rejected with {type(e).__name__}")
            ok = False

        allones = struct.pack("<QQQQII", 2**64 - 1, 2**64 - 1, 2**64 - 1, 2**64 - 1, 0, 0)
        try:
            _set_info(ta, victim, FileInformationClass.FILE_BASIC_INFORMATION, allones)
            print("PASS (set_info BASIC all-ones no-op): accepted (MS-FSCC 2.4.7 'do not change')")
        except Exception as e:  # noqa: BLE001
            print(f"FAIL (set_info BASIC all-ones no-op): rejected with {type(e).__name__}")
            ok = False

        victim.close()
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (set_info owner/basic): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tb, cb)
        _teardown(ta, ca)


def _basic_buffer(creation=0, access=0, write=0, change=0, attributes=0) -> bytes:
    """MS-FSCC 2.4.7 FileBasicInformation: four FILETIMEs, attributes, reserved."""
    return struct.pack("<QQQQII", creation, access, write, change, attributes, 0)


def _unix_to_filetime(seconds: int) -> int:
    return seconds * 10_000_000 + FILETIME_EPOCH_DELTA


def test_set_basic_information(port: int) -> bool:
    """FILE_BASIC_INFORMATION: timestamps are APPLIED, and the attribute check
    does not refuse the ordinary post-copy request.

    This is the class macOS and iOS send most -- having copied a file, they
    stamp it. Two things are under test, and neither was covered before:

    1. A real LastWriteTime must actually land on disk. Verified by reading it
       back through query_info AND off the filesystem, because "the call did
       not raise" is exactly the accept-and-drop behaviour that would be a lie.
    2. `FILE_ATTRIBUTE_ARCHIVE` (0x20) must not be refused. FAT sets ARCHIVE on
       every write and clients set it on a file they have just copied, so
       refusing it failed the whole request even when every timestamp said "do
       not change". The old harness only ever sent attributes = 0, so neither
       direction was tested.
    3. (v72) A date outside FAT's writable range is SKIPPED, not fatal. Step 6
       used to assert the opposite; see that step's own comment for why the
       refusal was the bug rather than the guard."""
    rel = "smoke_test_basic"
    _seed_tree(rel, {"stamped.txt": 12, "attrs.txt": 3, "dir_stamp": 0})
    os.makedirs(_host(rel, "stampdir"), exist_ok=True)
    ok = True
    connection, session, tree = connect_session(port)
    try:
        # 1. a real LastWriteTime, applied
        f = _open_rw(tree, f"{rel}\\stamped.txt")
        _set_info(tree, f, FileInformationClass.FILE_BASIC_INFORMATION,
                  _basic_buffer(write=_unix_to_filetime(KNOWN_MTIME)))
        basic = _query_info(tree, f, InfoType.SMB2_0_INFO_FILE, FileInformationClass.FILE_BASIC_INFORMATION)
        f.close()
        ok = _check_timestamp("BASIC set then read back", _filetime_to_unix(basic[16:24]), float(KNOWN_MTIME)) and ok
        on_disk = os.stat(_host(rel, "stamped.txt")).st_mtime
        if abs(on_disk - KNOWN_MTIME) > FAT_RESOLUTION_SECONDS:
            print(f"FAIL (BASIC set write time): the filesystem says {on_disk}, expected ~{KNOWN_MTIME}")
            ok = False
        else:
            print("PASS (BASIC set write time): applied, and visible on the filesystem")

        # 2. ChangeTime alone also drives the single FAT modify stamp.
        other = KNOWN_MTIME - 86400 * 10
        g = _open_rw(tree, f"{rel}\\stamped.txt")
        _set_info(tree, g, FileInformationClass.FILE_BASIC_INFORMATION,
                  _basic_buffer(change=_unix_to_filetime(other)))
        g.close()
        if abs(os.stat(_host(rel, "stamped.txt")).st_mtime - other) > FAT_RESOLUTION_SECONDS:
            print("FAIL (BASIC ChangeTime alone): did not reach FAT's single modify stamp")
            ok = False
        else:
            print("PASS (BASIC ChangeTime alone): folded onto the modify stamp")

        # 3. a directory can be stamped too (SdFat's isFileOrSubDir)
        d = _open_dir(tree, f"{rel}\\stampdir")
        try:
            _set_info(tree, d, FileInformationClass.FILE_BASIC_INFORMATION,
                      _basic_buffer(write=_unix_to_filetime(KNOWN_MTIME)))
            print("PASS (BASIC on a directory): accepted")
        except Exception as e:  # noqa: BLE001
            print(f"FAIL (BASIC on a directory): rejected with {type(e).__name__}")
            ok = False
        d.close()

        # 4. ARCHIVE must NOT be refused -- the review finding.
        a = _open_rw(tree, f"{rel}\\attrs.txt")
        for label, attrs in (("ARCHIVE alone", 0x20), ("ARCHIVE + NORMAL", 0x20 | 0x80), ("NORMAL alone", 0x80)):
            try:
                _set_info(tree, a, FileInformationClass.FILE_BASIC_INFORMATION, _basic_buffer(attributes=attrs))
                print(f"PASS (BASIC attributes {label}): accepted")
            except Exception as e:  # noqa: BLE001
                print(f"FAIL (BASIC attributes {label}): refused with {type(e).__name__} -- an ordinary "
                      f"post-copy request")
                ok = False

        # 5. ...but an attribute that would really change behaviour still is.
        for label, attrs in (("READ_ONLY", 0x01), ("HIDDEN", 0x02), ("SYSTEM", 0x04)):
            try:
                _set_info(tree, a, FileInformationClass.FILE_BASIC_INFORMATION, _basic_buffer(attributes=attrs))
                print(f"FAIL (BASIC attributes {label}): accepted a change it cannot make")
                ok = False
            except Exception as e:  # noqa: BLE001
                print(f"PASS (BASIC attributes {label}): refused rather than lying ({type(e).__name__})")

        # 6. Out of FAT's writable range (1980-2099): from v72 the FIELD is
        #    skipped and the REQUEST succeeds -- and because it was the only
        #    field asked for, the request writes nothing at all. Both halves are
        #    checked: "succeeded" alone would also be satisfied by a server that
        #    silently wrapped the date to something FAT can hold, which is the
        #    lie this used to guard against by refusing.
        #
        #    (This was "refused" through v71. The refusal was correct about the
        #    date and wrong about the consequence: iOS sends a date FAT cannot
        #    hold on an ordinary copy, and failing the request over it stopped
        #    the copy. See the module docstring and setBasicInfo()'s v72 comment,
        #    SmbFileHandlers.cpp:3696-3732.)
        skip_atime, skip_mtime = 1000000000, 1100000000  # 2001-09-09 / 2004-11-09
        for label, seconds in (("year 1970", 0), ("year 2100", 4102444800)):
            os.utime(_host(rel, "attrs.txt"), (skip_atime, skip_mtime))
            try:
                _set_info(tree, a, FileInformationClass.FILE_BASIC_INFORMATION,
                          _basic_buffer(write=_unix_to_filetime(seconds)))
            except Exception as e:  # noqa: BLE001
                print(f"FAIL (BASIC {label}): refused with {type(e).__name__} -- v72 skips a date FAT "
                      f"cannot hold rather than failing the request over it")
                ok = False
                continue
            st = os.stat(_host(rel, "attrs.txt"))
            if int(st.st_mtime) != skip_mtime or int(st.st_atime) != skip_atime:
                print(f"FAIL (BASIC {label}): succeeded but WROTE something -- "
                      f"atime {int(st.st_atime)} (was {skip_atime}), mtime {int(st.st_mtime)} "
                      f"(was {skip_mtime}); an unstorable date must be dropped, not wrapped")
                ok = False
            else:
                print(f"PASS (BASIC {label}): field skipped, request succeeded, nothing written")
        a.close()
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (set_info BASIC): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_set_basic_skips_unrepresentable_dates(port: int) -> bool:
    """A FILE_BASIC_INFORMATION carrying one representable and one
    unrepresentable timestamp must apply the first and drop the second, and
    report success.

    THIS CHECK WAS INVERTED IN v72, and the inversion is the point. Through v71
    it asserted that such a request was REFUSED with nothing written; that rule
    existed for a real defect (setBasicInfo() used to validate each field inside
    the apply loop, so `{LastAccessTime = valid, LastWriteTime = year 2100}`
    wrote the access stamp and THEN returned -1 -- a request reported as
    rejected but partially applied, with a diag.log line reading "reject" that
    would end an investigation in the wrong place). The defect was real; the
    remedy was aimed one step too far. iOS routinely sends a date FAT cannot
    hold alongside dates it can, so "refuse the request" meant "fail the copy".

    v72 keeps the mechanism that fixed the defect -- validation still happens
    ENTIRELY before the first write, so nothing is ever half-applied -- and
    changes only the verdict on an unstorable field, from fatal to dropped. The
    invariant that mattered is untouched: the reported result matches what
    happened.

    So what is asserted here is the honesty of the result, in both directions at
    once: the request SUCCEEDS, the representable field really IS on disk, and
    the unrepresentable one really is NOT. The on-disk value is read off the
    host filesystem rather than from the server's own reply, because a server
    that wrapped 2100 round to something FAT can hold would answer exactly the
    same way and only the filesystem would disagree.

    Both orderings are tested (unrepresentable first, unrepresentable second),
    because a loop that stops at the first bad field would pass one by luck."""
    rel = "smoke_test_basic_atomic"
    _seed_tree(rel, {"a.txt": 4, "b.txt": 4})
    base_atime = 1000000000  # 2001-09-09, distinct from every other date here
    base_mtime = 1100000000  # 2004-11-09
    ok = True
    connection, session, tree = connect_session(port)
    try:
        good = _unix_to_filetime(KNOWN_MTIME)
        bad = _unix_to_filetime(4102444800)  # year 2100 -- outside FAT's writable range

        # (expected atime, expected mtime) after the request: the field carrying
        # `good` moves, the field carrying `bad` stays exactly where it was.
        for name, buf, label, want_atime, want_mtime in (
            ("a.txt", _basic_buffer(access=good, write=bad),
             "representable access + unrepresentable write", KNOWN_MTIME, base_mtime),
            ("b.txt", _basic_buffer(access=bad, write=good),
             "unrepresentable access + representable write", base_atime, KNOWN_MTIME),
        ):
            os.utime(_host(rel, name), (base_atime, base_mtime))
            h = _open_rw(tree, f"{rel}\\{name}")
            refused = None
            try:
                _set_info(tree, h, FileInformationClass.FILE_BASIC_INFORMATION, buf)
            except Exception as e:  # noqa: BLE001
                refused = type(e).__name__
            h.close()
            st = os.stat(_host(rel, name))
            if refused is not None:
                print(f"FAIL (BASIC skip, {label}): refused with {refused} -- v72 drops the field FAT "
                      f"cannot store and applies the rest")
                ok = False
            elif abs(st.st_atime - want_atime) > FAT_RESOLUTION_SECONDS or \
                    abs(st.st_mtime - want_mtime) > FAT_RESOLUTION_SECONDS:
                print(f"FAIL (BASIC skip, {label}): wrong fields landed -- "
                      f"atime {int(st.st_atime)} (wanted {want_atime}), "
                      f"mtime {int(st.st_mtime)} (wanted {want_mtime})")
                ok = False
            else:
                print(f"PASS (BASIC skip, {label}): succeeded, the storable date landed, "
                      f"the unstorable one did not")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (BASIC skip): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_set_basic_structural_failure_writes_nothing(port: int) -> bool:
    """What is STILL all-or-nothing after v72: a request this server refuses for
    a STRUCTURAL reason must write nothing at all.

    v72 narrowed the class of things that fail -- an unstorable date is now
    dropped rather than fatal (see the sibling check above) -- but it did not
    weaken the guarantee for the things that DO still fail, and that guarantee is
    the one worth a test. Both refusals below are decided before any timestamp is
    written, and both requests carry a perfectly valid LastWriteTime that must
    therefore not appear on disk:

      * a payload shorter than the 36 bytes MS-FSCC 2.4.7 requires
        (`len < kBasicInfoMinBytes`, SmbFileHandlers.cpp:3601). The truncated
        buffer here deliberately still CONTAINS the write time in bytes 16-24,
        so a server that decoded first and length-checked second would apply it
        and be caught.
      * FileAttributes asking to change a bit this server does not model --
        READ_ONLY. That check is deliberately first in setBasicInfo()
        (SmbFileHandlers.cpp:3607-3632: "it is the cheap check, and refusing
        after having already written timestamps would leave the request
        half-applied"), and this is what proves the ordering rather than
        trusting the comment.

    The refusal is asserted as "it raised", not as a particular exception type:
    v72 changed the NT status these come back with (STATUS_NOT_IMPLEMENTED ->
    STATUS_INVALID_PARAMETER, setInfoCmd), and the property under test is that
    nothing was written, not which of the two words the client saw."""
    rel = "smoke_test_basic_structural"
    _seed_tree(rel, {"short.txt": 4, "attr.txt": 4})
    base_atime = 1000000000  # 2001-09-09
    base_mtime = 1100000000  # 2004-11-09
    ok = True
    connection, session, tree = connect_session(port)
    try:
        good = _unix_to_filetime(KNOWN_MTIME)
        for name, buf, label in (
            # 24 bytes: creation + access + write. Valid write time, illegal length.
            ("short.txt", _basic_buffer(write=good)[:24], "payload truncated to 24 of 36 bytes"),
            ("attr.txt", _basic_buffer(write=good, attributes=0x01), "FileAttributes READ_ONLY"),
        ):
            os.utime(_host(rel, name), (base_atime, base_mtime))
            h = _open_rw(tree, f"{rel}\\{name}")
            refused = False
            try:
                _set_info(tree, h, FileInformationClass.FILE_BASIC_INFORMATION, buf)
            except Exception:  # noqa: BLE001
                refused = True
            h.close()
            st = os.stat(_host(rel, name))
            if not refused:
                print(f"FAIL (BASIC structural, {label}): accepted a structurally invalid request")
                ok = False
            elif int(st.st_mtime) != base_mtime or int(st.st_atime) != base_atime:
                print(f"FAIL (BASIC structural, {label}): PARTIALLY APPLIED -- "
                      f"atime {int(st.st_atime)} (was {base_atime}), mtime {int(st.st_mtime)} (was {base_mtime})")
                ok = False
            else:
                print(f"PASS (BASIC structural, {label}): refused with nothing written")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (BASIC structural): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


# The exact CreationTime an iPhone put on the wire, copied from the device log
# (diag19.log:118304, quoted in setBasicInfo()'s v72 comment):
#
#   set_info basic reject: CreationTime 0x153b281e0fb4000 predates 1970,
#                          nothing applied path=/api
#
# 0x153b281e0fb4000 is 1904-01-01T00:00:00Z -- the Mac/HFS epoch. Apple's zero
# is 1904 where Microsoft's is 1601, so this is what iOS sends for a file whose
# source has no creation date: a "there isn't one" sentinel, not corruption.
IOS_MAC_EPOCH_CREATION_FILETIME = 0x153B281E0FB4000


def test_set_basic_ios_1904_creation_time(port: int) -> bool:
    """REGRESSION TEST FOR THE FIELD FAILURE v72 EXISTS TO FIX: the request an
    iPhone actually sent, byte for byte on the value that broke it.

    This is not a synthetic edge case. It is the request that stopped the
    first real copy: the phone CREATEd the file, opened it
    FILE_WRITE_DATA|FILE_APPEND_DATA, sent this SET_INFO, was refused (and told
    STATUS_NOT_IMPLEMENTED, which reads as "this server has no SET_INFO at all"),
    and then never sent a single WRITE -- idle for 144 seconds, then logoff. No
    check in this suite could see it, because every date this file had ever sent
    was one FAT could store.

    Two things are asserted, and the second is what makes it a regression test
    rather than a smoke test: the request SUCCEEDS, and the LastWriteTime
    travelling alongside the 1904 sentinel really lands on disk. A server that
    "succeeded" by ignoring the whole request would pass the first and fail the
    second.

    The CreationTime itself is deliberately NOT asserted on disk: this harness
    cannot see one. POSIX has no settable birthtime, so stub_hal/HalStorage.cpp's
    setTimestamp() accepts T_CREATE and drops it (documented in README.md's
    divergence table). That is a harness limit, not a gap in the fix -- the field
    is skipped before it ever reaches the storage layer."""
    rel = "smoke_test_basic_ios1904"
    _seed_tree(rel, {"iphone_copy.bin": 16})
    base_atime = 1000000000  # 2001-09-09
    base_mtime = 1100000000  # 2004-11-09
    connection, session, tree = connect_session(port)
    try:
        os.utime(_host(rel, "iphone_copy.bin"), (base_atime, base_mtime))
        h = _open_rw(tree, rf"{rel}\iphone_copy.bin")
        try:
            _set_info(tree, h, FileInformationClass.FILE_BASIC_INFORMATION,
                      _basic_buffer(creation=IOS_MAC_EPOCH_CREATION_FILETIME,
                                    write=_unix_to_filetime(KNOWN_MTIME)))
        except Exception as e:  # noqa: BLE001
            h.close()
            print(f"FAIL (BASIC iOS 1904 CreationTime): refused with {type(e).__name__} -- this is the "
                  f"exact request that stopped a real iPhone copy; the 1904 field must be skipped, "
                  f"not fatal")
            return False
        h.close()
        st = os.stat(_host(rel, "iphone_copy.bin"))
        if abs(st.st_mtime - KNOWN_MTIME) > FAT_RESOLUTION_SECONDS:
            print(f"FAIL (BASIC iOS 1904 CreationTime): accepted, but the LastWriteTime sent with it did "
                  f"not land -- filesystem says {int(st.st_mtime)}, expected ~{KNOWN_MTIME}")
            return False
        print("PASS (BASIC iOS 1904 CreationTime): 0x153b281e0fb4000 skipped, request succeeded, "
              "and the LastWriteTime alongside it landed on disk")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (BASIC iOS 1904 CreationTime): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_set_basic_on_share_root_refused(port: int) -> bool:
    """Stamping the share root must be refused, because SdFat refuses it.

    `FatFile::timestamp()` starts with `isFileOrSubDir()`, and the FAT root is
    neither -- it carries FILE_ATTR_ROOT_FIXED/ROOT32 and never
    FILE_ATTR_SUBDIR. POSIX has no such notion (utimensat on the tree root
    works fine), so the stub had to be taught the guard; without it the harness
    ACCEPTED a stamp the X3 rejects. The setter counterpart of the share-root
    row the getter already had in the divergence table."""
    connection, session, tree = connect_session(port)
    try:
        before = os.stat(SD_ROOT).st_mtime
        d = _open_dir(tree, "")
        refused = False
        try:
            _set_info(tree, d, FileInformationClass.FILE_BASIC_INFORMATION,
                      _basic_buffer(write=_unix_to_filetime(KNOWN_MTIME)))
        except Exception:  # noqa: BLE001
            refused = True
        d.close()
        if not refused:
            print("FAIL (BASIC on the share root): accepted -- the device refuses this outright")
            return False
        if abs(os.stat(SD_ROOT).st_mtime - before) > 1:
            print("FAIL (BASIC on the share root): the root's timestamp changed anyway")
            return False
        print("PASS (BASIC on the share root): refused, as SdFat's isFileOrSubDir() guard does")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (BASIC on the share root): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_replace_not_blocked_by_a_leftover(port: int) -> bool:
    """A leftover holding file must not block later replaces.

    The holding name used to be the fixed `crossmosa-replace.tmp`, so an
    interrupted replace left a file that then blocked EVERY later replace in
    that directory -- permanently, with one diag.log line as the only notice. A
    visible leftover is a nuisance; a directory that silently stops accepting
    replaces is undiagnosable from the outside.

    This plants exactly the file the old code would have chosen and requires
    the replace to succeed anyway. The leftover must also survive untouched --
    reusing it would destroy whatever it holds."""
    rel = "smoke_test_replace_leftover"
    _seed_tree(rel, {})
    with open(_host(rel, "source.txt"), "wb") as fh:
        fh.write(b"SOURCE")
    with open(_host(rel, "victim.txt"), "wb") as fh:
        fh.write(b"VICTIM")
    with open(_host(rel, "crossmosa-replace.tmp"), "wb") as fh:
        fh.write(b"LEFTOVER-FROM-AN-INTERRUPTED-REPLACE")
    ok = True
    connection, session, tree = connect_session(port)
    try:
        f = _open_rw(tree, f"{rel}\\source.txt")
        err = _try_rename(tree, f, f"{rel}\\victim.txt", replace=True)
        f.close()
        if err is not None:
            print(f"FAIL (replace with a leftover present): blocked by the leftover, rejected with {err}")
            ok = False
        else:
            with open(_host(rel, "victim.txt"), "rb") as fh:
                if fh.read() != b"SOURCE":
                    print("FAIL (replace with a leftover present): the destination does not hold the source bytes")
                    ok = False
                else:
                    print("PASS (replace with a leftover present): stepped over it and replaced")
        with open(_host(rel, "crossmosa-replace.tmp"), "rb") as fh:
            if fh.read() != b"LEFTOVER-FROM-AN-INTERRUPTED-REPLACE":
                print("FAIL (replace with a leftover present): the leftover was clobbered")
                ok = False
        strays = [n for n in os.listdir(_host(rel)) if n.startswith("crossmosa-replace-")]
        if strays:
            print(f"FAIL (replace with a leftover present): the holding copy was not cleaned up: {strays}")
            ok = False
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (replace with a leftover present): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_delete_on_close_with_a_second_handle(port: int) -> bool:
    """A second handle opened AFTER the disposition must stop the delete.

    `setDisposition()` refuses while another handle is open, but that is a
    check at one moment: `createCmd` has no same-path guard, so between the
    disposition and the close another connection can open the very file about
    to be deleted. On device `Storage.remove()` would then run while a live
    FatFile caches that directory entry and cluster chain -- "a given file must
    not be opened by more than one FatFile object or file corruption may
    occur".

    THE HARNESS CANNOT SEE THE CORRUPTION: POSIX unlink() is refcounted, so the
    delete simply succeeds there. That is why this asserts the REFUSAL (and the
    file surviving), which is observable on both."""
    rel = "smoke_test_delete_second_handle"
    _seed_tree(rel, {"contested.txt": 5})
    ok = True
    ca, sa, ta = connect_session(port)
    cb, sb, tb = connect_session(port)
    try:
        a = _open_rw(ta, f"{rel}\\contested.txt")
        _set_info(ta, a, FileInformationClass.FILE_DISPOSITION_INFORMATION, b"\x01")

        # B opens it AFTER the disposition -- createCmd has no reason to refuse.
        b = _create_file(tb, f"{rel}\\contested.txt", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.GENERIC_READ, CreateOptions.FILE_NON_DIRECTORY_FILE)

        closed_ok = True
        try:
            a.close()
        except Exception:  # noqa: BLE001
            closed_ok = False
        if closed_ok:
            print("FAIL (delete-on-close, second handle): close reported success and deleted under a live handle")
            ok = False
        else:
            print("PASS (delete-on-close, second handle): the delete was refused and reported")
        if not os.path.exists(_host(rel, "contested.txt")):
            print("FAIL (delete-on-close, second handle): THE FILE WAS DELETED while B still held it")
            ok = False
        else:
            print("PASS (delete-on-close, second handle): the file survived")

        # B still works, and once B is gone the path can be deleted normally.
        b.close()
        c = _open_rw(ta, f"{rel}\\contested.txt")
        _set_info(ta, c, FileInformationClass.FILE_DISPOSITION_INFORMATION, b"\x01")
        c.close()
        if os.path.exists(_host(rel, "contested.txt")):
            print("FAIL (delete-on-close, second handle): still there after the last handle closed")
            ok = False
        else:
            print("PASS (delete-on-close, second handle): deletes normally once it is the only handle")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (delete-on-close, second handle): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tb, cb)
        _teardown(ta, ca)


def test_rename_onto_case_variant(port: int) -> bool:
    """Renaming onto a name that differs from an existing one only by CASE.

    FAT is case-insensitive, so `VICTIM.TXT` and `victim.txt` are one file:
    `ReplaceIfExists=0` must be refused, and `ReplaceIfExists=1` must replace
    the existing entry rather than creating a second one. POSIX would happily
    hold both, so the stub had to be taught to case-fold
    (`resolveExistingCase()`); before that this test's first half passed
    wrongly and produced a directory containing two files that cannot coexist
    on the card."""
    rel = "smoke_test_rename_case"
    _seed_tree(rel, {})
    with open(_host(rel, "source.txt"), "wb") as fh:
        fh.write(b"SOURCE")
    with open(_host(rel, "victim.txt"), "wb") as fh:
        fh.write(b"VICTIM-MUST-SURVIVE")
    ok = True
    connection, session, tree = connect_session(port)
    try:
        f = _open_rw(tree, f"{rel}\\source.txt")
        err = _try_rename(tree, f, f"{rel}\\VICTIM.TXT", replace=False)
        f.close()
        entries = sorted(os.listdir(_host(rel)))
        if err is None:
            print(f"FAIL (rename onto case variant, no replace): SUCCEEDED, directory now {entries}")
            ok = False
        elif entries != ["source.txt", "victim.txt"]:
            print(f"FAIL (rename onto case variant, no replace): directory is {entries}")
            ok = False
        else:
            print(f"PASS (rename onto case variant, no replace): rejected with {err}, both files unchanged")

        f2 = _open_rw(tree, f"{rel}\\source.txt")
        err = _try_rename(tree, f2, f"{rel}\\VICTIM.TXT", replace=True)
        f2.close()
        entries = sorted(os.listdir(_host(rel)))
        if err is not None:
            print(f"FAIL (rename onto case variant, replace): rejected with {err}")
            ok = False
        elif len(entries) != 1:
            print(f"FAIL (rename onto case variant, replace): directory is {entries}, expected exactly one file")
            ok = False
        else:
            with open(_host(rel, entries[0]), "rb") as fh:
                landed = fh.read()
            if landed != b"SOURCE":
                print(f"FAIL (rename onto case variant, replace): {entries[0]} holds {landed!r}")
                ok = False
            else:
                print(f"PASS (rename onto case variant, replace): one file, {entries[0]}, holding the source bytes")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (rename onto case variant): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_query_info_filesystem_classes(port: int) -> bool:
    """The four SMB2_0_INFO_FILESYSTEM classes. FS_SIZE's numbers are a
    documented placeholder (HalStorage exposes no capacity accessor -- see
    queryInfoCmd's kNominal* comment), so what is asserted here is the shape a
    client depends on: non-zero sector/cluster geometry and non-zero free
    space, because zero free space makes iOS refuse to copy anything at all."""
    ok = True
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, "")

        size = _query_info(tree, d, InfoType.SMB2_0_INFO_FILESYSTEM, 3)  # FileFsSizeInformation
        total, avail, spc, bps = struct.unpack("<QQII", size[:24])
        if not (total > 0 and avail > 0 and spc > 0 and bps > 0 and avail <= total):
            print(f"FAIL (query_info FS_SIZE): total={total} avail={avail} sectors/cluster={spc} "
                  f"bytes/sector={bps}")
            ok = False

        dev = _query_info(tree, d, InfoType.SMB2_0_INFO_FILESYSTEM, 4)  # FileFsDeviceInformation
        dev_type, characteristics = struct.unpack("<II", dev[:8])
        if dev_type != 0x07:  # FILE_DEVICE_DISK
            print(f"FAIL (query_info FS_DEVICE): device_type=0x{dev_type:08x}, expected FILE_DEVICE_DISK")
            ok = False

        attr = _query_info(tree, d, InfoType.SMB2_0_INFO_FILESYSTEM, 5)  # FileFsAttributeInformation
        fs_attrs, max_comp, name_len = struct.unpack("<III", attr[:12])
        fs_name = attr[12:12 + name_len].decode("utf-16-le")
        if max_comp != 255 or not fs_name or (fs_attrs & 0x00000001):
            print(f"FAIL (query_info FS_ATTRIBUTE): attrs=0x{fs_attrs:08x} max_component={max_comp} "
                  f"name={fs_name!r} (FILE_CASE_SENSITIVE_SEARCH must NOT be set on FAT)")
            ok = False

        full = _query_info(tree, d, InfoType.SMB2_0_INFO_FILESYSTEM, 7)  # FileFsFullSizeInformation
        f_total, f_caller, f_actual, f_spc, f_bps = struct.unpack("<QQQII", full[:32])
        if not (f_total == total and f_caller > 0 and f_actual > 0 and f_spc == spc and f_bps == bps):
            print(f"FAIL (query_info FS_FULL_SIZE): total={f_total} caller_avail={f_caller} "
                  f"actual_avail={f_actual} sectors/cluster={f_spc} bytes/sector={f_bps} -- must agree "
                  f"with FS_SIZE's geometry ({total}, {spc}, {bps})")
            ok = False

        vol = _query_info(tree, d, InfoType.SMB2_0_INFO_FILESYSTEM, 1)  # FileFsVolumeInformation
        serial, label_len = struct.unpack("<II", vol[8:16])
        label = vol[18:18 + label_len].decode("utf-16-le")
        if serial == 0 or not label:
            print(f"FAIL (query_info FS_VOLUME): serial=0x{serial:08x} label={label!r}")
            ok = False

        d.close()
        if ok:
            print(f"PASS (query_info filesystem classes): FS_SIZE ({total} x {spc} x {bps} B clusters, "
                  f"{avail} free) / FS_FULL_SIZE / FS_DEVICE / FS_ATTRIBUTE ({fs_name}) / FS_VOLUME ({label})")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (query_info filesystem classes): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_query_info_unsupported_rejected(port: int) -> bool:
    """Anything not implemented must fail loudly AND leave a DiagLog line
    naming the numeric type/class. This device has no serial port, so that
    line is the only evidence that will exist when a real iPhone asks for
    something we do not answer -- a silent -1 is the difference between a
    one-day diagnosis and a week of blind flashing. (The line itself goes to
    stderr under the harness; here we can only assert the rejection.)"""
    ok = True
    connection, session, tree = connect_session(port)
    try:
        d = _open_dir(tree, "")
        for label, info_type, info_class in (
            ("INFO_SECURITY", InfoType.SMB2_0_INFO_SECURITY, 0),
            ("INFO_QUOTA", InfoType.SMB2_0_INFO_QUOTA, 0),
            ("FILE / FILE_STREAM_INFORMATION", InfoType.SMB2_0_INFO_FILE,
             FileInformationClass.FILE_STREAM_INFORMATION),
            ("FILESYSTEM / FILE_FS_SECTOR_SIZE_INFORMATION", InfoType.SMB2_0_INFO_FILESYSTEM, 11),
        ):
            try:
                buf = _query_info(tree, d, info_type, info_class)
                print(f"FAIL (query_info unsupported {label}): returned {len(buf)} bytes instead of an error")
                ok = False
            except Exception as e:  # noqa: BLE001
                print(f"PASS (query_info unsupported {label} rejected): {type(e).__name__}")
        d.close()
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (query_info unsupported): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_query_cross_connection_rejected(port: int) -> bool:
    """The ownership check has to hold for the two new handlers too, not just
    close_cmd: file ids are trivially predictable, and query_info on someone
    else's handle would leak the paths and sizes of files the other connection
    opened.

    Unlike its close_cmd sibling, both refusals here still RAISE on the client
    after v65, and for a reason worth recording rather than relying on by luck:
    v65 gave a specific NT status to close_cmd's unknown-handle path
    (STATUS_FILE_CLOSED, which smbprotocol's Open absorbs) but left
    query_directory's and query_info's at `return -1`, which libsmb2 still turns
    into its blanket NOT_IMPLEMENTED -- and nothing absorbs that. So these two
    kept failing loudly while the close check went quiet.

    A's own handle is verified by USING it afterwards, dot entries included: a
    healthy restarting enumeration, not a cursor B managed to disturb."""
    conn_a, sess_a, tree_a = connect_session(port)
    conn_b, sess_b, tree_b = connect_session(port)
    ok = True
    try:
        _seed_tree("smoke_test_xconn", {"secret.epub": 9})
        victim = _open_dir(tree_a, "smoke_test_xconn")

        thief = Open(tree_b, "smoke_test_xconn")
        thief.file_id = victim.file_id
        thief._connected = True
        try:
            thief.query_directory("*", FileInformationClass.FILE_ID_BOTH_DIRECTORY_INFORMATION)
            print("FAIL (cross-connection query_directory): B enumerated A's handle")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (cross-connection query_directory rejected): {type(e).__name__}")
        try:
            _query_info(tree_b, thief, InfoType.SMB2_0_INFO_FILE,
                        FileInformationClass.FILE_ALL_INFORMATION)
            print("FAIL (cross-connection query_info): B read info from A's handle")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (cross-connection query_info rejected): {type(e).__name__}")

        # A's handle must still work afterwards -- its FIRST query, so a
        # restart, so '.' and '..' must be there too.
        entries, _ = _drain_listing(victim)
        got = _entry_names(entries)
        if not _dots_present(got, "cross-connection, A's own handle"):
            ok = False
        elif set(_real_only(got)) != {"secret.epub"}:
            print(f"FAIL (cross-connection): A's own handle listed {sorted(_real_only(got))!r} after "
                  f"B's attempts, expected ['secret.epub']")
            ok = False
        victim.close()
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (cross-connection query): {type(e).__name__}: {e}")
        return False
    finally:
        for tree, conn in ((tree_a, conn_a), (tree_b, conn_b)):
            try:
                tree.disconnect()
            except Exception:  # noqa: BLE001
                pass
            try:
                conn.disconnect()
            except Exception:  # noqa: BLE001
                pass


# ---------------------------------------------------------------------------
# Task 6: read / write / flush -- the data-integrity handlers.
#
# Every check below compares whole-file SHA-256 rather than "the call did not
# raise", because the failure mode that matters here is not an exception: it
# is a transfer that reports success and lands the wrong bytes. All the sizes
# are chosen relative to the NEGOTIATED transfer size, so the multi-request path
# (and the boundary between requests) is exercised rather than assumed.
#
# THAT SIZE IS READ OFF THE CONNECTION, NEVER HARDCODED. It was 32768 through
# v71 and is 8192 from v72 (SmbServer.cpp), and it is expected to move again:
# signing allocates the whole PDU as one contiguous block, so the ceiling is a
# RAM decision that gets retuned whenever the device reports better headroom.
# A literal here would turn the next tuning pass into six red checks whose
# message ("greater than the maximum negotiated write size") describes the
# suite's own staleness and says nothing at all about the server.


def _negotiated_chunk(connection) -> int:
    """The largest payload this server accepts in ONE request, taken from its
    own NEGOTIATE reply -- smbprotocol fills Connection.max_read_size and
    .max_write_size in there. The minimum of the two drives both directions of
    a round trip, so one number describes a whole transfer."""
    chunk = min(connection.max_read_size, connection.max_write_size)
    if chunk <= 0:
        raise AssertionError(f"server negotiated a nonsense transfer size: "
                             f"max_read_size={connection.max_read_size}, "
                             f"max_write_size={connection.max_write_size}")
    return chunk


def _chunk_of(op) -> int:
    """The same value reached from an open handle (Open.tree_connect ->
    .session -> .connection), so the read/write helpers can default to it
    instead of every caller threading it through."""
    return _negotiated_chunk(op.tree_connect.session.connection)


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _pattern_bytes(size: int, seed: int = 0) -> bytes:
    """Deterministic high-entropy filler. High entropy is the point: a block of
    repeated bytes would hide a chunk written at the wrong offset, a chunk
    written twice, or two chunks swapped -- exactly the bugs this task is
    about. Deterministic so a failure can be reproduced and diffed."""
    out = bytearray()
    counter = 0
    while len(out) < size:
        out += hashlib.sha256(f"{seed}:{counter}".encode()).digest()
        counter += 1
    return bytes(out[:size])


def _host_path(relative: str) -> str:
    return os.path.join(SD_ROOT, *relative.split("/"))


def _seed_binary(relative: str, size: int, seed: int = 0) -> bytes:
    """Plants a binary file straight on the harness's SD root (not over SMB --
    the read tests must read something this server did not itself write) and
    returns its contents."""
    path = _host_path(relative)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = _pattern_bytes(size, seed)
    with open(path, "wb") as fh:
        fh.write(data)
    return data


def _read_all(op, size: int, chunk: int | None = None) -> bytes:
    """Reads `size` bytes the way a real client does: repeated bounded reads at
    advancing offsets, none larger than the negotiated max_read_size. Refuses
    to loop forever on a zero-length reply."""
    if chunk is None:
        chunk = _chunk_of(op)
    out = bytearray()
    offset = 0
    while offset < size:
        got = op.read(offset, min(chunk, size - offset))
        if not got:
            raise AssertionError(f"read returned 0 bytes at offset {offset} of {size}")
        out += got
        offset += len(got)
    return bytes(out)


def _write_all(op, data: bytes, chunk: int | None = None) -> None:
    """The write-side mirror of _read_all(), bounded by the negotiated
    max_write_size for the same reason."""
    if chunk is None:
        chunk = _chunk_of(op)
    offset = 0
    while offset < len(data):
        piece = data[offset:offset + chunk]
        n = op.write(piece, offset)
        if n != len(piece):
            raise AssertionError(f"short write at offset {offset}: server reported {n}, sent {len(piece)}")
        offset += len(piece)


def _open_read(tree, name: str):
    return _create_file(tree, name, CreateDisposition.FILE_OPEN,
                        FilePipePrinterAccessMask.GENERIC_READ, CreateOptions.FILE_NON_DIRECTORY_FILE)


def _open_write(tree, name: str, disposition=CreateDisposition.FILE_OVERWRITE_IF):
    return _create_file(tree, name, disposition,
                        FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.GENERIC_WRITE,
                        CreateOptions.FILE_NON_DIRECTORY_FILE)


def test_read_200k_roundtrip(port: int) -> bool:
    """200 KB is deliberately many times the negotiated transfer size, so it
    takes a long run of READ requests and an off-by-one in the per-request
    offset or length shows up as a hash mismatch instead of passing by accident
    on a single-request file.

    The request COUNT is derived and printed, never asserted: it is 7 at a
    32768-byte ceiling and 25 at v72's 8192, and the property under test is that
    200 KB of high-entropy bytes come back byte-exact however many round trips
    that took."""
    name = "smoke_read_200k.bin"
    size = 200 * 1024
    data = _seed_binary(name, size, seed=61)
    connection, session, tree = connect_session(port)
    try:
        chunk = _negotiated_chunk(connection)
        op = _open_read(tree, name)
        got = _read_all(op, size, chunk)
        op.close()
        if len(got) != size:
            print(f"FAIL (read 200K): got {len(got)} bytes, expected {size}")
            return False
        if _sha(got) != _sha(data):
            print(f"FAIL (read 200K): SHA-256 mismatch\n  on disk: {_sha(data)}\n  over SMB: {_sha(got)}")
            return False
        print(f"PASS (read 200K round trip): {size} B over {-(-size // chunk)} READ requests "
              f"of at most {chunk} B, SHA-256 {_sha(got)}")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (read 200K): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_write_200k_roundtrip(port: int) -> bool:
    """The same size in the other direction, verified against the bytes that
    actually landed on "the SD card" -- not against what the server said it
    wrote."""
    name = "smoke_write_200k.bin"
    size = 200 * 1024
    data = _pattern_bytes(size, seed=62)
    connection, session, tree = connect_session(port)
    try:
        chunk = _negotiated_chunk(connection)
        op = _open_write(tree, name)
        _write_all(op, data, chunk)
        op.close()
        with open(_host_path(name), "rb") as fh:
            on_disk = fh.read()
        if len(on_disk) != size:
            print(f"FAIL (write 200K): file is {len(on_disk)} B on disk, expected {size}")
            return False
        if _sha(on_disk) != _sha(data):
            print(f"FAIL (write 200K): SHA-256 mismatch\n  sent: {_sha(data)}\n  on disk: {_sha(on_disk)}")
            return False
        print(f"PASS (write 200K round trip): {size} B over {-(-size // chunk)} WRITE requests "
              f"of at most {chunk} B, SHA-256 {_sha(on_disk)}")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (write 200K): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_chunk_boundary_sizes(port: int) -> bool:
    """CHUNK-1, CHUNK and CHUNK+1, each read AND written, where CHUNK is the
    size the server NEGOTIATED rather than a literal -- a boundary test is only
    testing a boundary if it straddles the edge the server actually announced.
    Hardcoding 32768 here after v72 lowered the ceiling to 8192 would have left
    every size a plain multi-request transfer with no edge in it at all, which
    is the quiet version of this check breaking.

    An exact multiple of the transfer size is where "one more empty request" and
    "one request too few" both live; +-1 around it is where the clamp arithmetic
    lives. CHUNK+1 is the only one of the three that needs a second request at
    all, and its second request carries exactly one byte."""
    ok = True
    connection, session, tree = connect_session(port)
    try:
        chunk = _negotiated_chunk(connection)
        for label, size in (("CHUNK-1", chunk - 1), ("CHUNK", chunk), ("CHUNK+1", chunk + 1),
                            ("2*CHUNK", 2 * chunk)):
            read_name = f"smoke_boundary_r_{size}.bin"
            expected = _seed_binary(read_name, size, seed=size)
            op = _open_read(tree, read_name)
            got = _read_all(op, size, chunk)
            op.close()
            if _sha(got) != _sha(expected):
                print(f"FAIL (boundary read {label}={size}): SHA-256 mismatch, got {len(got)} B")
                ok = False
                continue

            write_name = f"smoke_boundary_w_{size}.bin"
            payload = _pattern_bytes(size, seed=size + 1)
            op = _open_write(tree, write_name)
            _write_all(op, payload, chunk)
            op.close()
            with open(_host_path(write_name), "rb") as fh:
                on_disk = fh.read()
            if _sha(on_disk) != _sha(payload) or len(on_disk) != size:
                print(f"FAIL (boundary write {label}={size}): SHA-256 mismatch, {len(on_disk)} B on disk")
                ok = False
                continue
            print(f"PASS (boundary {label} = {size} B, negotiated chunk {chunk}): "
                  f"read and write both byte-exact")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (boundary sizes): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_read_at_and_past_eof(port: int) -> bool:
    """Three cases that are easy to conflate:
      * a read STRADDLING the end must return only the bytes that exist;
      * a read starting exactly AT the end must answer STATUS_END_OF_FILE;
      * a read starting well PAST the end must answer STATUS_END_OF_FILE too --
        not a successful empty read, and certainly not stale bytes from wherever
        the seek landed.
    The last one matters more than it looks on this device: SdFat's seekSet()
    FAILS for pos > fileSize (FatFile.cpp:1184-1188, ExFatFile.cpp:715-719), so
    a handler that seeks first and checks later would read from the previous
    file position -- a successful-looking read of the wrong bytes.

    THE EXPECTED ANSWER FOR THE LAST TWO CHANGED IN v65. v64 answered a
    successful ZERO-BYTE read, and this check asserted that; the reasoning was
    that every sequential reader treats an empty read as the end (it is what
    POSIX read() does) and that a specific status was not reachable from a
    handler that could only return a sign. Measured cost of that choice: a
    zero-byte SUCCESS is not an END, so a client that trusts max_read_size keeps
    asking -- an 11-byte file took the Linux kernel client 64 READ round trips,
    63 of them for offsets from 32,768 to 1,015,819, and one 100 KB copy left 123
    "read at/after EOF" lines in diag.log. replyStatus() now makes
    STATUS_END_OF_FILE (0xC0000011, MS-SMB2 3.3.5.12) reachable, so it is what
    the server says.

    The exception TYPE is asserted, not just "it raised": a read past the end
    that came back as ACCESS_DENIED or NOT_IMPLEMENTED would look like a broken
    file to a client rather than a finished one, and that is the failure this
    change exists to remove. And the handle must still work afterwards -- an
    error reply must not be a connection or handle event."""
    name = "smoke_eof.bin"
    size = 100
    data = _seed_binary(name, size, seed=63)
    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = _open_read(tree, name)

        straddle = op.read(90, 1024)
        if straddle != data[90:]:
            print(f"FAIL (read straddling EOF): got {len(straddle)} B, expected 10")
            ok = False
        else:
            print("PASS (read straddling EOF): clamped to the 10 bytes that exist")

        for label, offset in (("read at EOF", size), ("read past EOF", size + 4096)):
            try:
                got = op.read(offset, 1024)
                print(f"FAIL ({label}): returned {len(got)} B successfully at offset {offset}; a "
                      f"zero-byte success is what made a client keep re-asking past the end")
                ok = False
            except EndOfFile:
                print(f"PASS ({label}): STATUS_END_OF_FILE at offset {offset}")
            except Exception as e:  # noqa: BLE001
                print(f"FAIL ({label}): refused with {type(e).__name__} instead of EndOfFile: {e} -- "
                      f"the client cannot tell 'finished' from 'broken' from that")
                ok = False

        # The handle must still be usable after all that.
        again = op.read(0, size)
        if again != data:
            print("FAIL (read after EOF probes): the handle stopped returning correct data")
            ok = False
        op.close()
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (read at/past EOF): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_non_sequential_and_overlapping_writes(port: int) -> bool:
    """SMB clients do not always write in ascending order. Three shapes here:
      1. a write that starts PAST the current end of file (the third chunk
         first), which leaves a hole the server has to zero-fill -- writing it
         at the wrong offset instead would silently corrupt the file;
      2. the two earlier chunks, filling that hole in;
      3. an OVERLAPPING write straddling the chunk-1/chunk-2 boundary.
    Verified byte-exact against the host file, not against reported counts."""
    name = "smoke_nonseq.bin"
    piece = 1000
    a = _pattern_bytes(piece, seed=71)
    b = _pattern_bytes(piece, seed=72)
    c = _pattern_bytes(piece, seed=73)
    overlap = _pattern_bytes(500, seed=74)
    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = _open_write(tree, name)
        op.write(c, 2 * piece)   # gap: bytes 0..1999 do not exist yet
        op.write(a, 0)
        op.write(b, piece)
        op.write(overlap, 750)   # straddles a/b
        op.close()

        expected = bytearray(a + b + c)
        expected[750:750 + len(overlap)] = overlap
        with open(_host_path(name), "rb") as fh:
            on_disk = fh.read()
        if on_disk != bytes(expected):
            print(f"FAIL (non-sequential writes): {len(on_disk)} B on disk, expected {len(expected)}; "
                  f"SHA {_sha(on_disk)} != {_sha(bytes(expected))}")
            ok = False
        else:
            print("PASS (non-sequential + overlapping writes): hole zero-filled, all three chunks and the "
                  "overlap byte-exact")

        # A gap the server must refuse rather than spend minutes zero-filling.
        # The file must be untouched afterwards, and the connection still live.
        op2 = _open_write(tree, name, disposition=CreateDisposition.FILE_OPEN)
        try:
            op2.write(b"x" * 16, 64 * 1024 * 1024)
            print("FAIL (absurd write gap): a 64 MB hole was accepted")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (absurd write gap rejected): {type(e).__name__}")
        op2.close()
        with open(_host_path(name), "rb") as fh:
            after = fh.read()
        if after != on_disk:
            print("FAIL (absurd write gap): the file changed despite the write being rejected")
            ok = False
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (non-sequential writes): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_cjk_data_roundtrip(port: int) -> bool:
    """A full write-then-read round trip through a Traditional Chinese path --
    this user's library is entirely Chinese, so a path that only survives
    create/close is not enough; the same name has to resolve on the
    transfer handlers too."""
    directory = "測試資料夾"
    name = f"{directory}/範例書坊-讀寫測試.bin"
    smb_name = name.replace("/", "\\")
    size = 100 * 1024
    data = _pattern_bytes(size, seed=64)
    connection, session, tree = connect_session(port)
    try:
        chunk = _negotiated_chunk(connection)
        _ensure_dir(tree, directory)
        op = _open_write(tree, smb_name)
        _write_all(op, data, chunk)
        op.close()

        with open(_host_path(name), "rb") as fh:
            on_disk = fh.read()
        if _sha(on_disk) != _sha(data):
            print(f"FAIL (CJK data round trip): on-disk SHA-256 {_sha(on_disk)} != sent {_sha(data)}")
            return False

        op = _open_read(tree, smb_name)
        got = _read_all(op, size, chunk)
        op.close()
        if _sha(got) != _sha(data):
            print(f"FAIL (CJK data round trip): read-back SHA-256 {_sha(got)} != sent {_sha(data)}")
            return False
        print(f"PASS (CJK data round trip): {size} B written and read back through {name}, SHA-256 {_sha(got)}")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (CJK data round trip): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_concurrent_handles_interleaved(port: int) -> bool:
    """Two connections, two files, writes interleaved request-by-request. The
    open-file table is a shared fixed array and the file position lives inside
    each slot's own HalFile, so a handler that kept any per-request state
    outside the slot -- a shared scratch buffer, a cached offset -- would
    cross-contaminate here and nowhere else.

    The size is three negotiated chunks plus a ragged remainder, so the
    interleaving covers several full-size requests AND a short final one in each
    direction; both connections talk to the same server and so negotiate the
    same ceiling."""
    name_a = "smoke_concurrent_a.bin"
    name_b = "smoke_concurrent_b.bin"
    conn_a, sess_a, tree_a = connect_session(port)
    conn_b, sess_b, tree_b = connect_session(port)
    chunk = _negotiated_chunk(conn_a)
    size = 3 * chunk + 123
    data_a = _pattern_bytes(size, seed=81)
    data_b = _pattern_bytes(size, seed=82)
    ok = True
    try:
        op_a = _open_write(tree_a, name_a)
        op_b = _open_write(tree_b, name_b)
        offset = 0
        while offset < size:
            piece = min(chunk, size - offset)
            n_a = op_a.write(data_a[offset:offset + piece], offset)
            n_b = op_b.write(data_b[offset:offset + piece], offset)
            if n_a != piece or n_b != piece:
                print(f"FAIL (concurrent handles): short write at {offset} (a={n_a} b={n_b}, wanted {piece})")
                ok = False
                break
            offset += piece

        # Read back over SMB too -- each connection reading its OWN file, still
        # interleaved, so a shared read buffer would show up here.
        got_a = bytearray()
        got_b = bytearray()
        offset = 0
        while offset < size:
            piece = min(chunk, size - offset)
            got_a += op_a.read(offset, piece)
            got_b += op_b.read(offset, piece)
            offset += piece
        op_a.close()
        op_b.close()

        for label, name, sent, got in (("A", name_a, data_a, bytes(got_a)), ("B", name_b, data_b, bytes(got_b))):
            with open(_host_path(name), "rb") as fh:
                on_disk = fh.read()
            if _sha(on_disk) != _sha(sent):
                print(f"FAIL (concurrent handles, {label} on disk): {_sha(on_disk)} != {_sha(sent)}")
                ok = False
            elif _sha(got) != _sha(sent):
                print(f"FAIL (concurrent handles, {label} read back): {_sha(got)} != {_sha(sent)}")
                ok = False
            else:
                print(f"PASS (concurrent handles, connection {label}): {size} B written and read back byte-exact")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (concurrent handles): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree_a, conn_a)
        _teardown(tree_b, conn_b)


def test_write_to_readonly_handle_rejected(port: int) -> bool:
    """A handle opened GENERIC_READ only must refuse writes, in the handler and
    with a logged reason -- not by handing the bytes to the filesystem and
    hoping it says no. The file must be byte-identical afterwards."""
    name = "smoke_readonly_handle.bin"
    data = _seed_binary(name, 4096, seed=65)
    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = _open_read(tree, name)
        try:
            op.write(b"CORRUPTED", 0)
            print("FAIL (write to read-only handle): the write was accepted")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (write to read-only handle rejected): {type(e).__name__}")
        # The handle must still read correctly after the refusal.
        got = op.read(0, len(data))
        if got != data:
            print("FAIL (write to read-only handle): reads broke after the refused write")
            ok = False
        op.close()
        with open(_host_path(name), "rb") as fh:
            if fh.read() != data:
                print("FAIL (write to read-only handle): the file on disk changed")
                ok = False
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (write to read-only handle): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_transfer_cross_connection_rejected(port: int) -> bool:
    """The ownership check has to cover read/write/flush too. These ids are
    predictable (a small counter plus a slot index), so without it connection B
    could read -- or overwrite -- any file connection A has open."""
    name = "smoke_xconn_transfer.bin"
    data = _seed_binary(name, 2048, seed=66)
    conn_a, sess_a, tree_a = connect_session(port)
    conn_b, sess_b, tree_b = connect_session(port)
    ok = True
    try:
        victim = _open_write(tree_a, name, disposition=CreateDisposition.FILE_OPEN)

        thief = Open(tree_b, name)
        thief.file_id = victim.file_id
        thief._connected = True
        for label, action in (
            ("read", lambda: thief.read(0, 16)),
            ("write", lambda: thief.write(b"CORRUPTED", 0)),
            ("flush", lambda: thief.flush()),
        ):
            try:
                action()
                print(f"FAIL (cross-connection {label}): B used A's handle")
                ok = False
            except Exception as e:  # noqa: BLE001
                print(f"PASS (cross-connection {label} rejected): {type(e).__name__}")

        got = victim.read(0, len(data))
        if got != data:
            print("FAIL (cross-connection transfer): A's own handle stopped working")
            ok = False
        victim.close()
        with open(_host_path(name), "rb") as fh:
            if fh.read() != data:
                print("FAIL (cross-connection transfer): the file changed despite every attempt being rejected")
                ok = False
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (cross-connection transfer): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree_a, conn_a)
        _teardown(tree_b, conn_b)


def test_flush(port: int) -> bool:
    """flush_cmd must succeed (the Task 3 stub returned -1, i.e.
    STATUS_NOT_IMPLEMENTED, which iOS reports as a failed copy) and the data
    must be readable afterwards. A flush on a directory handle is legal too --
    clients send it -- and must not be refused.

    The payload is one negotiated chunk plus a kilobyte so the flush lands after
    a MULTI-request write, which is the shape a real copy has. It was a flat
    9000 bytes, which was a single request at the old 32768 ceiling and became
    an outright protocol error at v72's 8192 -- the size only ever meant "more
    than a trivial amount", so it is now expressed that way."""
    name = "smoke_flush.bin"
    connection, session, tree = connect_session(port)
    ok = True
    try:
        chunk = _negotiated_chunk(connection)
        data = _pattern_bytes(chunk + 1024, seed=67)
        op = _open_write(tree, name)
        _write_all(op, data, chunk)
        op.flush()
        print("PASS (flush after write): accepted")
        got = _read_all(op, len(data))
        op.close()
        if got != data:
            print("FAIL (flush): data read back after flush does not match")
            ok = False
        with open(_host_path(name), "rb") as fh:
            if fh.read() != data:
                print("FAIL (flush): on-disk data does not match")
                ok = False

        d = _create_file(tree, "", CreateDisposition.FILE_OPEN, FilePipePrinterAccessMask.GENERIC_READ,
                         CreateOptions.FILE_DIRECTORY_FILE, attributes=FileAttributes.FILE_ATTRIBUTE_DIRECTORY)
        d.flush()
        d.close()
        print("PASS (flush on a directory handle): accepted")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (flush): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_sync_failure_surfaces(port: int) -> bool:
    """A failed write-back must reach the client as a failure, not as a clean
    "copied".

    `FsFile::flush()` is `void flush() { sync(); }` -- it throws away the bool
    that says whether the data actually landed. `HalFile::sync()` exposes it,
    and `flush_cmd` / `write_cmd`'s WRITE_THROUGH branch now check it. This
    matters most for WRITE_THROUGH, where MS-SMB2 2.2.21 makes "on stable
    storage before the response" a promise the reply is asserting: a card
    failing mid-copy would otherwise hand iOS Files a successful copy, leave a
    corrupt book behind, and put nothing in diag.log.

    The failure is injected by the harness HAL, keyed on SYNCFAIL in the
    filename -- POSIX fsync() on a healthy scratch file never fails, so
    without an injector this branch would be present but never executed. See
    stub_hal/HalStorage.cpp's sync()."""
    name = "smoke_SYNCFAIL_flush.bin"
    data = _pattern_bytes(2048, seed=69)
    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = _open_write(tree, name)

        # An ordinary write still succeeds -- only the write-BACK is broken,
        # so this must not become a blanket "writes to this file fail".
        if op.write(data, 0) != len(data):
            print("FAIL (sync failure): the ordinary write did not report the full count")
            ok = False
        else:
            print("PASS (sync failure): an ordinary write is unaffected")

        try:
            op.flush()
            print("FAIL (sync failure): flush reported success despite the sync failing")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (sync failure): flush surfaced the failure: {type(e).__name__}")

        try:
            op.write(data, 0, write_through=True)
            print("FAIL (sync failure): WRITE_THROUGH reported success despite the sync failing")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (sync failure): WRITE_THROUGH surfaced the failure: {type(e).__name__}")

        # close() on this handle is EXPECTED to fail too -- the final write-back
        # goes through the same sync. That is asserted properly by
        # test_close_sync_failure_surfaces; here it just must not abort this
        # test, whose subject is flush and WRITE_THROUGH.
        try:
            op.close()
        except Exception:  # noqa: BLE001
            pass

        # The control case: the same operations on a normally-named file must
        # still succeed, or this test would pass for the wrong reason.
        clean = _open_write(tree, "smoke_sync_ok.bin")
        clean.write(data, 0, write_through=True)
        clean.flush()
        clean.close()
        with open(_host_path("smoke_sync_ok.bin"), "rb") as fh:
            if fh.read() != data:
                print("FAIL (sync failure control): the clean file's data is wrong")
                ok = False
            else:
                print("PASS (sync failure control): flush and WRITE_THROUGH still succeed on a healthy file")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (sync failure): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_close_sync_failure_surfaces(port: int) -> bool:
    """`close` is where the last data of an upload actually lands, and it is
    the same discarded bool as `flush`.

    On device the whole chain is one expression: `HalFile::close()` ->
    `FsBaseFile::close()` (`bool rtn = m_fFile->close(); ...; return rtn;`,
    FsFile.cpp:58-63) -> `FatFile::close()`, which IS `bool rtn = sync(); ...;
    return rtn;` (FatFile.cpp:128-132). iOS's copy sequence is
    write, write, ..., close -- so a card that fails its final write-back and a
    server that answers 0 hand the Files app a successful copy of a truncated
    book.

    Uses the same `SYNCFAIL` injector as the flush test; see
    stub_hal/HalStorage.cpp."""
    name = "smoke_SYNCFAIL_close.bin"
    data = _pattern_bytes(3000, seed=75)
    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = _open_write(tree, name)
        if op.write(data, 0) != len(data):
            print("FAIL (close sync failure): the write itself did not report the full count")
            ok = False
        try:
            op.close()
            print("FAIL (close sync failure): close reported success despite the final sync failing")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (close sync failure): close surfaced the failure: {type(e).__name__}")

        # Control: a normally-named file must still close cleanly, or this test
        # would pass for the wrong reason (e.g. every close failing).
        clean = _open_write(tree, "smoke_close_ok.bin")
        clean.write(data, 0)
        clean.close()
        with open(_host_path("smoke_close_ok.bin"), "rb") as fh:
            if fh.read() != data:
                print("FAIL (close sync failure control): the clean file's data is wrong")
                ok = False
            else:
                print("PASS (close sync failure control): a healthy handle still closes cleanly")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (close sync failure): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_failed_close_still_frees_the_slot(port: int) -> bool:
    """Slot bookkeeping must NOT depend on the sync succeeding.

    The open-file table is eight fixed slots. If a failed close skipped the
    release, a failing card would exhaust the table and every later create
    would be refused by the table-full path instead of by the real problem --
    and the diag line would name the wrong cause. Both release paths are
    covered:

      * `close_cmd` -- twelve opens, each closed with a failing sync, on ONE
        connection. Only one slot is ever needed at a time, so if the failure
        leaked it, open nine would fail.
      * `destruction_event` -- twelve connections, each dropped while still
        holding a handle whose sync will fail, then a thirteenth connection
        that must still be able to open something.

    HONEST LIMITATION: this asserts the *observable consequence* (slots come
    back) for both paths. It does NOT assert that `destruction_event` LOGS the
    failed close, because that line goes to the server process's diag output
    and this harness starts the server separately -- the test has no handle on
    its stderr. The log line was verified by reading the server's output by
    hand, not by an assertion here."""
    ok = True
    connection, session, tree = connect_session(port)
    try:
        for i in range(1, 13):
            try:
                op = _open_write(tree, f"smoke_SYNCFAIL_slot_{i}.bin")
            except Exception as e:  # noqa: BLE001
                print(f"FAIL (failed close frees slot): open {i}/12 was refused -- "
                      f"the table leaked slots: {type(e).__name__}: {e}")
                return False
            op.write(b"x" * 16, 0)
            try:
                op.close()
                print(f"FAIL (failed close frees slot): close {i}/12 unexpectedly reported success")
                ok = False
            except Exception:  # noqa: BLE001
                pass
        print("PASS (failed close frees slot, close_cmd): 12/12 opens succeeded behind 12 failed closes")
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (failed close frees slot, close_cmd): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)

    # destruction_event path: drop the connection while the handle is still
    # open AND its sync will fail.
    for i in range(1, 13):
        conn = Connection(uuid.uuid4(), "127.0.0.1", port)
        try:
            conn.connect(dialect=WORKING_DIALECT, timeout=5)
            sess = Session(conn, "x3", "x3", require_encryption=False)
            sess.connect()
            t = TreeConnect(sess, r"\\127.0.0.1\SD")
            t.connect()
            op = _open_write(t, f"smoke_SYNCFAIL_destr_{i}.bin")
            op.write(b"y" * 16, 0)
        except Exception as e:  # noqa: BLE001
            print(f"FAIL (failed close frees slot, destruction_event): connection {i}/12 could not "
                  f"open a file -- the table leaked: {type(e).__name__}: {e}")
            try:
                conn.transport.close()
            except Exception:  # noqa: BLE001
                pass
            return False
        # No close, no disconnect -- just drop the socket, so destruction_event
        # (not close_cmd) is what has to reclaim the slot.
        conn.transport.close()
        time.sleep(0.1)  # give the server a tick to notice the dead fd and cull it
    print("PASS (failed close frees slot, destruction_event): 12/12 dropped connections reclaimed their slots")

    connection, session, tree = connect_session(port)
    try:
        op = _open_write(tree, "smoke_close_slot_final.bin")
        op.write(b"z" * 8, 0)
        op.close()
        print("PASS (failed close frees slot): the table is still usable afterwards")
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (failed close frees slot): the table was unusable afterwards: {type(e).__name__}: {e}")
        ok = False
    finally:
        _teardown(tree, connection)
    return ok


def test_read_only_request_cannot_write(port: int) -> bool:
    """`writable` must mean "did the client ask to write", not "can the
    filesystem write".

    FILE_OPEN_IF + GENERIC_READ against a name that does NOT exist is the case
    that separates the two: `needsWriteAccess()` forces write mode because the
    file has to be created somehow, so the handle really is O_RDWR even though
    the client asked only to read. A real server answers
    STATUS_ACCESS_DENIED to a write through it. Before the fix the write
    landed on disk.

    The MAXIMUM_ALLOWED half of the same check is the other side of the coin
    and is asserted here too: that path is the ordinary iOS upload, and
    tightening the mask too far would break it."""
    denied_name = "smoke_ropolicy_denied.bin"
    allowed_name = "smoke_ropolicy_maxallowed.bin"
    for n in (denied_name, allowed_name):
        path = _host_path(n)
        if os.path.exists(path):
            os.remove(path)
    connection, session, tree = connect_session(port)
    ok = True
    try:
        # Created by a read-only request: the handle must refuse writes.
        op = _create_file(tree, denied_name, CreateDisposition.FILE_OPEN_IF,
                          FilePipePrinterAccessMask.GENERIC_READ, CreateOptions.FILE_NON_DIRECTORY_FILE)
        try:
            op.write(b"CORRUPTED", 0)
            print("FAIL (read-only request): a handle opened GENERIC_READ accepted a write")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (read-only request refuses writes even when the open had to create): {type(e).__name__}")
        op.close()
        with open(_host_path(denied_name), "rb") as fh:
            if fh.read() != b"":
                print("FAIL (read-only request): bytes landed on disk anyway")
                ok = False

        # MAXIMUM_ALLOWED must stay writable -- this is the normal iOS upload.
        payload = _pattern_bytes(4096, seed=70)
        op = _create_file(tree, allowed_name, CreateDisposition.FILE_OVERWRITE_IF,
                          FilePipePrinterAccessMask.MAXIMUM_ALLOWED, CreateOptions.FILE_NON_DIRECTORY_FILE)
        _write_all(op, payload)
        op.close()
        with open(_host_path(allowed_name), "rb") as fh:
            if fh.read() != payload:
                print("FAIL (MAXIMUM_ALLOWED upload): the bytes did not land")
                ok = False
            else:
                print("PASS (MAXIMUM_ALLOWED upload still writable): 4096 B byte-exact")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (read-only request policy): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_maximum_allowed_readonly_file(port: int) -> bool:
    """MAXIMUM_ALLOWED means "grant me what I am entitled to" (MS-SMB2 2.2.13),
    not "grant me write". Against a file with the read-only attribute the
    write-mode open fails on both SdFat filesystems (FatFile.cpp:581-585), and
    the retry path used to discard the downgraded read handle because the
    target was not a directory -- so the file could not be opened at all, and
    therefore could not be read. macOS/iOS send MAXIMUM_ALLOWED routinely, so
    that made an entire class of file invisible to a copy.

    The downgrade must be exactly that and no more: read works, write is still
    refused."""
    name = "smoke_readonly_attr.bin"
    data = _seed_binary(name, 3000, seed=68)
    path = _host_path(name)
    os.chmod(path, 0o444)
    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = _create_file(tree, name, CreateDisposition.FILE_OPEN,
                          FilePipePrinterAccessMask.MAXIMUM_ALLOWED, CreateOptions.FILE_NON_DIRECTORY_FILE)
        got = _read_all(op, len(data))
        if got != data:
            print("FAIL (MAXIMUM_ALLOWED on a read-only file): read back the wrong bytes")
            ok = False
        else:
            print("PASS (MAXIMUM_ALLOWED on a read-only file): opened and read byte-exact")
        try:
            op.write(b"CORRUPTED", 0)
            print("FAIL (MAXIMUM_ALLOWED on a read-only file): the downgraded handle accepted a write")
            ok = False
        except Exception as e:  # noqa: BLE001
            print(f"PASS (MAXIMUM_ALLOWED downgraded handle refuses writes): {type(e).__name__}")
        op.close()
        with open(path, "rb") as fh:
            if fh.read() != data:
                print("FAIL (MAXIMUM_ALLOWED on a read-only file): the file changed")
                ok = False
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (MAXIMUM_ALLOWED on a read-only file): {type(e).__name__}: {e}")
        return False
    finally:
        os.chmod(path, 0o644)  # leave the scratch tree deletable
        _teardown(tree, connection)


# ---------------------------------------------------------------------------
# v74: NAMED STREAMS (NTFS alternate data streams) ON A FAT CARD.
#
# THIS SECTION EXISTS BECAUSE THERE WAS NONE. v74 taught createCmd to accept a
# named stream and discard it, and not one of this suite's checks could see a
# single thing about it -- including a change that would have REBOOTED THE X3
# at the close of every accepted stream. See "the guard" below.
#
# The mechanism, read rather than assumed:
#   * splitStreamSuffix() (SmbFileHandlers.cpp:329) cuts "path:stream" apart
#     before Storage ever sees the path, and is called at :1194, before every
#     filesystem step in createCmd.
#   * "file::$DATA" -- empty stream name, $DATA type -- is NOT a stream. It is
#     the file itself (:320-324), so it must open the FILE.
#   * OPENING an absent named stream (FILE_OPEN / FILE_OVERWRITE) is
#     OBJECT_NAME_NOT_FOUND; CREATING one is ACCEPTED (:1211-1219) and given a
#     slot flagged isNullStream (:1282) with NO HalFile behind it.
#   * That slot is a sink: writes report full success and are discarded
#     (:3151), reads answer END_OF_FILE (:2991), flush succeeds (:3277),
#     set_info FILE_BASIC/END_OF_FILE are successful no-ops and everything else
#     is refused (:3953), query_directory answers NOT_A_DIRECTORY (:2165), and
#     query_info reports zero length (:2674).
#
# WHY ACCEPTING IT AT ALL IS THE POINT. diag22.log, twice, identically: the
# iPhone created the book, opened it for writing, tried to write a 32-byte
# com.apple.FinderInfo stream, was refused -- and then DELETED THE BOOK IT HAD
# JUST CREATED and abandoned the copy. Writing FinderInfo is not decoration to
# iOS; it is part of creating the file.
#
# THE GUARD, and why these checks must be able to see a dead server.
# releaseSlot() (SmbFileHandlers.cpp:638) is
#     const bool closed = e.isNullStream ? true : e.file.close();
# and it used to close unconditionally. The device's HalFile::close() is
# `StorageLock lock; assert(impl != nullptr); ...` with no null branch, and the
# assert is in the shipping firmware -- so an empty handle does not return an
# error there, it ABORTS. The desktop stub used to `return false` instead,
# which is exactly why the suite stayed green. Both halves are fixed now (the
# guard, and stub_hal/HalStorage.cpp:667 asserts like the device); README.md's
# "Eighth divergence" section is the write-up.
#
# Consequence for THIS file: remove that guard and the harness process dies
# mid-request. A check that only inspects a returned status would report some
# generic connection error and say nothing about what happened, so every check
# below routes its failures through _stream_check_failed(), which probes the
# port and says THE SERVER DIED when it did. Measured against a mutant with the
# guard removed: close() raises SMBConnectionClosed after ~1.4s (it does not
# hang), and the port then refuses connections.

# The open-file table is eight fixed slots (SmbFileHandlers.cpp:133
# kMaxOpenFiles); discarded streams may hold at most kMaxOpenFiles - 2 of them
# (:1259 kMaxNullStreamSlots), so a stream flood can only ever cost the client
# its own streams and never an ordinary file create.
OPEN_FILE_SLOTS = 8
MAX_DISCARDED_STREAM_SLOTS = OPEN_FILE_SLOTS - 2

# The stream iOS actually sends, and its actual length (diag22.log:
# "write first: len=32"). FinderInfo is a fixed 32-byte structure.
FINDER_INFO_STREAM = "com.apple.FinderInfo"
FINDER_INFO_BYTES = bytes(range(32))


def _server_is_alive(port: int, timeout: float = 2.0) -> bool:
    """Will the harness still accept a TCP connection?

    Deliberately below the SMB layer: the question is whether the PROCESS is
    there, and smbprotocol cannot answer that -- its exception for "the server
    aborted mid-request" (SMBConnectionClosed) is the same one it raises for an
    ordinary transport hiccup."""
    probe = socket.socket()
    probe.settimeout(timeout)
    try:
        probe.connect(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        probe.close()


def _stream_check_failed(port: int, label: str, exc: Exception) -> bool:
    """One FAIL line, naming a dead server as a dead server. Always False."""
    if not _server_is_alive(port):
        print(f"FAIL ({label}): THE SERVER DIED -- it is no longer accepting connections. "
              f"On the X3 this is a reboot, not an error reply "
              f"(releaseSlot()'s isNullStream guard, SmbFileHandlers.cpp:638). "
              f"Last client-side symptom: {type(exc).__name__}: {exc}")
    else:
        print(f"FAIL ({label}): {type(exc).__name__}: {exc}")
    return False


def _create_stream(tree, path: str, stream: str, disposition=CreateDisposition.FILE_OVERWRITE_IF):
    """Opens `path:stream`. FILE_OVERWRITE_IF by default because that is a
    CREATING disposition, which is the half v74 accepts (SmbFileHandlers.cpp:
    1211) -- pass FILE_OPEN to exercise the half that must still say
    "not found"."""
    return _create_file(
        tree,
        f"{path}:{stream}",
        disposition,
        FilePipePrinterAccessMask.FILE_READ_DATA
        | FilePipePrinterAccessMask.FILE_WRITE_DATA
        | FilePipePrinterAccessMask.DELETE,
        CreateOptions.FILE_NON_DIRECTORY_FILE,
    )


def test_ios_finderinfo_copy_sequence(port: int) -> bool:
    """THE REGRESSION TEST FOR THE BUG v74 EXISTS TO FIX: replay the iPhone's
    whole copy, in the order diag22.log recorded it.

        create disp=2 path=...epub      the book, created
        create acc=0x00020006 ...epub   reopened for writing, payload written
        create ...epub:com.apple.FinderInfo
        write len=32
        close

    Every step must succeed, the reported write counts must equal what was
    sent -- and then the half that matters most: THE BOOK MUST STILL BE THERE,
    byte for byte. Closing the STREAM handle reaching the FILE is the failure
    this asserts against, and it is not hypothetical: the stream slot carries
    the full "base:stream" path precisely so that a handler which forgets the
    isNullStream check fails on an impossible name instead of quietly operating
    on the book (SmbFileHandlers.cpp:1283-1286).

    Nothing containing ':' may appear on the card either. FAT cannot hold such
    a name, so one appearing means the split at :1194 was bypassed."""
    label = "iOS FinderInfo copy sequence"
    rel = "smoke_test_stream_copy"
    book = "講者腳本_中文_X3.epub"     # the real name from the field log
    _seed_tree(rel, {})
    smb_book = f"{rel}\\{book}"
    ok = True
    connection = tree = None
    # Bound before the try only so that a fixture failure can never reach the
    # comparison below as a NameError: an exception raised out of a check kills
    # main() and takes every remaining result with it, which is the one failure
    # mode this section must not have.
    payload = b""
    try:
        connection, session, tree = connect_session(port)
        # Sized off NEGOTIATE, never a literal: the transfer ceiling is a RAM
        # decision that has already been retuned once (32768 -> 8192, v72) and
        # is expected to move again. The +137 keeps the last request partial.
        chunk = _negotiated_chunk(connection)
        payload = _pattern_bytes(3 * chunk + 137, seed=74)
        requests = (len(payload) + chunk - 1) // chunk

        # Two separate CREATEs, as the phone sends them: FILE_CREATE for the
        # name, then a second open for the data.
        _create_file(tree, smb_book, CreateDisposition.FILE_CREATE,
                     FilePipePrinterAccessMask.GENERIC_WRITE,
                     CreateOptions.FILE_NON_DIRECTORY_FILE).close()
        f = _open_write(tree, smb_book, disposition=CreateDisposition.FILE_OPEN)
        _write_all(f, payload, chunk)   # raises on any short write
        f.close()
        print(f"PASS ({label}, book): {len(payload)} B written in {requests} requests of <= {chunk} B")

        # The step that used to kill the whole copy.
        st = _create_stream(tree, smb_book, FINDER_INFO_STREAM)
        written = st.write(FINDER_INFO_BYTES, 0)
        if written != len(FINDER_INFO_BYTES):
            print(f"FAIL ({label}): stream write reported {written} of {len(FINDER_INFO_BYTES)} bytes")
            ok = False
        st.close()
        print(f"PASS ({label}, stream): '{FINDER_INFO_STREAM}' created, {written} B accepted and discarded, closed")
    except Exception as e:  # noqa: BLE001
        # connect_session() is inside the try here, unlike the checks above it:
        # if a previous check killed the server this one must report that, not
        # raise out of main() and take the remaining results with it.
        ok = _stream_check_failed(port, label, e)
    finally:
        if tree is not None:
            _teardown(tree, connection)

    # Filesystem assertions run WHATEVER happened on the wire -- if the server
    # died at the stream close, whether it took the book with it is still the
    # thing worth knowing.
    host_book = _host(rel, book)
    if not os.path.exists(host_book):
        print(f"FAIL ({label}): THE BOOK IS GONE -- a stream operation reached the base file")
        return False
    with open(host_book, "rb") as fh:
        disk = fh.read()
    if disk != payload:
        print(f"FAIL ({label}): the book's bytes changed: {len(disk)} B on disk, sha {_sha(disk)[:16]} "
              f"vs {len(payload)} B written, sha {_sha(payload)[:16]}")
        ok = False
    else:
        print(f"PASS ({label}): the book survived the stream and is byte-exact "
              f"({len(disk)} B, sha {_sha(disk)[:16]})")

    junk = sorted(n for n in os.listdir(_host(rel)) + os.listdir(SD_ROOT) if ":" in n)
    if junk:
        print(f"FAIL ({label}): a name with ':' reached the card: {junk}")
        ok = False
    else:
        print(f"PASS ({label}): no ':' name was created")
    return ok


def test_discarded_stream_is_a_sink_not_a_file(port: int) -> bool:
    """A discarded stream handle must behave like a sink, never like the file
    it is named after.

    The base file is seeded on the host with known bytes and is NOT empty, so
    every assertion here can tell "the stream answered" from "the file
    answered": a read that returned data, a query_info that reported the real
    size, a truncation that landed, or a delete/rename that took the book would
    each be visible.

    The two refusals are the sharp end. FILE_DISPOSITION on a stream must never
    become delete-on-close on the base file, and rename must never move it --
    those are the two set_info classes that would otherwise be handed a path
    with a colon in it (SmbFileHandlers.cpp:3953)."""
    label = "discarded stream is a sink"
    rel = "smoke_test_stream_sink"
    seeded = _seed_binary(f"{rel}/base.epub", 5000, seed=75)
    smb_base = f"{rel}\\base.epub"
    ok = True
    connection = tree = None
    try:
        connection, session, tree = connect_session(port)
        st = _create_stream(tree, smb_base, FINDER_INFO_STREAM)
        st.write(FINDER_INFO_BYTES, 0)   # give a leak somewhere to show up as

        try:
            got = st.read(0, 64)
            print(f"FAIL ({label}, read): returned {len(got)} bytes instead of END_OF_FILE")
            ok = False
        except EndOfFile:
            print(f"PASS ({label}, read): END_OF_FILE -- nothing was ever stored")

        std = _query_info(tree, st, InfoType.SMB2_0_INFO_FILE,
                          FileInformationClass.FILE_STANDARD_INFORMATION)
        # Same MS-FSCC 2.4.41 layout test_query_info_file_classes() decodes.
        eof = struct.unpack("<Q", std[8:16])[0]
        if eof != 0:
            print(f"FAIL ({label}, query_info): the stream reports {eof} bytes "
                  f"(the base file is {len(seeded)} -- it answered for the FILE)")
            ok = False
        else:
            print(f"PASS ({label}, query_info): the stream reports 0 bytes, not the base file's {len(seeded)}")

        # The two successful no-ops. END_OF_FILE(0) is included because if it
        # ever reached the filesystem it would TRUNCATE the book -- the final
        # byte comparison below is what proves it did not.
        for name, info_class, buf in (
            ("FILE_BASIC", FileInformationClass.FILE_BASIC_INFORMATION, _basic_buffer()),
            ("FILE_END_OF_FILE", FileInformationClass.FILE_END_OF_FILE_INFORMATION, struct.pack("<Q", 0)),
        ):
            try:
                _set_info(tree, st, info_class, buf)
                print(f"PASS ({label}, set_info {name}): accepted as a no-op")
            except Exception as e:  # noqa: BLE001
                print(f"FAIL ({label}, set_info {name}): refused with {type(e).__name__}: {e}")
                ok = False

        stolen = f"{rel}\\stolen.epub"
        refusals = (
            ("set_info FILE_DISPOSITION (delete-on-close)",
             lambda: _set_info(tree, st, FileInformationClass.FILE_DISPOSITION_INFORMATION, b"\x01")),
            ("set_info FILE_RENAME",
             lambda: _set_info(tree, st, FileInformationClass.FILE_RENAME_INFORMATION, _rename_buffer(stolen))),
            ("query_directory",
             lambda: st.query_directory("*", FileInformationClass.FILE_ID_BOTH_DIRECTORY_INFORMATION)),
        )
        for name, call in refusals:
            try:
                call()
                print(f"FAIL ({label}, {name}): UNEXPECTEDLY SUCCEEDED on a discarded stream")
                ok = False
            except Exception as e:  # noqa: BLE001
                print(f"PASS ({label}, {name}): refused with {type(e).__name__}")

        st.flush()
        print(f"PASS ({label}, flush): succeeded -- nothing was written, so nothing is unwritten")
        st.close()

        if os.path.exists(_host(rel, "stolen.epub")):
            print(f"FAIL ({label}): the refused rename moved the base file anyway")
            ok = False
    except Exception as e:  # noqa: BLE001
        ok = _stream_check_failed(port, label, e)
    finally:
        if tree is not None:
            _teardown(tree, connection)

    host_base = _host(rel, "base.epub")
    if not os.path.exists(host_base):
        print(f"FAIL ({label}): THE BASE FILE IS GONE -- a stream operation reached it")
        return False
    with open(host_base, "rb") as fh:
        disk = fh.read()
    if disk != seeded:
        print(f"FAIL ({label}): the base file changed: {len(disk)} B on disk vs {len(seeded)} seeded")
        ok = False
    else:
        print(f"PASS ({label}): the base file is untouched after all of that ({len(disk)} B)")
    return ok


def test_absent_stream_not_found_and_dollar_data_is_the_file(port: int) -> bool:
    """The two halves of splitStreamSuffix() that are NOT the discard path.

    1. OPENING a named stream that was never created is OBJECT_NAME_NOT_FOUND,
       exactly as before v74 (SmbFileHandlers.cpp:1213-1219). iOS asks this
       roughly 50 times per browse and carries on every time, so answering
       anything else -- including "accepted" -- would be a regression in the
       opposite direction from the one v74 fixed.
    2. "file::$DATA" is the UNNAMED data stream, which is not a stream at all:
       it is the file's own contents (:320-324). Proved by READING THE BYTES
       BACK through that name rather than by the open merely succeeding -- an
       open that quietly handed back a discarded-stream sink would also not
       raise, and would then read as empty."""
    label = "absent stream + ::$DATA"
    rel = "smoke_test_stream_names"
    seeded = _seed_binary(f"{rel}/book.epub", 3000, seed=76)
    smb_base = f"{rel}\\book.epub"
    ok = True
    connection = tree = None
    try:
        connection, session, tree = connect_session(port)
        for disposition_name, disposition in (("FILE_OPEN", CreateDisposition.FILE_OPEN),
                                              ("FILE_OVERWRITE", CreateDisposition.FILE_OVERWRITE)):
            try:
                _create_stream(tree, smb_base, "never.created", disposition).close()
                print(f"FAIL ({label}, {disposition_name}): opening an absent stream SUCCEEDED")
                ok = False
            except ObjectNameNotFound:
                print(f"PASS ({label}, {disposition_name}): absent stream reported as not found")

        d = _create_file(tree, f"{smb_base}::$DATA", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.FILE_READ_DATA, CreateOptions.FILE_NON_DIRECTORY_FILE)
        got = _read_all(d, len(seeded))
        d.close()
        if got != seeded:
            print(f"FAIL ({label}, ::$DATA): read {len(got)} B, sha {_sha(got)[:16]} -- "
                  f"expected {len(seeded)} B, sha {_sha(seeded)[:16]}")
            ok = False
        else:
            print(f"PASS ({label}, ::$DATA): opened the FILE and read back all {len(got)} B, sha "
                  f"{_sha(got)[:16]}")
        return ok
    except Exception as e:  # noqa: BLE001
        return _stream_check_failed(port, label, e)
    finally:
        if tree is not None:
            _teardown(tree, connection)


def test_discarded_stream_slot_cap(port: int) -> bool:
    """The cap on how much of the open-file table discarded streams may hold.

    Before v74 a stream create consumed no slot at all -- it was refused before
    allocation. Now each holds one of eight for as long as the client wants,
    so without a cap a client spraying streams would make an ORDINARY FILE
    CREATE fail: the copy breaks for a reason the user cannot see. That last
    assertion, not the refusal, is the point of the cap
    (SmbFileHandlers.cpp:1246-1268).

    Also checks the slots come BACK, because releaseSlot() is the function this
    whole section is really about: a cap that latched would look identical here
    until the next stream create.

    The client ASKS FOR ALL EIGHT rather than for one past the cap, and that is
    not padding: with the reserve widened to kMaxOpenFiles a "one too many"
    probe still leaves a free slot, so the starvation assertion below -- the
    one the cap exists for -- would not fire at all. Asking for the whole table
    is what makes it load-bearing (measured against exactly that mutant)."""
    label = "discarded stream slot cap"
    rel = "smoke_test_stream_cap"
    seeded = _seed_binary(f"{rel}/base.epub", 1500, seed=77)
    smb_base = f"{rel}\\base.epub"
    ok = True
    connection = tree = None
    held = []
    try:
        connection, session, tree = connect_session(port)
        refusal = None
        for i in range(OPEN_FILE_SLOTS):
            try:
                held.append(_create_stream(tree, smb_base, f"discarded.{i}"))
            except Exception as e:  # noqa: BLE001
                refusal = type(e).__name__
                break
        if len(held) != MAX_DISCARDED_STREAM_SLOTS or refusal is None:
            print(f"FAIL ({label}): {len(held)} streams accepted before "
                  f"{refusal or 'no refusal at all'} -- expected exactly {MAX_DISCARDED_STREAM_SLOTS} "
                  f"of {OPEN_FILE_SLOTS} slots")
            ok = False
        else:
            print(f"PASS ({label}): exactly {len(held)} of {OPEN_FILE_SLOTS} slots taken, "
                  f"the next refused with {refusal}")

        # THE POINT OF THE CAP.
        try:
            ordinary = _create_file(tree, f"{rel}\\ordinary.epub", CreateDisposition.FILE_CREATE,
                                    FilePipePrinterAccessMask.GENERIC_WRITE,
                                    CreateOptions.FILE_NON_DIRECTORY_FILE)
            ordinary.write(b"a real book", 0)
            ordinary.close()
            # Checked on the card, not just "it did not raise": this whole
            # section is about handles that report success and touch nothing.
            if os.path.exists(_host(rel, "ordinary.epub")):
                print(f"PASS ({label}): an ORDINARY file create still succeeded with the cap full")
            else:
                print(f"FAIL ({label}): the ordinary file was reported created but is not on the card")
                ok = False
        except Exception as e:  # noqa: BLE001
            print(f"FAIL ({label}): an ordinary file create was starved by the streams: "
                  f"{type(e).__name__}: {e}")
            ok = False

        for handle in held:
            handle.close()
        held = []
        # Recycled, not latched: the same create that was just refused works.
        _create_stream(tree, smb_base, "after.release").close()
        print(f"PASS ({label}): the slots came back -- a stream create works again after the releases")
    except Exception as e:  # noqa: BLE001
        ok = _stream_check_failed(port, label, e)
    finally:
        for handle in held:
            try:
                handle.close()
            except Exception:  # noqa: BLE001
                pass
        if tree is not None:
            _teardown(tree, connection)

    host_base = _host(rel, "base.epub")
    if not os.path.exists(host_base) or open(host_base, "rb").read() != seeded:
        print(f"FAIL ({label}): the base file did not survive {MAX_DISCARDED_STREAM_SLOTS} streams named after it")
        ok = False
    return ok


# ===========================================================================
# v76: THE GUARD THAT REFUSED A DELETE BECAUSE THE FOLDER WAS BEING LOOKED AT.
#
# diag24.log, an empty folder the iPhone's Files app would not delete:
#
#     query_directory done: 2 responses, buffer 4096, path=/Test_go好
#     set_info disposition reject: path also open on ctx=... path=/Test_go好
#
# iOS enumerates the folder to confirm it is empty, KEEPS that listing handle,
# and only then opens a second handle to delete through. The old guard refused
# whenever ANY other handle held the path, and its own comment asserted the
# opposite of what the phone actually does ("real clients delete with exactly
# one handle").
#
# WHAT CHANGED, read out of src/network/SmbFileHandlers.cpp rather than assumed:
#
#   * otherWritebackHandleOn() (:614) skips a handle that is a READ-ONLY
#     DIRECTORY -- `e.isDirectory && !e.writable` -- and nothing else. Four call
#     sites: deleteOnClose() (:810), setDisposition() (:3877), and BOTH rename
#     guards, source (:3659) and destination (:3664).
#   * refusals from disposition and rename now answer STATUS_SHARING_VIOLATION
#     (kSetInfoSharingViolation, :549) rather than STATUS_INVALID_PARAMETER.
#     "Right request, wrong moment" versus "your request was malformed" -- only
#     the first is a status a client will wait on and retry.
#   * because a read-only directory handle now SURVIVES a delete or a rename, it
#     is left holding a directory entry that has been freed or moved, so
#     markOtherHandlesStale() (:554) flags every other handle on the path
#     afterwards -- deleteOnClose() (:823), and rename for BOTH the old name
#     (:3789, before slot->path is overwritten) and the new one (:3797).
#     OpenFileEntry::stale (:188) is then read by the three paths that WRITE
#     through a handle -- setBasicInfo() (:3999), setDisposition() (:3826) and
#     renameOpenHandle() (:3643) -- and captured by both close paths
#     (closeCmd :1769, destructionEvent :1872) before releaseSlot() clears the
#     slot, so a delete-on-close armed BEFORE the path went away is dropped
#     rather than run BY PATH against whatever now holds the name.
#
# WHY THE PREDICATE IS `isDirectory && !writable` AND NOT JUST `!writable`.
# This is the load-bearing part, and the whole reason
# test_delete_still_blocked_by_writeback_handles() exists. `writable` records
# WHAT THE CLIENT ASKED FOR, not what was opened: needsWriteAccess() (:969)
# forces write mode for a truncating disposition regardless of desired_access,
# so FILE_OVERWRITE + GENERIC_READ opens O_RDWR|O_TRUNC -- a handle that has
# ALREADY truncated the file and is DIR_DIRTY -- while `writable` stays false
# because GENERIC_READ is in neither write mask (:1664). The first draft of the
# fix used `!writable` alone and would have declared exactly that handle
# harmless. `isDirectory` has no such gap: it comes from file.isDirectory(),
# the object actually opened (:1611), and a truncating open is never one.
#
# Separately, and pinned by test_protected_names_are_case_insensitive():
# ProtectedPath::isProtectedNameView() (src/util/ProtectedPath.cpp:17) compares
# with strncasecmp() instead of memcmp(), because FAT is case-insensitive and
# `xtcache` therefore reaches the protected `XTCache`.
#
# NON-VACUITY. Every check below was run against a mutant of today's tree, and
# the mutation each one answers to is named in its docstring. The one that
# needed it most is the stale check: the POSIX stub's setTimestamp() stats the
# path and fails on its own when nothing is there, so "refused, and the path is
# absent" would have been a false green. It is checked against a REPLACEMENT
# planted at the same name instead, which is also the real hazard.
# ===========================================================================

# The two statuses this section is about. Asserted numerically: smbprotocol has
# a distinct exception class for each, so `type(e).__name__` reads fine and
# still cannot tell you which value came off the wire.
STATUS_SHARING_VIOLATION = 0xC0000043
STATUS_INVALID_PARAMETER = 0xC000000D

# A LastWriteTime well inside FAT's 1980-2099 range, far from "now", so a
# stamp that lands is unmistakable in a mtime comparison. 2020-09-13 12:26:40Z.
STALE_STAMP_UNIX = 1_600_000_000


def _open_dir_to_delete(tree, name: str):
    """The SECOND handle iOS opens once its listing has said the folder is
    empty: a directory handle carrying DELETE, sharing everything.

    Deliberately distinct from _open_dir()'s GENERIC_READ listing handle --
    the two are the two sides of the reported bug, and a check that used one
    helper for both would not be reproducing anything."""
    d = Open(tree, name)
    d.create(
        ImpersonationLevel.Impersonation,
        FilePipePrinterAccessMask.DELETE,
        FileAttributes.FILE_ATTRIBUTE_DIRECTORY,
        ShareAccess.FILE_SHARE_READ | ShareAccess.FILE_SHARE_WRITE | ShareAccess.FILE_SHARE_DELETE,
        CreateDisposition.FILE_OPEN,
        CreateOptions.FILE_DIRECTORY_FILE,
    )
    return d


def _set_info_result(tree, handle, info_class: int, buffer: bytes):
    """None if the SET_INFO was ACCEPTED, else (exception type name, NT status).

    The suite's older helpers return only `type(e).__name__`, which was right
    while every refusal carried the same status. v76 changed the status on
    purpose, so these checks need the number."""
    try:
        _set_info(tree, handle, info_class, buffer)
        return None
    except Exception as e:  # noqa: BLE001
        return type(e).__name__, getattr(e, "status", None)


def _arm_delete(tree, handle):
    """FILE_DISPOSITION_INFORMATION = 1. Same return shape as above."""
    return _set_info_result(tree, handle, FileInformationClass.FILE_DISPOSITION_INFORMATION, b"\x01")


def _describe(result) -> str:
    if result is None:
        return "ACCEPTED"
    name, status = result
    return f"{name} status={'None' if status is None else hex(status)}"


def _must_be_sharing_violation(label: str, what: str, result) -> bool:
    """'Refused' AND 'refused with the right status', in one place.

    Both halves matter and for different reasons: the refusal is the safety
    property, and the status is what tells a real client to wait and retry
    rather than to give up on the request as malformed."""
    if result is None:
        print(f"FAIL ({label}): {what} was ACCEPTED -- the guard is gone, not merely narrowed")
        return False
    name, status = result
    if status != STATUS_SHARING_VIOLATION:
        print(f"FAIL ({label}): {what} was refused with {_describe(result)}, expected "
              f"{hex(STATUS_SHARING_VIOLATION)} STATUS_SHARING_VIOLATION -- "
              f"{hex(STATUS_INVALID_PARAMETER)} tells the client its request was malformed")
        return False
    print(f"PASS ({label}): {what} refused with {hex(STATUS_SHARING_VIOLATION)} ({name})")
    return True


def test_ios_listing_handle_does_not_block_delete(port: int) -> bool:
    """THE REPORTED BUG: the empty folder the iPhone could not delete.

    Replays diag24's shape. B opens the folder, enumerates it to exhaustion --
    which is how the Files app decides it is empty, and is why the log shows
    "2 responses" for a folder holding nothing but '.' and '..' -- and KEEPS
    the handle. A then opens a second handle carrying DELETE and sets the
    disposition, the step that used to be refused merely because B existed.

    Three assertions, because "the call did not raise" is the weakest of them:
    the disposition is ACCEPTED, the directory is really gone from the host
    filesystem after A closes, and B's own close still succeeds afterwards --
    B is left holding an entry that was freed under it, which is what the
    stale flag exists for and what test_stale_handle_after_its_entry_is_deleted
    pins in detail.

    Mutation: restore otherWritebackHandleOn()'s old body (`!e.inUse ||
    &e == self` only) and the disposition comes back SharingViolation."""
    rel = "smoke_test_ios_dirdel"
    _seed_tree(rel, {}, subdirs=("Test_go好",))
    target = f"{rel}\\Test_go好"
    label = "iOS folder delete"
    ok = True
    ca, sa, ta = connect_session(port)
    cb, sb, tb = connect_session(port)
    try:
        b = _open_dir(tb, target)
        entries, trips = _drain_listing(b)
        got = _entry_names(entries)
        if not _dots_present(got, label):
            return False
        if _real_only(got):
            print(f"FAIL ({label}): the folder is not empty: {sorted(_real_only(got))!r}")
            return False
        print(f"PASS ({label}): B enumerated the empty folder to exhaustion in {trips} round trips "
              f"and is still holding the handle")

        a = _open_dir_to_delete(ta, target)
        result = _arm_delete(ta, a)
        if result is not None:
            print(f"FAIL ({label}): the disposition was refused ({_describe(result)}) while B held "
                  f"nothing but a read-only listing handle -- THIS IS THE REPORTED BUG")
            a.close()
            b.close()
            return False
        print(f"PASS ({label}): the disposition was accepted with B's listing handle still open")

        # Its own try: deleteOnClose() re-applies the guard at CLOSE time
        # (SmbFileHandlers.cpp:806), which is a SECOND call site and a second
        # way for this to be refused. A close that raises here means the
        # disposition was accepted and then the delete was refused anyway, and
        # that deserves to be said rather than collected by the outer handler.
        try:
            a.close()
        except Exception as e:  # noqa: BLE001
            print(f"FAIL ({label}): A's close FAILED ({type(e).__name__}: {e}) -- the disposition was "
                  f"accepted but deleteOnClose()'s own guard still refused the removal")
            b.close()
            return False
        if os.path.isdir(_host(rel, "Test_go好")):
            print(f"FAIL ({label}): the folder is STILL on the host filesystem after A closed -- "
                  f"the disposition was accepted and then did nothing")
            ok = False
        else:
            print(f"PASS ({label}): the folder is gone from the card")

        try:
            b.close()
            print(f"PASS ({label}): B's listing handle still closes cleanly over the freed entry")
        except Exception as e:  # noqa: BLE001
            print(f"FAIL ({label}): B's close failed after the folder went away: {type(e).__name__}: {e}")
            ok = False
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL ({label}): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tb, cb)
        _teardown(ta, ca)


def test_delete_still_blocked_by_writeback_handles(port: int) -> bool:
    """THE NARROWING MUST NOT HAVE BECOME THE DELETION OF THE CHECK.

    Three other handles, each of which must STILL block a delete, and each
    failing a different half of `isDirectory && !writable`:

      1. a writable FILE handle -- writable, and not a directory;
      2. a READ-ONLY FILE handle -- not writable, but not a directory either. A
         file handle caches the entry and the cluster chain whatever access it
         was opened with, so the exemption must not extend to it;
      3. FILE_OVERWRITE + GENERIC_READ -- THE ONE THE FIRST DRAFT LET THROUGH.
         `writable` is false (GENERIC_READ is in no write mask) but the server
         had to open O_RDWR|O_TRUNC to honour the disposition, so the file HAS
         ALREADY BEEN TRUNCATED and the handle is DIR_DIRTY. This check asserts
         the truncation really happened, so it is demonstrably that handle and
         not just another read-only one.

    Every refusal must also carry STATUS_SHARING_VIOLATION, and every file must
    still be on the card once both handles are closed.

    Mutation: widen otherWritebackHandleOn() to skip every handle and all three
    deletes go through."""
    rel = "smoke_test_delete_blockers"
    label = "delete still blocked"
    seeded = b"CONTENT-THAT-MUST-SURVIVE"
    names = {"writable.txt": seeded, "readonly.txt": seeded, "truncated.txt": seeded}
    _seed_tree(rel, {})
    for name, payload in names.items():
        with open(_host(rel, name), "wb") as fh:
            fh.write(payload)
    ok = True
    ca, sa, ta = connect_session(port)
    cb, sb, tb = connect_session(port)
    try:
        # 1. A writable handle.
        b = _open_rw(tb, f"{rel}\\writable.txt")
        a = _create_file(ta, f"{rel}\\writable.txt", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.DELETE,
                         CreateOptions.FILE_NON_DIRECTORY_FILE)
        ok = _must_be_sharing_violation(label, "a delete under a WRITABLE file handle",
                                        _arm_delete(ta, a)) and ok
        a.close()
        b.close()

        # 2. A read-only FILE handle -- the case `!writable` alone would have
        #    exempted along with the directory it was aimed at.
        b = _create_file(tb, f"{rel}\\readonly.txt", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.GENERIC_READ, CreateOptions.FILE_NON_DIRECTORY_FILE)
        a = _create_file(ta, f"{rel}\\readonly.txt", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.DELETE,
                         CreateOptions.FILE_NON_DIRECTORY_FILE)
        ok = _must_be_sharing_violation(label, "a delete under a READ-ONLY file handle",
                                        _arm_delete(ta, a)) and ok
        a.close()
        b.close()

        # 3. FILE_OVERWRITE + GENERIC_READ: slot->writable is false and the
        #    file is already truncated. Review's counter-example to `!writable`.
        b = _create_file(tb, f"{rel}\\truncated.txt", CreateDisposition.FILE_OVERWRITE,
                         FilePipePrinterAccessMask.GENERIC_READ, CreateOptions.FILE_NON_DIRECTORY_FILE)
        size_now = os.path.getsize(_host(rel, "truncated.txt"))
        if size_now != 0:
            print(f"FAIL ({label}): FILE_OVERWRITE + GENERIC_READ left the file at {size_now} bytes; "
                  f"this check is only meaningful if that open really truncated, which is what makes "
                  f"the handle dangerous while slot->writable stays false")
            ok = False
        else:
            print(f"PASS ({label}): FILE_OVERWRITE + GENERIC_READ truncated the file (0 bytes) while "
                  f"asking for no write access at all")
        a = _create_file(ta, f"{rel}\\truncated.txt", CreateDisposition.FILE_OPEN,
                         FilePipePrinterAccessMask.GENERIC_READ | FilePipePrinterAccessMask.DELETE,
                         CreateOptions.FILE_NON_DIRECTORY_FILE)
        ok = _must_be_sharing_violation(label, "a delete under an ALREADY-TRUNCATED read-only handle",
                                        _arm_delete(ta, a)) and ok
        a.close()
        b.close()

        missing = [n for n in names if not os.path.exists(_host(rel, n))]
        if missing:
            print(f"FAIL ({label}): {missing!r} were deleted despite the refusals")
            ok = False
        else:
            print(f"PASS ({label}): all three files are still on the card")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL ({label}): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tb, cb)
        _teardown(ta, ca)


def test_rename_guards_after_the_narrowing(port: int) -> bool:
    """RENAME, BOTH GUARDS -- the same predicate one step to the left.

    Renaming a folder while iOS holds its listing handle is the same wall as
    deleting one, and the log that produced this fix has the user renaming
    things in that very session. So the narrowing is applied to rename too, and
    all three of its consequences are pinned here:

      * renaming a DIRECTORY must now SUCCEED while a read-only listing handle
        is held on the source;
      * a writable handle on the SOURCE must still block it;
      * a writable handle on the DESTINATION must still block it. Checked with
        ReplaceIfExists=1 on purpose: without the guard that rename would
        SUCCEED and destroy the destination, rather than being refused for the
        unrelated reason that the name is taken.

    Both refusals must carry STATUS_SHARING_VIOLATION.

    Mutation: the old predicate turns the first part red; skipping every handle
    turns the second and third red."""
    rel = "smoke_test_rename_guards"
    label = "rename guards"
    _seed_tree(rel, {}, subdirs=("listed",))
    for name, payload in (("src.txt", b"SRC"), ("from.txt", b"FROM"), ("to.txt", b"TO-MUST-SURVIVE")):
        with open(_host(rel, name), "wb") as fh:
            fh.write(payload)
    ok = True
    ca, sa, ta = connect_session(port)
    cb, sb, tb = connect_session(port)
    try:
        # 1. A listing handle no longer blocks a directory rename.
        b = _open_dir(tb, f"{rel}\\listed")
        _drain_listing(b)
        a = _open_dir(ta, f"{rel}\\listed")
        result = _set_info_result(ta, a, FileInformationClass.FILE_RENAME_INFORMATION,
                                  _rename_buffer(f"{rel}\\renamed"))
        a.close()
        if result is not None:
            print(f"FAIL ({label}): renaming a directory under a read-only listing handle was refused "
                  f"({_describe(result)})")
            ok = False
        elif os.path.isdir(_host(rel, "listed")) or not os.path.isdir(_host(rel, "renamed")):
            print(f"FAIL ({label}): the rename reported success but the card still shows "
                  f"listed={os.path.isdir(_host(rel, 'listed'))} renamed={os.path.isdir(_host(rel, 'renamed'))}")
            ok = False
        else:
            print(f"PASS ({label}): a directory renamed with its listing handle still open")
        try:
            b.close()
        except Exception as e:  # noqa: BLE001
            print(f"FAIL ({label}): B's close failed after its directory was renamed: {type(e).__name__}: {e}")
            ok = False

        # 2. Source guard: a writable handle on the source still blocks.
        b = _open_rw(tb, f"{rel}\\src.txt")
        a = _open_rw(ta, f"{rel}\\src.txt")
        ok = _must_be_sharing_violation(label, "renaming a file another handle has open for writing",
                                        _set_info_result(ta, a, FileInformationClass.FILE_RENAME_INFORMATION,
                                                         _rename_buffer(f"{rel}\\moved.txt"))) and ok
        a.close()
        b.close()
        if os.path.exists(_host(rel, "moved.txt")) or not os.path.exists(_host(rel, "src.txt")):
            print(f"FAIL ({label}): the source rename happened anyway")
            ok = False

        # 3. Destination guard, with ReplaceIfExists so a missing guard would
        #    destroy 'to.txt' rather than merely be refused for the name clash.
        b = _open_rw(tb, f"{rel}\\to.txt")
        a = _open_rw(ta, f"{rel}\\from.txt")
        ok = _must_be_sharing_violation(label, "replacing a destination another handle has open for writing",
                                        _set_info_result(ta, a, FileInformationClass.FILE_RENAME_INFORMATION,
                                                         _rename_buffer(f"{rel}\\to.txt", replace=True))) and ok
        a.close()
        b.close()
        survived = True
        for name, payload in (("from.txt", b"FROM"), ("to.txt", b"TO-MUST-SURVIVE")):
            if not os.path.exists(_host(rel, name)) or open(_host(rel, name), "rb").read() != payload:
                survived = False
        if not survived:
            print(f"FAIL ({label}): the replace happened anyway -- directory now "
                  f"{sorted(os.listdir(_host(rel)))!r}")
            ok = False
        else:
            print(f"PASS ({label}): both from.txt and to.txt are intact")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL ({label}): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tb, cb)
        _teardown(ta, ca)


def test_stale_handle_after_its_entry_is_deleted(port: int) -> bool:
    """A SURVIVING LISTING HANDLE IS HOLDING A FREED DIRECTORY ENTRY.

    That is the price of the narrowing: B's handle now outlives the delete A
    performs through a second handle, and the NAME it still carries can be
    reissued to something else. Every part below is therefore checked against a
    REPLACEMENT planted at that same name after the delete, not against an
    absent path -- because a path-keyed operation performed through the
    surviving handle lands on whatever now holds the name, and that is the
    whole hazard.

    ⚠️ THE REPLACEMENT IS ALSO WHAT MAKES THIS CHECK NON-VACUOUS. The POSIX
    stub's HalFile::setTimestamp() (stub_hal/HalStorage.cpp:428) resolves
    impl->path and ::stat()s it, so with nothing at that name it refuses on its
    own and a "refused" assertion would prove nothing about the stale flag. The
    device is the opposite -- FatFile::timestamp() writes through the handle's
    own cached entry and would happily stamp a dead one -- so this is a
    divergence that could have shipped a false green. With something at the
    name, the stub accepts unless the stale flag stops it, and the check moves.

      1. set_info FILE_BASIC (:3999) on the stale handle must be REFUSED, and
         the replacement's mtime must be untouched.
      2. arming delete-on-close on the stale handle must be REFUSED (:3826)
         AND the replacement must still be there after the handle closes. Both,
         because they are two different gates: setDisposition() refuses to arm,
         and closeCmd's `wantDelete && wasStale` drops a flag armed BEFORE the
         path went away, which setDisposition cannot see.
      3. FILE_RENAME_INFORMATION (:3643) through the stale handle must be
         REFUSED, and the replacement must still be at its own name with its
         contents. THIS ONE WAS A MEASURED DEFECT, not a hypothesis: before the
         gate existed this rename was ACCEPTED and the replacement directory --
         sentinel file and all -- was moved to the new name. Storage.rename()
         works by path, and FatFile::rename() has no isWritable() guard of its
         own either.
      4. THE SAME AFTER A RENAME rather than a delete, on the name the handle
         still carries. There are three markOtherHandlesStale() call sites --
         deleteOnClose() (:823) and rename's old (:3789) and new (:3797) names
         -- and parts 1-3 only reach the first. A rename MOVES the entry rather
         than freeing it, which is the same hazard for a handle left pointing
         at the old name.

    Mutations: remove markOtherHandlesStale()'s call in deleteOnClose() and
    parts 1-3 go red -- the FILE_BASIC is accepted and stamps the replacement,
    the close removes it, and the rename moves it. Remove the `slot->stale`
    gate at the top of renameOpenHandle() and part 3 goes red on its own, with
    the replacement actually moved. Remove the markOtherHandlesStale() call in
    renameOpenHandle() and part 4 goes red on its own."""
    rel = "smoke_test_stale_handle"
    label = "stale handle"
    _seed_tree(rel, {}, subdirs=("victim", "victim2", "moved"))
    target = f"{rel}\\victim"
    sentinel = b"do not move me"
    ok = True
    ca, sa, ta = connect_session(port)
    cb, sb, tb = connect_session(port)
    try:
        b = _open_dir(tb, target)
        _drain_listing(b)
        a = _open_dir_to_delete(ta, target)
        result = _arm_delete(ta, a)
        if result is not None:
            print(f"FAIL ({label}): the setup delete was refused ({_describe(result)}) -- this check "
                  f"depends on the narrowing working at all")
            a.close()
            b.close()
            return False
        a.close()
        if os.path.isdir(_host(rel, "victim")):
            print(f"FAIL ({label}): the setup delete did not remove the directory")
            b.close()
            return False

        # THE REPLACEMENT: a DIFFERENT object at the name B still holds. Empty,
        # so that an unguarded rmdir through B would actually succeed -- with a
        # file inside it would fail for the wrong reason and hide the bug.
        os.mkdir(_host(rel, "victim"))
        before = os.stat(_host(rel, "victim")).st_mtime

        # 1. FILE_BASIC through the stale handle.
        result = _set_info_result(tb, b, FileInformationClass.FILE_BASIC_INFORMATION,
                                  _basic_buffer(write=_unix_to_filetime(STALE_STAMP_UNIX)))
        if result is None:
            print(f"FAIL ({label}): FILE_BASIC through a handle whose entry was deleted was ACCEPTED")
            ok = False
        else:
            print(f"PASS ({label}): FILE_BASIC through the stale handle refused ({_describe(result)})")
        after = os.stat(_host(rel, "victim")).st_mtime
        if abs(after - before) > 0.5 or int(after) == STALE_STAMP_UNIX:
            print(f"FAIL ({label}): THE REPLACEMENT WAS STAMPED -- mtime {before} -> {after} "
                  f"(the request carried {STALE_STAMP_UNIX})")
            ok = False
        else:
            print(f"PASS ({label}): the replacement's mtime is untouched")

        # 2. delete-on-close armed on the stale handle: refused to ARM, and the
        #    replacement still there afterwards. Two gates, both asserted.
        armed = _arm_delete(tb, b)
        if armed is None:
            print(f"FAIL ({label}): arming delete-on-close on a stale handle was ACCEPTED -- "
                  f"setDisposition() must refuse, and only closeCmd's own capture would then be "
                  f"standing between the client and the replacement")
            ok = False
        else:
            print(f"PASS ({label}): arming delete-on-close on the stale handle refused ({_describe(armed)})")
        try:
            b.close()
        except Exception as e:  # noqa: BLE001
            print(f"FAIL ({label}): closing the stale handle raised {type(e).__name__}: {e}")
            ok = False
        if not os.path.isdir(_host(rel, "victim")):
            print(f"FAIL ({label}): THE REPLACEMENT WAS DELETED by a delete-on-close on a handle "
                  f"whose own entry had already been freed")
            ok = False
        else:
            print(f"PASS ({label}): the replacement survived the stale handle's close")

        # 3. THE MEASURED DEFECT: rename through the stale handle. Its own
        #    fixture, because this replacement must CARRY something -- the
        #    failure that was measured is the replacement arriving, intact,
        #    under the new name.
        b = _open_dir(tb, f"{rel}\\victim2")
        _drain_listing(b)
        a = _open_dir_to_delete(ta, f"{rel}\\victim2")
        result = _arm_delete(ta, a)
        a.close()
        if result is not None or os.path.isdir(_host(rel, "victim2")):
            print(f"FAIL ({label}): the setup delete for the rename case did not happen "
                  f"({_describe(result)})")
            b.close()
            return False
        os.mkdir(_host(rel, "victim2"))
        with open(_host(rel, "victim2", "SENTINEL.txt"), "wb") as fh:
            fh.write(sentinel)
        result = _set_info_result(tb, b, FileInformationClass.FILE_RENAME_INFORMATION,
                                  _rename_buffer(f"{rel}\\victim2_moved"))
        if result is None:
            print(f"FAIL ({label}): a RENAME through a handle whose entry was deleted was ACCEPTED")
            ok = False
        else:
            print(f"PASS ({label}): rename through the stale handle refused ({_describe(result)})")
        moved = _host(rel, "victim2_moved", "SENTINEL.txt")
        still = _host(rel, "victim2", "SENTINEL.txt")
        if os.path.exists(moved):
            print(f"FAIL ({label}): THE REPLACEMENT WAS MOVED -- {os.path.join(rel, 'victim2_moved')} now "
                  f"holds the sentinel that belonged to a directory this handle never opened")
            ok = False
        elif not os.path.exists(still) or open(still, "rb").read() != sentinel:
            print(f"FAIL ({label}): the replacement is damaged: "
                  f"{sorted(os.listdir(_host(rel))) if os.path.isdir(_host(rel)) else 'gone'!r}")
            ok = False
        else:
            print(f"PASS ({label}): the replacement is still at its own name with its sentinel intact")
        b.close()

        # 4. The OTHER markOtherHandlesStale() call site: a RENAME through a
        #    second handle, with a listing handle left behind on the old name.
        b2 = _open_dir(tb, f"{rel}\\moved")
        _drain_listing(b2)
        a2 = _open_dir(ta, f"{rel}\\moved")
        result = _set_info_result(ta, a2, FileInformationClass.FILE_RENAME_INFORMATION,
                                  _rename_buffer(f"{rel}\\moved_away"))
        a2.close()
        if result is not None:
            print(f"FAIL ({label}): the setup rename was refused ({_describe(result)})")
            b2.close()
            return False
        os.mkdir(_host(rel, "moved"))
        before = os.stat(_host(rel, "moved")).st_mtime
        result = _set_info_result(tb, b2, FileInformationClass.FILE_BASIC_INFORMATION,
                                  _basic_buffer(write=_unix_to_filetime(STALE_STAMP_UNIX)))
        after = os.stat(_host(rel, "moved")).st_mtime
        if result is None or abs(after - before) > 0.5 or int(after) == STALE_STAMP_UNIX:
            print(f"FAIL ({label}): after a RENAME moved its entry, FILE_BASIC through the left-behind "
                  f"handle was {_describe(result)} and the replacement's mtime went {before} -> {after}")
            ok = False
        else:
            print(f"PASS ({label}): a handle left on the OLD name after a rename is stale too "
                  f"({_describe(result)}), and the replacement's mtime is untouched")
        b2.close()
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL ({label}): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tb, cb)
        _teardown(ta, ca)


def test_protected_names_are_case_insensitive(port: int) -> bool:
    """`rmdir xtcache` MUST NOT REMOVE `XTCache`.

    isProtectedNameView() compared with memcmp(), and FAT is case-insensitive:
    `xtcache` and `XTCache` are ONE directory on the card, so a client naming it
    in another case walked straight past the check. Only the two entries in
    kHiddenItems could ever have been reached this way -- '.crossmosa' is caught
    by the leading-dot test, which has no case to get wrong.

    Two independent observables, both of which move if strncasecmp() goes back
    to memcmp():

      * the whole rmdir sequence against a case variant (open the directory
        carrying DELETE, set the disposition, close) is refused at the OPEN, and
        the real directory is still there afterwards. Seeded EMPTY on purpose:
        with contents, an unguarded rmdir would fail for the wrong reason and
        the check would stay green through the bug.
      * a directory whose ON-DISK name is the lower-case variant is absent from
        a listing, exactly as the canonical spelling is (queryDirectoryCmd
        filters through the same ProtectedPath::isProtectedName)."""
    rel = "smoke_test_case_protection"
    listrel = "smoke_test_case_protection_listing"
    label = "protected names are case-insensitive"
    _seed_tree(rel, {"visible.epub": 3}, subdirs=("XTCache", "System Volume Information"))
    _seed_tree(listrel, {"visible.epub": 3}, subdirs=("xtcache", "system volume information"))
    ok = True
    connection, session, tree = connect_session(port)
    try:
        for asked, on_disk in (("xtcache", "XTCache"),
                               ("XTCACHE", "XTCache"),
                               ("system volume information", "System Volume Information")):
            opened = None
            try:
                opened = _open_dir_to_delete(tree, f"{rel}\\{asked}")
            except Exception as e:  # noqa: BLE001
                print(f"PASS ({label}): {asked!r} refused at CREATE ({type(e).__name__})")
            if opened is not None:
                # Carry the whole rmdir through, so what gets reported is the
                # damage rather than a near miss.
                _arm_delete(tree, opened)
                try:
                    opened.close()
                except Exception:  # noqa: BLE001
                    pass
                print(f"FAIL ({label}): {asked!r} was OPENED for delete -- it names the protected "
                      f"{on_disk!r} on a case-insensitive filesystem")
                ok = False
            if not os.path.isdir(_host(rel, on_disk)):
                print(f"FAIL ({label}): {on_disk!r} IS GONE from the card")
                ok = False

        d = _open_dir(tree, listrel)
        entries, _ = _drain_listing(d)
        d.close()
        real = set(_real_only(_entry_names(entries)))
        if real != {"visible.epub"}:
            print(f"FAIL ({label}): the listing of {listrel} returned {sorted(real)!r}; a lower-case "
                  f"'xtcache' / 'system volume information' must be hidden exactly as the canonical "
                  f"spelling is")
            ok = False
        else:
            print(f"PASS ({label}): lower-case 'xtcache' and 'system volume information' are hidden "
                  f"from the listing")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL ({label}): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


# ===========================================================================
# v80: the 5 MB copy the iPhone could not make.
#
# diag27, copying a 5,218,624-byte book out of the iOS Files app -- five writes
# down ONE handle inside 168 ms:
#
#   139253  write first: len=4096 offset=0
#   139300  write reject: 1044480-byte hole ... offset=1048576
#   139341  write reject: 2093056-byte hole ... offset=2097152
#   139380  write reject: 3141632-byte hole ... offset=3145728
#   139421  write reject: 4190208-byte hole ... offset=4194304
#
# iOS splits a large copy into PARALLEL STREAMS ONE MEGABYTE APART and starts
# them all at once, and it sends no SET_INFO FILE_END_OF_FILE first -- so the
# server meets offset 1048576 while the file is still 4096 bytes long. FAT
# cannot express a hole, so each one has to be written out by hand or refused.
# Four of the five were refused; the copy hung and iOS deleted the partial file
# 25 s later.
#
# WHAT CHANGED, read out of src/network/SmbFileHandlers.cpp rather than assumed:
#
#   * kMaxWriteGapBytes (:3163) 64 KiB -> 4 * 1024 * 1024 + 64 * 1024 =
#     4,259,840 -- the largest hole actually observed (4,190,208) plus the old
#     budget as margin. Its old comment called a reorder "of a chunk or two"
#     the realistic case; diag27 is the real client disagreeing.
#   * kZeroFillChunkBytes (:3178) 512 -> 4096, so the fill takes SdFat's
#     multi-sector CMD25 path instead of a CMD24 per sector. DELIBERATELY NOT
#     ASSERTED ANYWHERE BELOW -- see the note after these checks.
#   * every failure return in writeCmd is now
#     replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INSUFFICIENT_RESOURCES) rather
#     than a bare `return -1`. libsmb2.c:3592-3596 hardcodes
#     STATUS_NOT_IMPLEMENTED for ANY negative return -- "this server does not
#     implement WRITE" -- which is a lie about a server that implements it
#     fine and is declining one request.
#
# WHY THE STATUS IS ASSERTED AS A NUMBER. 0xC0000002 has no exception class in
# smbprotocol at all, so it arrives as a bare SMBResponseException, while
# 0xC000009A arrives as InsufficientResources. A check that asserts only "an
# exception happened" -- which is exactly what the older absurd-gap probe
# inside test_non_sequential_and_overlapping_writes does -- is identically
# green before and after that third change. The status IS the third change, so
# it is compared as an integer.
#
# WHY THE FILL IS NOT VACUOUS HERE, given the stub is POSIX. It would be, but
# for the divergence table's `seek64() past end-of-file` row: the stub REFUSES
# it, as SdFat does. So the server cannot take POSIX's free sparse hole -- a
# fill that stops short leaves the following seek64(req->offset) failing and
# the write refused. What IS free here, and therefore never the sole claim of
# any check below, is the hole reading back as zeros.
#
# NON-VACUITY. Each check was run against a mutant of today's tree and names
# in its docstring the mutation it answers to.
# ===========================================================================

# Asserted numerically, for the reason in the header. The asymmetry is the
# point: smbprotocol maps 0xC000009A to a class and 0xC0000002 to nothing, so
# exception type alone cannot tell "declined this request" from "has no WRITE".
STATUS_INSUFFICIENT_RESOURCES = 0xC000009A
STATUS_NOT_IMPLEMENTED = 0xC0000002

# src/network/SmbFileHandlers.cpp's kMaxWriteGapBytes. MIRRORED, not imported --
# test_write_gap_budget_boundary is what says so if the constant moves.
MAX_WRITE_GAP_BYTES = 4 * 1024 * 1024 + 64 * 1024

# diag27's shape: five streams, one per megabyte, 4096 bytes each.
IOS_STREAM_STRIDE = 1024 * 1024
IOS_STREAM_COUNT = 5
IOS_STREAM_WRITE = 4096


def _write_refusal(op, data: bytes, offset: int):
    """None if the WRITE was ACCEPTED, else (exception type name, NT status).

    Same shape as _set_info_result() above, and for the same reason: these
    checks need the number, not just the fact that something was raised."""
    try:
        op.write(data, offset)
        return None
    except Exception as e:  # noqa: BLE001
        return type(e).__name__, getattr(e, "status", None)


def _must_be_insufficient_resources(label: str, what: str, result) -> bool:
    """'Refused' AND 'refused with the honest number', in one place.

    NOT_IMPLEMENTED is called out by name because it is not just some other
    wrong status -- it is the specific one libsmb2 substitutes for a bare
    `return -1`, so seeing it means the replyStatus() call is gone."""
    if result is None:
        print(f"FAIL ({label}): {what} was ACCEPTED -- the budget is gone, not merely raised")
        return False
    name, status = result
    if status == STATUS_NOT_IMPLEMENTED:
        print(f"FAIL ({label}): {what} was refused with {hex(STATUS_NOT_IMPLEMENTED)} "
              f"STATUS_NOT_IMPLEMENTED ({name}) -- libsmb2.c:3592-3596's hardcoded answer to a bare "
              f"`return -1`. It tells the client this server has no WRITE at all, which is what "
              f"diag27 shows iOS giving up on")
        return False
    if status != STATUS_INSUFFICIENT_RESOURCES:
        print(f"FAIL ({label}): {what} was refused with {_describe(result)}, expected "
              f"{hex(STATUS_INSUFFICIENT_RESOURCES)} STATUS_INSUFFICIENT_RESOURCES")
        return False
    print(f"PASS ({label}): {what} refused with {hex(STATUS_INSUFFICIENT_RESOURCES)} ({name})")
    return True


def test_ios_parallel_stream_copy_replay(port: int) -> bool:
    """THE REPORTED BUG: diag27's five parallel streams, replayed exactly.

    Five 4096-byte writes down ONE handle at 0 / 1M / 2M / 3M / 4M, in the
    order the phone sent them and with no FILE_END_OF_FILE first. Every one
    must be accepted and report exactly 4096; the file must end up 4,198,400
    bytes with those five ranges byte-exact and EVERY OTHER BYTE ZERO; and it
    must still be there afterwards.

    Mutation this answers to: kMaxWriteGapBytes reverted to 64 KiB. The very
    first stride is a 1,044,480-byte hole, so four of the five writes are
    refused -- diag27, reproduced."""
    name = "smoke_ios_parallel.bin"
    offsets = [i * IOS_STREAM_STRIDE for i in range(IOS_STREAM_COUNT)]
    payloads = [_pattern_bytes(IOS_STREAM_WRITE, seed=800 + i) for i in range(IOS_STREAM_COUNT)]
    expected_len = offsets[-1] + IOS_STREAM_WRITE

    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = _open_write(tree, name)
        high_water = 0
        for i, (offset, payload) in enumerate(zip(offsets, payloads)):
            hole = max(0, offset - high_water)
            try:
                count = op.write(payload, offset)
            except Exception as e:  # noqa: BLE001
                status = getattr(e, "status", None)
                print(f"FAIL (iOS parallel copy): stream {i} at offset {offset} was REFUSED "
                      f"({type(e).__name__} status={'None' if status is None else hex(status)}) -- "
                      f"a {hole}-byte hole. This IS diag27: the copy hangs here and iOS deletes "
                      f"the partial file")
                ok = False
                break
            if count != IOS_STREAM_WRITE:
                print(f"FAIL (iOS parallel copy): stream {i} at offset {offset} reported {count} "
                      f"bytes written, sent {IOS_STREAM_WRITE}")
                ok = False
            high_water = max(high_water, offset + IOS_STREAM_WRITE)
        op.close()

        host = _host_path(name)
        if not os.path.exists(host):
            print("FAIL (iOS parallel copy): the file is gone after the copy")
            return False
        with open(host, "rb") as fh:
            on_disk = fh.read()

        expected = bytearray(expected_len)
        for offset, payload in zip(offsets, payloads):
            expected[offset:offset + IOS_STREAM_WRITE] = payload

        if len(on_disk) != expected_len:
            print(f"FAIL (iOS parallel copy): file is {len(on_disk)} bytes, expected {expected_len}")
            ok = False
        elif on_disk != bytes(expected):
            # Which half is wrong matters: a payload at the wrong offset and a
            # hole filled with garbage are different bugs with different fixes.
            misplaced = [off for off, p in zip(offsets, payloads)
                         if on_disk[off:off + IOS_STREAM_WRITE] != p]
            filler = bytearray(on_disk)
            for off in offsets:
                filler[off:off + IOS_STREAM_WRITE] = bytes(IOS_STREAM_WRITE)
            nonzero = len(filler) - filler.count(0)
            print(f"FAIL (iOS parallel copy): content differs -- payload ranges wrong at {misplaced}; "
                  f"{nonzero} non-zero bytes outside the written ranges; "
                  f"SHA {_sha(on_disk)} != {_sha(bytes(expected))}")
            ok = False
        elif ok:
            print(f"PASS (iOS parallel copy): 5 streams {IOS_STREAM_STRIDE} B apart down one handle, "
                  f"all accepted at {IOS_STREAM_WRITE} B each, {expected_len} bytes on disk, five "
                  f"ranges byte-exact and every other byte zero")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (iOS parallel copy): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_oversized_hole_refused_with_honest_status(port: int) -> bool:
    """A hole too big to fill is refused -- with STATUS_INSUFFICIENT_RESOURCES
    (0xC000009A) and specifically NOT STATUS_NOT_IMPLEMENTED (0xC0000002).

    Refusing is the right direction: the alternative is not "write it anyway",
    it is "write it at the wrong offset". Saying the right thing while refusing
    is the part that was broken. The file must be byte-identical afterwards,
    and the handle must survive -- a refusal is one request failing, not the
    transfer, and a client can only back off to something that still exists.

    Mutation this answers to: the gap-path replyStatus() reverted to
    `return -1`. The refusal still happens and an exception is still raised, so
    a check asserting only that stays green; this one goes red on the number."""
    name = "smoke_hole_refused.bin"
    seed = _pattern_bytes(4096, seed=820)
    far = 64 * 1024 * 1024
    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = _open_write(tree, name)
        op.write(seed, 0)
        op.close()
        with open(_host_path(name), "rb") as fh:
            before = fh.read()

        op2 = _open_write(tree, name, disposition=CreateDisposition.FILE_OPEN)
        result = _write_refusal(op2, b"x" * 16, far)
        ok = _must_be_insufficient_resources(
            "oversized hole", f"a {far - len(before)}-byte hole at offset {far}", result) and ok

        with open(_host_path(name), "rb") as fh:
            after = fh.read()
        if after != before:
            print(f"FAIL (oversized hole): the file changed despite the refusal -- {len(before)} B "
                  f"-> {len(after)} B, SHA {_sha(before)} != {_sha(after)}")
            ok = False
        else:
            print(f"PASS (oversized hole): file byte-identical afterwards ({len(after)} B)")

        tail = _pattern_bytes(256, seed=821)
        count = op2.write(tail, len(before))
        if count != len(tail):
            print(f"FAIL (oversized hole): the handle did not survive the refusal -- follow-up "
                  f"write reported {count} of {len(tail)}")
            ok = False
        else:
            print("PASS (oversized hole): the handle survived; a legal write through it still lands")
        op2.close()
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (oversized hole): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_write_gap_budget_boundary(port: int) -> bool:
    """The budget is a number, so both sides of it are pinned: a hole of
    EXACTLY kMaxWriteGapBytes is filled, and one byte more is refused.

    "Bigger than a megabyte" would be satisfied by any value from 1 MiB up.
    This is what says the constant is 4,259,840 and not merely something that
    happens to clear diag27's stride -- and the refused half must leave NO
    partial fill behind, since a half-filled file is the shape that silently
    corrupts a book.

    Mutation this answers to: kMaxWriteGapBytes reverted to 64 KiB reddens the
    at-budget half. A future raise of the constant reddens the over-budget half
    instead, which is the other thing worth being told about."""
    marker = _pattern_bytes(16, seed=830)
    connection, session, tree = connect_session(port)
    ok = True
    try:
        # At the budget. A fresh file is empty, so the hole IS the offset.
        at_name = "smoke_gap_at_budget.bin"
        op = _open_write(tree, at_name)
        result = _write_refusal(op, marker, MAX_WRITE_GAP_BYTES)
        op.close()
        if result is not None:
            print(f"FAIL (gap budget boundary): a hole of exactly {MAX_WRITE_GAP_BYTES} B was "
                  f"refused with {_describe(result)} -- the budget in "
                  f"src/network/SmbFileHandlers.cpp is smaller than this check believes")
            ok = False
        else:
            with open(_host_path(at_name), "rb") as fh:
                blob = fh.read()
            want = MAX_WRITE_GAP_BYTES + len(marker)
            fill_ok = blob[:MAX_WRITE_GAP_BYTES].count(0) == MAX_WRITE_GAP_BYTES
            if len(blob) != want or blob[MAX_WRITE_GAP_BYTES:] != marker or not fill_ok:
                print(f"FAIL (gap budget boundary): the at-budget hole was filled wrongly -- "
                      f"{len(blob)} B, expected {want}; payload-at-offset "
                      f"{blob[MAX_WRITE_GAP_BYTES:] == marker}; fill-all-zero {fill_ok}")
                ok = False
            else:
                print(f"PASS (gap budget boundary): a hole of exactly {MAX_WRITE_GAP_BYTES} B is "
                      f"filled with zeros and the payload lands at the right offset")

        # One byte over.
        over_name = "smoke_gap_over_budget.bin"
        op2 = _open_write(tree, over_name)
        result2 = _write_refusal(op2, marker, MAX_WRITE_GAP_BYTES + 1)
        ok = _must_be_insufficient_resources(
            "gap budget boundary", f"a hole of {MAX_WRITE_GAP_BYTES + 1} B (budget + 1)",
            result2) and ok
        op2.close()
        size = os.path.getsize(_host_path(over_name))
        if size != 0:
            print(f"FAIL (gap budget boundary): the refused file is {size} B, expected 0 -- a "
                  f"refused write must not leave a partial fill behind")
            ok = False
        else:
            print("PASS (gap budget boundary): the refused write left the file empty, no partial fill")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (gap budget boundary): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


def test_write_reply_never_short_counts(port: int) -> bool:
    """Every ACCEPTED write reports exactly the length that was sent.

    writeCmd sets rep->count = req->length only on the success path and leaves
    it alone on every failure, so a short count can only ever arrive as a
    success that is not one -- and a client told "I wrote 4 KB of your 16 KB"
    mostly advances by 4 KB and carries on, which is the silent-truncation
    shape this subsystem has already paid for once.

    The shape matters most on the gap path this version widened: the server
    moves fill bytes AND payload bytes there, so a count including the fill --
    or reporting only the tail -- is a live hazard. The file is verified too,
    so "right count" cannot pass while the bytes went somewhere else.

    Deliberately ORTHOGONAL to the other three: its gap is 39,666 B, inside
    even the old 64 KiB budget, so none of the three reverts touch it. It was
    made to fail on purpose by a fourth mutation (rep->count = req->length - 1)
    -- a check no mutation can redden is not a check."""
    name = "smoke_write_counts.bin"
    connection, session, tree = connect_session(port)
    ok = True
    try:
        op = _open_write(tree, name)
        chunk = _chunk_of(op)
        model = bytearray()

        def put(label: str, size: int, offset: int, seed: int) -> None:
            nonlocal ok
            payload = _pattern_bytes(size, seed=seed)
            count = op.write(payload, offset)
            if count != size:
                print(f"FAIL (write counts): {label} -- sent {size} B at offset {offset}, server "
                      f"reported {count}")
                ok = False
            if len(model) < offset:
                model.extend(bytes(offset - len(model)))
            model[offset:offset + size] = payload

        put("a single byte at 0", 1, 0, 840)
        put("unaligned offset", 333, 1, 841)
        put("gap-creating write (the fill path)", 4096, 40_000, 842)
        put("adjacent to the filled hole", 512, 40_000 + 4096, 843)
        put("overlapping an earlier write", 200, 42_000, 844)
        put("exactly the negotiated chunk", chunk, len(model), 845)
        op.close()

        with open(_host_path(name), "rb") as fh:
            on_disk = fh.read()
        if on_disk != bytes(model):
            print(f"FAIL (write counts): the counts were right but the bytes are not -- "
                  f"{len(on_disk)} B on disk, expected {len(model)}; "
                  f"SHA {_sha(on_disk)} != {_sha(bytes(model))}")
            ok = False
        elif ok:
            print(f"PASS (write counts): 6 writes (1 B .. {chunk} B; unaligned, gap-creating, "
                  f"overlapping) each reported exactly their own length, file byte-exact at "
                  f"{len(on_disk)} B")
        return ok
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (write counts): {type(e).__name__}: {e}")
        return False
    finally:
        _teardown(tree, connection)


# NOT TESTED HERE, ON PURPOSE -- kZeroFillChunkBytes 512 -> 4096.
#
# Its whole justification is SdFat's single-sector CMD24 path versus the
# multi-sector CMD25 path, and roughly 20-25 s of frozen UI against 8-11 s.
# The stub is POSIX: `write()` is a bare ::write to a page-cached fd, so the
# chunk size changes nothing this harness can observe -- not the bytes, not
# the length, not any status, and a timing assertion would only be measuring
# the kernel. Reverting it to 512 was run as a mutation and every check above
# stayed green, which is the correct and expected result, not a gap. The
# constant is verifiable only on hardware, from the `zero-filled ... in N ms`
# line writeCmd now logs -- a line that, as of this version, no diag log has
# ever contained, because the path had never once executed on the device.
# ===========================================================================


# ===========================================================================
# Task 8, fix round 4: the accept-failure branch.
#
# SmbServer::acceptOneConnection() splits smb2_serve_port_async()'s single int
# return into two opposite behaviours, and the split is load-bearing:
#
#   -EIO     accept() failed and THE CONNECTION IS STILL QUEUED
#            (lib/smb2/lib/socket.c:1483-1494 reaches its `else` only when
#            clientfd < 0). select() is level-triggered, so this repeats on
#            every tick -- once per activity-loop iteration plus up to 8 more
#            inside the HTTP burst. LATCHED, because an unlatched SD write at
#            that rate burns the whole 192 KB diag budget inside one session.
#
#   -ENOMEM  accept() already SUCCEEDED and dequeued; smb2_init_context()
#            returned NULL (lib/smb2/lib/libsmb2.c:4455-4458). Cannot repeat by
#            itself, so it never needed suppressing -- and it is this project's
#            #1 documented crash class. NEVER LATCHED.
#
# An earlier revision latched on "have I logged anything at all", which let an
# -EIO swallow a later -ENOMEM: zero evidence for the one failure that matters
# most. These two tests exist so that regression cannot come back silently.
#
# Both spawn their OWN smbhost on their own port and root -- they deliberately
# break the process they test, so they must not share the one the rest of the
# suite is using.
# ===========================================================================

# Returned instead of True/False by a test that could not set its fixture up.
# main() prints these in a loud block of their own; a skip must never read as
# a pass.
SKIP = "skip"

_HERE = os.path.dirname(os.path.abspath(__file__))
_SMBHOST = os.path.join(_HERE, "smbhost")

# errno values as the negated codes libsmb2 returns.
_ERR_EIO = -5
_ERR_ENOMEM = -12


def _spawn_smbhost(port, root, env_extra=None, nofile=None, log_path=None):
    """Start a private smbhost. Returns (proc, log_path) or (None, reason)."""
    os.makedirs(root, exist_ok=True)
    env = dict(os.environ)
    env["SMBHOST_ROOT"] = root
    if env_extra:
        env.update(env_extra)

    preexec = None
    if nofile is not None:
        def preexec():  # noqa: E306 -- runs in the child, after fork, before exec
            resource.setrlimit(resource.RLIMIT_NOFILE, (nofile, nofile))

    # OSError AND SubprocessError. A preexec_fn that raises -- e.g. a sandbox or
    # seccomp profile that denies a process lowering its own RLIMIT_NOFILE --
    # surfaces as subprocess.SubprocessError, which is NOT an OSError subclass.
    # Letting it escape crashed the whole script before main()'s results loop,
    # discarding the record of every check that had already passed and printing
    # no verdict at all. That is a different and in one way worse failure than
    # the silent pass this fixture exists to avoid: it destroys the suite's own
    # reporting for everything else. Both route into the same skip path.
    try:
        log = open(log_path, "wb")
    except OSError as exc:
        return None, f"could not open {log_path}: {exc}"
    try:
        proc = subprocess.Popen([_SMBHOST, str(port)], stdout=log, stderr=subprocess.STDOUT,
                                env=env, preexec_fn=preexec, close_fds=True)
    except (OSError, subprocess.SubprocessError) as exc:
        log.close()
        return None, f"could not start {_SMBHOST}: {type(exc).__name__}: {exc}"
    log.close()

    # The server prints "SMB tables allocated" once it is up; wait for the
    # listener rather than sleeping a guessed amount.
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            return None, f"smbhost exited immediately (rc={proc.returncode}); log: {_tail(log_path)}"
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                pass
            return proc, log_path
        except OSError:
            continue
    _kill(proc)
    return None, f"smbhost never accepted a connection on port {port}; log: {_tail(log_path)}"


def _kill(proc):
    if proc is None:
        return
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        try:
            proc.kill()
            proc.wait(timeout=5)
        except Exception:
            pass


def _read_log(path):
    try:
        with open(path, "r", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def _tail(path, lines=3):
    return " | ".join(_read_log(path).strip().splitlines()[-lines:])


def _count_diag_accept_failures(text, err_code=None):
    """DiagLog lines only -- the SD-card evidence, not the free LOG_DBG line."""
    out = []
    for line in text.splitlines():
        if "SMB accept FAILED" not in line:
            continue
        if err_code is not None and f"err={err_code}," not in line:
            continue
        out.append(line)
    return out


def _count_raw_accept_failures(text):
    """LOG_DBG lines -- how often the condition actually fired, unfiltered."""
    return sum(1 for line in text.splitlines() if "smb2_serve_port_async failed" in line)


def _hammer(port, held, count, settle=1.0):
    """Open `count` more raw TCP connections and let the server tick on them."""
    for _ in range(count):
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            sock.setblocking(False)
            held.append(sock)
        except OSError:
            pass
    time.sleep(settle)


def test_accept_eio_latched_and_rearms():
    """A storm of -EIO must produce exactly ONE DiagLog line -- and a later
    success must re-arm the latch so a NEW storm is reported again.

    Forced by starting smbhost under a small RLIMIT_NOFILE so accept() returns
    EMFILE. The connection stays queued, so the failure re-fires on every tick:
    that is the flood the latch exists to stop, and it is also what makes the
    'exactly one line' assertion meaningful."""
    port = 4581
    root = os.path.join(tempfile.gettempdir(), "smbhost-eio-root")
    log_path = os.path.join(tempfile.gettempdir(), "smbhost-eio.log")
    shutil.rmtree(root, ignore_errors=True)

    # 6 leaves room for stdio + the listener + a couple of clients, and not
    # much else -- accept() then fails once a few connections are held open.
    proc, info = _spawn_smbhost(port, root, nofile=6, log_path=log_path)
    if proc is None:
        print(f"SKIP (accept -EIO): {info}")
        shutil.rmtree(root, ignore_errors=True)
        return SKIP

    held = []
    try:
        # Phase 1: force the storm.
        for _ in range(6):
            _hammer(port, held, 2, settle=0.5)
            if _count_diag_accept_failures(_read_log(log_path), _ERR_EIO):
                break

        text = _read_log(log_path)
        first = _count_diag_accept_failures(text, _ERR_EIO)
        if not first:
            print("SKIP (accept -EIO): could not make accept() fail under RLIMIT_NOFILE=6 in this "
                  f"environment (raw failures seen: {_count_raw_accept_failures(text)})")
            return SKIP

        time.sleep(1.5)  # let many more ticks hit the same condition
        text = _read_log(log_path)
        raw = _count_raw_accept_failures(text)
        latched = _count_diag_accept_failures(text, _ERR_EIO)
        if raw < 5:
            print(f"SKIP (accept -EIO): the condition did not repeat enough to test the latch (raw={raw})")
            return SKIP
        if len(latched) != 1:
            print(f"FAIL (accept -EIO): {raw} raw failures produced {len(latched)} DiagLog lines, expected 1 "
                  "-- the latch is not suppressing repeats")
            return False
        if "still queued" not in latched[0]:
            print(f"FAIL (accept -EIO): the logged line does not identify the queued-connection case: {latched[0]}")
            return False

        # Phase 2: release the pressure so an accept succeeds, which must clear
        # the latch, then force a SECOND storm and require a SECOND line.
        for sock in held:
            try:
                sock.close()
            except OSError:
                pass
        held = []
        time.sleep(1.5)  # cullOneDeadClient() reclaims, queued connection is accepted

        for _ in range(6):
            _hammer(port, held, 2, settle=0.5)
            if len(_count_diag_accept_failures(_read_log(log_path), _ERR_EIO)) > 1:
                break
        time.sleep(1.0)

        text = _read_log(log_path)
        second = _count_diag_accept_failures(text, _ERR_EIO)
        if len(second) < 2:
            print(f"FAIL (accept -EIO): after a successful accept the latch did not re-arm -- still "
                  f"{len(second)} line(s) after a second storm ({_count_raw_accept_failures(text)} raw failures)")
            return False
        print(f"PASS (accept -EIO): {raw}+ raw failures -> 1 line; after a success, a second storm -> "
              f"{len(second)} lines total")
        return True
    finally:
        for sock in held:
            try:
                sock.close()
            except OSError:
                pass
        _kill(proc)
        shutil.rmtree(root, ignore_errors=True)
        try:
            os.unlink(log_path)
        except OSError:
            pass


def test_accept_enomem_never_suppressed():
    """EVERY -ENOMEM must be logged, with the contiguous-heap figure.

    Forced with smbfail_calloc.c (LD_PRELOAD), which fails the
    calloc(1, sizeof(struct smb2_context)) inside smb2_init_context() while an
    arm file exists. Unlike -EIO the connection has already been dequeued, so
    nothing here repeats by itself: three connections must give three lines.
    A latch keyed on 'have I logged anything' gives one."""
    attempts = 3
    port = 4583
    workdir = tempfile.mkdtemp(prefix="smbhost-enomem-")
    shim_so = os.path.join(workdir, "libsmbfail.so")
    shim_src = os.path.join(_HERE, "smbfail_calloc.c")
    arm_file = os.path.join(workdir, "arm")
    trace_file = os.path.join(workdir, "trace")
    root = os.path.join(workdir, "root")
    log_path = os.path.join(workdir, "smbhost.log")

    proc = None
    try:
        if not os.path.exists(shim_src):
            print(f"SKIP (accept -ENOMEM): {shim_src} is missing")
            return SKIP
        compiler = shutil.which("cc") or shutil.which("gcc")
        if compiler is None:
            print("SKIP (accept -ENOMEM): no cc/gcc available to build the LD_PRELOAD shim")
            return SKIP
        build = subprocess.run([compiler, "-shared", "-fPIC", "-O1", "-o", shim_so, shim_src],
                               capture_output=True, text=True)
        if build.returncode != 0:
            print("SKIP (accept -ENOMEM): could not build the LD_PRELOAD shim (glibc's __libc_calloc "
                  f"may not exist here): {build.stderr.strip()[:200]}")
            return SKIP

        # SMBFAIL_TRACE turns on size recording; the arm file stays absent, so
        # nothing fails until the exact context size has been discovered. See
        # smbfail_calloc.c's header for why an exact size and not a range:
        # struct smb2_pdu is calloc(1, ~12,568)'d on EVERY PDU and sits inside
        # the same window struct smb2_context (~7,256) does.
        env = {
            "LD_PRELOAD": shim_so,
            "SMBFAIL_ARM_FILE": arm_file,
            "SMBFAIL_TRACE": trace_file,
            "SMBFAIL_MAX": str(attempts),
            # v73: 4096 -> 1024. The vendored SMB2_MAX_VECTORS 256 -> 32 cut
            # sizeof(struct smb2_context) from 7,256 to 1,880, dropping it out of
            # the old window: the trace phase then recorded nothing, there was no
            # size to arm, and this check SKIPPED while its message pointed at
            # LD_PRELOAD instead. A floor tied to today's struct size is a check
            # that silently stops checking the next time the struct shrinks.
            "SMBFAIL_MIN_SIZE": "1024",
            "SMBFAIL_MAX_SIZE": "65536",
        }
        proc, info = _spawn_smbhost(port, root, env_extra=env, log_path=log_path)
        if proc is None:
            print(f"SKIP (accept -ENOMEM): {info}")
            return SKIP

        # Phase 1 -- baseline AND discovery in one connection. Unarmed, the shim
        # must be inert (the server stays healthy), and the trace it writes
        # gives the exact allocation to target. The FIRST in-window size on a
        # fresh trace is smb2_init_context()'s: accept_cb() calls it before
        # anything else on that connection can allocate.
        with open(trace_file, "w"):
            pass  # truncate, so entry 0 belongs to the connection below
        held = []
        _hammer(port, held, 1, settle=0.8)
        for sock in held:
            sock.close()
        held = []
        if _count_diag_accept_failures(_read_log(log_path)):
            print("SKIP (accept -ENOMEM): the shim failed allocations before being armed; fixture is unsound")
            return SKIP

        traced = [int(line) for line in _read_log(trace_file).split() if line.isdigit()]
        if not traced:
            print("SKIP (accept -ENOMEM): the shim recorded no in-window allocation on an accepted "
                  "connection -- LD_PRELOAD may not be in effect here")
            return SKIP
        context_size = traced[0]
        if not 1024 <= context_size <= 65536:
            print(f"SKIP (accept -ENOMEM): first traced allocation is {context_size} B, which does not look "
                  "like struct smb2_context; the allocation shape has drifted")
            return SKIP

        # Phase 2 -- arm with that exact size, then connect once per expected
        # failure, closing in between so each connection is an independent
        # accept and nothing accumulates.
        with open(arm_file, "w") as handle:
            handle.write(f"{context_size}\n")
        for _ in range(attempts):
            _hammer(port, held, 1, settle=0.7)
            for sock in held:
                try:
                    sock.close()
                except OSError:
                    pass
            held = []
        time.sleep(0.8)

        text = _read_log(log_path)
        lines = _count_diag_accept_failures(text, _ERR_ENOMEM)
        if not lines:
            print("SKIP (accept -ENOMEM): the shim did not reach smb2_init_context()'s allocation "
                  f"(no err={_ERR_ENOMEM} lines); raw failures seen: {_count_raw_accept_failures(text)}")
            return SKIP
        if len(lines) != attempts:
            print(f"FAIL (accept -ENOMEM): {attempts} injected failures produced {len(lines)} DiagLog lines "
                  "-- -ENOMEM must never be suppressed")
            return False
        missing_heap = [line for line in lines if "largest free block" not in line]
        if missing_heap:
            print(f"FAIL (accept -ENOMEM): a line carries no heap figure: {missing_heap[0]}")
            return False
        if not all("already dequeued" in line for line in lines):
            print(f"FAIL (accept -ENOMEM): a line does not identify the dequeued case: {lines[0]}")
            return False
        print(f"PASS (accept -ENOMEM): {attempts}/{attempts} injected failures logged, each with the heap figure "
              f"(targeted calloc(1, {context_size}) exactly)")
        return True
    finally:
        _kill(proc)
        shutil.rmtree(workdir, ignore_errors=True)


def test_forced_signing_escape_hatch():
    """SmbServer::setForceSigning(true) must actually force signing on: the SAME
    ENABLED-only client that gets an unsigned session from the default server
    must get SIGNING_REQUIRED advertised and a SIGNED TREE_CONNECT response.

    This is the other half of test_signing_enabled_only_client(), and it needs
    its own server because the switch is set once, before begin(), for the whole
    process (SmbServer.cpp's gForceSigning; test/host/main.cpp reads SMBHOST_SIGN
    at startup). Hence the private port/root/log and _spawn_smbhost(env_extra=).

    WHY THIS CHECK HAS TO EXIST. The firmware does not call setForceSigning() --
    signing is adaptive there, deliberately, see the long note at the absent
    smb2_set_sign() call. So NOTHING on the device path exercises this, and a
    switch nothing exercises rots silently. What it protects is not a device
    behaviour but this suite's own foundation: Samba's smbclient and the Linux
    kernel cifs client refuse an unsigned reply once they have required signing
    (MEASURED, A/B: signing on -> smbclient lists the share; adaptive -> "tree
    connect failed: NT_STATUS_ACCESS_DENIED"), and those two are how this project
    avoids testing libsmb2 with libsmb2 -- every blocker in the SMB work came
    through one of them. If setForceSigning ever stopped working, the first
    symptom would be an independent client mysteriously unable to connect, in
    some later task, with nothing pointing back to here.

    The ENABLED-only client shape is deliberate and is what makes this the exact
    mirror of the adaptive check: the server must sign because IT requires
    signing, not because the client asked. A require_signing=True client would
    raise smb2->sign through libsmb2's own NEGOTIATE path and would pass whether
    setForceSigning worked or not -- the same blindness described over there.

    A fixture that cannot run says so (SKIP, which main() reports loudly and
    exits 2 for); it never returns a quiet pass."""
    port = 4585
    root = os.path.join(tempfile.gettempdir(), "smbhost-forcesign-root")
    log_path = os.path.join(tempfile.gettempdir(), "smbhost-forcesign.log")
    shutil.rmtree(root, ignore_errors=True)

    proc, info = _spawn_smbhost(port, root, env_extra={"SMBHOST_SIGN": "1"}, log_path=log_path)
    if proc is None:
        print(f"SKIP (forced signing): {info}")
        shutil.rmtree(root, ignore_errors=True)
        return SKIP

    connection = None
    try:
        # Fixture soundness, before any assertion about the wire. main.cpp prints
        # this from the ENVIRONMENT VARIABLE, not from anything setForceSigning()
        # does, so it separates "the harness never asked for signing" (a broken
        # or stale fixture -> SKIP) from "it asked and the server did not do it"
        # (the regression this check is for -> FAIL below). A no-op
        # setForceSigning still prints FORCED ON and still fails on the wire.
        banner = _read_log(log_path)
        if "FORCED ON" not in banner:
            print("SKIP (forced signing): smbhost did not report 'SMB2 signing: FORCED ON' with "
                  "SMBHOST_SIGN=1 -- the harness never asked for signing, so nothing below would be "
                  f"testing setForceSigning(). Rebuild test/host and re-run. Log: {_tail(log_path)}")
            return SKIP

        connection = Connection(uuid.uuid4(), "127.0.0.1", port, require_signing=False)
        enabled_only = SecurityMode.SMB2_NEGOTIATE_SIGNING_ENABLED
        if int(connection.client_security_mode) != int(enabled_only):
            print(f"FAIL (forced signing): this client would advertise SecurityMode "
                  f"0x{int(connection.client_security_mode):02x}, not SIGNING_ENABLED "
                  f"(0x{int(enabled_only):02x}). With SIGNING_REQUIRED in there libsmb2 raises "
                  f"smb2->sign by itself and this check would pass even with setForceSigning() "
                  f"gutted -- fix the client setup, do not relax the assertion")
            return False

        connection.connect(dialect=WORKING_DIALECT, timeout=5)
        server_mode = int(connection.server_security_mode)
        if not server_mode & SecurityMode.SMB2_NEGOTIATE_SIGNING_REQUIRED:
            print(f"FAIL (forced signing): with SMBHOST_SIGN=1 the server advertised SecurityMode "
                  f"0x{server_mode:02x} without SIGNING_REQUIRED, i.e. smb2->sign is still 0 -- "
                  f"SmbServer::setForceSigning() is not reaching smb2_set_sign(ctx, 1) in "
                  f"acceptOneConnection(). Samba smbclient and the Linux kernel cifs client depend "
                  f"on this switch")
            return False

        session = Session(connection, "x3", "x3", require_encryption=False)
        session.connect()
        if session.signing_key is None:
            print("FAIL (forced signing): no session signing key -- a guest session has none and a "
                  "guest cannot sign, so this is the allow_anonymous regression, not a signing one")
            return False

        request = SMB2TreeConnectRequest()
        request["buffer"] = r"\\127.0.0.1\SD".encode("utf-16-le")
        sent = connection.send(request, sid=session.session_id)
        response = connection.receive(sent)
        flags = response["flags"].get_value()
        signature = response["signature"].get_value()
        if not flags & Smb2Flags.SMB2_FLAGS_SIGNED:
            print(f"FAIL (forced signing): TREE_CONNECT response header flags 0x{flags:02x} without "
                  f"SMB2_FLAGS_SIGNED (0x08). This is the exact wire shape Samba rejects with "
                  f"ACCESS_DENIED while this server's own log says 'tree_connect ok'")
            return False
        if signature == b"\x00" * 16:
            print("FAIL (forced signing): TREE_CONNECT response claims SMB2_FLAGS_SIGNED but the "
                  "signature field is sixteen zero bytes -- worse than unsigned, since a verifying "
                  "client is now guaranteed to reject it")
            return False
        if len(signature) != 16:
            print(f"FAIL (forced signing): signature field is {len(signature)} bytes, not 16")
            return False
        print(f"PASS (forced signing): SMBHOST_SIGN=1 -> the same SIGNING_ENABLED-only client gets "
              f"SecurityMode 0x{server_mode:02x} (REQUIRED) and a SIGNED TREE_CONNECT response with a "
              f"real 16-byte signature ({signature.hex()[:16]}...) -- the escape hatch for smbclient "
              f"and kernel cifs still works")
        return True
    except Exception as e:  # noqa: BLE001
        print(f"FAIL (forced signing): {type(e).__name__}: {e}")
        return False
    finally:
        # Same last resort as test_signing_enabled_only_client(): the hand-built
        # TREE_CONNECT leaves no TreeConnect object to disconnect. This server is
        # killed immediately below, so a leaked socket cannot reach another
        # check, but the order is kept identical so the two read the same.
        if connection is not None:
            try:
                connection.disconnect()
            except Exception:  # noqa: BLE001
                pass
            try:
                connection.transport.close()
            except Exception:  # noqa: BLE001
                pass
        _kill(proc)
        shutil.rmtree(root, ignore_errors=True)
        try:
            os.unlink(log_path)
        except OSError:
            pass


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4450

    results = []
    results.append(("anonymous rejected (server-side now, by design -- allow_anonymous=0)",
                     test_anonymous_rejected(port)))
    results.append(("x3/wrongpassword rejected with real LogonFailure", test_wrong_password_rejected(port)))
    results.append((f"x3/x3 full session + tree_connect(SD)/tree_connect(NOPE), no bypasses, dialect<={WORKING_DIALECT}",
                     test_share_validation(port)))
    results.append(("create/close (file + directory), no bypasses", test_create_close(port)))
    results.append(("protected path rejected on write-intent create", test_protected_path_rejected(port)))
    # The four checks below are the ones that would have FAILED before the
    # review fixes: the first two are the create_cmd bypass itself
    # (FILE_OVERWRITE_IF / FILE_OPEN_IF + MAXIMUM_ALLOWED), the third is
    # protected paths having been readable, the fourth is the ".." rule that
    # existed but was never exercised.
    results.append(("protected path + FILE_OVERWRITE_IF + MAXIMUM_ALLOWED rejected, file intact",
                     test_protected_overwrite_if_maximum_allowed(port)))
    results.append(("protected path + FILE_OPEN_IF + MAXIMUM_ALLOWED rejected, file intact",
                     test_protected_open_if_maximum_allowed(port)))
    results.append(("protected path rejected on READ open too (not just write)",
                     test_protected_read_open_rejected(port)))
    results.append(("'..' traversal rejected", test_dotdot_traversal_rejected(port)))
    results.append(("CJK filename create/open round-trip", test_cjk_filename_roundtrip(port)))
    results.append(("existing-directory open matrix (disposition x access x DIR flag, subdir + share root)",
                     test_directory_open_matrix(port)))
    results.append(("truncating open of a directory still refused (stub models SdFat)",
                     test_truncating_open_of_directory_refused(port)))
    results.append(("FILE_NON_DIRECTORY_FILE against a directory rejected",
                     test_non_directory_flag_against_directory(port)))
    results.append(("FILE_DIRECTORY_FILE against a plain file rejected",
                     test_directory_flag_against_file(port)))
    results.append(("connection B cannot close connection A's handle", test_cross_connection_close_rejected(port)))
    # v65: the compound-FileId path. Registered next to the ownership check above
    # because its negative half is the same property -- the 0xFF placeholder must
    # not name another connection's handle.
    results.append(("compound CREATE+QUERY_INFO+CLOSE with the 0xFF related FileId (the iPhone's "
                    "opener), and the placeholder refused on an unarmed connection",
                     test_compound_related_file_id(port)))
    results.append(("echo keepalive", test_echo_keepalive(port)))
    results.append(("destruction_event recycles open-file table slots", test_destruction_event_recycles_slots(port)))
    results.append(("SMB 3.1.1 not offered (clean negotiate rejection, not the old signature bug)",
                     test_smb311_dialect_not_offered(port)))
    results.append(("default client (full modern dialect list, like iOS) lands on SMB 3.0.2 + tree_connect",
                     test_default_client_lands_on_302(port)))
    # v65: registered next to the other negotiate-level checks. Unlike them it
    # deliberately does NOT use connect_session() -- that helper's client sends
    # SIGNING_REQUIRED, which is precisely the condition that hides this axis.
    # v82: the assertion is REVERSED (signing is adaptive now); read the
    # docstring before "fixing" it back.
    results.append(("a SIGNING_ENABLED-only client (Samba/iOS shape) gets an UNSIGNED session, and "
                    "SIGNING_ENABLED is still advertised (v82: adaptive, reverses v65)",
                     test_signing_enabled_only_client(port)))
    # v82: the other half -- the escape hatch that keeps smbclient and kernel
    # cifs usable. Spawns its own server (SMBHOST_SIGN=1 is read once at
    # startup), so it takes no port argument.
    results.append(("the forced-signing escape hatch works: SMBHOST_SIGN=1 -> the same client gets "
                    "SIGNING_REQUIRED and a SIGNED response",
                     test_forced_signing_escape_hatch()))
    # Task 5: query_directory / query_info.
    results.append(("directory listing: 3 files + 1 subdir + '.' and '..', names/sizes/dir flag exact",
                     test_directory_listing(port)))
    results.append(("directory listing with Traditional Chinese names, byte-length correct",
                     test_directory_listing_cjk(port)))
    results.append(("listing HIDES protected entries (.crossmosa, dotfiles, reserved names)",
                     test_listing_hides_protected(port)))
    results.append(("listing spans multiple responses (80 files -> 82 entries with '.' and '..', "
                    "cursor + pattern-less continuations)",
                     test_listing_multi_response(port)))
    results.append(("an empty directory lists exactly '.' and '..' and then ends",
                     test_listing_empty_directory(port)))
    results.append(("SMB2_RESTART_SCANS rewinds an exhausted handle", test_listing_restart_scans(port)))
    results.append(("search pattern actually filters (exact name and '*.epub')",
                     test_listing_pattern_filter(port)))
    results.append(("scan bound: complete listing behind 300 hidden entries, needle still found",
                     test_listing_scan_bound(port)))
    results.append(("next_entry_offset chain walks relatively and covers the whole reply buffer",
                     test_listing_next_entry_offset_chain(port)))
    results.append(("unencodable directory info classes rejected, not answered as 'empty folder'",
                     test_listing_unsupported_class_rejected(port)))
    results.append(("query_info FILE classes (BASIC/STANDARD/NETWORK_OPEN/ALL), file and directory",
                     test_query_info_file_classes(port)))
    results.append(("query_info FILESYSTEM classes (SIZE/DEVICE/ATTRIBUTE/VOLUME)",
                     test_query_info_filesystem_classes(port)))
    results.append(("unsupported query_info classes rejected (and DiagLogged)",
                     test_query_info_unsupported_rejected(port)))
    results.append(("connection B cannot query_directory/query_info connection A's handle",
                     test_query_cross_connection_rejected(port)))
    # Task 6: read / write / flush.
    # The request counts these used to claim (7 and 7) were a function of the
    # 32768-byte ceiling and became wrong at v72's 8192. They are printed by the
    # checks themselves, derived from NEGOTIATE; a label is the wrong place to
    # pin a number the server is expected to retune.
    results.append(("200 KB read round trip, SHA-256 exact over many READ requests",
                     test_read_200k_roundtrip(port)))
    results.append(("200 KB write round trip, SHA-256 exact on disk over many WRITE requests",
                     test_write_200k_roundtrip(port)))
    results.append(("chunk-boundary sizes (CHUNK-1 / CHUNK / CHUNK+1 / 2*CHUNK), read and write",
                     test_chunk_boundary_sizes(port)))
    results.append(("read straddling / at / past EOF", test_read_at_and_past_eof(port)))
    results.append(("non-sequential + overlapping writes (hole zero-filled, absurd hole refused)",
                     test_non_sequential_and_overlapping_writes(port)))
    results.append(("Traditional Chinese path: 100 KB write + read back, SHA-256 exact",
                     test_cjk_data_roundtrip(port)))
    results.append(("two connections, two files, interleaved writes and reads, per-connection verification",
                     test_concurrent_handles_interleaved(port)))
    results.append(("write to a read-only handle refused, file intact", test_write_to_readonly_handle_rejected(port)))
    results.append(("connection B cannot read/write/flush connection A's handle",
                     test_transfer_cross_connection_rejected(port)))
    results.append(("flush reaches storage (file handle and directory handle)", test_flush(port)))
    results.append(("a failed write-back surfaces as a failure (flush and WRITE_THROUGH), healthy file unaffected",
                     test_sync_failure_surfaces(port)))
    results.append(("a failed final sync at CLOSE surfaces as a failure, healthy handle still closes cleanly",
                     test_close_sync_failure_surfaces(port)))
    results.append(("a failed close still frees its slot (close_cmd and destruction_event)",
                     test_failed_close_still_frees_the_slot(port)))
    results.append(("a read-only request cannot write even when the open had to create the file; "
                    "MAXIMUM_ALLOWED still can", test_read_only_request_cannot_write(port)))
    results.append(("MAXIMUM_ALLOWED opens a read-only-attribute file read-only, and writes stay refused",
                     test_maximum_allowed_readonly_file(port)))
    # Task 7: timestamps. (set_info is NOT tested here -- it cannot be reached
    # at all; see SmbFileHandlers.cpp's header comment and task-7-report.md.)
    results.append(("FatTimestamp conversion unit test (leap years, century rules, range ends)",
                     test_fat_timestamp_unit_test(port)))
    results.append(("query_info timestamps: BASIC/NETWORK_OPEN/ALL carry the file's real mtime as a FILETIME",
                     test_timestamps_query_info(port)))
    results.append(("directory listing timestamps: three entries, three distinct correct dates",
                     test_timestamps_directory_listing(port)))
    results.append(("a file written through this server reports a real, recent timestamp (not 0)",
                     test_timestamp_of_freshly_written_file(port)))
    results.append(("the share root reports 'no time information', not a decode of sector 0",
                     test_share_root_reports_no_timestamp(port)))
    # Task 7: set_info. Every one of these was UNREACHABLE before the vendored
    # patch -- upstream tore the connection down instead of failing the
    # request. See docs/third-party/libsmb2-vendoring.md, "The third patch".
    results.append(("rename: file, directory, across directories, and CJK on both sides",
                     test_rename_file_and_directory(port)))
    results.append(("rename onto an existing name: refused without ReplaceIfExists, replaces with it",
                     test_rename_onto_existing(port)))
    results.append(("rename respects path protection in BOTH directions (into and out of /.crossmosa)",
                     test_rename_protected_both_directions(port)))
    results.append(("delete-on-close: file, empty dir, non-empty dir refused with contents intact, protected path",
                     test_delete_on_close(port)))
    results.append(("delete-on-close where the close itself fails: reported, not swallowed; slot still freed",
                     test_delete_on_close_where_close_fails(port)))
    results.append(("set size: smaller / same / zero / larger / absurd / read-only, bytes verified",
                     test_set_end_of_file(port)))
    results.append(("set_info rejections are per-request -- 7 bad requests and the session is still usable",
                     test_set_info_rejections_and_survival(port)))
    results.append(("set_info owner check (B cannot rename/delete A's handle) + no-op FILE_BASIC accepted",
                     test_set_info_cross_connection_and_no_op_basic(port)))
    # Task 7, fix round 2: the three review findings.
    results.append(("FILE_BASIC_INFORMATION really applies timestamps, and ARCHIVE is not refused",
                     test_set_basic_information(port)))
    results.append(("delete-on-close is refused when a second handle opened the path after the disposition",
                     test_delete_on_close_with_a_second_handle(port)))
    results.append(("rename onto a CASE-VARIANT name (FAT is case-insensitive), both ReplaceIfExists values",
                     test_rename_onto_case_variant(port)))
    # Task 7, fix round 3.
    # v72: an unstorable date is skipped rather than fatal, so the old
    # "all-or-nothing" label now names two different properties -- what gets
    # dropped, and what is still refused outright.
    results.append(("FILE_BASIC_INFORMATION drops a date FAT cannot hold and applies the rest, "
                    "verified on disk",
                     test_set_basic_skips_unrepresentable_dates(port)))
    results.append(("FILE_BASIC_INFORMATION still writes NOTHING when a request fails structurally "
                    "(short payload, unmodelled attribute)",
                     test_set_basic_structural_failure_writes_nothing(port)))
    results.append(("regression: the iPhone's 1904 CreationTime is skipped and the LastWriteTime sent "
                    "with it still lands",
                     test_set_basic_ios_1904_creation_time(port)))
    results.append(("stamping the share root is refused, as SdFat's isFileOrSubDir() guard does",
                     test_set_basic_on_share_root_refused(port)))
    results.append(("a leftover holding file does not block later replaces",
                     test_replace_not_blocked_by_a_leftover(port)))

    # v74: named streams. This suite had NO coverage of them at all, which is
    # how a change that would have rebooted the X3 at the close of every
    # accepted stream stayed green across every check above -- see the section
    # header. The first of these is the regression test for the copy iOS
    # abandoned rather than finish.
    results.append(("the iPhone's copy sequence including the com.apple.FinderInfo stream: "
                    "every step succeeds and the book survives byte-exact",
                     test_ios_finderinfo_copy_sequence(port)))
    results.append(("a discarded stream is a sink, not the file: reads end, size is 0, delete/rename/"
                    "query_directory refused, base file untouched",
                     test_discarded_stream_is_a_sink_not_a_file(port)))
    results.append(("an absent named stream still reports 'not found', and 'file::$DATA' opens the FILE",
                     test_absent_stream_not_found_and_dollar_data_is_the_file(port)))
    results.append((f"discarded streams cap at {MAX_DISCARDED_STREAM_SLOTS} of {OPEN_FILE_SLOTS} slots and an "
                    "ordinary file create still succeeds while they are held",
                     test_discarded_stream_slot_cap(port)))

    # v76: the guard that refused a delete because the folder was being looked
    # at, and the case-insensitive protected-name compare that shipped with it.
    # See that section's header for the mechanism and for the mutation each of
    # these answers to.
    results.append(("the iPhone's folder delete: a listing handle held open no longer blocks the "
                    "disposition, and the folder really goes",
                     test_ios_listing_handle_does_not_block_delete(port)))
    results.append(("the narrowing is not the deletion of the check: writable, read-only-file and "
                    "already-truncated handles all still block a delete, with SHARING_VIOLATION",
                     test_delete_still_blocked_by_writeback_handles(port)))
    results.append(("rename: a listing handle no longer blocks it; a writable handle on the source or "
                    "on the destination still does",
                     test_rename_guards_after_the_narrowing(port)))
    results.append(("a handle whose entry was deleted under it is stale: no FILE_BASIC write, and its "
                    "delete-on-close does not take the replacement",
                     test_stale_handle_after_its_entry_is_deleted(port)))
    results.append(("`rmdir xtcache` does not remove `XTCache`: protected names compare case-"
                    "insensitively, as FAT does",
                     test_protected_names_are_case_insensitive(port)))

    # v80: the 5 MB copy the iPhone could not make. iOS opens five parallel
    # streams a megabyte apart with no FILE_END_OF_FILE first, so the server
    # meets a 1 MB hole while the file is 4 KB long. See that section's header
    # for the mechanism, for why the status is compared as an integer, and for
    # the mutation each of these answers to.
    results.append(("the iPhone's 5-stream parallel copy: five 4 KiB writes 1 MiB apart down ONE "
                    "handle, all accepted, holes zero-filled, 4,198,400 B byte-exact",
                     test_ios_parallel_stream_copy_replay(port)))
    results.append(("a hole too big to fill is refused with INSUFFICIENT_RESOURCES (0xC000009A), NOT "
                    "libsmb2's hardcoded NOT_IMPLEMENTED (0xC0000002); file intact, handle survives",
                     test_oversized_hole_refused_with_honest_status(port)))
    results.append((f"the fill budget is exactly {MAX_WRITE_GAP_BYTES}: that hole is filled, one byte "
                    "more is refused and leaves no partial fill",
                     test_write_gap_budget_boundary(port)))
    results.append(("every accepted write reports exactly its own length -- never the fill bytes, "
                    "never a short count", test_write_reply_never_short_counts(port)))

    # Task 8, fix round 4: the accept-failure branch.
    results.append(("accept -EIO is latched (one line for a storm) and RE-ARMS after a success",
                     test_accept_eio_latched_and_rearms()))
    results.append(("accept -ENOMEM is NEVER suppressed: every occurrence logged, with the heap figure",
                     test_accept_enomem_never_suppressed()))

    print()
    ok = True
    skipped = []
    for label, passed in results:
        if passed == SKIP:
            status = "SKIP"
            skipped.append(label)
        else:
            status = "PASS" if passed else "FAIL"
        print(f"{status}: {label}")
        ok = ok and (passed is not False)

    if skipped:
        # Loud, and after the per-test lines so it cannot scroll away. A test
        # that quietly does nothing is worse than no test at all -- it is the
        # same failure shape as the log line that said "reject" for a
        # half-applied request (see README, fix round 3).
        print()
        print(f"!!! {len(skipped)} CHECK(S) SKIPPED -- NOT VERIFIED IN THIS RUN !!!")
        for label in skipped:
            print(f"  SKIPPED: {label}")

    if not ok:
        print("\nFAIL: one or more checks did not pass")
        return 1

    # A skip is not a failure, but it is emphatically NOT a pass, and the
    # aggregate verdict has to say so -- for a while this printed
    # "PASS: all checks passed" and exited 0 after skipping checks, which is
    # precisely the false green this whole task is about. Distinct exit code
    # (2), so a shell that chains on success does not treat an unverified run
    # as a verified one.
    if skipped:
        verified = len(results) - len(skipped)
        print(f"\nINCOMPLETE: {verified} of {len(results)} checks passed, {len(skipped)} SKIPPED "
              "-- this run did NOT verify everything")
        return 2

    print("\nPASS: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
