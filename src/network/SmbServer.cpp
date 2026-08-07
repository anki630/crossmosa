#include "SmbServer.h"

#include "CrossPointSmb2.h"
#include "SmbFileHandlers.h"

#include <Logging.h>
#include <Memory.h>

#include "util/DiagLog.h"

// Device-only: the desktop harness has no ESP-IDF and defines neither macro.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#define CROSSMOSA_SMB_HAS_HEAP_CAPS 1
#endif

#include <cerrno>   // EIO -- see acceptOneConnection()'s failure enumeration
#include <cstring>
#include <sys/poll.h>  // POLLIN/POLLOUT -- HAVE_SYS_POLL_H is set for this target in
                        // lib/smb2/include/esp/config.h, the same header libsmb2.c
                        // itself relies on for these constants (see libsmb2.c:63-68).
#include <sys/socket.h>  // fd_set/FD_*/select() -- same header libsmb2.c:88 includes
                         // for the identical purpose.
#include <sys/time.h>    // struct timeval
#include <unistd.h>      // close()

// ============================================================================
// A note on what this file can and cannot mirror from
// lib/smb2/lib/libsmb2.c's smb2_serve_port() (~lines 4479-4664), for whoever
// reads this next. See task-3-report.md for the full history; this is the
// short version, kept next to the code it explains.
//
// smb2_serve_port() is written as code that lives *inside* libsmb2.c and
// therefore has access to two libsmb2.c-private things that a fresh
// server-side connection's very first PDU registration needs:
//   - `struct connect_data` -- not just "private", but defined nowhere at
//     all outside libsmb2.c itself (lib/smb2/include/libsmb2-private.h:714
//     forward-declares it opaquely with the comment "defined in libsmb2.c" --
//     even other files in lib/smb2/lib/ cannot construct one).
//   - `smb2_negotiate_request_cb` -- declared `static` in libsmb2.c, i.e.
//     zero external linkage. No header, no patch to *this* file, no amount
//     of struct-layout replication recovers a pointer to a static function
//     from outside its translation unit; that's C/C++ linkage, not a
//     documentation gap.
// This genuinely cannot be reproduced from a separate translation unit --
// confirmed independently by this task's reviewer, not just this file's own
// analysis. The maintainer's decision (relayed by the coordinator, after weighing
// the alternative of moving SmbServer onto its own FreeRTOS task -- rejected
// because this device has no serial port, making task-stack-overflow/
// teardown-race failure modes undebuggable): patch libsmb2.c directly,
// committed to the tree (not a build-time pre: script -- see
// docs/third-party/libsmb2-vendoring.md's "why not a pre: script" section),
// with the patch confined to one function appended at the very end of that
// file, verified against pristine upstream by scripts/verify_libsmb2_patch.py.
// See that appended function, `crossmosa_smb2_finish_accept()`, and its own
// banner comment for the full rationale; this file just calls it from
// acceptOneConnection() below.
//
// Separately, walking libsmb2's own active-context list
// (smb2_active_contexts()) to service already-connected clients would *also*
// need private struct access, just for a much less exotic reason: the list
// is singly-linked via a `next` field that only exists in the private
// struct smb2_context definition (the public header only forward-declares
// `struct smb2_context;`). That part has a clean, fully-public-API
// alternative -- SmbServer keeps its own fixed-size registry of the
// contexts *it* accepted (see SmbServer.h) -- so it does not need to touch
// any private header, unlike the negotiate-bootstrap problem above (which
// is why that one alone needed the appended-function patch, not this one).
// ============================================================================

namespace {
constexpr char kTag[] = "SMB";
}  // namespace

namespace {
// An ordinary disconnect arrives here as a service failure: libsmb2 turns a
// zero-length read into smb2_set_error("Read from socket failed, remote closed
// connection.") plus -1 (lib/smb2/lib/socket.c:394-401), which is
// indistinguishable at this level from a real one -- there is no separate
// return code. Measured on the harness: 95 of 96 service(in) failures in a
// clean suite run were this, one per session teardown.
//
// Matching on the string is safe HERE in a way it would not be against a
// moving dependency: lib/smb2/ is vendored and every byte of it is pinned by
// scripts/verify_libsmb2_patch.py against pristine upstream, so this literal
// cannot change without that check failing first.
//
// Suppressed rather than downgraded: destructionEvent() already writes one
// line per teardown (slot accounting), so the disconnect is recorded either
// way. Logging it again -- as the word "FAILED", to the SD card, in the log
// that exists for first-contact diagnosis -- would mean ~99% of the "FAILED"
// lines in a healthy run are benign, and someone hunting for why an iPhone
// would not connect would have to filter out the very word they searched for.
bool isCleanRemoteClose(const char* err) {
  return err != nullptr && strstr(err, "remote closed connection") != nullptr;
}
}  // namespace

unsigned smbLargestFreeBlock() {
#ifdef CROSSMOSA_SMB_HAS_HEAP_CAPS
  // Same call ESP.getMaxAllocHeap() makes, without pulling Arduino.h into a
  // file the host harness also compiles.
  return static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#else
  return 0;
#endif
}

SmbServer::SmbServer(uint16_t port) : port_(port) {}

SmbServer::~SmbServer() { end(); }

namespace {
// v82. Default false: the firmware wants adaptive signing. See the header.
bool gForceSigning = false;
}  // namespace

void SmbServer::setForceSigning(bool on) { gForceSigning = on; }

bool SmbServer::begin() {
  if (running_) return true;

  auto srv = makeUniqueNoThrow<smb2_server>();
  if (!srv) {
    LOG_ERR(kTag, "alloc smb2_server failed (%u bytes)", (unsigned)sizeof(smb2_server));
    DiagLog::line("SMB begin FAILED: alloc smb2_server %u bytes, largest free block %u",
                  (unsigned)sizeof(smb2_server), smbLargestFreeBlock());
    return false;
  }
  memset(srv.get(), 0, sizeof(smb2_server));

  srv->port = port_;
  // Global Constraints: these three must never be left at zero. libsmb2.c:4495
  // defaults an unset max_transact_size to 0x100000 (1 MB) -- an allocation
  // this device's ~380KB RAM cannot survive on its first real transfer.
  // v72: 32768 -> 8192, because signing allocates the WHOLE PDU.
  //
  // smb2_calc_signature() (lib/smb2/lib/smb2-signing.c:186) concatenates every
  // iovec of the message into one malloc'd block before running AES-CMAC over
  // it, in BOTH directions -- inbound verification became live the moment
  // smb2_set_sign() went in (socket.c:831-846). So a 32 KB write request needed
  // a single ~32.8 KB contiguous allocation on arrival, per PDU.
  //
  // This device does not have that reliably. diag19.log, one ordinary session:
  // `accept FAILED: err=-12 ... largest free block 3188` and
  // `service(in) FAILED ... largest free block 2036`, against 40,948 at server
  // start. A 32.8 KB request is at the edge even when things are going well,
  // and the failure mode is not graceful: smb2_calc_signature() returns -1,
  // socket.c:838 turns that into "Signature calc failed." and TEARS THE
  // CONNECTION DOWN, so the client reconnects and starts the copy over.
  //
  // 8192 makes the scratch ~8.3 KB. The cost is round trips, not throughput:
  // AES is linear in bytes either way, and on this 160 MHz core AES dominates
  // (roughly 300-600 KB/s ceiling from signing alone), so four times as many
  // 8 KB PDUs adds latency measured in seconds on a whole book, against a
  // failure mode that costs the entire transfer.
  //
  // RAISE THIS ONLY WITH A MEASUREMENT. `write first: largest free block N` in
  // diag.log (added in this version) is that measurement -- it reports the real
  // headroom at the moment it matters, which no log before v72 ever captured
  // because no WRITE had ever reached this server.
  //
  // v73: 8192 -> 16384. That was justified against the wrong number and the
  // wrong count of allocations, and diag23.log is the correction.
  //
  // Wrong count: signing is not the only contiguous block. Receiving a PDU
  // mallocs its VARIABLE tail -- exactly req->length bytes, socket.c:664, with
  // NO server-side ceiling check anywhere in smb2-cmd-write.c (READ has one at
  // smb2-cmd-read.c:360; WRITE has none) -- and that block is STILL LIVE when
  // socket.c:836 calls smb2_calc_signature(), which mallocs a second block
  // holding a whole copy of the PDU. Measured live together on the desktop
  // harness at N=16384: 16,384 and 16,496, both alive at once. Peak demand is
  // therefore 2N + 112, not N.
  //
  // Wrong number: "~41 KB, which has real margin" is the largest free block at
  // SERVER START, with no client attached. During a transfer it is not that.
  // diag23.log, one live connection (peak simultaneous connections = 1, so this
  // is not connection pile-up), three failures 38 s apart on separate
  // reconnects all landing on the SAME value -- a steady state, not a leak:
  //
  //   service(in) FAILED, largest free block 15348: malloc failed while adding
  //   VARIABLE tail
  //
  // 16,384 > 15,348, so the FIRST allocation fails outright, before signing is
  // even reached. libsmb2 returns -1 up through smb2_service(), our loop tears
  // the connection down, iOS reconnects and restarts the copy: 18 identical
  // cycles in that log, and the copy never completes.
  //
  //   N       tail     signing scratch   both live
  //   16384   16,384   16,496            32,880   <- 2.1x over the 15,348 seen
  //    8192    8,192    8,304            16,496   <- still over it
  //    4096    4,096    4,208             8,304   <- 1.85x under; each block 3.6x under
  //
  // 8192 is not enough. Its tail fits, but the scratch then needs a SECOND
  // region, and diag.log records only the LARGEST free block -- never the
  // histogram -- so nothing in the evidence shows a second region exists. The
  // design rule against a single measured largest block L is N <= (L-128)/2,
  // which at L=15,348 gives 7,610: 4096 satisfies it, 8192 does not.
  //
  // All three, not just max_write_size: the failing PDU died during RECEIVE,
  // before dispatch, so nothing records which command it was. Lowering all
  // three is the hedge against being wrong about which one. It costs nothing --
  // max_transact_size is already capped first by our own kSoftWireBudgetBytes
  // (8192) and by what iOS actually asks for (1024 in every query_directory).
  //
  // Cost is round trips, not throughput: AES is linear in bytes either way and
  // dominates on this 160 MHz core. The honest comparison is "slower" against
  // "has never once completed on this hardware".
  //
  // ⚠️ v82 CHANGED ONE OF THIS BLOCK'S INPUTS, AND DELIBERATELY DID NOT ACT ON
  // IT YET. Signing is now adaptive (see acceptOneConnection), so for a client
  // that does not require it -- an iPhone -- the signing scratch is never
  // allocated and peak demand per in-flight PDU drops from 2N + 112 back to N.
  // 8192 would then need 8,192 against the ~15,348 steady state measured in
  // diag27: comfortable, and it would halve the round trips.
  //
  // Not done in the same version, for two reasons. Signing is ADAPTIVE, not
  // off, so a client that requires it brings the scratch back and the size has
  // to be safe for both cases or become per-context -- a much bigger change
  // than a constant. And v82 already moves the one variable whose effect we are
  // trying to observe (how much faster a copy gets); measuring two changes at
  // once is how v16 through v21 failed to attribute anything.
  //
  // The next log decides it: if the copy completes and `zero-filled ... in
  // NNN ms` is small, what is left is round trips, and this is the lever.
  //
  // RAISE THIS ONLY WITH A MEASUREMENT -- and note that the measurement asked
  // for since v72 STILL does not exist, because v74's 32-byte FinderInfo
  // null-stream write was consuming the latch in writeCmd before any real write
  // reached it. That is fixed in the same version as this line.
  srv->max_transact_size = 4096;
  srv->max_read_size = 4096;
  srv->max_write_size = 4096;
  // Must be 1, not 0: a client that requires signing (smbprotocol defaults to
  // require_signing=True; macOS/iOS prefer or require it) gets silently
  // dropped after NEGOTIATE with signing_enabled == 0 -- see
  // lib/smb2/lib/libsmb2.c:4377-4385 and this project's task-2 findings.
  //
  // NECESSARY BUT NOT SUFFICIENT. An earlier revision of this comment claimed
  // "setting it to 1 only advertises the capability; libsmb2 does all key
  // derivation and per-message signing itself". That was wrong, and it is why
  // v64 shipped a server that signed nothing: `signing_enabled` feeds exactly
  // one expression, the advertised SecurityMode bitmask at libsmb2.c:4389. What
  // actually gates key derivation (libsmb2.c:4172-4177,
  // `if (smb2->sign) smb2_create_signing_key()`) and per-PDU signing
  // (pdu.c:701-707, `if (smb2->sign) smb2_pdu_add_signature()`) is the separate
  // per-context `smb2->sign` flag -- which the server-side NEGOTIATE handler
  // raises only under one of three conditions (libsmb2.c:4355-4382): the CLIENT
  // sent SMB2_NEGOTIATE_SIGNING_REQUIRED, or the negotiated dialect is 2.1.0,
  // or it is >= 3.1.1. This server pins the dialect to 3.0.2 (see
  // smb2_set_version() in acceptOneConnection()), and real clients advertise
  // SIGNING_ENABLED WITHOUT SIGNING_REQUIRED -- so none of the three could ever
  // fire. See smb2_set_sign() in acceptOneConnection() for the fix and the
  // captured wire evidence.
  //
  // Note that the 206-check harness suite could not see this and still cannot:
  // its client (smbprotocol) defaults to require_signing=True and therefore
  // sends SIGNING_REQUIRED, which raised `smb2->sign` by itself on every suite
  // run. The suite's green result is invariant to the fix -- it can neither
  // validate it nor catch its removal. That is what the new
  // test_signing_enabled_only_client() check is for.
  srv->signing_enabled = 1;
  // Must be 0, not 1 -- despite looking like the "permissive" choice, 1 is
  // the one that breaks x3 authentication. libsmb2.c:4179-4183 sets
  // SMB2_SESSION_FLAG_IS_GUEST in the session-setup reply whenever
  // `server->allow_anonymous && (no user || no password)` -- and by the time
  // that check runs, `smb2->password` has *already* been wiped back to ""
  // by ntlmssp_authenticate_blob() (ntlmssp.c:1276, "wipe pw out now that
  // its been used"), unconditionally, for every successful authentication,
  // not just anonymous ones. With allow_anonymous=1 this makes EVERY
  // correctly-authenticated x3 session get flagged guest -- confirmed
  // empirically in Task 4 round 1 (see task-4-report.md's "Concerns" and
  // test/host/README.md's "Baseline (Task 4, round 1 -- superseded, kept for
  // history)"): setting the password in authorize_user is necessary but was
  // not sufficient on its own. Guest
  // sessions have no session key and cannot sign, and signing is mandatory
  // here (signing_enabled=1 above) -- so the guest fallback this field
  // enables was never actually usable, only actively breaking the one path
  // that is. With allow_anonymous=0: the guest-flag branch above can never
  // fire (line 4179's own condition requires it); libsmb2.c:4361's "does
  // this dialect force signing" check becomes unconditionally true instead
  // of being gated on `smb2->password` (irrelevant here, since we always
  // require signing anyway); ntlmssp.c:1224 (`!smb2->password &&
  // !allow_anonymous` -> error) is satisfied because authorize_user()
  // always sets a password for the one user it accepts (unknown users are
  // already rejected earlier, at ntlmssp.c:1219); and genuinely anonymous
  // connection attempts now correctly fail instead of silently succeeding
  // as guest. See the outer docs repo's updated spec, section
  // "`allow_anonymous` 從 1 改成 0", for the full before/after trace through
  // all four call sites that read this field. DO NOT change this back to 1
  // -- it looks more permissive but it is strictly worse: it cannot enable
  // any working anonymous path (guest can never sign), and it actively
  // disables the only session type (authenticated x3) that can.
  srv->allow_anonymous = 0;
  strncpy(srv->hostname, "crossmosa", sizeof(srv->hostname) - 1);
  strncpy(srv->domain, "WORKGROUP", sizeof(srv->domain) - 1);
  // Matches the defaults smb2_serve_port() itself would apply if left zero
  // (libsmb2.c:4500-4502, 4521) -- reproduced here since this class never
  // calls that function.
  memcpy(srv->guid, "libsmb2-srvrguid", 16);
  srv->session_counter = 0x1234;

  srv->handlers = smbGetRequestHandlers();
  if (srv->handlers == nullptr) {
    LOG_ERR(kTag, "smbGetRequestHandlers() returned null");
    return false;
  }

  int fd = -1;
  int err = smb2_bind_and_listen(srv->port, /*max_connections=*/8, &fd);
  if (err != 0) {
    LOG_ERR(kTag, "smb2_bind_and_listen(port=%u) failed: %d", (unsigned)srv->port, err);
    DiagLog::line("SMB begin FAILED: bind_and_listen(port=%u) err=%d, largest free block %u", (unsigned)srv->port, err,
                  smbLargestFreeBlock());
    return false;
  }
  srv->fd = fd;

  // Last, so no earlier failure path has to unwind it -- and before
  // `running_ = true`, because a handler must never see the tables null (see
  // SmbFileHandlers.h). Failing here is the one case that needs the socket
  // closed by hand, since `srv` has not been released into `server_` yet.
  if (!smbAllocateTables()) {
    close(fd);
    return false;
  }

  for (auto& c : clients_) c = nullptr;

  server_ = srv.release();
  running_ = true;
  LOG_INF(kTag, "listening on port %u", (unsigned)port_);
  return true;
}

void SmbServer::acceptOneConnection() {
  // Check for a free registry slot BEFORE touching the listening socket at
  // all (fix round 1, review finding 1). The previous version accepted the
  // connection first and only discovered the registry was full afterward,
  // at which point the least-bad option left was to reset a client we'd
  // already bootstrapped into the negotiate state machine -- with no log
  // line, since that path predated this one. Checking first means a full
  // registry simply leaves the pending connection sitting in the OS-level
  // listen backlog (smb2_bind_and_listen()'s backlog=8) instead: a client
  // sees an ordinary "still connecting" wait, not an abrupt reset, and it
  // costs nothing to wait, since cullOneDeadClient() now runs every tick
  // (see tick()) and will free a slot as soon as any client disconnects.
  smb2_context** freeSlot = nullptr;
  for (auto& slot : clients_) {
    if (slot == nullptr) {
      freeSlot = &slot;
      break;
    }
  }
  if (freeSlot == nullptr) {
    // select() is level-triggered and this pending connection is never
    // drained (it just sits in the listen backlog, per the comment above),
    // so server_->fd reports readable again on every subsequent tick with a
    // zero timeout (tick()'s comment) -- without the latch this LOG_ERR would
    // fire once per activity-loop iteration for as long as the registry
    // stays full. That matters more than usual here: lib/Logging/Logging.cpp
    // keeps only a 16-entry RTC ring buffer for crash post-mortems, and this
    // device has no serial port -- that ring buffer plus the SD-card
    // diag.log are the *only* evidence that survives a crash. A flood of one
    // repeated line would evict every other recent diagnostic from it.
    // registryFullLogged_ is cleared in cullOneDeadClient() as soon as a
    // slot actually frees up, so a fresh full-registry occurrence still gets
    // logged.
    if (!registryFullLogged_) {
      LOG_ERR(kTag, "registry full (%zu/%zu clients) -- leaving pending connection in the listen backlog", kMaxClients,
              kMaxClients);
      registryFullLogged_ = true;
    }
    return;
  }

  smb2_context* ctx = nullptr;
  int err = smb2_serve_port_async(server_->fd, /*to_msecs=*/10, &ctx);

  if (err != 0) {
    LOG_DBG(kTag, "smb2_serve_port_async failed: %d", err);
    // LOG_DBG is compiled OUT of gh_release (platformio.ini LOG_LEVEL=1,
    // Logging.h:47-51), which is the build that ships -- so without a
    // DiagLog companion a failed accept leaves NOTHING behind on a device with
    // no serial port. Same reasoning at every failure site in this file.
    //
    // TWO CAUSALLY DIFFERENT FAILURES ARRIVE THROUGH THIS ONE int, and they
    // need OPPOSITE treatment. Read out of the vendored source, not inferred:
    //
    //   -EIO     the raw accept() failed and THE CONNECTION IS STILL QUEUED --
    //            socket.c:1483-1494 reaches its `else` only when clientfd < 0,
    //            so nothing was dequeued. select() is level-triggered, so this
    //            repeats on every tick for as long as the condition lasts:
    //            once per activity-loop iteration plus up to 8 more inside the
    //            HTTP burst. It is the ONLY shape that can flood, so it is the
    //            only one that gets latched.
    //
    //   -ENOMEM  accept() already SUCCEEDED and dequeued the connection; it was
    //            smb2_init_context() that returned NULL (libsmb2.c:4455-4458).
    //            Nothing is left queued, so it CANNOT repeat by itself -- it
    //            never needed suppressing. And it is this project's #1
    //            documented crash class, the reason v61 and v62 exist: one
    //            dropped line here could be the only evidence that this device
    //            ran out of heap while a client was connecting.
    //
    // DO NOT collapse this back into a single boolean. A latch keyed on "have
    // I logged anything at all" lets an earlier -EIO swallow a later -ENOMEM,
    // which is strictly worse than the flood the latch was added to stop.
    if (err == -EIO) {
      if (!acceptFailLogged_) {
        DiagLog::line(
            "SMB accept FAILED: accept() err=%d, connection still queued, largest free block %u"
            " (repeats suppressed while this persists)",
            err, smbLargestFreeBlock());
        acceptFailLogged_ = true;
      }
    } else {
      // Never latched. `ctx` is NULL on every path upstream can currently take
      // here (accept_cb sets *psmb2 = NULL up front and fills it only on
      // success, libsmb2.c:4455), so there is normally no error string to
      // report -- checked rather than assumed, and destroyed if one ever does
      // come back, since this function would otherwise drop the only pointer
      // to it.
      DiagLog::line("SMB accept FAILED: err=%d, connection already dequeued, largest free block %u: %s", err,
                    smbLargestFreeBlock(), ctx != nullptr ? smb2_get_error(ctx) : "(no context was created)");
      if (ctx != nullptr) smb2_destroy_context(ctx);
    }
    return;
  }

  // err == 0: the self-repeating condition is not in effect right now, whether
  // because a connection was accepted or because poll() timed out with nothing
  // pending. Cleared HERE, at the point the state actually flips -- the way
  // registryFullLogged_ is cleared inside cullOneDeadClient() -- rather than on
  // whatever later thing happens to succeed.
  acceptFailLogged_ = false;
  if (ctx == nullptr) return;  // poll() timeout: nothing pending, nothing to do

  // Pin the dialect ceiling to SMB 3.0.2 -- do NOT "upgrade" this to 3.1.1 or
  // remove it. See the outer docs repo's spec section "方言上限釘死在
  // SMB 3.0.2" for the full writeup; short version:
  //
  // SMB 3.1.1 has a real, currently-unfixed bug on this server: the
  // session-setup reply's signature fails the client's own verification
  // (`Server message signature could not be verified: X != Y`) -- most
  // likely because 3.1.1's signing-key derivation folds in a running
  // preauth-integrity hash that libsmb2's server side doesn't reproduce
  // identically to a real client. See test/host/README.md's "Baseline
  // (Task 4, round 2)" for how this was found and
  // test/host/smb_smoke_test.py's test_smb311_dialect_not_offered for the
  // regression test that pins the *fixed* form of this (a 3.1.1-only client
  // failing to negotiate at all, cleanly, rather than failing later at a
  // signature mismatch).
  //
  // `smb2_negotiate_request_cb` (lib/smb2/lib/libsmb2.c:4222) switches on
  // `smb2->version` to decide which dialect(s) to offer back
  // (libsmb2.c:4250-4279): SMB2_VERSION_ANY offers all five (2.0.2 through
  // 3.1.1, including the broken one); pinning to a single concrete version
  // like SMB2_VERSION_0302 makes `dialect_count = 1` and
  // `dialects[0] = smb2->version` -- 3.0.2 is the only dialect this server
  // ever advertises, so a client cannot land on 3.1.1 even if it offers it.
  // A real client (iOS's SMB2 client included, per Visuality Systems' iOS
  // SMB article: iOS is reported to use the 3.0.2 dialect variant
  // internally and Visuality's own guidance is to configure servers for
  // 3.0.2 rather than 3.1.1-only) sends a dialect *list*; as long as 3.0.2
  // is in that list -- true for any modern client, since it predates
  // 3.1.1 -- negotiation lands there instead of failing outright.
  //
  // Ordering: `smb2_set_version()` (a trivial `smb2->version = version;`
  // setter, lib/smb2/lib/init.c:768-772) MUST run before
  // `smb2_negotiate_request_cb` can possibly execute, since that's the one
  // function that reads `smb2->version` to build the dialect list. Verified
  // by reading the accept path, not assumed: `smb2_serve_port_async()` (via
  // `smb2_accept_connection_async()`, lib/smb2/lib/socket.c:1458) only does
  // a non-blocking TCP `poll()`+`accept()` -- it never reads any SMB2 PDU
  // bytes. `crossmosa_smb2_finish_accept()` below only *registers*
  // `smb2_negotiate_request_cb` as the handler for the first PDU
  // (`smb2_allocate_pdu(smb2, SMB2_NEGOTIATE, smb2_negotiate_request_cb,
  // ...)`, libsmb2.c:4736) -- it does not invoke it. That registered
  // callback can only run inside `smb2_service(ctx, POLLIN)`, called from
  // this class's own `tick()` -- and `ctx` isn't even in `clients_[]` (the
  // only thing `tick()`'s `select()` polls) until `*freeSlot = ctx;` below
  // runs, at the very end of this same function. So the earliest possible
  // call to `smb2_negotiate_request_cb` is on a *later* invocation of
  // `tick()` (the next iteration of the caller's activity loop) -- strictly
  // after this entire function, and therefore after this line, returns.
  smb2_set_version(ctx, SMB2_VERSION_0302);

  // Turn signing ON for this connection. Without this line the server answers
  // every request after session setup with SMB2_FLAGS_SIGNED clear and a
  // sixteen-byte zero signature, and a client that signs rejects that.
  //
  // This is not a theory. Captured off the wire against this exact code, driven
  // by Samba's smbclient 4.19.5 -- an SMB2 client implementation with no
  // libsmb2 in it:
  //
  //   frame 12  TREE_CONNECT  request   flags=0x08 SIGNED  sig=0a1a7e7d66...
  //   frame 13  TREE_CONNECT  response  flags=0x01         sig=0000000000...
  //   -> smbclient: "tree connect failed: NT_STATUS_ACCESS_DENIED", and it
  //      sends nothing further.
  //
  // Note the SHAPE of that failure: NT_STATUS in the response header is
  // STATUS_SUCCESS and this server logs "tree_connect ok". The rejection is the
  // client refusing a response it was obliged to verify (MS-SMB2 3.2.5.1.3 --
  // once Session.SigningRequired is set, an unsigned response is discarded;
  // Samba surfaces that as ACCESS_DENIED). So the server-side log reads healthy
  // right up to the moment the client gives up.
  //
  // WHAT THIS DOES NOT FIX: iOS does not enforce signed responses. The v64
  // field log (output/v64-smb-server, diag11) shows an iPhone getting past
  // TREE_CONNECT to IOCTL and QUERY_INFO against the unsigned server, so this
  // was never what the field failure was -- that was the compound-FileId gap, see
  // gLastCreatedId in SmbFileHandlers.cpp. This fix is what makes Samba,
  // macOS Finder, Windows and the Linux kernel client able to connect at all.
  //
  // Why setting it HERE works, and why it must be here and not later:
  //   - `smb2_set_sign()` is public API (include/smb2/libsmb2.h:371,
  //     init.c:753-756, a plain `smb2->sign = val`). No fourth divergence in
  //     the vendored tree.
  //   - The NEGOTIATE handler cannot clear it. Its only writes to `smb2->sign`
  //     are `if (smb2->seal) sign = 0; else if (will_sign) sign = 1;`
  //     (libsmb2.c:4375-4382). `seal` is never set on this server, and
  //     `will_sign` false simply means neither branch executes -- a pre-existing
  //     1 survives untouched.
  //   - It must be set BEFORE the first PDU is serviced, because
  //     libsmb2.c:4389 computes the advertised SecurityMode from it. With it
  //     set, this server advertises SIGNING_ENABLED|SIGNING_REQUIRED, which is
  //     both self-consistent (we do sign) and what makes clients sign back. The
  //     accept-path argument spelled out above smb2_set_version() applies
  //     verbatim: no PDU can be serviced until a later tick().
  //   - Key derivation then happens on the CORRECT branch: libsmb2.c:4172-4177
  //     calls smb2_create_signing_key(), which for dialect <= 3.0.2 derives via
  //     SP800-108 with label "SMB2AESCMAC" / context "SmbSign"
  //     (libsmb2.c:638-647). That is a DIFFERENT branch from the preauth-hash
  //     derivation used for >= 3.1.1 (libsmb2.c:660-682) -- the one with the
  //     known bug that motivated pinning to 3.0.2. Pinning the dialect and
  //     enabling signing are therefore not in tension: the pin is precisely
  //     what keeps signing on the branch that works.
  //
  // THREE CONSEQUENCES a future reader must not "fix":
  //   1. The interim (STATUS_MORE_PROCESSING_REQUIRED) session-setup reply goes
  //      out unsigned, correctly -- and the ONLY thing that keeps it unsigned is
  //      smb2_pdu_add_signature()'s SESSION_SETUP-with-non-zero-status check
  //      (smb2-signing.c:226-233). NOT the `session_id == 0` guard at
  //      smb2-signing.c:244: libsmb2.c:4079 has already assigned a session id
  //      (0x1234 upward, libsmb2.c:4521) by then, verified on the wire. That
  //      matters because the very next guard returns -1 when session_key_size
  //      is 0, so relaxing the status check believing session_id was a second
  //      line of defence would attempt to sign with a garbage key.
  //   2. Round 1 of session setup calls smb2_create_signing_key() with
  //      session_key still NULL, so smb2_derive_key() does
  //      memcpy(input_key, NULL, 0) (libsmb2.c:589-590). n == 0, so nothing is
  //      dereferenced; the garbage key it produces is overwritten in round 2 and
  //      is never used to sign anything (see consequence 1). Accepted, not a bug
  //      to chase. The adjacent `sign && !have_valid_session_key -> close`
  //      guard cannot fire either: have_valid_session_key starts at 1
  //      (libsmb2.c:4011) and is only cleared if ntlmssp_get_session_key fails.
  //   3. This also activates INBOUND verification, which was dead code before:
  //      socket.c:831-846 verifies a request's signature when `smb2->sign` is
  //      set and the request carries SMB2_FLAGS_SIGNED, and a mismatch returns
  //      -1, which tears the connection down. (smb2_pdu_check_signature() at
  //      smb2-signing.c:272-279 returning 0 unconditionally is a red herring --
  //      it is not the path in use.) A new failure mode, never exercised
  //      against iOS: "Wrong signature in received PDU" in diag.log means a
  //      transfer died there. Note the check is gated on the client actually
  //      setting the flag, so an unsigned request is still accepted despite the
  //      REQUIRED advertisement -- we do not enforce what we advertise.
  // v82: adaptive by default; forced only when a caller asked for it (the
  // desktop harness does, so smbclient and kernel cifs keep working). See
  // SmbServer::setForceSigning() in the header for the measurement.
  if (gForceSigning) {
    smb2_set_sign(ctx, 1);
  }

  // ⚠️ v82: NOT CALLED UNCONDITIONALLY ANY MORE. The owner's decision, made explicitly and
  // reaffirmed after I set out the cost.
  //
  //     smb2_set_sign(ctx, 1);
  //
  // WHAT THIS CHANGES, precisely. `smb2->sign` now starts at 0 and libsmb2's
  // own NEGOTIATE logic decides (libsmb2.c:4357-4385): `will_sign` is raised
  // only when the CLIENT sends SIGNING_REQUIRED, because the dialect 2.1.0 and
  // >= 3.1.1 clauses cannot fire while we pin 3.0.2. So this is ADAPTIVE, not
  // "signing off" -- a client that requires signing still gets a signed
  // session, and `server->signing_enabled` stays 1 so such a client is never
  // dropped before the reply (that was the v64 bug; it stays fixed). An iPhone
  // does not require signing, so it now gets an unsigned, fast session.
  //
  // WHY. Signing runs AES-CMAC over every PDU in BOTH directions, and the
  // vendored AES is a reference implementation whose ECB entry point re-derives
  // the whole 176-byte key schedule for every 16-byte block
  // (aes_reference.c:444-456). A 5 MB file is 326,164 key expansions per
  // direction -- measured at ~0.2 s on a desktop, which on a 160 MHz RV32 is
  // this project's long-recorded ~300-600 KB/s signing ceiling, i.e. tens of
  // seconds on one book.
  //
  // WHY THAT TRADE IS ACCEPTABLE HERE. The server is off except during a
  // file-transfer session, on a home LAN, on a personal device. And the
  // confidentiality half of the argument was already moot: **this server has
  // never encrypted anything**, so the bytes were always in the clear. Signing
  // buys integrity against an ACTIVE on-path attacker, which is not a threat
  // that exists in that setting.
  //
  // ⚠️ NTLM authentication (x3/x3) is UNTOUCHED and still gates every session.
  // Do not "simplify" that away as well by the same reasoning -- signing and
  // authentication protect different things, and authentication is the part
  // that keeps a stranger off the share at all.
  //
  // ⚠️ WHAT IT ACTUALLY COSTS, and it is not security: any client that
  // advertises SIGNING_ENABLED and then refuses an unsigned reply stops
  // working. Two of the three INDEPENDENT implementations the desktop suite
  // uses -- Samba smbclient and the Linux kernel cifs client -- are exactly the
  // thing that keeps this project from testing libsmb2 with libsmb2, and every
  // blocker found in the SMB work came through one of them. Their behaviour was
  // MEASURED before shipping this; see test/host/README.md.
  //
  // TO PUT IT BACK: restore the call. Everything the block above describes --
  // the accept-path ordering, the 3.0.2 key-derivation branch, the inbound
  // verification side effect -- remains accurate and applies unchanged.

  // Finishes preparing `ctx` to receive an SMB2_NEGOTIATE request -- the one
  // piece of smb2_serve_port()'s own accept path this file cannot do itself
  // (see the header comment above and crossmosa_smb2_finish_accept()'s own
  // banner in lib/smb2/lib/libsmb2.c). On failure, the context has already
  // been closed via smb2_close_context() inside that function (matching
  // upstream's own error handling) -- still register it below so the normal
  // cull path reclaims it and fires destruction_event, rather than a second,
  // ad hoc teardown here.
  err = crossmosa_smb2_finish_accept(ctx, server_);
  if (err != 0) {
    LOG_ERR(kTag, "crossmosa_smb2_finish_accept failed: %d", err);
    DiagLog::line("SMB finish_accept FAILED: err=%d ctx=%p, largest free block %u: %s", err, (void*)ctx,
                  smbLargestFreeBlock(), smb2_get_error(ctx));
  }

  *freeSlot = ctx;
}

void SmbServer::cullOneDeadClient() {
  for (auto& ctx : clients_) {
    if (ctx == nullptr) continue;
    if (smb2_get_fd(ctx) >= 0) continue;  // still connected, nothing to cull

    if (server_->handlers != nullptr && server_->handlers->destruction_event != nullptr) {
      server_->handlers->destruction_event(server_, ctx);
    }
    smb2_destroy_context(ctx);
    ctx = nullptr;
    // A slot just became available again -- clear the "registry full" latch
    // here, at the exact point the state actually flips, rather than at the
    // next successful accept (which might not happen for a while, or might
    // never happen if no client is currently waiting). This is the only
    // place a slot transitions from occupied to free, so it is the one spot
    // that can't be missed. See acceptOneConnection() for why the latch
    // exists.
    registryFullLogged_ = false;
    return;  // at most one per tick -- the active list mutates on destroy
  }
}

void SmbServer::tick() {
  if (!running_ || server_ == nullptr) return;

  // Runs first, unconditionally -- must not be gated behind select()'s
  // result (fix round 1, review finding 1). A dead context contributes no
  // fd to select() (see the loop below), so once every *other* client has
  // gone quiet, select() starts returning 0 on every later tick. The
  // previous version called this only after `ready > 0`, which meant that
  // once that happened, the cull never ran again: dead smb2_context
  // allocations (iovectors, queues, strings) sat there for the rest of
  // file-transfer mode, and acceptOneConnection() found no free registry
  // slot for the next real client. See task-3-report.md's fix-round-1
  // writeup.
  cullOneDeadClient();

  fd_set rfds;
  fd_set wfds;
  FD_ZERO(&rfds);
  FD_ZERO(&wfds);
  FD_SET(server_->fd, &rfds);
  int maxfd = server_->fd;

  for (smb2_context* ctx : clients_) {
    if (ctx == nullptr) continue;
    t_socket fd = smb2_get_fd(ctx);
    if (fd < 0) continue;  // closed this tick or an earlier one, will be culled next tick
    int events = smb2_which_events(ctx);
    if (events & POLLIN) FD_SET(fd, &rfds);
    if (events & POLLOUT) FD_SET(fd, &wfds);
    if (fd > maxfd) maxfd = fd;
  }

  // The only intended difference from smb2_serve_port()'s own loop: a zero
  // timeout, so servicing already-connected clients never blocks the caller's
  // activity loop. Upstream uses a fixed 100ms timeout because it owns a
  // dedicated blocking loop; this class is called once per iteration of a loop
  // that also has to keep driving the e-ink display, so it cannot afford to
  // wait at all. (The accept path below is the one exception and is bounded at
  // 10 ms -- smb2_serve_port_async(to_msecs=10) in acceptOneConnection().)
  timeval zeroTimeout{0, 0};
  int ready = select(maxfd + 1, &rfds, &wfds, nullptr, &zeroTimeout);
  if (ready <= 0) return;

  for (smb2_context* ctx : clients_) {
    if (ctx == nullptr) continue;
    t_socket fd = smb2_get_fd(ctx);
    if (fd < 0) continue;

    if (FD_ISSET(fd, &rfds)) {
      if (smb2_service(ctx, POLLIN) < 0) {
        const char* err = smb2_get_error(ctx);
        LOG_DBG(kTag, "smb2_service(in) failed: %s", err);
        // THE line first-contact diagnosis needs. smb2_get_error() is public
        // (libsmb2.h:608) and libsmb2 fills it with its own descriptions --
        // including its internal allocation failures ("Failed to allocate
        // pdu", pdu.c:95; several "malloc failed while adding ..." in
        // socket.c). So this reports the vendored library's own OOMs in its
        // own words, with the contiguity figure beside them, without touching
        // lib/smb2 -- which is why no fourth divergence is needed after all.
        if (!isCleanRemoteClose(err)) {
          DiagLog::line("SMB service(in) FAILED ctx=%p, largest free block %u: %s", (void*)ctx, smbLargestFreeBlock(),
                        err);
        }
        smb2_close_context(ctx);
        continue;  // don't also service POLLOUT on a context we just closed
      }
    }
    fd = smb2_get_fd(ctx);  // smb2_service(POLLIN) can change the fd's validity
    if (fd >= 0 && FD_ISSET(fd, &wfds)) {
      if (smb2_service(ctx, POLLOUT) < 0) {
        const char* err = smb2_get_error(ctx);
        LOG_DBG(kTag, "smb2_service(out) failed: %s", err);
        if (!isCleanRemoteClose(err)) {
          DiagLog::line("SMB service(out) FAILED ctx=%p, largest free block %u: %s", (void*)ctx, smbLargestFreeBlock(),
                        err);
        }
        smb2_close_context(ctx);
      }
    }
  }

  if (FD_ISSET(server_->fd, &rfds)) {
    acceptOneConnection();
  }
}

void SmbServer::end() {
  if (!running_ && server_ == nullptr) {
    smbReleaseTables();  // no-op unless a half-built begin() left them behind
    return;
  }

  for (auto& ctx : clients_) {
    if (ctx != nullptr) {
      // Fire destruction_event exactly as cullOneDeadClient() does. Skipping
      // it was harmless while end() did not really close anything -- the
      // tables were in .bss and outlived the activity -- but fix round 1 made
      // releasing them close every handle, which turned "closed but never
      // deleted" into a reachable outcome: a client sets delete-on-close, the
      // user leaves file-transfer mode, and the file it asked to have removed
      // is still on the card. destructionEvent() is what performs the pending
      // delete (and logs the result); it must run BEFORE smbReleaseTables()
      // below, which it does.
      if (server_ != nullptr && server_->handlers != nullptr && server_->handlers->destruction_event != nullptr) {
        server_->handlers->destruction_event(server_, ctx);
      }
      smb2_destroy_context(ctx);
      ctx = nullptr;
    }
  }

  if (server_ != nullptr) {
    if (server_->fd >= 0) {
      close(server_->fd);
    }
    delete server_;  // matches makeUniqueNoThrow<smb2_server>() in begin()
    server_ = nullptr;
  }

  running_ = false;

  // AFTER every context is destroyed, never before: releasing runs
  // ~OpenFileEntry, which closes (and therefore syncs) any handle a client
  // left open, and no handler may still be able to reach the tables.
  smbReleaseTables();
}
