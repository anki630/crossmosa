// Task 4: real connection-layer handlers (authentication, tree connect,
// create/close) plus the eleven trivial-but-not-optional handlers.
// Task 5: query_directory and query_info (browsing).
// Task 6: read/write/flush (transfer).
// Task 7: real file timestamps AND set_info (rename / delete / set size).
//
// set_info required a patch to the vendored library to be reachable AT ALL --
// upstream refuses every SET_INFO payload from a DECODE function, which tears
// the connection down instead of failing the request. See setInfoCmd()'s
// banner below and docs/third-party/libsmb2-vendoring.md, "The third patch".
#include "SmbFileHandlers.h"

#include "SmbServer.h"  // kSmbUser / kSmbPassword / kSmbShareName -- shared with the on-screen
                        // connection details (Task 8); see that header for why they live there

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>  // makeUniqueNoThrow -- smbAllocateTables()

#include "util/DiagLog.h"
#include "util/SdDateTime.h"
#include "util/FatTimestamp.h"
#include "util/ProtectedPath.h"

// Task 8: the directory scan below can legitimately run to the end of a very
// large directory inside one network callback (see queryDirectoryCmd()'s
// scan-bound comment for why it cannot be bounded), and tick() is now driven
// from the file-transfer activity's own loop -- so this file has to feed the
// task watchdog itself. Device-only: the desktop harness (test/host/) has no
// ESP-IDF, and defines neither ARDUINO nor ESP_PLATFORM.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp_task_wdt.h>
#define CROSSMOSA_SMB_HAS_TASK_WDT 1
#else
#include <chrono>
#endif

// v80: a monotonic millisecond source for the zero-fill measurement, following
// the same rule as the watchdog block above -- millis() is Arduino's, and the
// harness defines neither ARDUINO nor ESP_PLATFORM. Only ever used to time a
// duration, never as a wall clock.
inline uint32_t smbMonotonicMs() {
#if defined(ARDUINO) || defined(ESP_PLATFORM)
  return static_cast<uint32_t>(millis());
#else
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

#include <ctime>  // time_t, for the smb2_timeval range clamp in modifyTimeOf()
#include <cstdarg>  // va_list -- trace(), the bounded mount-time log
#include <cstdlib>
#include <cstdio>   // snprintf, for the ReplaceIfExists holding name
#include <cstring>
#include <limits>
#include <memory>
#include <strings.h>  // strcasecmp -- share-name matching should be case-insensitive
#include <utility>

namespace {

constexpr char kTag[] = "SMB";

// ---------------------------------------------------------------------------
// Task 8, spec "diagnostics": every allocation >= 8 KB, with its size and its
// outcome.
//
// Rate-limited on the SUCCESS side, and that is a deliberate deviation worth
// stating plainly rather than burying. DiagLog::append() opens, writes,
// flushes and closes diag.log for every single line -- tens of milliseconds of
// SD time each. The only >= 8 KB allocation this file makes is read_cmd's
// reply buffer, one per READ request, and the negotiated max_read_size is
// 32 KB: reading a 100 MB book over SMB is ~3,200 of them. One line each would
// add roughly a minute of SD writes to that transfer, on the same card the
// transfer is using -- the instrumentation would become the dominant cost of
// the thing it instruments, and would say so in the very log meant to explain
// why transfers are slow.
//
// So: EVERY FAILURE IS LOGGED, individually and unconditionally -- that is the
// line anybody will ever actually go looking for. Successes are logged on a
// 1, 2, 4, 8, 16, ... schedule per site: full detail over the first handful
// (where a fragmented heap fails, if it is going to) and a running summary
// after that. Every line carries the attempt count, the largest size seen and
// the failure count, so one line still answers "how many, how big, how many
// failed".
//
// NOT covered, and it needs to be said: libsmb2's own allocations. The receive
// buffer and the encoded reply (output_buffer_length + 1024) are both in
// max_transact_size territory, i.e. bigger than anything here, and they are
// plain malloc()s inside lib/smb2 with no REGISTERABLE allocator hook: there is
// no smb2_set_allocator() or equivalent, and while smb2_free_data() is public
// (libsmb2-raw.h:37) that is a deallocation call, not an interception point.
// Instrumenting those sizes directly would mean patching the vendored tree = a
// fourth divergence needing the maintainer's approval.
//
// It turned out not to be needed. libsmb2 sets a descriptive string on its own
// allocation failures ("Failed to allocate pdu", pdu.c:95; several "malloc
// failed while adding ..." in socket.c), and SmbServer::tick() now logs
// smb2_get_error() -- which is public API -- on every service failure, with
// smbLargestFreeBlock() beside it. So libsmb2's internal OOMs are reported in
// its own words, from outside, with the contiguity figure attached.
// Feeds the task watchdog on device, no-op on the desktop harness. Named
// rather than #if'd at each call site so the scan loop below reads the same in
// both builds.
inline void smbFeedWatchdog() {
#ifdef CROSSMOSA_SMB_HAS_TASK_WDT
  esp_task_wdt_reset();
#endif
}

constexpr size_t kBigAllocLogThreshold = 8 * 1024;

struct BigAllocStats {
  uint32_t attempts;
  uint32_t failures;
  uint32_t nextLogAt;  // power-of-two schedule; see above
  size_t maxBytes;
};

void diagBigAlloc(BigAllocStats& stats, const char* site, size_t bytes, bool ok) {
  if (bytes < kBigAllocLogThreshold) return;
  stats.attempts++;
  if (bytes > stats.maxBytes) stats.maxBytes = bytes;
  if (!ok) {
    stats.failures++;
    DiagLog::line("SMB alloc FAILED site=%s bytes=%zu attempt=%u failed=%u largest=%zu", site, bytes,
                  (unsigned)stats.attempts, (unsigned)stats.failures, stats.maxBytes);
    return;
  }
  if (stats.attempts < stats.nextLogAt) return;
  DiagLog::line("SMB alloc ok site=%s bytes=%zu attempt=%u failed=%u largest=%zu next=%u", site, bytes,
                (unsigned)stats.attempts, (unsigned)stats.failures, stats.maxBytes, (unsigned)(stats.nextLogAt * 2));
  stats.nextLogAt *= 2;
}

// One counter set per allocation site. Only read_cmd has a >= 8 KB site today;
// a second site gets its own object rather than sharing this one, so the
// power-of-two schedules stay independent.
BigAllocStats gReadReplyAllocStats = {0, 0, 1, 0};

// ---------------------------------------------------------------------------
// Open-file table. Fixed-size array, not a growing container (Global
// Constraints ban growth-reallocation patterns) -- keyed by the 16-byte
// smb2_file_id the client echoes back on every subsequent close/read/write/
// query_info/set_info call. 8 slots (Task 4 brief's own example size); Step
// 7's smoke test opens and closes 12 connections in a row specifically to
// prove destruction_event() reclaims slots rather than exhausting the table.
constexpr size_t kMaxOpenFiles = 8;

// 512, not 256. This buffer holds a WHOLE path, not one component, and this
// device's library is entirely Traditional Chinese: UTF-8 CJK is 3 bytes per
// character, so 256 bytes is only ~85 characters for the path *including*
// every parent directory. WebDAV already spends `char name[500]` on a single
// component (WebDAVHandler.cpp). Cost of the raise: 8 slots x 512 B = 4 KB of
// static BSS, allocated once at link time, never grown, never migrated.
constexpr size_t kMaxSmbPathLen = 512;

// Task 5: the search pattern of an in-progress directory enumeration.
//
// MS-SMB2 3.3.5.18: the client sends FileName on the FIRST QUERY_DIRECTORY of
// an enumeration and omits it on every continuation, so the server has to
// remember it. 96 bytes, not kMaxSmbPathLen: a pattern is a single name
// component, not a path, and in practice it is "*" (that is what macOS/iOS,
// Windows Explorer and smbclient all send for a plain listing). A pattern too
// long to fit degrades to match-all and is logged -- deliberately in the
// over-returning direction, because a client that receives entries it did not
// ask for filters them, whereas a client that silently does not receive a
// file it did ask for has no way to notice.
constexpr size_t kMaxSearchPatternLen = 96;

struct OpenFileEntry {
  bool inUse = false;
  smb2_context* owner = nullptr;  // connection that opened this -- destruction_event() matches on this
  smb2_file_id id{};
  HalFile file;
  bool isDirectory = false;
  // Task 6: the access mode the SURVIVING handle really has, not the one the
  // client asked for. Needed because there is no way to ask a HalFile whether
  // it was opened for writing (HalFile exposes no oflag, and neither does
  // SdFat's FsFile), and because createCmd can hand back strictly less access
  // than was requested -- an existing directory probed with write access, or
  // a read-only file opened with MAXIMUM_ALLOWED, both come back O_RDONLY.
  // write_cmd checks this instead of handing the bytes to the filesystem and
  // reading the tea leaves: SdFat's write() returns 0 for "not writable" and
  // 0 for "the card died", and those must not produce the same log line.
  bool writable = false;
  // v76: this handle's directory entry has been freed or moved out from under
  // it by a delete or rename on another handle. Set only by
  // markOtherHandlesStale(). Consumed by setBasicInfo(), setDisposition() and
  // renameOpenHandle() -- the three that WRITE through a handle -- and captured
  // by closeCmd()/destructionEvent() before the slot is cleared, so a
  // delete-on-close armed earlier is dropped rather than applied to whatever now
  // holds the name.
  //
  // Narrow on purpose. Once otherWritebackHandleOn() stopped refusing
  // read-only handles, exactly ONE write path can still reach a dead entry:
  // FatFile::timestamp() does its own cacheDirEntry(CACHE_FOR_WRITE) and writes
  // the date bytes directly, bypassing both FILE_FLAG_DIR_DIRTY and the
  // FAT_NAME_DELETED check that protects sync(). Reads and enumeration are left
  // alone -- a surviving cursor may return stale entries, which is untidy but
  // touches nothing, and changing enumeration semantics mid-listing is a bigger
  // risk than the tidiness is worth.
  bool stale = false;
  // Task 5 enumeration state. The CURSOR itself is not here: it is the
  // directory position inside `file` (SdFat's FatFile::openNext() advances
  // curPosition, and nothing else in this file moves it), which is what makes
  // "return the next N entries" work across the several QUERY_DIRECTORY round
  // trips one listing takes. These two only record what that cursor cannot:
  // whether the enumeration has been started at all (so the first query
  // rewinds, per MS-SMB2 3.3.5.18) and the pattern to keep filtering by.
  bool enumStarted = false;
  // v66: how many QUERY_DIRECTORY responses this enumeration has taken so far.
  // The one number that decides whether the small-client-buffer path needs more
  // work: iOS asks for 1024 bytes, and a worst-case entry is 1664, so the loop
  // can only guarantee ONE entry per response at that size. Whether that is
  // fine or unusably slow depends on how many files are in the folder and on
  // whether iOS grows its buffer once it starts getting answers -- neither of
  // which is knowable from here. Reported when the enumeration ends.
  uint16_t enumResponses = 0;
  // Task 7: FILE_DISPOSITION_INFORMATION sets this; close consumes it.
  // MS-FSCC 2.4.11 makes deletion happen when the handle closes, not when the
  // request arrives, and lets the client clear the flag again in between --
  // so it lives WITH THE HANDLE, and is therefore reachable only through
  // findOpenFile(owner, id). A flag keyed on anything less than that would let
  // one connection mark another connection's file for deletion.
  bool deletePending = false;
  char searchPattern[kMaxSearchPatternLen] = {};
  // v74: this handle is a NAMED STREAM that exists only for as long as the
  // handle does. There is no HalFile behind it and there never will be -- FAT
  // has nowhere to put an alternate data stream -- so every operation on it is
  // answered from nothing: writes are accepted and discarded, reads are at end
  // of file, and it must NEVER reach the filesystem. See createCmd's named
  // stream branch for why this exists at all, and closeCmd for the one thing
  // that would be genuinely dangerous if it were forgotten.
  bool isNullStream = false;
  // v67: the filter for this enumeration was too long to store, so it was used
  // straight out of the request and NOTHING usable is in searchPattern. Only
  // matters for a continuation, which cannot re-apply a filter it does not
  // have -- see the continuation branch in queryDirectoryCmd.
  bool patternOverlong = false;
  // Resolved local path -- Tasks 5-7 (query_directory/query_info/set_info)
  // need it. A fixed array, NOT std::string: this is assigned inside a
  // network callback on every single create, and any path >= 16 bytes
  // (libstdc++'s SSO limit -- i.e. essentially every real path) would
  // heap-allocate right there, unguarded. Under -fno-exceptions an
  // allocation failure is abort(), the exact crash class v61 and v62 were
  // shipped to eliminate. It also repeatedly grew and shrank 8 long-lived
  // buffers, the "grow-migrate leaves a permanent hole" pattern CLAUDE.md
  // hard-limit #6 warns about for pool p2. A fixed array in BSS has neither
  // problem: no allocation, no migration, no failure mode.
  char path[kMaxSmbPathLen] = {};
};

// Points into the single heap block smbAllocateTables() owns (defined at the
// end of this namespace, called from SmbServer::begin()). It was a .bss array
// until Task 8 fix round 1 measured what that costs: these three tables are
// 12,606 B, and .bss sits immediately below pool p3 -- the ~143,728 B pool
// malloc hits FIRST, and the one CLAUDE.md records background chapter reflow
// already filling before overflowing ~65 KB into p2. A permanent 10% cut to p3
// invalidates v59's alloc_fail=0 baseline for a server that only exists while
// one activity is on screen. Heap-resident, allocated once, never grown, freed
// in SmbServer::end() -- and nothing at all while the user is reading.
//
// NEVER null while a handler can run: begin() allocates before the listening
// socket joins the registry, and end() releases only after every context is
// destroyed, so no request can be in flight either side of the window.
OpenFileEntry* gOpenFiles = nullptr;
uint64_t gNextFileHandleCounter = 1;  // monotonically increasing, encoded into each new file_id

// Scratch for the normalized path of the ONE request currently in flight.
// Static rather than a stack local: at 512 bytes this is twice CLAUDE.md's
// ~256-byte ceiling for function locals, and these are network callbacks
// running on a task whose stack budget is not ours to set. Two such frames
// could previously be live in one callback chain.
//
// INVARIANT: exactly one SMB2 request is in flight at any moment. Handlers are
// reached only from SmbServer::tick() -> smb2_service(), which dispatches one
// PDU at a time on one task, and no handler here calls another. If that ever
// stops being true -- a second task, or a handler that re-enters the table --
// each caller needs its own buffer instead. Nothing outside this file may
// hold a pointer into it across a request boundary; createCmd() copies what
// it needs into the slot's own storage before returning.
char gPathScratch[kMaxSmbPathLen];

void makeFileId(size_t slotIndex, smb2_file_id& id) {
  memset(id, 0, sizeof(smb2_file_id));
  uint64_t counter = gNextFileHandleCounter++;
  uint32_t idx = static_cast<uint32_t>(slotIndex);
  memcpy(id, &counter, sizeof(counter));                  // bytes 0-7: uniqueness across the process lifetime
  memcpy(id + sizeof(counter), &idx, sizeof(idx));         // bytes 8-11: slot index, for a cheap sanity cross-check
}

// v65: THE FILE ID PRODUCED BY THE MOST RECENT SUCCESSFUL createCmd, and the
// connection it belongs to. This is the whole of SMB2 compound-request support
// on this server, and without it the iPhone Files app cannot open the share.
//
// MS-SMB2 2.2.1.2 / 3.3.5.2.7: a client may chain several requests into one
// packet, and every request after the first carries
// SMB2_FLAGS_RELATED_OPERATIONS plus the reserved FileId 0xFF..FF, meaning "use
// the FileId the PREVIOUS operation in this chain produced". macOS and iOS do
// all path-based metadata that way -- the first thing an iPhone sends after
// TREE_CONNECT is one packet containing
// CREATE(share root) + QUERY_INFO(FileAllInformation) + CLOSE.
//
// libsmb2 defines the constant (lib/smb2/lib/libsmb2.c:138 `compound_file_id`)
// but every one of its dozen references is a `memcpy(xx_req.file_id,
// compound_file_id, ...)` inside a CLIENT-side helper; nothing on the server
// side substitutes it. So findOpenFile() looked 0xFF..FF up literally, missed,
// and both chained requests were refused. The device's own diag.log from the
// first real iPhone attempt reads, three times over:
//   SMB query_info reject: no such handle for ctx=0x3fcd36dc type=1 class=18
//   SMB close reject: no such handle for ctx=0x3fcd36dc id=ffffffffffffffffffffffff
// and then iOS gave up with "could not get contents". Reproduced verbatim on
// the desktop harness (test/host/ios_compound_test.py) before this fix.
//
// ONE entry is enough, and is safer than a per-connection table. libsmb2
// dispatches a whole received chain inside a single smb2_service() call
// (lib/smb2/lib/socket.c's chained-PDU loop) and SmbServer::tick() services one
// context at a time, so no other connection's CREATE can interleave between a
// chain's CREATE and its dependent requests. A per-connection table would
// instead keep a resolvable id alive across unrelated chains -- strictly more
// than the spec allows.
//
// CLEARED, not just set -- three places, each load-bearing:
//   * the TOP of createCmd, so EVERY one of its ~10 failure returns invalidates
//     it in one line rather than needing a clear on each. MS-SMB2 requires the
//     requests related to a FAILED create to fail too; a stale id would answer
//     them against whatever was opened before, i.e. the wrong file.
//   * after a successful closeCmd, so a later stray 0xFF cannot name a handle
//     the client has already closed.
//   * in destructionEvent, because a freed smb2_context's ADDRESS can be handed
//     to the next connection, and `owner` is compared by pointer.
smb2_file_id gLastCreatedId = {};
const smb2_context* gLastCreatedOwner = nullptr;

// v71: split "path:stream" and "path:stream:$TYPE" (MS-FSCC 2.1.5.4).
//
// Advertising FILE_NAMED_STREAMS in v70 is what made iOS treat the card as
// writable -- and it also made iOS start ASKING for streams. diag16.log, one
// ordinary browse: 36 opens of names like
//   /crash_report.txt:com.apple.metadata_kMDItemUserTags
//   /diag.off:com.apple.lastuseddate#PS
//   /firmware.bin:com.apple.FinderInfo
//
// Today those happen to fail safely: they are FILE_OPEN, Storage.exists() says
// no, and the client is told OBJECT_NAME_NOT_FOUND. That safety is accidental.
// **`:` is an illegal character in a FAT name**, and the moment iOS copies a
// file IN it will try to CREATE these streams -- at which point the colon-laden
// path would be handed to Storage.open() with O_CREAT. What SdFat does with it
// is not something to find out on a real user's library.
//
// Returns the stream name (may be empty) and truncates `path` at the colon.
// The UNNAMED data stream -- written "file::$DATA", i.e. empty stream name with
// type $DATA -- is not a stream at all: it IS the file's contents, so callers
// treat it as the plain path. Anything else is a named stream, which a FAT card
// cannot hold.
//
// Only the LAST component is examined, and only for ':': no legal FAT name can
// contain one, so there is no ambiguity to resolve and no legitimate filename
// this can damage.
const char* splitStreamSuffix(char* path) {
  char* lastSlash = strrchr(path, '/');
  char* colon = strchr(lastSlash != nullptr ? lastSlash : path, ':');
  if (colon == nullptr) return nullptr;
  *colon = '\0';
  char* stream = colon + 1;
  // Drop the ":$TYPE" tail if present -- "$DATA" is the only type FAT could
  // ever mean anything by, and the name before it is what decides.
  char* typeSep = strchr(stream, ':');
  if (typeSep != nullptr) *typeSep = '\0';
  return stream;
}

bool isCompoundFileId(const smb2_file_id& id) {
  for (size_t i = 0; i < sizeof(smb2_file_id); i++) {
    if (id[i] != 0xFF) return false;
  }
  return true;
}

// v71: is this "no such handle" the EXPECTED tail of a compound chain whose
// CREATE failed, rather than a real stale-handle bug?
//
// When the first request in a chain fails, MS-SMB2 requires the related ones to
// fail too, and createCmd's clear-on-entry is what makes that happen. So every
// failed create in a chain produces one rejected READ and one rejected CLOSE,
// each carrying the 0xFF..FF placeholder. diag16.log is 140 such lines from a
// single browse -- pure consequence, no information, and a real threat to the
// 192 KB log budget on a card full of books.
//
// The condition is exact rather than a blanket mute: the placeholder resolves
// only while a create is armed, so "placeholder AND nothing armed" IS the failed
// -create case, and a genuinely stale or forged handle -- a literal id, or a
// placeholder from a connection that never created anything -- still logs.
bool isExpectedCompoundCascade(const smb2_context* owner, const smb2_file_id& id) {
  return isCompoundFileId(id) && gLastCreatedOwner != owner;
}

void forgetLastCreated(const smb2_context* owner) {
  if (owner == nullptr || gLastCreatedOwner == owner) {
    gLastCreatedOwner = nullptr;
    memset(gLastCreatedId, 0, sizeof(gLastCreatedId));
  }
}

// Owner-scoped lookup: a handle belongs to the connection that opened it, and
// nothing else may name it. Matching on file_id ALONE would be a
// cross-connection hole, because these ids are trivially predictable --
// makeFileId() above builds them from a small monotonic counter plus the slot
// index, so connection B could simply guess connection A's id and close (and,
// once Tasks 6-7 land read/write, read and write) A's handle. Every lookup
// must therefore carry the smb2_context. Tasks 5-7 inherit this signature
// deliberately: threading the context through is far cheaper now than after
// six more call sites exist.
OpenFileEntry* findOpenFile(const smb2_context* owner, const smb2_file_id& id) {
  if (owner == nullptr) return nullptr;
  const uint8_t* wanted = id;
  if (isCompoundFileId(id)) {
    if (gLastCreatedOwner != owner) return nullptr;
    wanted = gLastCreatedId;
  }
  for (size_t i = 0; i < kMaxOpenFiles; i++) {
    OpenFileEntry& e = gOpenFiles[i];
    if (e.inUse && e.owner == owner && memcmp(e.id, wanted, sizeof(smb2_file_id)) == 0) return &e;
  }
  return nullptr;
}

// Answer the request currently being dispatched with a SPECIFIC NT status,
// instead of the one status libsmb2 substitutes for every negative return.
//
// Every server dispatcher in libsmb2 turns `ret < 0` into a single hard-coded
// SMB2_STATUS_NOT_IMPLEMENTED (lib/smb2/lib/libsmb2.c:3485 CREATE, :3515 CLOSE,
// :3663 LOCK, :3709 IOCTL, :3781 QUERY_DIRECTORY, :3822 CHANGE_NOTIFY,
// :3850 QUERY_INFO, :3885 SET_INFO). So "no such file", "that name is taken",
// "directory not empty" and "this command does not exist" all arrived at the
// client as NOT_IMPLEMENTED, which OS-level clients map to ENOTSUP rather than
// ENOENT/EEXIST/ENOTEMPTY. That is not cosmetic: a negative lookup is the FIRST
// step of any copy or mkdir, so answering "unsupported" to "does this name exist
// yet?" aborts the whole operation. Measured against v64 with the Linux kernel
// cifs client: `mkdir` failed with "File exists" and `cp` with "Operation not
// supported", neither of which was true.
//
// Public API only, so no vendored divergence:
//   smb2_cmd_error_reply_async()        lib/smb2/include/smb2/libsmb2-raw.h:488
//   smb2_get_last_request_message_id()  lib/smb2/include/smb2/libsmb2.h:672
//   smb2_set_pdu_message_id()           libsmb2.h:670
//   smb2_queue_pdu()                    libsmb2.h:661
//
// The message id is the one libsmb2's own dispatchers use:
// smb2_get_last_request_message_id() returns `smb2->message_id`, which
// lib/smb2/lib/pdu.c:562-565 refreshes from the header of EVERY inbound PDU when
// the context is a server -- including each PDU of a compound chain. So this is
// the id of the request being handled right now, not of the chain's first.
//
// CALLERS MUST RETURN A POSITIVE VALUE. A dispatcher builds its own reply only
// when `ret < 0`; for `ret > 0` it leaves `pdu == NULL` and queues nothing, so a
// positive return is exactly what stops libsmb2 sending a SECOND reply carrying
// the same message id. Returning 0 instead would send two.
//
// Returns +1 so a caller can write `return replyStatus(...)`; returns -1 if the
// reply PDU could not be allocated, which deliberately falls back to libsmb2's
// own NOT_IMPLEMENTED reply -- a wrong status is recoverable, no reply at all
// hangs the client until it times out.
int replyStatus(smb2_context* smb2, uint8_t causingCommand, uint32_t status) {
  struct smb2_error_reply err;
  memset(&err, 0, sizeof(err));
  struct smb2_pdu* pdu =
      smb2_cmd_error_reply_async(smb2, &err, causingCommand, static_cast<int>(status), nullptr, nullptr);
  if (pdu == nullptr) {
    DiagLog::line("SMB alloc FAILED site=error_reply cmd=%u status=0x%08x, largest free block %u",
                  (unsigned)causingCommand, (unsigned)status, smbLargestFreeBlock());
    return -1;
  }
  smb2_set_pdu_message_id(smb2, pdu, smb2_get_last_request_message_id(smb2));
  smb2_queue_pdu(smb2, pdu);
  return 1;
}

// A stable, non-zero 64-bit identity for a path.
//
// FAT genuinely has no inode numbers, and v64 reported 0 everywhere on that
// basis (MS-FSCC 2.4.17 does permit 0 for filesystems without unique ids). That
// turned out to be listing-breaking rather than honest: the Linux kernel client
// answers a zero file id by logging "Autodisabling the use of server inode
// numbers" and then RE-ASKING for the directory with
// FileFullDirectoryInformation (class 0x02) -- which this server does not
// encode, so the listing failed outright ("SMB query_directory unsupported
// class=2"). The same zero produces "bogus file nlink value 0" in dmesg.
//
// FNV-1a over the case-folded path. Case-folded because FAT is
// case-insensitive, so "Book.epub" and "book.epub" are one file and must not
// get two identities. Never 0, because 0 is the sentinel that triggers the
// above. This is a hash, so the honest description is "stable", not "unique": a
// collision would make two files look hard-linked to a client that caches by
// id, which is a far smaller problem than not being able to list a directory.
constexpr uint64_t kFnvBasis = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t fnvFoldChar(uint64_t h, char raw) {
  unsigned char c = static_cast<unsigned char>(raw);
  if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
  return (h ^ c) * kFnvPrime;
}

uint64_t fnvFold(uint64_t h, const char* s, size_t len) {
  for (size_t i = 0; i < len; ++i) h = fnvFoldChar(h, s[i]);
  return h;
}

uint64_t pathIdentity(const char* path) {
  const uint64_t h = fnvFold(kFnvBasis, path, strlen(path));
  return h != 0 ? h : 1;
}

// The identity a child of `dirPath` will report for itself once it is opened by
// path. These two MUST agree: a client that sees id X for "book.epub" in a
// listing and then opens it and is told id Y treats them as two different
// objects. So this folds exactly the bytes pathIdentity() would see for the
// child's full path, WITHOUT building that path in a buffer -- there is no
// 512-byte stack local to spare here (CLAUDE.md's stack rule) and gPathScratch
// is already spoken for by the caller.
//
// The separator rule mirrors smbNormalizeUtf8Path()'s output: the share root is
// the single character "/", so a child of it is "/name" with ONE slash, while a
// child of "/books" is "/books/name".
uint64_t childIdentity(const char* dirPath, const char* name) {
  uint64_t h = fnvFold(kFnvBasis, dirPath, strlen(dirPath));
  if (!(dirPath[0] == '/' && dirPath[1] == '\0')) h = fnvFoldChar(h, '/');
  h = fnvFold(h, name, strlen(name));
  return h != 0 ? h : 1;
}

// The identity of `path`'s parent directory, for the ".." entry. Folds only the
// bytes before the last '/', which is exactly pathIdentity(parent) without
// materialising the parent path. A path with no separator except the leading one
// has the share root as its parent.
uint64_t parentIdentity(const char* path) {
  size_t lastSlash = 0;
  for (size_t i = 0; path[i] != '\0'; ++i) {
    if (path[i] == '/') lastSlash = i;
  }
  if (lastSlash == 0) return pathIdentity("/");
  const uint64_t h = fnvFold(kFnvBasis, path, lastSlash);
  return h != 0 ? h : 1;
}

// Is any OTHER open handle pointing at this path?
//
// Not defensive padding -- SdFat says so directly: "A given file must not be
// opened by more than one FatFile object or file corruption may occur"
// (FatFile.h:570-571), and FatVolume.h:193-195 repeats it specifically for
// rename. Renaming or deleting a path that a second handle still holds moves
// or frees the directory entry that handle is caching. Comparison is
// case-INSENSITIVE because FAT is: "Book.epub" and "book.epub" are one file.
// Eight slots, so this is a handful of string compares.
// v76: a distinct return for "understood, but another handle is in the way".
// The dispatcher maps it to STATUS_SHARING_VIOLATION instead of
// STATUS_INVALID_PARAMETER. That matters to a real client: INVALID_PARAMETER
// says "your request was malformed", which is a reason not to retry, and iOS
// duly did not. SHARING_VIOLATION says "right request, wrong moment" -- the
// status Windows and Samba return for exactly this, and one clients know how to
// wait on. -1 stays the answer for genuinely bad requests.
constexpr int kSetInfoSharingViolation = -3;

// v76: after a delete or rename succeeds, every OTHER handle still sitting on
// that path is holding a directory entry that has been freed or moved. See
// OpenFileEntry::stale for what consults the flag and why.
void markOtherHandlesStale(const OpenFileEntry* self, const char* path) {
  for (size_t i = 0; i < kMaxOpenFiles; i++) {
    OpenFileEntry& e = gOpenFiles[i];
    if (!e.inUse || &e == self || e.isNullStream) continue;
    if (strcasecmp(e.path, path) != 0) continue;
    e.stale = true;
    DiagLog::line("SMB handle now stale: entry removed under it path=%s slot=%d", path, (int)i);
  }
}

// v76: "is any OTHER open handle sitting on this path?", except that a handle
// which is a READ-ONLY DIRECTORY does not count as a conflict. Used by delete
// and rename. (It replaced a plain otherHandleOn(), which had no exemption and
// is gone -- this is the only such search left.)
//
// WHY IT EXISTS. diag24.log, deleting an empty folder from the iOS Files app:
//
//   query_directory done: 2 responses, buffer 4096, path=/Test_go好
//   set_info disposition reject: path also open on ctx=... path=/Test_go好
//
// iOS enumerates the folder to confirm it is empty, KEEPS that handle, and then
// opens a second one to delete. Perfectly ordinary -- and the old guard's own
// comment asserted the opposite ("real clients delete with exactly one handle").
//
// WHY THIS EXEMPTION IS SAFE. SdFat's rule is that two FatFile objects on one
// entry corrupt the card, but the mechanism is one of them FLUSHING STALE
// CACHED STATE over that entry -- two objects alone cannot be the problem,
// since FatVolume::rmdir opens a second one itself in order to delete. A handle
// that can never flush cannot corrupt anything, and a directory handle here can
// never flush:
//
//   * every directory is opened O_RDONLY (createCmd's directory branch), and
//     SdFat refuses a write-mode open of a subdirectory anyway
//     (FatFile.cpp:596-599);
//   * FILE_FLAG_DIR_DIRTY is set in exactly nine places in vendored SdFat 2.3.1
//     (FatFile.cpp:43,49 addCluster / :102 attrib / :602 O_TRUNC / :755,758
//     preAllocate / :1354 truncate / :1494,1497 write) and every one of them
//     requires write mode;
//   * FatFile::sync() wraps its entire entry-writing body in
//     `if (m_flags & FILE_FLAG_DIR_DIRTY)` (FatFile.cpp:1237-1262). Outside the
//     gate there is only cacheSync(), which flushes the shared block cache and
//     writes no entry.
//
// ⚠️ WHY THE TEST IS `isDirectory && !writable` AND NOT JUST `!writable`.
// The first draft of this used `!writable` alone, and adversarial review killed
// it: `writable` is this server's record of what the CLIENT ASKED FOR, not what
// the filesystem opened -- the code that sets it says so at its own definition.
// They diverge. needsWriteAccess() returns true for a truncating disposition
// REGARDLESS of desired_access, so `FILE_OVERWRITE` + `GENERIC_READ` opens
// O_RDWR|O_TRUNC -- a handle that has already called freeChain() and IS
// DIR_DIRTY -- while slot->writable stays false because GENERIC_READ is in
// neither write mask. That handle would have been declared harmless, and its
// eventual sync() would write fileSize and firstCluster into a slot another
// handle had since freed and the card reissued. `isDirectory` has no such gap:
// it is taken from file.isDirectory() -- the object that was actually opened,
// not the request -- and a truncating open is never a directory.
//
// Null-stream slots (v74) are skipped for a simpler reason: they have no
// HalFile behind them, so nothing about them can reach the card.
//
// What this still refuses, and must keep refusing: any handle that is not a
// read-only directory. Narrowing must not turn into deleting the check.
const OpenFileEntry* otherWritebackHandleOn(const OpenFileEntry* self, const char* path) {
  for (size_t i = 0; i < kMaxOpenFiles; i++) {
    const OpenFileEntry& e = gOpenFiles[i];
    if (!e.inUse || &e == self || e.isNullStream) continue;
    if (e.isDirectory && !e.writable) continue;  // the iOS listing handle -- see above
    if (strcasecmp(e.path, path) == 0) return &e;
  }
  return nullptr;
}

// Compact rendering of a file id, for the "no such handle" log lines. Those
// are the one case where slot->path is by definition unavailable, so without
// the id the line says only that *something* was rejected -- and on a device
// with no serial port that is the whole of the evidence. The id is not opaque
// to us: makeFileId() packs a uint64 counter into bytes 0-7 and the slot index
// into bytes 8-11, so twelve bytes is all of it and a stale-handle bug is
// diagnosable from diag.log alone.
//
// Static buffer, same one-request-in-flight invariant as gPathScratch (see its
// declaration), and every caller uses the result within a single DiagLog::line
// before returning.
// v67: a bounded "what did the client actually ask for" trace.
//
// Why it exists: diag13.log shows the iPhone listing the card perfectly and
// then treating the share as READ-ONLY -- without ever attempting a write. So
// the verdict is being formed from something we ANSWERED, not from something we
// refused, and every one of those answers is currently silent (only rejections
// are logged). That leaves nothing to attribute the behaviour to.
//
// Budgeted rather than conditional: the interesting traffic is entirely at the
// beginning (mount, first browse), a share with hundreds of books would
// otherwise flood the 192 KB log, and a budget cannot be forgotten the way a
// debug flag can. Reset per file-transfer session in smbAllocateTables().
int gTraceBudget = 0;
constexpr int kTraceBudget = 80;

void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void trace(const char* fmt, ...) {
  if (gTraceBudget <= 0) return;
  gTraceBudget--;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  DiagLog::line("SMB trace %s%s", buf, gTraceBudget == 0 ? " [trace budget spent]" : "");
}

const char* fileIdBrief(const smb2_file_id& id) {
  static constexpr char kHex[] = "0123456789abcdef";
  static char buf[25];  // 12 bytes * 2 hex digits + NUL
  size_t o = 0;
  for (size_t i = 0; i < 12; ++i) {
    buf[o++] = kHex[(id[i] >> 4) & 0x0F];
    buf[o++] = kHex[id[i] & 0x0F];
  }
  buf[o] = '\0';
  return buf;
}

// One line describing a blocking handle, for the four "also open" refusals.
// v76: those lines used to print only `other->owner`, which cannot distinguish
// "one connection holding two handles" from "two connections" -- and that is
// exactly the question diag24 could not answer about the folder that would not
// delete. The slot index and the file id (whose first eight bytes are a
// monotonic counter, see makeFileId) date the handle, so a listing cursor
// opened 165 ms ago is tellable apart from a mount-time handle opened 51 s ago.
//
// Static buffer, and it consumes fileIdBrief()'s static buffer too, so one
// DiagLog::line may use this once and must not also call fileIdBrief() itself.
const char* handleBrief(const OpenFileEntry* e) {
  static char buf[112];
  if (e == nullptr) return "(none)";
  snprintf(buf, sizeof(buf), "slot=%d ctx=%p id=%s dir=%d writable=%d enum=%d", (int)(e - &gOpenFiles[0]),
           (void*)e->owner, fileIdBrief(e->id), e->isDirectory ? 1 : 0, e->writable ? 1 : 0,
           e->enumStarted ? 1 : 0);
  return buf;
}

OpenFileEntry* allocateOpenFileSlot() {
  for (size_t i = 0; i < kMaxOpenFiles; i++) {
    if (!gOpenFiles[i].inUse) return &gOpenFiles[i];
  }
  return nullptr;
}

// Frees the slot and reports whether the FINAL WRITE-BACK succeeded.
//
// `close()` is not bookkeeping, it is where the last data lands. On device the
// whole chain is one expression: `HalFile::close()` -> `FsBaseFile::close()`
// (`bool rtn = m_fFile->close(); ...; return rtn;`, FsLib/FsFile.cpp:58-63) ->
// `FatFile::close()`, which IS
//
//     bool rtn = sync(); m_attributes = FILE_ATTR_CLOSED; m_flags = 0; return rtn;
//
// (FatLib/FatFile.cpp:128-132; ExFatFile.cpp:75-80 identical). So a card that
// fails its last write-back says so HERE -- and this function used to throw
// that bool away, which made close_cmd answer 0 for a copy whose tail never
// reached the card. iOS's copy sequence is write, write, ..., close; the Files
// app would report success and the user would have a truncated book.
//
// [[nodiscard]] on purpose: this exact defect was written three times (flush,
// WRITE_THROUGH, and here) because a bool that may be ignored eventually is.
// A caller that does not want to act on it must now say so out loud.
//
// TWO INVARIANTS, and the second is why the result is captured before anything
// else happens:
//   * the slot is freed and the HalFile released UNCONDITIONALLY. A failing
//     card must not leak table entries -- eight of those and every subsequent
//     create is refused by the table-full path instead of by the real problem.
//   * SdFat releases its own handle unconditionally too (see the quote above:
//     m_attributes/m_flags are assigned after rtn is taken), so "close failed"
//     never means "still open".
[[nodiscard]] bool releaseSlot(OpenFileEntry& e) {
  // v74 (review): a NULL STREAM has no HalFile at all, and calling close() on
  // one is not merely pointless on the device -- it is fatal.
  //
  // lib/hal/HalStorage.cpp's HAL_FILE_WRAPPED_CALL is
  //     StorageLock lock; assert(impl != nullptr); return impl->file.method(...)
  // with NO null branch, and the assert is compiled into the shipping firmware
  // (confirmed by disassembling gh_release's HalStorage.cpp.o: `bnez a0` then
  // `li a1,218; call __assert_func`). So this line would abort() and reboot the
  // X3 at the CLOSE of every accepted named stream -- exactly the step v74
  // exists to enable -- and again from destructionEvent and SmbServer::end().
  // There is no path that frees a null-stream slot without reaching here.
  //
  // The desktop stub does the OPPOSITE (`if (!impl) return false;`,
  // test/host/stub_hal/HalStorage.cpp:652), which is why the 73-check suite was
  // green and an earlier revision of the comment in closeCmd asserted that an
  // empty handle "returns false, which is not a failure". That was true of the
  // harness and false of the target: the eighth entry in this project's
  // harness/device divergence table, and the first one where the stub was
  // permissive about something that CRASHES on hardware.
  const bool closed = e.isNullStream ? true : e.file.close();  // captured FIRST; everything below runs either way
  e.file = HalFile();                  // drop the underlying handle eagerly, don't wait for slot reuse
  e.inUse = false;
  e.owner = nullptr;
  e.isDirectory = false;
  e.writable = false;
  e.deletePending = false;
  e.enumStarted = false;
  e.searchPattern[0] = '\0';
  e.patternOverlong = false;
  e.isNullStream = false;
  e.stale = false;  // v76: slots are reused; a stale flag must never outlive its handle
  e.path[0] = '\0';
  memset(e.id, 0, sizeof(smb2_file_id));
  return closed;
}

// Task 7: consumes a handle's delete-on-close flag. Returns true when there
// was nothing to delete or the delete succeeded.
//
// MUST BE CALLED AFTER releaseSlot(), never before: CLAUDE.md's HAL section is
// explicit that a file has to be closed before Storage.remove() on the same
// path, and on FAT unlinking an open file frees the cluster chain a live
// FatFile still caches. releaseSlot() clears slot->path, so callers copy the
// path out first -- which they already do for their failure logs.
//
// [[nodiscard]], for the same reason releaseSlot() is: a delete that quietly
// failed leaves the client believing a file is gone when it is still on the
// card, and this project has already paid twice for a discarded bool.
[[nodiscard]] bool deleteOnClose(bool pending, bool isDirectory, const char* path) {
  if (!pending) return true;

  // RE-CHECKED HERE, not only at disposition time. setDisposition() refuses to
  // set the flag while a second handle holds the path, but that is a check at
  // ONE MOMENT: createCmd has no same-path guard, so between the disposition
  // and the close another connection can open the very file about to be
  // deleted. Reproduced in review:
  //
  //   PROBE1: second open of a delete-pending path was ALLOWED
  //   PROBE1: after A.close(), file deleted=True while B still holds a handle=True
  //
  // On device that runs Storage.remove() while a live FatFile caches that
  // directory entry and cluster chain -- "a given file must not be opened by
  // more than one FatFile object or file corruption may occur"
  // (FatFile.h:570-571). The harness CANNOT see it: POSIX unlink() is
  // refcounted, so the delete simply succeeds there. That makes it a
  // stub-looser-than-device divergence with no stub-side fix available, which
  // is why the guard is here in the handler and why the harness test asserts
  // the REFUSAL rather than the corruption.
  //
  // `nullptr` as the self-slot: releaseSlot() ran before this, so the closing
  // handle's own entry already has inUse = false and cannot match. Anything
  // otherWritebackHandleOn() finds now is a genuine second holder.
  //
  // ⚠️ RESIDUAL, PRE-EXISTING AND DELIBERATELY NOT FIXED HERE: createCmd has
  // no same-path guard at all, so two live handles on one file remain possible
  // for READ, WRITE and setEndOfFile. SdFat's warning is about any two FatFile
  // objects (FatFile.h:570-571), not only about deletes -- so the write half
  // is equally unguarded, it simply predates this task (Task 6 already allowed
  // concurrent handles and does not move a directory entry, which is the part
  // FatVolume.h:193-195 calls out). This guard exists because delete and
  // rename FREE or MOVE the entry, which is the sharp end. A same-path policy
  // at create time would be the cleaner fix if anything else ever needs it.
  if (const OpenFileEntry* other = otherWritebackHandleOn(nullptr, path)) {
    DiagLog::line("SMB delete-on-close REFUSED: writable handle still open [%s] path=%s", handleBrief(other),
                  path);
    return false;
  }

  // rmdir(), NOT HalStorage::removeDir(). removeDir() is RECURSIVE -- it walks
  // the tree and unlinks every child. MS-FSCC/MS-SMB2 delete only an EMPTY
  // directory and a client expects a failure otherwise, so using the recursive
  // one would silently destroy a whole folder on a request that was supposed
  // to fail. On this device that is a book library.
  const bool ok = isDirectory ? Storage.rmdir(path) : Storage.remove(path);
  DiagLog::line("SMB delete-on-close %s: %s path=%s", ok ? "ok" : "FAILED", isDirectory ? "rmdir" : "remove", path);
  if (ok) markOtherHandlesStale(nullptr, path);
  return ok;
}

// ---------------------------------------------------------------------------
// Path handling.
//
// Normalizes a UTF-8 SMB-style path (possibly using '\' separators, possibly
// relative) into a safe, absolute local filesystem path for Storage/HalFile:
// backslash -> forward slash, reject any path containing "..", collapse
// repeated slashes, and ensure a single leading '/'. "Prefix the share root"
// (per the brief) is exactly that leading '/' and nothing more -- the SD
// share root *is* HalStorage's own root (see e.g.
// test/host/stub_hal/HalStorage.cpp's resolvePath(), which treats every
// device path as already relative to the SD root); there is no separate
// share-root directory to additionally prefix.
bool smbNormalizeUtf8Path(const char* utf8, char* out, size_t outSize) {
  if (out == nullptr || outSize < 2) return false;
  if (utf8 == nullptr) utf8 = "";
  if (strstr(utf8, "..") != nullptr) {
    return false;
  }

  size_t oi = 0;
  out[oi++] = '/';
  for (const char* p = utf8; *p != '\0'; ++p) {
    char c = (*p == '\\') ? '/' : *p;
    if (c == '/' && out[oi - 1] == '/') continue;  // collapse repeated separators
    if (oi + 1 >= outSize) {
      // Both channels deliberately: LOG_ERR goes nowhere on this device (no
      // serial port -- see CLAUDE.md), so DiagLog's SD-card diag.log is the
      // only way a truncation-shaped bug report ever reaches anyone.
      LOG_ERR(kTag, "path too long (>%zu bytes): %.64s", outSize, utf8);
      DiagLog::line("SMB path too long (>%zu bytes): %.64s", outSize, utf8);
      return false;
    }
    out[oi++] = c;
  }
  if (oi > 1 && out[oi - 1] == '/') oi--;  // strip a single trailing slash (root "/" is left alone)
  out[oi] = '\0';
  return true;
}

// General SMB wire path (UTF-16LE code units, backslash-separated) -> local
// filesystem path. `len16` is a code-unit count (matches
// smb2_utf16_to_utf8()'s own convention, e.g. smb2-cmd-create.c:477's
// `name_length / 2`) -- NOT a byte count. Used by tree_connect_cmd below for
// its `\\host\share` path today; kept general (matching the signature the
// Task 4 brief specifies) for Task 7's set_info rename target, which is the
// other place a raw UTF-16 SMB path reaches a handler.
//
// Note this is deliberately NOT used for create_cmd's path: smb2-cmd-create.c
// already converts `req->name` to UTF-8 for us before create_cmd ever sees it
// (smb2-cmd-create.c:477) -- calling smb2_utf16_to_utf8() on it a second time
// would be wrong (it's not UTF-16 anymore) and unnecessary. create_cmd calls
// smbNormalizeUtf8Path() directly on req->name instead, sharing the same
// normalize-and-reject-".." logic without a second copy of it.
bool smbPathFromSmb(const uint16_t* utf16, size_t len16, char* out, size_t outSize) {
  if (utf16 == nullptr || len16 == 0) {
    return smbNormalizeUtf8Path("", out, outSize);
  }
  const char* utf8 = smb2_utf16_to_utf8(utf16, len16);
  if (utf8 == nullptr) return false;
  bool ok = smbNormalizeUtf8Path(utf8, out, outSize);
  free(const_cast<char*>(utf8));  // smb2_utf16_to_utf8()'s own doc comment: caller frees with free()
  return ok;
}

// Shared with WebDAVHandler::isProtectedPath (see src/util/ProtectedPath.h
// for why this was extracted rather than duplicated or called cross-class).
bool smbIsProtectedPath(const char* path) { return ProtectedPath::isProtected(path); }

// The share root, in the normalized form smbNormalizeUtf8Path() produces.
// Several handlers have to special-case it -- the FAT root directory has no
// directory entry, so it has no timestamp to read and is not a thing that can
// be renamed or deleted. One spelling, so the three call sites cannot drift.
bool isShareRootPath(const char* path) { return path != nullptr && path[0] == '/' && path[1] == '\0'; }

// ---------------------------------------------------------------------------
// Access mode vs creation bits.
//
// These are two INDEPENDENT questions and the code keeps them apart, the way
// POSIX open(2) does:
//
//   * creation bits (O_CREAT / O_TRUNC / O_EXCL) come from create_disposition;
//   * access mode  (O_RDONLY vs O_RDWR)          comes from desired_access.
//
// Collapsing them -- "any disposition other than FILE_OPEN means write" -- is
// what made a pure-read FILE_OPEN_IF against an existing directory ask for
// O_RDWR and die on SdFat's subdirectory guard. FILE_OPEN_IF must still
// *create* when the target is absent; that is a creation bit, not an access
// mode.
//
// Three facts about the on-device filesystems constrain the pairing. Both the
// FAT and exFAT implementations agree, and both were read rather than assumed:
//   (a) write-mode open of a subdirectory or a read-only file is refused
//       (FatFile.cpp:581-585; ExFatFile.cpp:399-405);
//   (b) O_TRUNC without write mode is refused
//       (FatFile.cpp:552-556 rejects O_RDONLY|O_TRUNC outright;
//        ExFatFile.cpp:407-412 the same);
//   (c) O_CREAT only creates when the open is ALSO write mode -- otherwise
//       the entry is simply not created and the open fails
//       (FatFileLFN.cpp:372-373 / FatFileSFN.cpp:99-100:
//        `if (!(oflag & O_CREAT) || !isWriteMode(oflag))`;
//        ExFatFile.cpp:432-433 is the same test).
// (b) and (c) are why needsWriteAccess() below forces write for a truncating
// disposition and for a creating disposition whose target does not yet exist.

// Access bits by which the client ASSERTS it will modify the object.
constexpr uint32_t kExplicitWriteAccessMask = SMB2_FILE_WRITE_DATA | SMB2_FILE_APPEND_DATA | SMB2_FILE_WRITE_EA |
                                              SMB2_FILE_WRITE_ATTRIBUTES | SMB2_DELETE | SMB2_FILE_DELETE_CHILD |
                                              SMB2_WRITE_DACL | SMB2_WRITE_OWNER | SMB2_GENERIC_WRITE |
                                              SMB2_GENERIC_ALL;

// "May write", not "will write". MS-SMB2 2.2.13 defines SMB2_MAXIMUM_ALLOWED
// (0x02000000, smb2.h:253) as best-effort -- "grant me the most access I am
// entitled to" -- NOT an assertion of write intent; a real server resolves it
// per object. It is kept on this side rather than dropped because Task 6's
// write_cmd needs a writable handle for a FILE the client did mean to write,
// and macOS/iOS send MAXIMUM_ALLOWED for exactly those opens. For an existing
// DIRECTORY it is narrowed back to O_RDONLY in createCmd -- that narrowing,
// not this mask, is what makes directory probes work.
//
// It buys nothing for security any more, and the comment that used to claim
// otherwise was left over from the design where path protection consulted
// write intent. Protection is unconditional now (see createCmd), so this mask
// has exactly one job: choosing an oflag.
constexpr uint32_t kMayWriteAccessMask = kExplicitWriteAccessMask | SMB2_MAXIMUM_ALLOWED;

// Destroys existing content -> needs O_TRUNC, and therefore write mode (b).
bool dispositionTruncates(uint32_t disposition) {
  return disposition == SMB2_FILE_OVERWRITE || disposition == SMB2_FILE_OVERWRITE_IF ||
         disposition == SMB2_FILE_SUPERSEDE;
}

// Brings the target into existence when it is absent -> needs O_CREAT, and
// therefore write mode as well, but ONLY when the target really is absent (c).
bool dispositionCreates(uint32_t disposition) {
  return disposition == SMB2_FILE_CREATE || disposition == SMB2_FILE_OPEN_IF ||
         disposition == SMB2_FILE_OVERWRITE_IF || disposition == SMB2_FILE_SUPERSEDE;
}

// The one place the access mode is decided. Note `existsAlready`: a creating
// disposition needs write access only when it will actually create something.
// FILE_OPEN_IF against an EXISTING object is a plain open, and forcing write
// on it is the bug this round fixes.
bool needsWriteAccess(const smb2_create_request& req, bool existsAlready) {
  if ((req.desired_access & kMayWriteAccessMask) != 0) return true;
  if (dispositionTruncates(req.create_disposition)) return true;
  if (dispositionCreates(req.create_disposition) && !existsAlready) return true;
  return false;
}

// MS-SMB2 CreateAction values (2.2.14) -- upstream doesn't name these
// constants (grep confirms smb2-cmd-create.c only ever moves the raw
// uint32_t), so they're defined locally for readability.
constexpr uint32_t kFileSuperseded = 0;
constexpr uint32_t kFileOpened = 1;
constexpr uint32_t kFileCreated = 2;
constexpr uint32_t kFileOverwritten = 3;

// `writeAccess` MUST be needsWriteAccess()'s result for this same request.
// The two halves are assembled separately and never cross-derive: the access
// mode is the caller's boolean, the creation bits are the disposition's.
//
// The pairing keeps the filesystems' rules satisfied by construction:
// O_TRUNC only ever appears with O_RDWR (a truncating disposition forces
// write), and an O_CREAT that must genuinely create only ever appears with
// O_RDWR (a creating disposition forces write when the target is absent). An
// O_CREAT left on an existing read-only open is inert -- it is simply never
// consulted, since the entry is found.
oflag_t resolveFileOflag(uint32_t disposition, bool writeAccess) {
  oflag_t oflag = writeAccess ? O_RDWR : O_RDONLY;  // access mode <- desired_access
  switch (disposition) {                            // creation bits <- create_disposition
    case SMB2_FILE_CREATE:
      oflag |= O_CREAT | O_EXCL;
      break;
    case SMB2_FILE_OPEN_IF:
      oflag |= O_CREAT;
      break;
    case SMB2_FILE_OVERWRITE:
      oflag |= O_TRUNC;
      break;
    case SMB2_FILE_OVERWRITE_IF:
    case SMB2_FILE_SUPERSEDE:
      oflag |= O_CREAT | O_TRUNC;
      break;
    case SMB2_FILE_OPEN:
    default:
      break;
  }
  return oflag;
}

// ---------------------------------------------------------------------------
// authorize_user -- the one handler libsmb2 calls with no null-pointer guard
// on the function pointer itself (lib/smb2/lib/ntlmssp.c:1211-1219, unlike
// every `handlers->*_cmd` call site in lib/smb2/lib/libsmb2.c, which all
// guard with `server->handlers && server->handlers->foo`) -- so this field
// must never be null, or an unauthenticated client crashes the device
// instead of getting a clean logon failure.
//
// This handler's ONE job is to SUPPLY the password for an identity it's
// willing to accept, via smb2_set_password() -- it is not, itself, where
// verification happens. ntlmssp.c's NTOWFv2 check (ntlmssp.c:1271) is what
// actually verifies the client's NTLMv2 proof against whatever password this
// function set; the Task 3 stub's original comment ("password verification
// is libsmb2's own NTLM job") described that backwards -- libsmb2 verifies,
// but only a password this handler already provided. Confirmed empirically:
// a wrong password now fails with STATUS_LOGON_FAILURE, where the Task 3
// stub (which only checked the username) silently admitted it either way.
//
// There is no anonymous path anymore -- it's not just unused, it's
// unreachable by design. SmbServer::begin() sets `allow_anonymous = 0` (see
// that field's own long comment in SmbServer.cpp for the full trace); with
// that setting, an empty/anonymous user is rejected downstream in
// ntlmssp.c regardless of what this function returns for it, so this
// function rejects it explicitly too, rather than returning 0 and leaving a
// misleading "we still accept anonymous at the identity level" impression
// for the next reader. (History, for context: an earlier version of this
// task set allow_anonymous=1 and had this function return 0 for anonymous,
// on the assumption that setting x3's password was sufficient on its own.
// It was necessary but not sufficient -- with allow_anonymous=1,
// libsmb2.c's guest-flag decision (libsmb2.c:4179-4183) ran *after*
// ntlmssp_authenticate_blob() had already wiped smb2->password back to ""
// post-verification (ntlmssp.c:1276), so it marked even a correctly-
// authenticated x3 session SMB2_SESSION_FLAG_IS_GUEST -- unusable by any
// client that requires signing, which is all of them here. The
// allow_anonymous=0 fix closes that branch entirely: see
// task-4-report.md's "Concerns, addendum" and test/host/README.md's
// "Baseline (Task 4, round 2)" for the full before/after story.)
int authorizeUser(smb2_server*, smb2_context* smb2, const char* user, const char* domain,
                  const char* workstation) {
  // Task 8 diagnostics. This is the FIRST handler a connection reaches after
  // NEGOTIATE, so it is also the earliest point at which the negotiated
  // dialect is knowable from a handler -- hence logging it here rather than
  // inventing a negotiate hook. `smb2_get_dialect()` is public API
  // (libsmb2.h:340); 0x0302 is the only value this server can produce, since
  // SmbServer::acceptOneConnection() pins the ceiling -- anything else in the
  // log means that pin stopped working.
  //
  // The password is NEVER logged, in any branch, at any level. The client's
  // NTLMv2 proof is not a password and is not logged either: what appears
  // below is the identity being claimed and the verdict, nothing more.
  // ONE line per call, not one per stage. An earlier revision logged the
  // attempt and then the verdict separately; every field on the second line
  // was already on the first, and each DiagLog line is a full SD
  // open/write/flush/close. The verdict word is the only thing that varies, so
  // it goes in the same line.
  //
  // "accepted" here means the IDENTITY was accepted and a password was
  // supplied: libsmb2's ntlmssp.c still has to verify the client's proof
  // against it, and that verdict arrives as either sessionEstablished() below
  // or a client-visible STATUS_LOGON_FAILURE -- which is what keeps "wrong
  // user" and "wrong password" distinguishable in the log.
  const char* verdict;
  int result;
  if (user == nullptr || user[0] == '\0') {
    verdict = "REJECT (anonymous, allow_anonymous=0)";
    result = -1;
  } else if (strcmp(user, kSmbUser) == 0) {
    smb2_set_password(smb2, kSmbPassword);
    verdict = "identity accepted, NTLMv2 proof pending";
    result = 0;
  } else {
    verdict = "REJECT (unknown user)";
    result = -1;
  }

  DiagLog::line("SMB session_setup: user=%s domain=%s workstation=%s dialect=0x%04x -> %s",
                user != nullptr ? user : "(none)", (domain != nullptr && domain[0] != '\0') ? domain : "-",
                (workstation != nullptr && workstation[0] != '\0') ? workstation : "-",
                (unsigned)smb2_get_dialect(smb2), verdict);
  return result;
}

// ---------------------------------------------------------------------------
// tree_connect_cmd -- accepts share "SD" only; keeps upstream's IPC$
// detection (iOS may probe it first before connecting to the real share).
int treeConnectCmd(smb2_server*, smb2_context*, smb2_tree_connect_request* req, smb2_tree_connect_reply* rep) {
  if (req == nullptr || rep == nullptr) return -1;

  // gPathScratch, not a 512-byte stack local -- see its declaration for the
  // one-request-in-flight invariant this relies on.
  if (!smbPathFromSmb(req->path, req->path_length / 2, gPathScratch, sizeof(gPathScratch))) {
    DiagLog::line("SMB tree_connect reject: cannot decode path");
    return -1;
  }

  const char* lastSlash = strrchr(gPathScratch, '/');
  const char* shareName = (lastSlash != nullptr) ? lastSlash + 1 : gPathScratch;

  if (strcasecmp(shareName, "IPC$") == 0) {
    rep->share_type = SMB2_SHARE_TYPE_PIPE;
    rep->maximal_access = 0x1f00a9;
    rep->share_flags = 0;
    rep->capabilities = 0;
    DiagLog::line("SMB tree_connect ok: IPC$ (pipe share, probe only) path=%s", gPathScratch);
    return 0;
  }

  if (strcasecmp(shareName, kSmbShareName) != 0) {
    DiagLog::line("SMB tree_connect REJECT: unknown share '%s' (only %s and IPC$ exist) path=%s", shareName,
                  kSmbShareName, gPathScratch);
    return -1;
  }

  rep->share_type = SMB2_SHARE_TYPE_DISK;
  rep->maximal_access = 0x101f01ff;
  rep->share_flags = 0;
  rep->capabilities = 0;
  // Logged on success as well as failure: "the share connected but nothing
  // listed" and "the share never connected" are different investigations, and
  // on a device with no serial port diag.log is the only thing that can tell
  // them apart.
  DiagLog::line("SMB tree_connect ok: %s (disk share) path=%s", kSmbShareName, gPathScratch);
  return 0;
}

// ---------------------------------------------------------------------------
// create_cmd / close_cmd.
// Defined further down, next to query_info, which was its only caller until v65
// gave createCmd the job of filling in the reply's timestamps too. Forward
// declared rather than moved: its comment block belongs with the info
// structures it exists to feed, and moving 60 lines of that would bury this
// change in noise.
struct smb2_timeval modifyTimeOf(HalFile& file, const char* path, bool logFailure);

// ---------------------------------------------------------------------------
// SMB2 create contexts (MS-SMB2 2.2.13.2).
//
// WHY THIS EXISTS: An iPhone in the field mounts the share and lists it perfectly, and
// then shows a LOCK next to it -- read-only, decided at mount time, without the
// client ever attempting a write (diag13.log: not one create/write/set_info
// rejection in the whole session). So the verdict comes from something we
// answered. Everything else that could carry it has been checked and is
// correct: TREE_CONNECT reports maximal_access 0x101f01ff, the device
// characteristics carry no FILE_READ_ONLY_DEVICE, the filesystem attributes
// carry no FILE_READ_ONLY_VOLUME, and no file is ever given
// SMB2_FILE_ATTRIBUTE_READONLY.
//
// That leaves the one thing we were silently discarding. macOS and iOS attach
// an "MxAc" context to the CREATE of the share root at mount time, asking "what
// access would I actually get here?", and read the answer out of the CREATE
// RESPONSE rather than out of TREE_CONNECT. v67 and earlier parsed no contexts
// and returned none, so the answer was absent.
//
// This is a hypothesis, not a proven diagnosis -- no iOS client is reachable
// from the desktop harness, and neither smbclient nor the Linux kernel client
// sends MxAc, so the shipped v67 trace (`create ... ctx=N`) is what will
// confirm or kill it. It is worth doing anyway: answering a question the client
// asked is correct regardless, it is inert when the context is absent, and it
// costs no vendored divergence -- libsmb2 hands us the raw request bytes
// (smb2-cmd-create.c's variable-stage decoder) and emits whatever we put in
// rep->create_context (smb2-cmd-create.c:248-262).

// The one access mask we can honestly report: this server has no per-user
// permission model, so every authenticated caller gets everything. Same value
// as tree_connect's maximal_access minus GENERIC_ALL, which is what belongs in
// a per-object answer.
constexpr uint32_t kMaximalAccess = 0x001F01FF;

// Response buffer. 32 bytes, static for the same one-request-in-flight reason
// gPathScratch is (see its declaration): createCmd fills it and libsmb2 copies
// it out during smb2_encode_create_reply(), both inside this one callback.
uint8_t gCreateContextReply[32];

// Walk the chain and report whether `tag` (a 4-byte context name) is present.
// Bounds-checked at every step: this is attacker-supplied length arithmetic on
// a device with no exception handling, so a malformed chain must terminate the
// walk, never index outside the buffer.
bool hasCreateContext(const smb2_create_request& req, const char tag[4], char* seenOut, size_t seenSize) {
  bool found = false;
  size_t seenUsed = 0;
  if (seenSize > 0) seenOut[0] = '\0';
  const uint8_t* const base = req.create_context;
  const uint32_t total = req.create_context_length;
  if (base == nullptr || total < 16) return false;

  uint32_t off = 0;
  for (int guard = 0; guard < 16; ++guard) {  // a real client sends a handful
    if (off + 16 > total) break;
    uint32_t next;
    uint16_t nameOff, nameLen;
    memcpy(&next, base + off + 0, sizeof(next));
    memcpy(&nameOff, base + off + 4, sizeof(nameOff));
    memcpy(&nameLen, base + off + 6, sizeof(nameLen));
    // The offsets are relative to the START OF THIS CONTEXT, not to the chain.
    if (nameLen == 4 && static_cast<uint32_t>(off) + nameOff + 4 <= total) {
      const char* name = reinterpret_cast<const char*>(base + off + nameOff);
      if (memcmp(name, tag, 4) == 0) found = true;
      // Record every tag seen, for diag.log: knowing that iOS sends MxAc/QFid/
      // AAPL is exactly the evidence this whole question turns on.
      if (seenUsed + 6 < seenSize) {
        if (seenUsed > 0) seenOut[seenUsed++] = ',';
        memcpy(seenOut + seenUsed, name, 4);
        seenUsed += 4;
        seenOut[seenUsed] = '\0';
      }
    }
    if (next == 0 || next > total - off) break;  // 0 == last; anything past the end is malformed
    off += next;
  }
  return found;
}

// Build the SMB2_CREATE_QUERY_MAXIMAL_ACCESS_RESPONSE (MS-SMB2 2.2.14.2.5) into
// gCreateContextReply and attach it. Layout, byte for byte:
//   0..3   Next        = 0 (this is the only context)
//   4..5   NameOffset  = 16
//   6..7   NameLength  = 4
//   8..9   Reserved    = 0
//   10..11 DataOffset  = 24   (16 + 4 name + 4 pad; the data must be 8-aligned)
//   12..15 DataLength  = 8
//   16..19 Name        = "MxAc"
//   20..23 padding
//   24..27 QueryStatus = 0 (STATUS_SUCCESS -- we did compute an answer)
//   28..31 MaximalAccess
void attachMaximalAccessContext(smb2_create_reply* rep) {
  uint8_t* p = gCreateContextReply;
  memset(p, 0, sizeof(gCreateContextReply));
  const uint16_t nameOffset = 16, nameLength = 4, dataOffset = 24;
  const uint32_t dataLength = 8, queryStatus = 0, access = kMaximalAccess;
  memcpy(p + 4, &nameOffset, sizeof(nameOffset));
  memcpy(p + 6, &nameLength, sizeof(nameLength));
  memcpy(p + 10, &dataOffset, sizeof(dataOffset));
  memcpy(p + 12, &dataLength, sizeof(dataLength));
  memcpy(p + 16, "MxAc", 4);
  memcpy(p + 24, &queryStatus, sizeof(queryStatus));
  memcpy(p + 28, &access, sizeof(access));
  rep->create_context = gCreateContextReply;
  rep->create_context_length = sizeof(gCreateContextReply);
}

int createCmd(smb2_server*, smb2_context* smb2, smb2_create_request* req, smb2_create_reply* rep) {
  if (req == nullptr || rep == nullptr) return -1;

  // v65: invalidate the compound placeholder FIRST, so every one of this
  // function's failure returns leaves it invalid without each of them having to
  // remember. Re-armed only at the very end, on success. See gLastCreatedId.
  forgetLastCreated(smb2);

  // gPathScratch, not a 512-byte stack local -- see its declaration.
  char* const localPath = gPathScratch;
  if (!smbNormalizeUtf8Path(req->name, localPath, sizeof(gPathScratch))) {
    DiagLog::line("SMB create reject: bad or too-long path");
    return -1;
  }

  // Protected paths are refused for EVERY open, read or write -- not just
  // write-intent ones. WebDAV has always blocked them on every method
  // including GET (WebDAVHandler.cpp:51, 296); /.crossmosa/ holds the Wi-Fi
  // credentials, the settings file and reading progress, so the moment Task 6
  // lands read_cmd, a read-only open of a "merely" blocked-for-writing path
  // would hand all of that to anyone on the network.
  //
  // NOTE for Task 5: the matching *hiding* rule belongs to query_directory --
  // a listing must not advertise entries that a read now refuses. That is
  // deliberately not implemented here (it is Task 5's scope);
  // ProtectedPath::isProtectedName() is the segment-level predicate for it.
  if (smbIsProtectedPath(localPath)) {
    DiagLog::line("SMB create reject: protected path disp=%u access=0x%x path=%s",
                  (unsigned)req->create_disposition, (unsigned)req->desired_access, localPath);
    return -1;
  }

  // The four fields a client's read/write verdict is built from, plus whether
  // it sent any create contexts (MxAc / QFid / DHnQ / AAPL ...).
  //
  // v74: hoisted above the named-stream branch below, which now needs
  // `wantsMaximalAccess` too -- and which is a happy side effect for the log,
  // because this trace line is emitted while `localPath` still carries the
  // ":streamname" suffix. That is what the client actually asked for.
  char seenContexts[64];
  const bool wantsMaximalAccess = hasCreateContext(*req, "MxAc", seenContexts, sizeof(seenContexts));
  trace("create acc=0x%08x disp=%u opts=0x%08x ctx=%u[%s] path=%s", (unsigned)req->desired_access,
        (unsigned)req->create_disposition, (unsigned)req->create_options,
        (unsigned)req->create_context_length, seenContexts, localPath);

  // v71: named streams. MUST come before Storage sees the path -- see
  // splitStreamSuffix(). After this, localPath is the plain file and `stream`
  // is either nullptr (no suffix), "" (the unnamed data stream, i.e. the file
  // itself) or a real stream name we cannot store.
  const char* const stream = splitStreamSuffix(localPath);

  // v77: CHECK AGAIN, because the path just changed under the first check.
  //
  // splitStreamSuffix() writes a NUL over the colon, so the string the guard
  // above inspected is not the string Storage is about to open. Review measured
  // the consequence: `/CROSSM~1::$DATA` failed every protected-name test (the
  // colons are not part of any protected shape), was truncated here to
  // `/CROSSM~1`, and was opened -- the data directory, holding Wi-Fi
  // credentials. `/XTCache::$DATA` and `/System Volume Information::$DATA` went
  // the same way, missing the name list purely on length.
  //
  // The first check is kept as well, not replaced: it is what produces the
  // honest "protected path" refusal for a plainly-named request, and it runs
  // before the named-stream branch answers anything.
  if (smbIsProtectedPath(localPath)) {
    DiagLog::line("SMB create reject: protected path behind stream suffix stream=%s path=%s",
                  stream == nullptr ? "(none)" : stream, localPath);
    return -1;
  }

  if (stream != nullptr && stream[0] != '\0') {
    // Creating dispositions get a different answer from opening ones, and the
    // difference matters. An OPEN of a stream that does not exist is an
    // ordinary "not found" -- exactly what iOS already receives today and
    // tolerates 36 times per browse. A CREATE is iOS trying to STORE Apple
    // metadata (tags, QuickLook thumbnails, last-used date, FinderInfo) that a
    // FAT card has nowhere to put; STATUS_NOT_SUPPORTED says so honestly
    // instead of inventing a file whose name FAT cannot represent.
    //
    // IF A COPY INTO THE SHARE EVER FAILS, THIS IS THE FIRST LINE TO LOOK FOR.
    // The fallback, if iOS turns out to require the write to succeed, is to
    // accept named-stream writes and discard the bytes -- the metadata is lost
    // either way on this medium, and losing it silently is better than losing
    // the book. That is deliberately NOT done pre-emptively: "reports success
    // and does nothing" is the exact failure shape that cost this project two
    // versions over delete-on-close, and it should not be adopted on a guess.
    const bool creating = req->create_disposition != SMB2_FILE_OPEN &&
                          req->create_disposition != SMB2_FILE_OVERWRITE;
    if (!creating) {
      // Unchanged, and deliberately so: a stream that was never stored does not
      // exist, "not found" is the truthful answer, and iOS asks this roughly 50
      // times per browse and carries on every time.
      DiagLog::line("SMB create: named stream open (absent) '%s' on %s", stream, localPath);
      return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_OBJECT_NAME_NOT_FOUND);
    }

    // v74: ACCEPT THE CREATE AND THROW THE CONTENT AWAY.
    //
    // v71 refused this, wrote down what would justify changing that, and said
    // the fallback should not be adopted on a guess. diag22.log is the evidence:
    //
    //   create disp=2 path=/Test_go好/講者腳本_中文_X3.epub   <- the book, created
    //   create acc=0x00020006 path=...epub                     <- opened for writing
    //   named stream CREATE refused 'com.apple.FinderInfo'     <- us
    //   write first: len=32 ...                                <- FinderInfo is 32 bytes
    //   write reject: no such handle ... len=32
    //   ...
    //   delete-on-close ok: remove path=...epub                <- iOS DELETED THE BOOK
    //
    // Twice, identically. iOS treats writing FinderInfo as part of creating the
    // file, and rolls the whole copy back when it fails. So the choice is not
    // "lose the metadata or keep it" -- FAT cannot keep it either way -- it is
    // "lose the metadata or lose the book".
    //
    // This IS the "reports success and does nothing" shape that cost this
    // project two versions over delete-on-close, and it is adopted here with
    // that in mind, because the two cases differ where it matters: a delete the
    // user asked for and did not get is a lie about the user's own data, while
    // Finder tags and QuickLook thumbnails have no representation on a FAT card
    // at all and are regenerated by the client from the file itself. It is also
    // not silent -- every accepted stream is logged, once, right here.
    // v74 (review): cap how much of the table discarded streams may hold.
    //
    // Before v74 a stream create consumed no slot at all (it was refused before
    // allocation). Now each holds one of eight for as long as the client wants,
    // and a client holding all eight would make an ORDINARY file create fail --
    // which is worse than refusing the stream, because it breaks the copy for a
    // reason the user cannot even see. Reserving two slots means a stream flood
    // can only ever cost the client its own streams.
    //
    // Not a fix for the underlying shape: a discarded stream carries no state,
    // so it need not occupy an OpenFileEntry at all (encode "null stream" in the
    // file id, or share one reserved entry). That is the right answer and it is
    // a bigger change than this version should carry; recorded in CLAUDE.md.
    constexpr size_t kMaxNullStreamSlots = kMaxOpenFiles - 2;
    size_t nullStreamsHeld = 0;
    for (size_t i = 0; i < kMaxOpenFiles; i++) {
      if (gOpenFiles[i].inUse && gOpenFiles[i].isNullStream) nullStreamsHeld++;
    }
    if (nullStreamsHeld >= kMaxNullStreamSlots) {
      DiagLog::line("SMB create reject: %zu discarded streams already open (cap %zu), refusing '%s' on %s",
                    nullStreamsHeld, kMaxNullStreamSlots, stream, localPath);
      return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }

    OpenFileEntry* streamSlot = allocateOpenFileSlot();
    if (streamSlot == nullptr) {
      DiagLog::line("SMB create reject: open-file table full for stream '%s' on %s", stream, localPath);
      return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    smb2_file_id streamId;
    makeFileId(static_cast<size_t>(streamSlot - gOpenFiles), streamId);
    streamSlot->inUse = true;
    streamSlot->stale = false;
    streamSlot->owner = smb2;
    memcpy(streamSlot->id, streamId, sizeof(smb2_file_id));
    streamSlot->isDirectory = false;
    streamSlot->writable = true;
    streamSlot->isNullStream = true;
    // The path stored is the FULL stream path, colon and all -- never the base
    // file. Defence in depth: if some future handler forgets the isNullStream
    // check and reaches the filesystem anyway, it does so with a name FAT
    // cannot represent and fails, instead of quietly operating on the book.
    // Composed explicitly rather than via snprintf's truncation, so the bound is
    // checked instead of argued about: smbNormalizeUtf8Path() already rejects a
    // request path longer than this buffer, so `base:stream` -- which is that
    // same request path with the colon put back -- always fits. Belt and braces
    // because a silently truncated stream path would defeat the whole point of
    // storing it (a truncated name could, in principle, collide with a real
    // file's). Also silences a -Wformat-truncation warning that was noise.
    const size_t baseLen = strlen(localPath);
    const size_t streamLen = strlen(stream);
    if (baseLen + 1 + streamLen >= sizeof(streamSlot->path)) {
      DiagLog::line("SMB create reject: composed stream path too long (%zu+1+%zu) on %s", baseLen, streamLen,
                    localPath);
      streamSlot->inUse = false;
      return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_OBJECT_NAME_INVALID);
    }
    memcpy(streamSlot->path, localPath, baseLen);
    streamSlot->path[baseLen] = ':';
    memcpy(streamSlot->path + baseLen + 1, stream, streamLen + 1);
    DiagLog::line("SMB create: named stream '%s' accepted and discarded on %s", stream, localPath);

    rep->create_action = kFileCreated;
    rep->file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
    memcpy(rep->file_id, streamId, sizeof(smb2_file_id));
    rep->end_of_file = 0;
    rep->allocation_size = 0;
    if (wantsMaximalAccess) attachMaximalAccessContext(rep);
    memcpy(gLastCreatedId, streamId, sizeof(smb2_file_id));
    gLastCreatedOwner = smb2;
    return 0;
  }

  // v65: MS-SMB2 2.2.13 -- FILE_DELETE_ON_CLOSE carried on the OPEN means
  // exactly what FILE_DISPOSITION_INFORMATION carried on a later SET_INFO
  // means. v64 read only two bits out of create_options (the two below), so this
  // one was dropped: the CREATE succeeded, the CLOSE succeeded, the client was
  // told STATUS_SUCCESS twice, and the file was still there. A silent
  // successful no-op, the worst shape a failure can have. It is also the ONLY
  // delete path several real clients use -- Samba's `del` (verified on the wire:
  // Create Options 0x00001000, both responses 0x00000000, file survived) and
  // libsmb2's OWN smb2_unlink() (lib/smb2/lib/libsmb2.c:1798-1826), meaning
  // this server could not be deleted from by the library it is built on.
  const bool deleteOnCloseRequested = (req->create_options & SMB2_FILE_DELETE_ON_CLOSE) != 0;
  // Checked HERE rather than left to close: setDisposition() refuses to mark the
  // share root for deletion, and that guard lives on the SET_INFO path only.
  // Without re-applying it, `rmdir \` arrives as CREATE(root, DELETE_ON_CLOSE)
  // and reaches deleteOnClose() with path "/", i.e. a request to delete the
  // card's root directory. The protected-path check above does not cover it:
  // "/" is not a protected NAME.
  if (deleteOnCloseRequested && isShareRootPath(localPath)) {
    DiagLog::line("SMB create reject: DELETE_ON_CLOSE on the share root");
    return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_ACCESS_DENIED);
  }

  OpenFileEntry* slot = allocateOpenFileSlot();
  if (slot == nullptr) {
    DiagLog::line("SMB create reject: open-file table full (%zu slots)", kMaxOpenFiles);
    for (size_t i = 0; i < kMaxOpenFiles; i++) {
      const OpenFileEntry& e = gOpenFiles[i];
      if (e.inUse) DiagLog::line("SMB   slot[%zu] owner=%p path=%s", i, (const void*)e.owner, e.path);
    }
    return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }

  const bool existsAlready = Storage.exists(localPath);
  uint32_t createAction;
  switch (req->create_disposition) {
    case SMB2_FILE_OPEN:
      // v65: a REAL status, not NOT_IMPLEMENTED. This exact answer -- "does this
      // name exist?" -- is the first step of every copy and mkdir a client
      // performs, and NOT_IMPLEMENTED told it the CREATE command itself was
      // unavailable. See replyStatus() for the measurements.
      if (!existsAlready) return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_OBJECT_NAME_NOT_FOUND);
      createAction = kFileOpened;
      break;
    case SMB2_FILE_CREATE:
      if (existsAlready) return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_OBJECT_NAME_COLLISION);
      createAction = kFileCreated;
      break;
    case SMB2_FILE_OPEN_IF:
      createAction = existsAlready ? kFileOpened : kFileCreated;
      break;
    case SMB2_FILE_OVERWRITE:
      if (!existsAlready) return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_OBJECT_NAME_NOT_FOUND);
      createAction = kFileOverwritten;
      break;
    case SMB2_FILE_OVERWRITE_IF:
      createAction = existsAlready ? kFileOverwritten : kFileCreated;
      break;
    case SMB2_FILE_SUPERSEDE:
      createAction = existsAlready ? kFileSuperseded : kFileCreated;
      break;
    default:
      DiagLog::line("SMB create reject: unsupported disposition %u", (unsigned)req->create_disposition);
      return -1;
  }

  // Write access is decided once, here -- after existsAlready, because a
  // creating disposition only needs write when it will really create.
  const bool writeAccess = needsWriteAccess(*req, existsAlready);

  // These two are the CLIENT'S request, not facts about the filesystem. They
  // decide what we attempt; file.isDirectory() below decides what we report.
  const bool wantDirectory = (req->create_options & SMB2_FILE_DIRECTORY_FILE) != 0;
  const bool wantNonDirectory = (req->create_options & SMB2_FILE_NON_DIRECTORY_FILE) != 0;
  HalFile file;
  // Task 6: what the handle we end up with can actually do, tracked alongside
  // every open below rather than re-derived afterwards. `writeAccess` is the
  // REQUEST; this is the RESULT, and the two differ on all three narrowing
  // paths (directory opens, the directory retry, the MAXIMUM_ALLOWED retry).
  bool openedWritable = false;

  if (wantDirectory) {
    if (!existsAlready) {
      // Only CREATE/OPEN_IF make sense for "make me a new directory" --
      // OVERWRITE*/SUPERSEDE against a directory that doesn't exist is a
      // client-side contradiction, not something to paper over.
      if (req->create_disposition != SMB2_FILE_CREATE && req->create_disposition != SMB2_FILE_OPEN_IF) {
        DiagLog::line("SMB create reject: directory disposition mismatch path=%s", localPath);
        return -1;
      }
      if (!Storage.mkdir(localPath)) {
        DiagLog::line("SMB create: mkdir failed path=%s", localPath);
        return -1;
      }
    }
    // Directories are always opened read-only regardless of writeAccess: both
    // filesystems refuse a write-mode open of a subdirectory, and nothing can
    // be written through a directory handle anyway. Strictly less access than
    // was asked for, so this narrowing is always safe. openedWritable stays
    // false for the same reason.
    file = Storage.open(localPath, O_RDONLY);
  } else {
    file = Storage.open(localPath, resolveFileOflag(req->create_disposition, writeAccess));
    openedWritable = static_cast<bool>(file) && writeAccess;

    // Same narrowing, for the client that did NOT say it wanted a directory.
    // A write-mode open of an existing subdirectory is refused by both
    // filesystems, so a probe like FILE_OPEN + MAXIMUM_ALLOWED with no
    // create_options -- which macOS/iOS send when they do not yet know
    // whether a name is a file or a folder -- would otherwise be answered
    // "no such thing" for every folder on the card.
    //
    // Retry-on-refusal rather than probe-first: a probe-first shape would
    // cost an extra SD open on EVERY create against an existing target,
    // including the common file-upload path, and an open is a full directory
    // scan (12-18ms measured, see CLAUDE.md's v55 notes). This costs nothing
    // except where the alternative was an outright failure.
    //
    // Two guards keep it honest. It is skipped for truncating dispositions,
    // because "truncate this directory" has no read-only reading and should
    // stay an error. And it is skipped when the target does not exist, so an
    // O_CREAT that genuinely had to create something is never quietly turned
    // into a read of nothing.
    //
    // WHICH DOWNGRADES ARE ACCEPTED (enumerated, not inferred -- only
    // FILE_OPEN and FILE_OPEN_IF can reach here at all, since FILE_CREATE
    // against an existing target already returned above and the truncating
    // three are excluded):
    //
    //   a) target is a DIRECTORY -> accept. The write-mode open could never
    //      have succeeded (both filesystems refuse it) and nothing is written
    //      through a directory handle anyway.
    //   b) target is a FILE and the client asserted NO explicit write access
    //      (write mode came only from SMB2_MAXIMUM_ALLOWED) -> accept, and
    //      record that the handle is read-only. MS-SMB2 2.2.13 defines
    //      MAXIMUM_ALLOWED as "grant me the access I am entitled to", which is
    //      precisely a request to be downgraded rather than refused; a real
    //      server resolves it per object. Without this, a file carrying the
    //      read-only attribute (FatFile.cpp:581-585, ExFatFile.cpp:399-405 --
    //      the same guard as the directory case) cannot be opened AT ALL, and
    //      therefore cannot be read, by a client that sends MAXIMUM_ALLOWED --
    //      which macOS and iOS do routinely. This was Task 4's recorded
    //      deferred item; it lands here because "a file that cannot be opened
    //      cannot be transferred" is this task's problem.
    //   c) target is a FILE and the client DID ask for write access
    //      (GENERIC_WRITE, FILE_WRITE_DATA, DELETE, ...) -> reject, exactly as
    //      before. Handing back a read handle to a client that said it would
    //      write is how a copy fails at the last byte instead of the first.
    if (!file && existsAlready && writeAccess && !dispositionTruncates(req->create_disposition)) {
      HalFile retry = Storage.open(localPath, O_RDONLY);
      if (retry) {
        if (retry.isDirectory()) {
          file = std::move(retry);
          openedWritable = false;
        } else if ((req->desired_access & kExplicitWriteAccessMask) == 0) {
          file = std::move(retry);
          openedWritable = false;
          DiagLog::line("SMB create: MAXIMUM_ALLOWED downgraded to read-only path=%s", localPath);
        }
      }
    }
  }

  if (!file) {
    DiagLog::line("SMB create: open failed path=%s disp=%u", localPath, (unsigned)req->create_disposition);
    // Two different facts, two different statuses. If the name is not there, say
    // so -- that is what a client's negative lookup needs. If it IS there and
    // the open still failed, the honest summary is "you cannot have this
    // handle", not "no such file" (which would make a client discard its own
    // cached copy of a file that exists) and not NOT_IMPLEMENTED.
    return replyStatus(smb2, SMB2_CREATE,
                       existsAlready ? SMB2_STATUS_ACCESS_DENIED : SMB2_STATUS_OBJECT_NAME_NOT_FOUND);
  }

  // Directory-ness comes from the filesystem, never from the client's hint.
  // The old code set isDirectory = true purely *because*
  // SMB2_FILE_DIRECTORY_FILE was set, so pointing that flag at a plain file
  // produced a reply claiming SMB2_FILE_ATTRIBUTE_DIRECTORY for a file, and
  // handed Tasks 5-7 a slot whose isDirectory flag was simply wrong. The
  // mirror case (FILE_NON_DIRECTORY_FILE against a directory) wasn't rejected
  // at all. Both are checked here, against the real object.
  const bool isDirectory = file.isDirectory();

  // v79: a file we just created must not report "no timestamp".
  //
  // Only when the date/time callback is live. SdFat's create path writes only
  // createDate/createTime when a callback exists, leaving modify and access
  // zero (FatFileLFN.cpp:426-441) -- and SMB2_FILE_CREATE maps to O_CREAT|O_EXCL
  // with no O_TRUNC, so nothing marks the entry dirty until the first byte
  // arrives. A client that reads the CREATE reply before writing therefore saw
  // all-zero times for a file that plainly has one. With no callback registered
  // SdFat writes all three itself, so this is a no-op on a device that does not
  // know the date -- which is every device until SNTP lands.
  //
  // Failure is not fatal: the file is real and open, and a wrong-but-present
  // date is not worth refusing a create over. It is logged, not returned.
  if (!isDirectory && !existsAlready && SdDateTime::isRegistered()) {
    struct tm utc {};
    if (SdDateTime::nowUtc(&utc)) {
      if (!file.setTimestamp(T_WRITE | T_ACCESS, static_cast<uint16_t>(utc.tm_year + 1900),
                             static_cast<uint8_t>(utc.tm_mon + 1), static_cast<uint8_t>(utc.tm_mday),
                             static_cast<uint8_t>(utc.tm_hour), static_cast<uint8_t>(utc.tm_min),
                             static_cast<uint8_t>(utc.tm_sec))) {
        DiagLog::line("SMB create: could not stamp new file path=%s", localPath);
      }
    }
  }

  if (wantDirectory && !isDirectory) {
    DiagLog::line("SMB create reject: FILE_DIRECTORY_FILE but target is a file path=%s", localPath);
    return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_NOT_A_DIRECTORY);
  }
  if (wantNonDirectory && isDirectory) {
    DiagLog::line("SMB create reject: FILE_NON_DIRECTORY_FILE but target is a directory path=%s", localPath);
    return replyStatus(smb2, SMB2_CREATE, SMB2_STATUS_FILE_IS_A_DIRECTORY);
  }

  smb2_file_id id;
  makeFileId(static_cast<size_t>(slot - gOpenFiles), id);

  slot->inUse = true;
  slot->stale = false;
  // v76: assigned, not assumed. One early-return branch releases a stream slot
  // with a bare `inUse = false` instead of releaseSlot(), leaving isNullStream
  // set; an ordinary create landing on that slot afterwards would inherit it,
  // and a null-stream slot silently discards writes while reporting success.
  // Unreachable today by a path-length argument -- but that branch exists
  // precisely because its author did not want to rely on one.
  slot->isNullStream = false;
  slot->owner = smb2;
  memcpy(slot->id, id, sizeof(smb2_file_id));
  slot->file = std::move(file);
  slot->isDirectory = isDirectory;
  // Three conjuncts, three different questions -- all of them necessary:
  //
  //   openedWritable                        did the open we performed use
  //                                         write mode, after every narrowing?
  //   !isDirectory                          FatFile's openRoot() path ignores
  //                                         oflag entirely (FatFile.cpp:
  //                                         456-461), so the share ROOT can
  //                                         come back from a write-mode open
  //                                         as a writable-looking directory
  //                                         handle. write_cmd rejects
  //                                         directory handles before it reads
  //                                         this flag, but the flag should not
  //                                         claim something untrue.
  //   desired_access & kMayWriteAccessMask  DID THE CLIENT ASK TO WRITE?
  //
  // The third one is not redundant with the first, because needsWriteAccess()
  // forces write mode for a *creating* disposition against a missing target
  // regardless of desired_access -- the file has to be created somehow. So
  // FILE_OPEN_IF + GENERIC_READ against a name that does not exist yields a
  // genuinely writable O_RDWR handle for a client that asked only to read, and
  // without this conjunct a write through it would land on disk. A real server
  // answers STATUS_ACCESS_DENIED. Requirement: this flag means "did the client
  // ask to write", not "can the filesystem write".
  //
  // kMayWriteAccessMask, NOT kExplicitWriteAccessMask: MAXIMUM_ALLOWED must
  // stay on the writable side, because that is what macOS/iOS send on the
  // ordinary upload path.
  slot->writable =
      openedWritable && !isDirectory && (req->desired_access & kMayWriteAccessMask) != 0;
  // Both buffers are char[kMaxSmbPathLen] and smbNormalizeUtf8Path() already
  // guaranteed localPath is NUL-terminated within that bound, so this cannot
  // overrun. No allocation, no truncation, nothing to fail.
  memcpy(slot->path, localPath, strlen(localPath) + 1);

  // v65: consumed by closeCmd() and destructionEvent() via deleteOnClose(),
  // which re-applies the second-handle guard itself. Set AFTER the
  // wantDirectory/wantNonDirectory rejections above, so a client sending
  // FILE_NON_DIRECTORY_FILE|DELETE_ON_CLOSE against a directory has already
  // been refused and never gets a slot carrying this flag.
  slot->deletePending = deleteOnCloseRequested;
  if (deleteOnCloseRequested) {
    // Not decoration. Until v65 a failed delete left NOTHING in diag.log, and on
    // a device with no serial port that made it undiagnosable -- the v64 field
    // failure had to be found by reproducing it on a desktop instead of by
    // reading the device's own log.
    DiagLog::line("SMB create: delete-on-close SET from create_options path=%s (directory=%d)", localPath,
                  isDirectory ? 1 : 0);
  }

  rep->create_action = createAction;
  rep->file_attributes = isDirectory ? SMB2_FILE_ATTRIBUTE_DIRECTORY : SMB2_FILE_ATTRIBUTE_NORMAL;
  memcpy(rep->file_id, id, sizeof(smb2_file_id));

  // v65: fill in the REST of the reply. v64 set only the three fields above and
  // left the rest at the zeros libsmb2's dispatcher memsets in
  // (libsmb2.c:3478) -- but smb2-cmd-create.c:240-245 writes all of them onto
  // the wire unconditionally, so every CREATE response claimed EndOfFile 0,
  // AllocationSize 0 and all four timestamps 0 (the year 1601) for files with a
  // real size and a real date. Verified on the wire against a 22-byte file:
  // "End Of File: 0", "Last Write: No time specified (0)". MS-SMB2 2.2.14
  // defines these as the file's real values and a client is entitled to cache
  // them rather than re-ask -- which is how a copy of a real book becomes a
  // zero-byte file at the far end.
  //
  // `slot->file`, not `file`: `file` was moved into the slot above and is empty
  // now. Reading it instead would have reintroduced the same zeros by a subtler
  // route.
  rep->end_of_file = isDirectory ? 0 : slot->file.fileSize64();
  rep->allocation_size = rep->end_of_file;
  // FAT stores ONE timestamp; modifyTimeOf() explains why all four SMB fields
  // get it. logFailure=false because a create that cannot read back its own
  // mtime is not worth its own log line -- query_info already logs that case.
  struct smb2_timeval mtime = modifyTimeOf(slot->file, localPath, /*logFailure=*/false);
  // smb2_timeval -> Windows FILETIME. The reply struct's time fields are
  // uint64_t 100-ns-since-1601, NOT the struct smb2_timeval that query_info's
  // info structures take, and confusing the two is exactly how a 1601-epoch bug
  // ships. smb2_timeval_to_win() is public API (libsmb2.h:413).
  const uint64_t winTime = smb2_timeval_to_win(&mtime);
  rep->creation_time = winTime;
  rep->last_access_time = winTime;
  rep->last_write_time = winTime;
  rep->change_time = winTime;

  // v68: answer the maximal-access question if it was asked. Only on success --
  // a failed create has no object to report access for, and every failure path
  // above returns before this point without touching rep->create_context (which
  // the dispatcher memset to zero), so no reply can carry a stale context.
  if (wantsMaximalAccess) attachMaximalAccessContext(rep);

  // v65: arm the compound placeholder. LAST, and only here -- this is the one
  // success path, and the clear at the top of this function covers every failure
  // path. See gLastCreatedId.
  memcpy(gLastCreatedId, id, sizeof(smb2_file_id));
  gLastCreatedOwner = smb2;
  return 0;
}

int closeCmd(smb2_server*, smb2_context* smb2, smb2_close_request* req, smb2_close_reply* rep) {
  if (req == nullptr || rep == nullptr) return -1;
  // Scoped to this connection -- see findOpenFile()'s comment. A handle this
  // connection did not open is "no such handle", not someone else's to close.
  OpenFileEntry* slot = findOpenFile(smb2, req->file_id);
  if (slot == nullptr) {
    if (!isExpectedCompoundCascade(smb2, req->file_id)) {
      DiagLog::line("SMB close reject: no such handle for ctx=%p id=%s", (void*)smb2, fileIdBrief(req->file_id));
    }
    // STATUS_FILE_CLOSED, not NOT_IMPLEMENTED: a client told that CLOSE is
    // unsupported has no way to learn its handle is gone, and some retry the
    // whole operation. This is the line that fired three times in the v64 field
    // log, once per iPhone retry.
    return replyStatus(smb2, SMB2_CLOSE, SMB2_STATUS_FILE_CLOSED);
  }
  // v65: is THIS the handle the compound placeholder resolves to? Read before
  // releaseSlot() clears the slot. Belt and braces -- findOpenFile() would
  // already miss a released slot because it checks inUse -- but the state should
  // not outlive the handle it names.
  const bool wasLastCreated =
      gLastCreatedOwner == smb2 && memcmp(slot->id, gLastCreatedId, sizeof(smb2_file_id)) == 0;
  // Copy the path out BEFORE releasing -- releaseSlot() clears it, and it is
  // the only thing that identifies the file in the failure line below.
  // gPathScratch rather than a 512-byte stack local, per its own declaration:
  // close_cmd uses it nowhere else, and the one-request-in-flight invariant
  // holds here as everywhere else in this file.
  memcpy(gPathScratch, slot->path, strlen(slot->path) + 1);
  // Both consumed before releaseSlot() clears them.
  const bool wantDelete = slot->deletePending;
  const bool wasDirectory = slot->isDirectory;
  // v76: the flag was armed while this path existed, and another handle has
  // since deleted or renamed it. deleteOnClose() works BY PATH, so honouring the
  // flag now could remove whatever has taken that name since. setDisposition
  // refuses to ARM on a stale handle; this covers the other order, where the
  // path goes away AFTER the flag was set, which setDisposition cannot see.
  const bool wasStale = slot->stale;

  // v74: THE ONE THAT WOULD REALLY HURT. A null stream's slot carries no
  // HalFile, so releaseSlot() has nothing to sync -- but deleteOnClose() takes
  // a PATH, and if a client ever set delete-on-close on a stream handle, running
  // it here would ask the filesystem to remove something derived from the base
  // file's name. setInfoCmd refuses to set that flag on a stream at all, so this
  // is belt and braces; it is written down because "the flag can never be set"
  // is exactly the kind of invariant a later change breaks silently.
  const bool wasNullStream = slot->isNullStream;
  // releaseSlot() still runs -- it is what frees the slot -- but its result is
  // "did the final write-back reach the card", and a null stream has no card
  // side and no HalFile: close() on an empty handle returns false, which is not
  // a failure, it is the absence of anything to fail. Reporting it as one made
  // the CLOSE answer NOT_IMPLEMENTED, and the copy died right there even though
  // the CREATE and the WRITE had both succeeded.
  const bool syncOk = releaseSlot(*slot);
  const bool closed = wasNullStream ? true : syncOk;
  if (wasLastCreated) forgetLastCreated(smb2);
  // Strictly after the close -- see deleteOnClose()'s comment.
  if (wantDelete && wasStale) {
    DiagLog::line("SMB delete-on-close SKIPPED: entry already removed under this handle path=%s", gPathScratch);
  }
  const bool deleted =
      (wasNullStream || wasStale) ? true : deleteOnClose(wantDelete, wasDirectory, gPathScratch);

  // THE CLIENT IS TOLD THE TRUTH. A failed close means the tail of the file
  // may not be on the card; answering 0 here is what turns that into a
  // successful-looking copy of a truncated book. The slot is already free
  // either way (see releaseSlot), so returning -1 costs nothing but honesty.
  //
  // THE TWO RESULTS CAN DISAGREE, and all three failing combinations are
  // reported the same way -- as a failure -- with the log line naming which
  // half went wrong. Enumerated rather than reasoned about:
  //
  //   closed=1 deleted=0  the file is STILL THERE and the client thinks it is
  //                       gone. Its next listing contradicts its own UI.
  //   closed=0 deleted=1  the delete the client asked for did happen, but the
  //                       final write-back did not. Reported as a failure
  //                       anyway: "close must not report unverified success"
  //                       is the rule this handler already lives under, and a
  //                       card that cannot complete a write-back is a fact the
  //                       user needs, even on a file being destroyed. The cost
  //                       is a client that may retry a delete that already
  //                       happened; the log line is what tells the two apart.
  //   closed=0 deleted=0  both.
  if (!closed || !deleted) {
    DiagLog::line("SMB close FAILED: sync=%s delete=%s id=%s path=%s", closed ? "ok" : "FAILED",
                  !wantDelete ? "n/a" : (deleted ? "ok" : "FAILED"), fileIdBrief(req->file_id), gPathScratch);
    return -1;
  }
  memset(rep, 0, sizeof(*rep));
  return 0;
}

// ---------------------------------------------------------------------------
// The eleven trivial-but-not-optional handlers (Task 4 brief, Step 6).
int sessionEstablished(smb2_server*, smb2_context* smb2) {
  // The success end of the session-setup stage sequence started in
  // authorizeUser(): reaching here means the NTLMv2 proof verified. Carries
  // the dialect again so a single grep for "session_established" answers
  // "did anything ever authenticate, and over which dialect".
  DiagLog::line("SMB session_established ctx=%p dialect=0x%04x", (void*)smb2, (unsigned)smb2_get_dialect(smb2));
  return 0;
}

int logoffCmd(smb2_server*, smb2_context* smb2) {
  DiagLog::line("SMB logoff ctx=%p", (void*)smb2);
  return 0;
}

int treeDisconnectCmd(smb2_server*, smb2_context*, const uint32_t) { return 0; }

// destruction_event -- releases this connection's open-file table slots
// (closing any HalFile). Missing this exhausts the 8-slot table after
// repeated connections (Task 4 brief, Step 6) -- Step 7's smoke test opens
// and closes 12 connections in a row specifically to prove this works.
// DELIBERATELY DIFFERENT FROM close_cmd, and the difference is not incidental:
// there is no client left to tell. The connection is being torn down, so a
// failed final write-back cannot be reported to anybody -- but it still has to
// be RECORDED, because on a device with no serial port this line is the only
// trace that will ever exist that a file was left short. So this path logs and
// keeps going where close_cmd logs and returns -1.
//
// Do not "unify" the two by giving them a shared helper that swallows the
// result: that is precisely how the bool got discarded here in the first
// place. The return value of releaseSlot() is [[nodiscard]] so that a future
// unification has to make this choice explicitly rather than by omission.
int destructionEvent(smb2_server*, smb2_context* smb2) {
  // v65: the compound placeholder must not survive the connection that armed it.
  // `owner` is compared by POINTER, and a freed smb2_context's address can be
  // handed straight back to the next accepted connection -- at which point a
  // stray 0xFF..FF from that new connection would satisfy the owner check.
  forgetLastCreated(smb2);
  size_t freed = 0;
  size_t failed = 0;
  for (size_t i = 0; i < kMaxOpenFiles; i++) {
    OpenFileEntry& e = gOpenFiles[i];
    if (e.inUse && e.owner == smb2) {
      // Same reason as close_cmd: releaseSlot() clears these.
      memcpy(gPathScratch, e.path, strlen(e.path) + 1);
      const bool wantDelete = e.deletePending;
      const bool wasDirectory = e.isDirectory;
      const bool wasStale = e.stale;  // v76, same reason as closeCmd
      const bool wasNullStream = e.isNullStream;
      const bool syncOk = releaseSlot(e);
      const bool closed = wasNullStream ? true : syncOk;  // see closeCmd
      // A connection dropped with delete-on-close set still has to delete:
      // MS-FSCC 2.4.11 ties deletion to the handle being released, not to a
      // graceful CLOSE, and iOS dropping the connection after marking a file
      // for deletion is an ordinary way for this to happen.
      if (wantDelete && wasStale) {
        DiagLog::line("SMB delete-on-close SKIPPED: entry already removed under this handle path=%s",
                      gPathScratch);
      }
      const bool deleted =
          (wasNullStream || wasStale) ? true : deleteOnClose(wantDelete, wasDirectory, gPathScratch);
      freed++;
      if (!closed || !deleted) {
        failed++;
        DiagLog::line("SMB destruction_event: sync=%s delete=%s (no client left to tell) ctx=%p path=%s",
                      closed ? "ok" : "FAILED", !wantDelete ? "n/a" : (deleted ? "ok" : "FAILED"), (void*)smb2,
                      gPathScratch);
      }
    }
  }
  if (freed > 0) {
    // `failed` counts a slot whose close OR whose delete-on-close went wrong,
    // so calling it sync_failed was a lie in exactly the case someone would be
    // reading this line to understand. The per-slot line above says which.
    DiagLog::line("SMB destruction_event ctx=%p freed=%zu failed=%zu", (void*)smb2, freed, failed);
  }
  return 0;
}

int changeNotifyCmd(smb2_server*, smb2_context* smb2, smb2_change_notify_request*,
                    smb2_change_notify_reply*) {
  // v67: STATUS_NOT_SUPPORTED rather than NOT_IMPLEMENTED. A directory watch is
  // optional and clients are expected to fall back to polling when it is
  // refused -- but only if the refusal says "not supported". NOT_IMPLEMENTED is
  // a different claim and this is one of only two things the iPhone ever gets
  // refused (diag13.log: eight change_notify and two ioctl, nothing else), so
  // it is worth being exactly right about.
  DiagLog::line("SMB change_notify rejected (not supported)");
  return replyStatus(smb2, SMB2_CHANGE_NOTIFY, SMB2_STATUS_NOT_SUPPORTED);
}

int lockCmd(smb2_server*, smb2_context*, smb2_lock_request*) {
  DiagLog::line("SMB lock rejected (not implemented)");
  return -1;
}

int ioctlCmd(smb2_server*, smb2_context* smb2, smb2_ioctl_request* req, smb2_ioctl_reply*) {
  // v67: the ctl_code was being thrown away, so "ioctl rejected" said nothing
  // about WHICH control code -- and they are not interchangeable.
  // FSCTL_VALIDATE_NEGOTIATE_INFO never reaches here (libsmb2 answers it
  // itself), so whatever does is something we have never identified.
  //
  // STATUS_NOT_SUPPORTED, not NOT_IMPLEMENTED: MS-SMB2 3.3.5.15 makes
  // NOT_SUPPORTED (or INVALID_DEVICE_REQUEST) the answer for a control code the
  // server does not implement, and NOT_IMPLEMENTED means something else -- that
  // the IOCTL command itself is unavailable, which is a much larger claim.
  DiagLog::line("SMB ioctl rejected: ctl_code=0x%08x", req != nullptr ? (unsigned)req->ctl_code : 0u);
  return replyStatus(smb2, SMB2_IOCTL, SMB2_STATUS_NOT_SUPPORTED);
}

int oplockBreakCmd(smb2_server*, smb2_context*, smb2_oplock_break_acknowledgement*) {
  DiagLog::line("SMB oplock_break rejected (not implemented)");
  return -1;
}

int leaseBreakCmd(smb2_server*, smb2_context*, smb2_lease_break_acknowledgement*) {
  DiagLog::line("SMB lease_break rejected (not implemented)");
  return -1;
}

int cancelCmd(smb2_server*, smb2_context*) { return 0; }  // no async pending requests to cancel

int echoCmd(smb2_server*, smb2_context*) { return 0; }  // keepalive -- -1 would make the client conclude we're dead

// ---------------------------------------------------------------------------
// Task 7: real file timestamps.
//
// Task 5 left every timestamp field zero. That is not a cosmetic gap: with all
// of them zero, every book in the Files app carries the same date, so sorting
// and "is this the copy I just made?" -- most of what a file browser is FOR --
// stop working. The fix goes through the HAL (HalFile::getModifyDateTime(),
// added for this and documented there), never around it.
//
// TWO THINGS THE LIBRARY DOES WITH THESE FIELDS, both read from lib/smb2
// rather than assumed, because between them they decide what a "missing"
// timestamp looks like on the wire:
//
//   * The struct fields are `struct smb2_timeval` (smb2.h:36-39) -- Unix epoch
//     seconds + microseconds -- NOT FILETIME. The 1601-epoch, 100 ns FILETIME
//     conversion is done by the LIBRARY, on the way out
//     (smb2_timeval_to_win(), timestamps.c:59-64:
//     `tv_sec * 10000000 + 116444736000000000 + tv_usec * 10`). So writing a
//     second FILETIME conversion here would mean converting to 1601 and
//     immediately back to 1970 for the library to redo -- two chances to get
//     an epoch wrong in place of none. What this file produces is therefore
//     Unix seconds (FatTimestamp::toUnixSeconds()), and the assertion that
//     the FILETIME on the wire is right is made against the WIRE, in
//     test/host/smb_smoke_test.py, where it is actually observable.
//   * The two encoders disagree about {0,0}. query_info's classes use
//     smb2_tv_timeval_to_win() (smb2-data-file-info.c:82-91), which
//     special-cases {0,0} to a literal 0 = MS-FSCC 2.4.7's "no time
//     information". query_directory's encoder uses plain
//     smb2_timeval_to_win(), which has no such case, so {0,0} there becomes
//     the FILETIME for 1970-01-01. Both read to a client as "no real date",
//     and there is no third value that means "none" in a timeval, so this is
//     left as it is rather than papered over with a magic negative tv_sec.
constexpr struct smb2_timeval kNoTimestamp = {0, 0};

// FAT keeps ONE usable time per entry and this reports it for all four
// MS-FSCC fields (creation / last access / last write / change), deliberately.
// SdFat can also return create and access stamps, but each accessor is another
// dirEntry() -- an SD read under the storage mutex, per entry, in a listing
// loop that already costs one. And on this device the extra two would mostly
// be noise anyway: nothing in this firmware registers an FsDateTime callback,
// so a file the X3 itself creates gets SdFat's FS_DEFAULT_DATE
// (SdFatConfig.h:311, "1 January <compile year>") for create AND modify, and
// no modify update on write at all. Files that arrive from a computer carry
// real stamps; files this device writes do not. Worth knowing before reading
// too much into a date on screen -- and worth fixing one day by registering a
// date/time callback against the device clock, which is a firmware-wide
// change and not this task's.
//
// `path` is used for the share-root check and for the failure log; pass
// nullptr-safe values only. `logFailure` is false on the per-directory-entry
// path: a failed read there is not a rejection of anything, and one line per
// entry would drown diag.log on the first 268-book folder.
struct smb2_timeval modifyTimeOf(HalFile& file, const char* path, bool logFailure) {
  // The share root has no directory entry to read, and asking anyway returns
  // TRUE holding garbage from sector 0 -- see HalFile::getModifyDateTime()'s
  // header comment for the FatFile::openRoot() trace. Never ask.
  if (isShareRootPath(path)) return kNoTimestamp;

  uint16_t fatDate = 0;
  uint16_t fatTime = 0;
  if (!file.getModifyDateTime(&fatDate, &fatTime)) {
    if (logFailure) {
      DiagLog::line("SMB timestamp: directory entry unreadable path=%s", path != nullptr ? path : "?");
    }
    return kNoTimestamp;
  }

  const int64_t secs = FatTimestamp::toUnixSeconds(fatDate, fatTime);
  if (secs <= 0) {
    // 0 is "no time information" (an unwritten entry) rather than an error, so
    // this is not logged even when logFailure is set -- it is the normal state
    // of a file whose creator never set a timestamp.
    return kNoTimestamp;
  }

  // time_t is not guaranteed 64-bit, and FAT's year field reaches 2107 --
  // past the 2038 wrap of a signed 32-bit time_t. smb2_timeval_to_win() then
  // does `(uint64_t)tv->tv_sec * 10000000` on a value that overflowed on the
  // way in. Refusing the (necessarily corrupt or absurd) date is the safe
  // direction; on a 64-bit time_t this branch is unreachable.
  if (secs > static_cast<int64_t>(std::numeric_limits<time_t>::max())) {
    if (logFailure) {
      DiagLog::line("SMB timestamp: %lld s does not fit time_t, reported as none path=%s",
                    (long long)secs, path != nullptr ? path : "?");
    }
    return kNoTimestamp;
  }

  struct smb2_timeval tv;
  tv.tv_sec = static_cast<time_t>(secs);
  tv.tv_usec = 0;  // FAT's modify time has two-second resolution; there is nothing finer to report
  return tv;
}

// ---------------------------------------------------------------------------
// Task 5: query_directory.
//
// REPLY CONTRACT (read from lib/smb2, not guessed -- the vendored tree has no
// examples/ directory to copy from):
//
//   * `rep->output_buffer` is an ARRAY OF C STRUCTS, not wire bytes.
//     smb2_encode_query_directory_reply() (smb2-cmd-query-directory.c:245-260)
//     walks it with a stride of PAD_TO_64BIT(sizeof(struct
//     smb2_fileidbothdirectoryinformation)) and does the UTF-8 -> UTF-16 name
//     conversion, per-entry padding and next_entry_offset chaining itself.
//     `rep->output_buffer_length` on INPUT is that array's size in bytes; the
//     encoder overwrites it with the true wire length.
//   * WE own that memory. smb2_query_directory_request_cb (libsmb2.c:3767-3803)
//     never frees rep.output_buffer, and neither does the encoder -- it copies
//     everything into its own iovectors. But the encoder runs AFTER this
//     handler returns, so the storage cannot be a local either. Hence the
//     static arena below: no allocation on a per-request path, and no lifetime
//     puzzle.
//   * Termination is `return 0` with `output_buffer_length == 0`, which
//     libsmb2.c:3785-3787 turns into STATUS_NO_MORE_FILES. Returning -1 would
//     instead send STATUS_NOT_IMPLEMENTED and abort the whole listing.
//
// ONLY TWO INFO CLASSES ARE ENCODABLE. The encoder's switch
// (smb2-cmd-query-directory.c:238-247 and :303-357) handles
// SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION (0x25) and
// SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION (0x26); every other class falls to
// `default: fs_size = 0`, which would make the encoder emit a SUCCESSFUL reply
// containing zero bytes -- i.e. an empty directory. So the plain
// FILE_DIRECTORY_INFORMATION (0x01) / FILE_FULL_DIRECTORY_INFORMATION (0x02) /
// FILE_BOTH_DIRECTORY_INFORMATION (0x03) classes named in the task brief are
// rejected with a logged -1 instead: an honest STATUS_NOT_IMPLEMENTED, not a
// silent "this folder is empty". Supporting them would need either an edit to
// the machine-checked vendored tree or smb2_set_passthrough() context-wide,
// which also rewires read/write/set_info/ioctl (Tasks 6-7's ground). 0x25 is
// what macOS/iOS and Windows actually send.

// Entries returned per response. Enumeration continues across as many
// QUERY_DIRECTORY round trips as it takes, so this is not a limit on directory
// size -- it, and the two budgets below it, decide how much RAM one reply
// costs and how many round trips a folder takes.
//
// HISTORY, because this constant was 2 for one round and the reason matters:
// upstream's reply encoder wrote an ABSOLUTE next_entry_offset where MS-FSCC
// 2.4.8 requires a RELATIVE one, so a client walking `p += next_entry_offset`
// over-shot on every entry after the first and silently skipped files
// (measured: 4 entries in, entry 2 at offset 124 declared next=240 instead of
// 116, so the client jumped to 364 and never saw the file at 240). Only
// entries at offset 0 and the encoder's hard-zeroed final entry were safe, so
// two per reply was the largest set the bug could not corrupt. **That bug is
// now fixed in the vendored tree** -- one in-line expression change in
// lib/smb2/lib/smb2-cmd-query-directory.c, pinned by
// scripts/verify_libsmb2_patch.py and documented in
// docs/third-party/libsmb2-vendoring.md ("The second patch") -- so entries can
// pack a reply again. test_listing_next_entry_offset_chain in
// test/host/smb_smoke_test.py is the wire-level regression test: it walks the
// chain relatively and requires it to land exactly on the buffer end.
//
// 32 is a memory choice, and the binding limit is usually this one. It is
// paid for TWICE, and anyone retuning it should see both halves:
//
//   * Transient: the library mallocs the whole encoded reply as ONE contiguous
//     block, and on this device contiguous is the scarce resource, not total
//     free (CLAUDE.md hard limit 6 -- the reading-time pool tops out near
//     53 KB, and OPDS already needs 40-55 KB of it). 32 entries with typical
//     Chinese book filenames is ~6 KB; the wire budget below caps the worst
//     case near 10 KB.
//   * Resident: these buffers are static BSS, so they are held for the whole
//     run -- including while the user is just reading a book, with the SMB
//     server idle -- on a device with 320 KB of RAM. Measured from the .map at
//     32 entries: gDirEntries 4,608 + gDirNameArena 2,814 + gInfoNameScratch
//     512 + the nine query_info structs 432 + gPathScratch 512, plus the 768 B
//     (8 x 96) searchPattern adds to gOpenFiles == **9,646 B always
//     resident**. (gOpenFiles' other 4,416 B is Task 4's open-file table, not
//     counted here.) gDirEntries alone is 144 B per entry, so this one
//     constant sets nearly half the total.
//
// Doubling to 64 would cost another 4.6 KB resident and double the transient
// block, to save one round trip out of nine on a 268-book folder. Halving to
// 16 gives 2.3 KB back for one extra round trip per 16 books. 32 is the
// middle; the numbers above are what to re-do the arithmetic with, not a
// reason the value is untouchable.
constexpr size_t kMaxDirEntriesPerResponse = 32;

// Entries EXAMINED per request. Distinct from the accepted count above:
// hidden and non-matching entries cost an SD read each but fill no buffer, so
// only bounding the accepted count leaves the scan itself unbounded. See the
// loop for why this is conditional on having accepted something first --
// that condition is a correctness requirement, not caution.
//
// 128 = 4x the accepted cap, so it never fires on an ordinary folder, nor on
// one that is up to three-quarters hidden entries -- which is a real shape,
// not a hypothetical: macOS writes an AppleDouble "._name" sidecar next to
// every file it copies to an SMB share, and every one of those is a dotfile
// ProtectedPath hides. It bounds a pathological run at 128 openNextFile() +
// getName() pairs per callback.
constexpr size_t kMaxDirEntriesScannedPerResponse = 128;

// Worst-case UTF-8 length of one FAT long filename: 255 UTF-16 code units x 3
// bytes + NUL. SdFat's getName8() does NOT truncate -- it fails and returns 0
// if the buffer is short (FatName.cpp:99-150) -- so sizing for the true
// maximum is what keeps a legal filename from becoming an invisible one.
constexpr size_t kDirNameMaxBytes = 766;

// Name arena. Sized soft-cap + one worst-case name: the read loop stops
// starting a new entry once `arenaUsed >= kDirNameArenaSoftBytes`, so at the
// moment getName() is called there are always >= kDirNameMaxBytes free. That
// is what lets the loop decide to stop BEFORE consuming a directory entry --
// openNextFile() has no push-back, so an entry read and then found not to fit
// would be lost from the listing entirely.
constexpr size_t kDirNameArenaSoftBytes = 2048;
constexpr size_t kDirNameArenaBytes = kDirNameArenaSoftBytes + kDirNameMaxBytes;

// Encoded wire size of one entry, worst case:
// PAD_TO_32BIT(SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE + 2 * 765) = 1636.
constexpr uint32_t kMaxEntryWireBytes = 1664;

// Our own ceiling on the reply the library will malloc and sign -- one
// contiguous block, which is the scarce resource here (see the entry-count
// constant above). Clamped against the client's own output_buffer_length in
// the handler, and in practice ours is the smaller of the two: clients send
// 64 KiB. Same soft/hard split as the arena -- stop STARTING entries at the
// soft cap, so the one entry that may still be added cannot overshoot, which
// bounds the real worst case at 8192 + kMaxEntryWireBytes.
constexpr uint32_t kSoftWireBudgetBytes = 8192;

// v66: THERE IS NO MINIMUM. The constant that used to live here
// (kMinClientOutputBuffer = 4096) refused any QUERY_DIRECTORY whose
// OutputBufferLength was smaller, on the reasoning quoted verbatim from its own
// comment: "Below this we cannot promise even one worst-case entry fits the
// client's buffer. Real clients send 64 KiB+."
//
// The second sentence is false, and it cost a whole flash cycle. The
// reporting iPhone sends **1024**. diag12.log has eighteen consecutive
// `query_directory reject: client buffer 1024 < 4096 bytes` lines across two
// sessions -- authentication fine, TREE_CONNECT fine, and then every single
// attempt to list the card refused by us. The decode is not in doubt: MS-SMB2
// 2.2.33 puts OutputBufferLength at offset 28 and
// smb2-cmd-query-directory.c:... reads `smb2_get_uint32(iov, 28, ...)`, so 1024
// is really what iOS asked for. It is entirely legal: the protocol has no
// minimum, and MS-SMB2 3.3.5.18 says what to do when the buffer is too small
// for even one entry -- answer STATUS_INFO_LENGTH_MISMATCH so the client can
// retry bigger -- not refuse the command.
//
// What replaces it: the client's OutputBufferLength is a HARD ceiling and
// kSoftWireBudgetBytes is our own soft one, and the entry loop stops STARTING a
// new entry once a worst-case one could no longer fit. The first entry of a
// response is always attempted, because a response with nothing in it is
// indistinguishable on the wire from end-of-directory.

// The encoder's stride is PAD_TO_64BIT(sizeof(...)); a plain C array only
// matches that if the struct is already 8-byte aligned in size. It is (it
// contains uint64_t members), on both the 32-bit device and the 64-bit host --
// but assert it rather than assume it, because if it ever stopped being true
// every entry after the first would be decoded from the wrong offset.
static_assert(sizeof(struct smb2_fileidbothdirectoryinformation) % 8 == 0,
              "query_directory entry stride must equal sizeof(struct) for a plain array to work");
// The encoder decides "is this the last entry" with `in_remain >=
// SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE` while stepping by sizeof(struct)
// (smb2-cmd-query-directory.c:288-294). The two only agree while the stride is
// at least that size -- and "is this the last entry" is what zeroes
// next_entry_offset, i.e. what terminates the client's walk.
static_assert(sizeof(struct smb2_fileidbothdirectoryinformation) >=
                  SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE,
              "query_directory last-entry detection requires stride >= the on-wire fixed size");

// Both point into the same single heap block as gOpenFiles -- see its comment
// for why none of the three is in .bss any more.
struct smb2_fileidbothdirectoryinformation* gDirEntries = nullptr;
char* gDirNameArena = nullptr;

// ONE allocation, not three: three separate blocks would leave three separate
// holes in p3 when a later allocation outlives them, and this is exactly the
// alloc-once-reuse shape CLAUDE.md endorses over anything that can grow or
// migrate. Sized entirely from compile-time constants; nothing here is ever
// resized, and the whole thing is released together.
struct SmbTables {
  OpenFileEntry openFiles[kMaxOpenFiles];
  struct smb2_fileidbothdirectoryinformation dirEntries[kMaxDirEntriesPerResponse];
  char dirNameArena[kDirNameArenaBytes];
};
std::unique_ptr<SmbTables> gTables;

// Case-insensitive '*'/'?' glob, iterative (no recursion -- this runs on a
// network callback's stack). Case folding is ASCII-only, which is exactly
// right here: SMB and FAT are case-insensitive for ASCII, and for a UTF-8
// multi-byte sequence every byte is >= 0x80 and so compares byte-for-byte
// unchanged. DOS's other wildcards (`<`, `>`, `"`) are not implemented; they
// are only reachable from legacy clients and would simply fail to match.
char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool wildcardMatch(const char* pattern, const char* name) {
  const char* p = pattern;
  const char* s = name;
  const char* starP = nullptr;
  const char* starS = nullptr;
  while (*s != '\0') {
    if (*p == '?' || (*p != '\0' && *p != '*' && asciiLower(*p) == asciiLower(*s))) {
      ++p;
      ++s;
    } else if (*p == '*') {
      starP = p++;
      starS = s;
    } else if (starP != nullptr) {
      p = starP + 1;
      s = ++starS;
    } else {
      return false;
    }
  }
  while (*p == '*') ++p;
  return *p == '\0';
}

// "*" and "" both mean "everything"; short-circuiting them keeps the common
// case out of the matcher entirely.
bool patternMatchesAll(const char* pattern) {
  return pattern[0] == '\0' || (pattern[0] == '*' && pattern[1] == '\0');
}

int queryDirectoryCmd(smb2_server*, smb2_context* smb2, smb2_query_directory_request* req,
                      smb2_query_directory_reply* rep) {
  if (req == nullptr || rep == nullptr) return -1;

  // MUST BE FIRST, ahead of every other early return.
  //
  // This is a defence against an upstream defect in the vendored library, not
  // housekeeping. smb2_process_query_directory_request_fixed()
  // (smb2-cmd-query-directory.c:505-545) allocates the request struct with
  // malloc() and, when file_name_length == 0, returns before
  // ..._request_variable() (the only place that assigns req->name) ever runs --
  // socket.c:642 skips the variable stage entirely for a zero-length tail. So
  // req->name is left holding whatever the recycled heap block held. libsmb2.c:
  // 3798-3800 then does `if (req->name) smb2_free_data(smb2, req->name)`, and
  // smb2_free_data (alloc.c:131-154) dereferences 16 bytes BEHIND that pointer
  // and free()s a chain from it.
  //
  // This is not theoretical and it is not rare: MS-SMB2 3.3.5.18 has the client
  // send FileName only on the FIRST query of an enumeration, so EVERY listing's
  // continuation/termination request arrives with file_name_length == 0.
  // Reproduced on the harness -- after other traffic had recycled the heap,
  // req->name came back as 0x747874 ("txt", the tail of an earlier request's
  // filename) and the server process died. We cannot fix it in lib/smb2/ (it is
  // machine-checked against pristine upstream by
  // scripts/verify_libsmb2_patch.py), but req is ours to write to, and
  // normalising it here makes the library's own guard correct.
  if (req->file_name_length == 0) req->name = nullptr;

  OpenFileEntry* slot = findOpenFile(smb2, req->file_id);
  if (slot == nullptr) {
    DiagLog::line("SMB query_directory reject: no such handle for ctx=%p", (void*)smb2);
    return -1;
  }
  if (!slot->isDirectory || !slot->file) {
    DiagLog::line("SMB query_directory reject: handle is not an open directory path=%s", slot->path);
    // v74 (review): NOT_A_DIRECTORY, not NOT_IMPLEMENTED. Same over-claim this
    // file has been walking back everywhere else -- and now reachable by an
    // ordinary client, since a discarded stream handle lands here.
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NOT_A_DIRECTORY);
  }

  const uint8_t infoClass = req->file_information_class;
  if (infoClass != SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION &&
      infoClass != SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION) {
    DiagLog::line("SMB query_directory unsupported class=%u (0x%02x); only 0x25/0x26 are encodable",
                  (unsigned)infoClass, (unsigned)infoClass);
    return -1;
  }

  // SMB2_RESTART_SCANS / SMB2_REOPEN mean "start over"; so does the first query
  // on a freshly opened handle. SMB2_INDEX_SPECIFIED asks us to resume at an
  // opaque index we have no way to seek to, so it is logged and treated as a
  // plain continuation -- which is what it means in practice, since clients
  // that set it pass the index they were already at.
  const bool restart =
      (req->flags & (SMB2_RESTART_SCANS | SMB2_REOPEN)) != 0 || !slot->enumStarted;
  if (restart) {
    slot->file.rewindDirectory();
    slot->enumStarted = true;
    slot->enumResponses = 0;
    DiagLog::line("SMB query_directory start: client buffer %u path=%s",
                  (unsigned)req->output_buffer_length, slot->path);
  }
  if (slot->enumResponses < 0xFFFF) slot->enumResponses++;
  if ((req->flags & SMB2_INDEX_SPECIFIED) != 0) {
    DiagLog::line("SMB query_directory: SMB2_INDEX_SPECIFIED (index=%u) ignored, continuing from cursor",
                  (unsigned)req->file_index);
  }

  // Points at the stored filter normally, or straight at the request's own
  // string when that was too long to store (see below). Everything downstream
  // matches against THIS, never against slot->searchPattern directly.
  const char* pattern = slot->searchPattern;
  if (req->name != nullptr && req->name[0] != '\0') {
    const size_t patternLen = strlen(req->name);
    if (patternLen < sizeof(slot->searchPattern)) {
      memcpy(slot->searchPattern, req->name, patternLen + 1);
      slot->patternOverlong = false;
    } else {
      // v67: USE IT IN PLACE. Neither of the two previous answers was right.
      //
      // v64 degraded to match-all, which was actively dangerous: macOS/iOS look
      // one file up by sending its exact name as the pattern together with
      // SMB2_RETURN_SINGLE_ENTRY, so the single entry they got back was the
      // FIRST entry of the directory -- one real file's metadata attributed to
      // a different real file, silently. v65 refused instead, which is at least
      // visible, but diag13.log then showed TWELVE refusals in one browsing
      // session against a real Chinese-language library, at 98, 104 and 105 bytes.
      // The 95-byte buffer is 31 Han characters; ordinary Traditional-Chinese
      // book titles are longer than that. Refusing them means iOS cannot
      // resolve those files at all.
      //
      // The buffer only exists so a filter can SURVIVE ACROSS CALLS. It is not
      // needed for the call that carries the filter -- `req->name` is a
      // NUL-terminated UTF-8 string owned by libsmb2 for the duration of this
      // handler. So point at it and match against it directly: correct answer,
      // zero extra memory, and none of the 8 x 670 bytes that growing the slot
      // buffer to a full 766-byte FAT name would have added to the very
      // allocation that already competes with the connections (diag12.log has
      // an accept -ENOMEM at 7,156 bytes largest free block).
      //
      // What is NOT covered is a CONTINUATION of an over-long filter, handled
      // in the `else if` below. It cannot arise for the shape that produced
      // this: a literal-name probe scans to the end of the directory in one
      // call (the scan bound deliberately does not apply while nothing has
      // matched yet, see the loop), so the answer -- the one entry, or
      // NO_MORE_FILES -- is always complete in the first response.
      slot->patternOverlong = true;
      slot->searchPattern[0] = '\0';
      pattern = req->name;
      DiagLog::line("SMB query_directory: pattern %zu bytes > %zu, matched in place path=%s",
                    patternLen, sizeof(slot->searchPattern) - 1, slot->path);
    }
  } else if (restart) {
    // New enumeration with no pattern supplied: match everything. (A
    // continuation with no pattern keeps the one already stored -- that is the
    // whole reason it is stored.)
    slot->searchPattern[0] = '*';
    slot->searchPattern[1] = '\0';
    slot->patternOverlong = false;
  } else if (slot->patternOverlong) {
    // v67: a continuation of a filter we never stored. Re-applying it is
    // impossible and matching everything instead would return files the client
    // did not ask for and would attribute to the name it did ask for. Ending
    // the enumeration is the honest answer, and is also the complete one for
    // the only shape that gets here -- see the in-place branch above.
    DiagLog::line("SMB query_directory: continuation of an unstorable pattern, ending path=%s", slot->path);
    rep->output_buffer = nullptr;
    rep->output_buffer_length = 0;
    return 0;
  }
  const bool matchAll = patternMatchesAll(pattern);

  // HARD ceiling: never write past what the client said it can take.
  const uint32_t clientCap = req->output_buffer_length;
  // SOFT ceiling: our own limit on the contiguous block libsmb2 will malloc and
  // sign. Whichever is smaller governs when the loop stops starting entries.
  //
  // Written as a min(), not as `clientCap - kMaxEntryWireBytes`: that form
  // underflows on uint32 for any clientCap below 1664 and wraps to ~4 billion,
  // which then fails the "< kSoftWireBudgetBytes" test and quietly yields a cap
  // LARGER than the client's entire buffer. The old 4096 floor was the only
  // thing making that unreachable, so removing the floor without also removing
  // the subtraction would have turned a clean refusal into an overrun.
  const uint32_t wireCap = (clientCap < kSoftWireBudgetBytes) ? clientCap : kSoftWireBudgetBytes;
  // Set when an entry had to be abandoned purely for lack of room. Only
  // distinguishable from "the directory is exhausted" by remembering it: both
  // otherwise arrive at the bottom of this function with count == 0.
  bool tooSmallForOneEntry = false;
  const uint32_t fixedSize = (infoClass == SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION)
                                 ? SMB2_FILEID_BOTH_DIRECTORY_INFORMATION_SIZE
                                 : SMB2_FILEID_FULL_DIRECTORY_INFORMATION_SIZE;

  size_t count = 0;      // entries ACCEPTED into this reply
  size_t scanned = 0;    // entries EXAMINED, including hidden and non-matching ones
  size_t arenaUsed = 0;
  uint32_t wireUsed = 0;

  // v65: '.' and '..', first, on a restarting enumeration only.
  //
  // v64 emitted neither, because the underlying openNextFile() does not return
  // them on either backend. The visible result was that an EMPTY directory
  // answered its very first QUERY_DIRECTORY with zero entries -- which
  // libsmb2 turns into STATUS_NO_MORE_FILES (libsmb2.c:3785) and a client
  // reports as an ERROR, not as an empty folder: measured `cd emptydir; ls` ->
  // "NT_STATUS_NO_SUCH_FILE listing \emptydir\*". Every real SMB server emits
  // these two, they are how a client learns a directory is present-but-empty,
  // and macOS/iOS also use '..' when walking back up.
  //
  // Subject to the same pattern filter as any other name (a client probing for
  // one specific filename must not be handed '.'), and to the same wire/arena
  // accounting -- two entries of 1 and 2 bytes cannot approach any of the three
  // budgets, but doing the bookkeeping inline keeps one set of rules.
  if (restart) {
    // The directory's own timestamp for both, which is what a FAT volume can
    // actually say. logFailure=false: the real entries below do not log this
    // either, and a folder whose mtime is unreadable is not worth two lines.
    const struct smb2_timeval dirTime = modifyTimeOf(slot->file, slot->path, /*logFailure=*/false);
    const char* const kDotNames[2] = {".", ".."};
    for (int d = 0; d < 2; ++d) {
      const char* const dotName = kDotNames[d];
      if (!matchAll && !wildcardMatch(pattern, dotName)) continue;
      char* const nameDst = gDirNameArena + arenaUsed;
      const size_t nameLen = strlen(dotName);
      const uint32_t dotWire = (fixedSize + 2 * static_cast<uint32_t>(nameLen) + 3) & ~3u;
      // v66: even these have to fit. One is ~108 bytes so any sane buffer takes
      // both, but "sane" is exactly the assumption that produced the 4096 floor,
      // so it is checked rather than asserted.
      if (wireUsed + dotWire > clientCap) {
        tooSmallForOneEntry = true;
        break;
      }
      memcpy(nameDst, dotName, nameLen + 1);
      struct smb2_fileidbothdirectoryinformation& e = gDirEntries[count];
      memset(&e, 0, sizeof(e));
      e.creation_time = dirTime;
      e.last_access_time = dirTime;
      e.last_write_time = dirTime;
      e.change_time = dirTime;
      e.end_of_file = 0;
      e.allocation_size = 0;
      e.file_attributes = SMB2_FILE_ATTRIBUTE_DIRECTORY;
      // '.' is this directory and '..' is its parent, and both must report the
      // SAME id those directories report for themselves -- see childIdentity().
      e.file_id = (d == 0) ? pathIdentity(slot->path) : parentIdentity(slot->path);
      e.name = nameDst;
      arenaUsed += nameLen + 1;
      wireUsed += dotWire;
      count++;
    }
    // SMB2_RETURN_SINGLE_ENTRY is honoured for these too: a client that asked
    // for exactly one entry and got two would mis-parse the reply.
    if (count > 0 && (req->flags & SMB2_RETURN_SINGLE_ENTRY) != 0) {
      rep->output_buffer = reinterpret_cast<uint8_t*>(gDirEntries);
      rep->output_buffer_length = static_cast<uint32_t>(count * sizeof(gDirEntries[0]));
      return 0;
    }
  }

  // `count == 0 ||` is what makes a small client buffer work at all: with
  // clientCap 1024 and a 1664-byte worst case, the second clause is false from
  // the very first iteration, so without it the loop would never run and every
  // listing would answer "no more files" -- the same invisible-directory
  // outcome as the floor this replaced, just reached differently.
  //
  // The second clause is what keeps entries from being LOST rather than merely
  // deferred: openNextFile() has no push-back, so an entry read and then found
  // not to fit is gone from this enumeration for good. Requiring room for a
  // worst-case entry before starting one means that for every iteration after
  // the first, the entry is guaranteed to fit. Only the first can overshoot,
  // and that case is answered with STATUS_INFO_LENGTH_MISMATCH below.
  while (count < kMaxDirEntriesPerResponse && arenaUsed < kDirNameArenaSoftBytes &&
         (count == 0 || wireUsed + kMaxEntryWireBytes <= wireCap)) {
    // Bound the SCAN, not just the accepted count. Hidden and non-matching
    // entries cost a real openNextFile() + getName() each -- an SD directory
    // read under the storage mutex -- but consume no budget above, so without
    // this a folder with a long run of skipped entries walks arbitrarily far
    // inside a single network callback. SmbServer::tick() is supposed to be
    // non-blocking (Task 3's whole design); a multi-thousand-entry scan there
    // stalls the e-ink UI. Continuation is free -- the cursor lives in the
    // directory handle -- so stopping early costs one extra round trip.
    //
    // `count > 0` IS LOAD-BEARING, not caution. Stopping with zero accepted
    // entries is indistinguishable, on the wire, from "end of directory":
    // libsmb2.c turns output_buffer_length == 0 into STATUS_NO_MORE_FILES and
    // there is no "ask me again" status to return instead. So a client
    // probing for one specific filename in a large folder would be told the
    // file does not exist. The bound therefore only applies once we have
    // something to hand back -- which covers the trailing-junk case (accept a
    // few books, then wade through hundreds of macOS "._" files) but
    // deliberately does NOT bound a search that has matched nothing yet. That
    // case must still reach the end of the directory to answer correctly, and
    // its total cost equals one ordinary full listing.
    //
    // TASK 8 RE-EXAMINATION (this handler now runs inside the activity loop
    // that also drives the e-ink UI, so "how long can one call take" stopped
    // being theoretical). The conclusion above survives -- there is no bound
    // that does not reintroduce a false "not found" -- but three things are
    // now pinned down rather than hand-waved:
    //
    //  * WHEN it happens. `matchAll` skips only ProtectedPath names, and a
    //    non-matching pattern skips everything else. So the unbounded shape is
    //    "a pattern that matches nothing (yet)" -- overwhelmingly a client
    //    probing one specific name (.DS_Store, desktop.ini, ._foo), which
    //    macOS and iOS do constantly and which genuinely IS absent, so the
    //    full walk is the honest answer, not a pathology.
    //  * HOW BAD it gets. One walk of one directory, per such request. The
    //    device has to be able to do that anyway to list the folder at all;
    //    what is new is that it happens in ONE call instead of spread over
    //    ~N/32 calls. For this user's library (~270 books in a folder) that is
    //    a few hundred milliseconds of an otherwise-idle screen.
    //  * WHAT WOULD BREAK. The failure mode of an unbounded scan is the task
    //    watchdog, not the UI -- so the loop feeds it below. The UI here is a
    //    static QR screen; the only interaction is Back, which is late by the
    //    length of the scan and then works.
    //
    // The real fix, deliberately NOT taken in a wiring task: a wildcard-free
    // pattern is a single-name probe and could be answered with one
    // Storage.exists()/open() instead of a walk, turning the dominant case
    // from O(N) into O(1). It needs new per-handle state ("this literal probe
    // has already been answered") so the continuation still reports
    // end-of-directory rather than the same entry forever -- new state, new
    // failure modes, in the listing path, in the task that first puts any of
    // this on real hardware. Written up in task-8-report.md instead.
    if (count > 0 && scanned >= kMaxDirEntriesScannedPerResponse) {
      DiagLog::line("SMB query_directory: scan bound hit in %s (scanned=%zu accepted=%zu), continuing next request",
                    slot->path, scanned, count);
      break;
    }
    HalFile child = slot->file.openNextFile();
    if (!child) {
      // End of directory. This is ALSO what SdFat reports for an entry it
      // cannot open (FatFile::openNext, FatFile.cpp:676-680, `goto fail` on a
      // bad LFN checksum or a failed openCachedEntry) -- HalFile carries no
      // error channel to tell the two apart, so a corrupt entry truncates the
      // listing there. test/host/stub_hal/HalStorage.cpp's openNextFile() was
      // changed in this same commit to stop instead of silently skipping, so
      // the harness reproduces that rather than certifying a listing the
      // device would cut short.
      break;
    }
    scanned++;
    // Feed the watchdog, not every entry (that would be a syscall per
    // directory entry) but often enough that no scan length can reach the
    // 5 s timeout: 64 entries is a handful of SD sector reads. The caller's
    // own resets (CrossPointWebServerActivity::loop()) cannot help here --
    // they bracket the whole tick(), and this loop is inside it.
    if ((scanned & 0x3F) == 0) smbFeedWatchdog();

    // Read straight into the arena's free tail; the loop's soft cap guarantees
    // at least kDirNameMaxBytes are available here. Nothing is committed until
    // the entry is accepted, so a hidden or non-matching name simply gets
    // overwritten by the next one.
    char* const nameDst = gDirNameArena + arenaUsed;
    const size_t nameLen = child.getName(nameDst, kDirNameMaxBytes);
    if (nameLen == 0) {
      DiagLog::line("SMB query_directory: unreadable entry name in %s (skipped)", slot->path);
      continue;
    }

    // Hiding, mirroring WebDAV's PROPFIND: createCmd refuses to open any
    // protected path at all (read included), so listing one would advertise
    // exactly what we then refuse -- and /.crossmosa/ holds the Wi-Fi
    // credentials, the settings file and reading progress. Same ruleset, one
    // copy: src/util/ProtectedPath.
    if (ProtectedPath::isProtectedName(nameDst)) continue;
    if (!matchAll && !wildcardMatch(pattern, nameDst)) continue;

    const bool childIsDirectory = child.isDirectory();
    const uint64_t endOfFile = childIsDirectory ? 0 : child.fileSize64();

    // Read BEFORE the entry is committed but AFTER it has passed the hiding
    // and pattern filters, so a folder full of skipped entries does not pay an
    // SD read each for a timestamp nobody will see.
    const struct smb2_timeval mtime = modifyTimeOf(child, nameDst, /*logFailure=*/false);

    struct smb2_fileidbothdirectoryinformation& e = gDirEntries[count];
    memset(&e, 0, sizeof(e));
    // FAT has one time; see modifyTimeOf() for why all four fields get it.
    e.creation_time = mtime;
    e.last_access_time = mtime;
    e.last_write_time = mtime;
    e.change_time = mtime;
    e.end_of_file = endOfFile;
    // Allocation size == end of file. The real on-disk size would need the
    // volume's cluster size, which HalStorage does not expose; reporting the
    // logical size is self-consistent and never claims less space than the
    // file occupies.
    e.allocation_size = endOfFile;
    e.file_attributes = childIsDirectory ? SMB2_FILE_ATTRIBUTE_DIRECTORY : SMB2_FILE_ATTRIBUTE_NORMAL;
    // v65: a REAL, stable id. v64 sent 0 here on the honest grounds that FAT has
    // no unique per-file id and MS-FSCC 2.4.17 permits 0 for such filesystems.
    // Measured consequence: the Linux kernel client logs "Autodisabling the use
    // of server inode numbers ... The server doesn't seem to support them
    // properly", then RE-ASKS for the directory using
    // FileFullDirectoryInformation (class 0x02) -- which this server does not
    // encode -- so `ls` failed outright with "Operation not supported". Being
    // honest about a missing capability cost the whole listing. See
    // childIdentity() for why this value has to match what QUERY_INFO will later
    // report for the same file.
    e.file_id = childIdentity(slot->path, nameDst);
    e.name = nameDst;
    // next_entry_offset / file_name_length / short_name* are all computed by
    // the encoder; the memset above leaves them zero for it.

    // Upper bound, not the exact figure: smb2_utf8_to_utf16() never produces
    // more UTF-16 code units than the UTF-8 string has bytes, so 2 * nameLen
    // can only over-estimate. Budgeting high is the safe direction.
    const uint32_t entryWire = (fixedSize + 2 * static_cast<uint32_t>(nameLen) + 3) & ~3u;
    if (wireUsed + entryWire > clientCap) {
      // Reachable ONLY with count == 0 -- for every later iteration the loop
      // guard already proved a worst-case entry fits, and entryWire is bounded
      // by that worst case. So this is "the client's whole buffer cannot hold
      // one entry", which needs a name of roughly 460+ UTF-8 bytes at
      // clientCap 1024.
      //
      // The entry is consumed either way (no push-back), so it is skipped in
      // this enumeration. That is the honest cost of not carrying a pending
      // entry across calls, which would need ~870 bytes per slot in the same
      // table whose allocation already competes with the connections -- see the
      // accept -ENOMEM in diag12.log. STATUS_INFO_LENGTH_MISMATCH is what
      // MS-SMB2 3.3.5.18 prescribes and tells the client to retry with a bigger
      // buffer; the log line is how we would ever know it happened.
      DiagLog::line("SMB query_directory: entry needs %u bytes, client buffer is %u -- skipped, path=%s",
                    (unsigned)entryWire, (unsigned)clientCap, slot->path);
      tooSmallForOneEntry = true;
      break;
    }
    arenaUsed += nameLen + 1;
    wireUsed += entryWire;
    count++;

    if ((req->flags & SMB2_RETURN_SINGLE_ENTRY) != 0) break;
  }

  if (count == 0 && tooSmallForOneEntry) {
    // NOT end-of-directory. Answering 0 entries here would be read as
    // STATUS_NO_MORE_FILES (libsmb2.c:3785) and the folder would look empty.
    return replyStatus(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_INFO_LENGTH_MISMATCH);
  }

  if (count == 0) {
    // Task 8: make the deliberately-unbounded case visible. If a zero-match
    // scan walked past the bound that a producing scan would have stopped at,
    // say so with the real number -- this is the one input shape that can hold
    // up the activity loop, and diag.log is the only place a device with no
    // serial port can ever report it. Quiet for ordinary end-of-enumeration
    // and for genuinely small directories.
    if (scanned >= kMaxDirEntriesScannedPerResponse) {
      DiagLog::line("SMB query_directory: zero-match scan walked %zu entries in one call, pattern='%s' path=%s",
                    scanned, pattern, slot->path);
    }
    // v66: the round-trip count for the listing that just finished. Cheap (one
    // line per completed enumeration, not per response) and it is the whole
    // evidence base for whether the client-buffer path needs a pending-entry
    // stash in a later version.
    DiagLog::line("SMB query_directory done: %u responses, buffer %u, path=%s",
                  (unsigned)slot->enumResponses, (unsigned)req->output_buffer_length, slot->path);
    // libsmb2.c:3785 turns this into STATUS_NO_MORE_FILES -- the correct end of
    // an enumeration, and also the correct answer for a genuinely empty
    // directory on the very first query.
    rep->output_buffer = nullptr;
    rep->output_buffer_length = 0;
    return 0;
  }

  rep->output_buffer = reinterpret_cast<uint8_t*>(gDirEntries);
  rep->output_buffer_length = static_cast<uint32_t>(count * sizeof(gDirEntries[0]));
  return 0;
}

// ---------------------------------------------------------------------------
// Task 5: query_info.
//
// Same ownership rule as query_directory: `rep->output_buffer` points at a
// plain C struct that smb2_encode_query_info_reply() (smb2-cmd-query-info.c:
// 129-333) packs into wire format, it runs after this handler returns, and
// nothing frees it -- so the structs below are static, not locals. The input
// `output_buffer_length` is only the encoder's malloc hint (it allocates that
// plus 1024 and then overwrites the field with the true encoded length), so
// each branch sets it to that class's own upper bound rather than one padded
// constant.
struct smb2_file_basic_info gFileBasicInfo;
struct smb2_file_standard_info gFileStandardInfo;
struct smb2_file_all_info gFileAllInfo;
struct smb2_file_network_open_info gFileNetworkOpenInfo;
struct smb2_file_fs_size_info gFsSizeInfo;
struct smb2_file_fs_full_size_info gFsFullSizeInfo;
struct smb2_file_fs_device_info gFsDeviceInfo;
struct smb2_file_fs_attribute_info gFsAttributeInfo;
struct smb2_file_fs_volume_info gFsVolumeInfo;
// v70: FileStreamInformation. Re-zeroed and refilled on every use, which is not
// optional here: smb2_encode_file_stream_info() (smb2-data-file-info.c:213-261)
// MUTATES what it is given -- `fs->stream_name_length *= 2` on entry -- so a
// static carried across calls would double its name length every time.
struct smb2_file_stream_info gFileStreamInfo;
char gInfoNameScratch[kMaxSmbPathLen];

// MS-FSCC 2.5.1 FileFsAttributeInformation flags -- smb2.h defines the
// FileFsDeviceInformation ones (FILE_DEVICE_DISK etc.) but not these, so they
// are named locally the same way Task 4 named the CreateAction values.
constexpr uint32_t kFileCasePreservedNames = 0x00000002;
constexpr uint32_t kFileUnicodeOnDisk = 0x00000004;
// v70: FILE_NAMED_STREAMS (MS-FSCC 2.5.1). Not in the vendored header, same as
// the two above -- declared here for the same reason.
//
// THIS IS THE ONE. An iPhone in the field mounted the share, listed it perfectly, and
// showed a padlock: read-only, decided at mount, with not one write ever
// attempted (diag15.log: 313 SMB lines, every create carrying a read-only
// access mask, zero set_info, zero write). Everything that could have carried
// that verdict had been checked and was correct -- tree_connect maximal_access,
// the device characteristics, the file attributes, and MxAc, which v68 answered
// without changing anything.
//
// iOS 18 added a requirement that the server support NTFS alternate data
// streams, and marks the share read-only when it does not. The widely-reported
// fix for exactly this symptom on Samba is `vfs objects = streams_xattr`, whose
// whole job is to provide named streams (it stores them in POSIX xattrs) -- and
// what that changes on the wire is this attribute bit plus real answers to
// FileStreamInformation. Both halves are done here.
//
// The evidence lines up with the device's own log: FileFsAttributeInformation is
// queried EXACTLY ONCE per mount, on "/", at 630153 -- the moment the verdict is
// formed -- and FileStreamInformation (class 22) was asked for a file at 636255
// and refused. This file's own comment on that refusal, written four tasks ago,
// says "if a real client is ever seen asking for it, the DiagLog line below is
// what will say so". It said so.
constexpr uint32_t kFileNamedStreams = 0x00040000;
// Deliberately NOT FILE_CASE_SENSITIVE_SEARCH: FAT/exFAT are case-insensitive,
// and claiming otherwise would make a client treat "Book.epub" and "book.epub"
// as two files that the card cannot actually hold at once.

// ---------------------------------------------------------------------------
// FS_SIZE_INFORMATION capacity -- A DELIBERATE PLACEHOLDER, flagged here
// because it is the one field in this file that is not a fact about the card.
//
// HalStorage exposes no capacity accessor at all. The real numbers exist one
// layer down (SDCardManager::sdTotalBytes() / sdUsedBytes(),
// freeink-sdk/libs/hardware/SDCardManager/include/SDCardManager.h:34-40), but
// reaching them means either calling SDCardManager directly -- forbidden, it
// bypasses HalStorage's mutex, which is the whole point of the HAL -- or
// adding HAL methods, which is outside this task's file scope and needs its own
// stub. And sdUsedBytes() is not free: it calls freeClusterCount(), a full FAT
// scan (its own comment says "too slow to call on every frame"), and macOS/iOS
// query FS_SIZE on essentially every folder they open, so wiring it in
// naively would park a multi-second scan inside a network callback.
//
// Direction of the lie is chosen, not accidental: reporting zero free space
// makes iOS refuse to copy anything at all (a hard block on the feature),
// whereas reporting more than exists lets normal book-sized transfers work and
// fails only at the point a genuinely full card would fail anyway.
constexpr uint32_t kNominalBytesPerSector = 512;
constexpr uint32_t kNominalSectorsPerCluster = 64;              // 32 KiB clusters, typical for a large FAT32 card
constexpr uint64_t kNominalTotalClusters = 8ull * 1024 * 1024 * 1024 / (512 * 64);  // 8 GiB
constexpr uint64_t kNominalFreeClusters = kNominalTotalClusters / 2;               // 4 GiB

// The FileNameInformation embedded in FILE_ALL_INFORMATION is share-relative
// and backslash-separated (MS-FSCC 2.4.2). Our stored paths are already
// share-relative with a leading '/', so this is a separator swap and nothing
// more.
const char* smbNameFromLocalPath(const char* localPath, char* out, size_t outSize) {
  size_t i = 0;
  for (; localPath[i] != '\0' && i + 1 < outSize; ++i) {
    out[i] = (localPath[i] == '/') ? '\\' : localPath[i];
  }
  out[i] = '\0';
  return out;
}

int queryInfoCmd(smb2_server*, smb2_context* smb2, smb2_query_info_request* req, smb2_query_info_reply* rep) {
  if (req == nullptr || rep == nullptr) return -1;

  OpenFileEntry* slot = findOpenFile(smb2, req->file_id);
  if (slot == nullptr) {
    DiagLog::line("SMB query_info reject: no such handle for ctx=%p type=%u class=%u", (void*)smb2,
                  (unsigned)req->info_type, (unsigned)req->file_info_class);
    return -1;
  }

  trace("query_info type=%u class=%u path=%s", (unsigned)req->info_type,
        (unsigned)req->file_info_class, slot->path);

  const bool isDirectory = slot->isDirectory;
  const uint32_t attributes = isDirectory ? SMB2_FILE_ATTRIBUTE_DIRECTORY : SMB2_FILE_ATTRIBUTE_NORMAL;
  // v74: a null stream has no HalFile at all, so neither fileSize64() nor
  // modifyTimeOf() below may be called on it. It reports as a zero-length file
  // with no timestamps, which is exactly what it is.
  const uint64_t endOfFile = (isDirectory || slot->isNullStream) ? 0 : slot->file.fileSize64();
  // Read once, before the switch: three of the classes below need it, and it
  // costs an SD directory-entry read. The FILESYSTEM classes never touch it,
  // and paying for it on their behalf would put an SD read behind the
  // FS_SIZE probe macOS/iOS send for essentially every folder they open.
  const struct smb2_timeval mtime =
      (req->info_type == SMB2_0_INFO_FILE && !slot->isNullStream)
          ? modifyTimeOf(slot->file, slot->path, /*logFailure=*/true)
          : kNoTimestamp;

  switch (req->info_type) {
    case SMB2_0_INFO_FILE:
      switch (req->file_info_class) {
        case SMB2_FILE_BASIC_INFORMATION:
          memset(&gFileBasicInfo, 0, sizeof(gFileBasicInfo));
          gFileBasicInfo.creation_time = mtime;
          gFileBasicInfo.last_access_time = mtime;
          gFileBasicInfo.last_write_time = mtime;
          gFileBasicInfo.change_time = mtime;
          gFileBasicInfo.file_attributes = attributes;
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFileBasicInfo);
          rep->output_buffer_length = 40;
          return 0;

        case SMB2_FILE_STANDARD_INFORMATION:
          memset(&gFileStandardInfo, 0, sizeof(gFileStandardInfo));
          gFileStandardInfo.allocation_size = endOfFile;
          gFileStandardInfo.end_of_file = endOfFile;
          gFileStandardInfo.number_of_links = 1;
          gFileStandardInfo.delete_pending = 0;
          gFileStandardInfo.directory = isDirectory ? 1 : 0;
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFileStandardInfo);
          rep->output_buffer_length = 24;
          return 0;

        case SMB2_FILE_NETWORK_OPEN_INFORMATION:
          memset(&gFileNetworkOpenInfo, 0, sizeof(gFileNetworkOpenInfo));
          gFileNetworkOpenInfo.creation_time = mtime;
          gFileNetworkOpenInfo.last_access_time = mtime;
          gFileNetworkOpenInfo.last_write_time = mtime;
          gFileNetworkOpenInfo.change_time = mtime;
          gFileNetworkOpenInfo.allocation_size = endOfFile;
          gFileNetworkOpenInfo.end_of_file = endOfFile;
          gFileNetworkOpenInfo.file_attributes = attributes;
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFileNetworkOpenInfo);
          rep->output_buffer_length = 56;
          return 0;

        case SMB2_FILE_ALL_INFORMATION: {
          memset(&gFileAllInfo, 0, sizeof(gFileAllInfo));
          gFileAllInfo.basic.creation_time = mtime;
          gFileAllInfo.basic.last_access_time = mtime;
          gFileAllInfo.basic.last_write_time = mtime;
          gFileAllInfo.basic.change_time = mtime;
          gFileAllInfo.basic.file_attributes = attributes;
          gFileAllInfo.standard.allocation_size = endOfFile;
          gFileAllInfo.standard.end_of_file = endOfFile;
          gFileAllInfo.standard.number_of_links = 1;
          gFileAllInfo.standard.directory = isDirectory ? 1 : 0;
          // v65: the same stable identity the directory listing reported for
          // this path. Leaving it 0 is what made the Linux kernel client
          // disable server inode numbers and then fail the listing outright,
          // and it is also the source of its "bogus file nlink value 0"
          // complaint -- see pathIdentity() and childIdentity().
          gFileAllInfo.index_number = pathIdentity(slot->path);
          gFileAllInfo.access_flags = 0x001f01ff;  // FILE_ALL_ACCESS, matching tree_connect's maximal_access
          gFileAllInfo.mode = 0;                   // no FILE_*_ON_CLOSE / write-through modes
          gFileAllInfo.alignment_requirement = 0;  // FILE_BYTE_ALIGNMENT
          gFileAllInfo.name = reinterpret_cast<const uint8_t*>(
              smbNameFromLocalPath(slot->path, gInfoNameScratch, sizeof(gInfoNameScratch)));
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFileAllInfo);
          // 100-byte fixed part + the UTF-16 name. 2 * strlen over-estimates
          // the UTF-16 length (never under), and the encoder adds 1024 on top
          // before writing, so the destination cannot be short.
          rep->output_buffer_length = static_cast<uint32_t>(100 + 2 * strlen(gInfoNameScratch));
          return 0;
        }

        // v70: FILE_STREAM_INFORMATION, implemented. The comment that used to
        // stand here explained why it had been left out and ended "if a real
        // client is ever seen asking for it, the DiagLog line below is what will
        // say so". It said so -- diag15.log:636255, an iPhone asking for it by
        // name -- and refusing it is (with the attribute bit above) why that
        // iPhone treated the whole card as read-only.
        //
        // FAT has no alternate data streams, so the honest answer for a file is
        // the single unnamed data stream every file has: "::$DATA", sized to the
        // file. That is what a Windows server returns for an ordinary file, and
        // it is not a fiction -- the unnamed stream IS the file's contents.
        //
        // The encoder's three quirks, each handled rather than hoped past:
        //  * it does `fs->stream_name_length *= 2` on entry, so the field must be
        //    the UTF-8 length (7) and NOT the on-wire UTF-16 length (14), and the
        //    struct must be re-zeroed each call -- see gFileStreamInfo.
        //  * it walks an array through `fs++` and only stops when it finds a
        //    zero next_entry_offset, so a single entry MUST carry 0 there.
        //  * it does its own 8-byte padding between entries; with one entry
        //    there is none, and the returned length (24 + 14 = 38) is what the
        //    dispatcher uses for the reply, so output_buffer_length here only
        //    has to be a correct estimate, not the final figure.
        //
        // Directories fall through to the default below: MS-FSCC 2.4.40 says a
        // directory's stream list is empty, and "empty" is the one answer this
        // interface cannot express (libsmb2 maps a zero-length reply to
        // STATUS_NOT_SUPPORTED). No client has been observed asking -- iOS asked
        // only about a file -- and the log line below is what would say
        // otherwise.
        case SMB2_FILE_STREAM_INFORMATION: {
          if (isDirectory) break;  // -> default:, which logs and refuses
          memset(&gFileStreamInfo, 0, sizeof(gFileStreamInfo));
          gFileStreamInfo.next_entry_offset = 0;  // the only entry
          gFileStreamInfo.stream_name = "::$DATA";
          gFileStreamInfo.stream_name_length = 7;  // UTF-8 length; the encoder doubles it
          gFileStreamInfo.stream_size = endOfFile;
          gFileStreamInfo.stream_allocation_size = endOfFile;
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFileStreamInfo);
          rep->output_buffer_length = 24 + 2 * 7;
          return 0;
        }

        default:
          DiagLog::line("SMB query_info unsupported type=%u class=%u (0x%02x) path=%s",
                        (unsigned)req->info_type, (unsigned)req->file_info_class,
                        (unsigned)req->file_info_class, slot->path);
          return replyStatus(smb2, SMB2_QUERY_INFO, SMB2_STATUS_NOT_SUPPORTED);
      }

    case SMB2_0_INFO_FILESYSTEM:
      switch (req->file_info_class) {
        case SMB2_FILE_FS_SIZE_INFORMATION:
          memset(&gFsSizeInfo, 0, sizeof(gFsSizeInfo));
          // PLACEHOLDER CAPACITY -- see kNominal* below for why this is not
          // read from the card.
          gFsSizeInfo.bytes_per_sector = kNominalBytesPerSector;
          gFsSizeInfo.sectors_per_allocation_unit = kNominalSectorsPerCluster;
          gFsSizeInfo.total_allocation_units = kNominalTotalClusters;
          gFsSizeInfo.available_allocation_units = kNominalFreeClusters;
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFsSizeInfo);
          rep->output_buffer_length = 24;
          return 0;

        case SMB2_FILE_FS_FULL_SIZE_INFORMATION:
          // The same placeholder geometry as FS_SIZE above, in the shape
          // macOS/iOS usually asks for -- they commonly probe FULL_SIZE
          // alongside (or instead of) SIZE, and the whole risk posture here is
          // "iOS might fail and we cannot debug it on-device", so answering
          // one more plausible probe is cheap insurance. The only extra field
          // is the caller/actual split, which exists for per-user quotas; with
          // no quotas both are the same number.
          memset(&gFsFullSizeInfo, 0, sizeof(gFsFullSizeInfo));
          gFsFullSizeInfo.bytes_per_sector = kNominalBytesPerSector;
          gFsFullSizeInfo.sectors_per_allocation_unit = kNominalSectorsPerCluster;
          gFsFullSizeInfo.total_allocation_units = kNominalTotalClusters;
          gFsFullSizeInfo.caller_available_allocation_units = kNominalFreeClusters;
          gFsFullSizeInfo.actual_available_allocation_units = kNominalFreeClusters;
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFsFullSizeInfo);
          rep->output_buffer_length = 32;
          return 0;

        case SMB2_FILE_FS_DEVICE_INFORMATION:
          memset(&gFsDeviceInfo, 0, sizeof(gFsDeviceInfo));
          gFsDeviceInfo.device_type = FILE_DEVICE_DISK;
          gFsDeviceInfo.characteristics = FILE_REMOVABLE_MEDIA | FILE_DEVICE_IS_MOUNTED;
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFsDeviceInfo);
          rep->output_buffer_length = 8;
          return 0;

        case SMB2_FILE_FS_ATTRIBUTE_INFORMATION:
          memset(&gFsAttributeInfo, 0, sizeof(gFsAttributeInfo));
          gFsAttributeInfo.filesystem_attributes =
              kFileCasePreservedNames | kFileUnicodeOnDisk | kFileNamedStreams;
          gFsAttributeInfo.maximum_component_name_length = 255;  // FAT long filename limit
          // Must never be null: smb2_encode_file_fs_attribute_info()
          // (smb2-data-filesystem-info.c:235) passes it straight to
          // smb2_utf8_to_utf16() with no null check, and that dereferences it.
          gFsAttributeInfo.filesystem_name = reinterpret_cast<const uint8_t*>("FAT32");
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFsAttributeInfo);
          rep->output_buffer_length = 12 + 2 * 5;
          return 0;

        case SMB2_FILE_FS_VOLUME_INFORMATION:
          memset(&gFsVolumeInfo, 0, sizeof(gFsVolumeInfo));
          // Fixed serial: clients key their metadata caches on it, so it has to
          // be the same on every query and across reconnects.
          gFsVolumeInfo.volume_serial_number = 0x43524f53;  // 'CROS'
          gFsVolumeInfo.supports_objects = 0;
          // Same null-dereference hazard as filesystem_name above
          // (smb2-data-filesystem-info.c:107).
          //
          // v65: "SD" (two UTF-16 units, a 22-byte reply) was rejected outright
          // by an independent client -- measured: smbclient's `volume` returned
          // NT_STATUS_INVALID_NETWORK_RESPONSE, i.e. it discarded the whole
          // response as malformed rather than just ignoring the label. Samba's
          // parser wants more than that minimum. The label is cosmetic to us
          // (the SHARE is what a client displays, and that is still "SD"), so
          // the cheap fix is to send a longer one. The serial number must stay
          // as it is for the reason the comment above gives.
          gFsVolumeInfo.volume_label = reinterpret_cast<const uint8_t*>("CROSSMOSA");
          rep->output_buffer = reinterpret_cast<uint8_t*>(&gFsVolumeInfo);
          rep->output_buffer_length = 18 + 2 * 9;
          return 0;

        default:
          DiagLog::line("SMB query_info unsupported type=%u class=%u (0x%02x) filesystem",
                        (unsigned)req->info_type, (unsigned)req->file_info_class,
                        (unsigned)req->file_info_class);
          return -1;
      }

    default:
      // SMB2_0_INFO_SECURITY / SMB2_0_INFO_QUOTA and anything else. This device
      // has no serial port, so this line is the only evidence that will exist
      // if a real iPhone asks for something we do not answer.
      DiagLog::line("SMB query_info unsupported type=%u class=%u (0x%02x)", (unsigned)req->info_type,
                    (unsigned)req->file_info_class, (unsigned)req->file_info_class);
      return -1;
  }
}

// ---------------------------------------------------------------------------
// Task 6: read / write / flush -- the actual file transfer.
//
// THE FAILURE MODE THIS CODE IS WRITTEN AGAINST is not a crash and not an
// error reply: it is a transfer that reports success and lands the wrong
// bytes. Every path below therefore either does exactly what was asked or
// fails and says why; nothing here ever reports a partial success, and
// nothing ever proceeds after a seek whose result it did not check.
//
// THREE FACTS ABOUT THE STORAGE LAYER shape all three handlers. All read from
// SdFat rather than assumed, and all three differ from POSIX -- which is why
// test/host/stub_hal/HalStorage.cpp had to be taught the third one in this
// same commit (see its seek64()):
//
//   1. read() returns the byte count, already clamped to the end of the file,
//      or -1 on error (FatFile.cpp:780-784 clamps, :877-879 returns -1). So a
//      short read can only mean an I/O error once the caller has done its own
//      end-of-file arithmetic.
//   2. write() returns the FULL count or 0. There is no partial return value
//      (FatFile.cpp:1499 returns nbyte, :1501-1504 returns 0 on any failure).
//      So "written != length" is unambiguous: it is a failure, never a short
//      write to be resumed.
//   3. seekSet() REFUSES to move past the end of a file -- FatFile.cpp:
//      1184-1188 and ExFatFile.cpp:715-719 both `goto fail` for
//      pos > fileSize. POSIX lseek() allows it and makes a sparse hole; SdFat
//      has no such mechanism. And a failed seek leaves the position where it
//      was, so ignoring the return value would write the client's bytes at
//      whatever offset the previous request left behind. That is the exact
//      silent-corruption shape this task exists to avoid.
//
// REPLY OWNERSHIP (read): rep->data is handed to libsmb2, which registers it
// in the outgoing iovector with `free` as the release function
// (smb2-cmd-read.c:200) and calls that when the PDU is freed after sending.
// So it MUST come from malloc() -- not new[] (mismatched deallocation), not a
// static buffer (free() on a static is undefined behaviour), and it must NOT
// be freed here on the success path. This is the "a C API takes ownership"
// exception CLAUDE.md's heap rules name explicitly; every failure path after
// the allocation frees it, since libsmb2 never sees it in that case.

// Zero-fill budget for a write that starts past the current end of file.
//
// MS-SMB2 3.3.5.13 has such a write extend the file with the hole reading as
// zeros. Fact 3 above means SdFat cannot express that at all, so the hole has
// to be written out by hand -- and that is real SD time spent inside a network
// callback that is supposed to be non-blocking (Task 3's whole design).
//
// ⚠️ v80: THE OLD VALUE WAS 64 KiB, AND ITS JUSTIFICATION WAS FALSE.
//
// It read: "enough for a client that reorders its requests by a chunk or two,
// which is the realistic case". diag27 is the real client. Copying a 5,218,624
// byte file from the iOS Files app, within 168 ms:
//
//   139253  write first: len=4096 offset=0
//   139300  write reject: 1044480-byte hole ... offset=1048576
//   139341  write reject: 2093056-byte hole ... offset=2097152
//   139380  write reject: 3141632-byte hole ... offset=3145728
//   139421  write reject: 4190208-byte hole ... offset=4194304
//
// **iOS splits a large copy into parallel streams, one per megabyte**, and
// starts them all at once. It sends no FILE_END_OF_FILE first -- ten logs
// contain no such line -- so the server learns the length only by being asked
// to write near the end of it. Four of the five writes were refused and the
// copy hung; iOS deleted the partial file 25 s later. This is the sixth time in
// this project that a "real clients do not do that" comment has been
// contradicted by the actual client.
//
// The new value is the largest hole actually observed (4,190,208) plus the old
// budget as margin. Deliberately not a round number: it is a measurement, and
// if a future log shows a wider span this is the one constant to raise -- at a
// cost of roughly 2.5 s of frozen UI per extra MiB.
constexpr uint64_t kMaxWriteGapBytes = 4 * 1024 * 1024 + 64 * 1024;

// Flash-resident zero source (constexpr => .rodata, no RAM, no allocation).
//
// v80: 512 -> 4096, and this had to land BEFORE the budget above. At 512 B
// every chunk takes SdFat's single-sector CMD24 path (FatFile.cpp:1479-1485);
// at two sectors or more it takes the multi-sector CMD25 path (:1466-1477).
// Filling the 4,177,920 bytes iOS's stride implies is 8,160 sectors: roughly
// 20-25 s of blocked UI on the single-sector path against 8-11 s on the other,
// and there is NO watchdog to escape either (no task is subscribed to the
// TWDT). The first-chunk alignment loop in extendWithZeros still works: it
// aligns to a 4,096 boundary, which is also a 512 boundary, and eight sectors
// from there always fall inside one cluster.
//
// Cost is 3,584 bytes of flash and zero RAM -- it is constexpr, so .rodata.
constexpr size_t kZeroFillChunkBytes = 4096;
constexpr uint8_t kZeroFillChunk[kZeroFillChunkBytes] = {};

// Extends `file` from `from` to `to` with zeros. Caller has already bounded
// (to - from) by kMaxWriteGapBytes.
//
// The FIRST chunk is short -- only as far as the next sector boundary -- so
// every chunk after it starts sector-aligned. That is not cosmetic: SdFat
// takes CACHE_RESERVE_FOR_WRITE only when `sectorOffset == 0` and the position
// is at or past the end of file (FatFile.cpp:1443-1446), and this fill is
// always at the end of file, so aligned chunks skip the read half of a
// read-modify-write. An unaligned chunk straddles two sectors and touches
// both. (The obvious loop -- take a full 512 first and leave the remainder for
// last -- inherits `from`'s misalignment and never corrects it, so every chunk
// straddles. Byte-for-byte identical output either way; this one just costs
// less.)
bool extendWithZeros(HalFile& file, uint64_t from, uint64_t to) {
  if (!file.seek64(from)) return false;
  uint64_t pos = from;
  while (pos < to) {
    uint64_t chunk = kZeroFillChunkBytes - (pos % kZeroFillChunkBytes);
    if (chunk > to - pos) chunk = to - pos;
    if (file.write(kZeroFillChunk, static_cast<size_t>(chunk)) != chunk) return false;
    pos += chunk;
  }
  return true;
}

int readCmd(smb2_server*, smb2_context* smb2, smb2_read_request* req, smb2_read_reply* rep) {
  if (req == nullptr || rep == nullptr) return -1;

  // Owner-scoped, like every other lookup here -- file ids are a small
  // monotonic counter plus a slot index, so an id-only match would let any
  // connection read any file another connection has open.
  OpenFileEntry* slot = findOpenFile(smb2, req->file_id);
  if (slot == nullptr) {
    if (!isExpectedCompoundCascade(smb2, req->file_id)) {
      DiagLog::line("SMB read reject: no such handle for ctx=%p id=%s offset=%llu len=%u", (void*)smb2,
                    fileIdBrief(req->file_id), (unsigned long long)req->offset, (unsigned)req->length);
    }
    return -1;
  }
  if (slot->isNullStream) {
    // Nothing was ever stored, so the stream is empty. END_OF_FILE rather than
    // a zero-byte success, for the same reason the real read path uses it.
    return replyStatus(smb2, SMB2_READ, SMB2_STATUS_END_OF_FILE);
  }
  if (slot->isDirectory || !slot->file) {
    DiagLog::line("SMB read reject: handle is not an open file path=%s offset=%llu len=%u", slot->path,
                  (unsigned long long)req->offset, (unsigned)req->length);
    return -1;
  }

  // Defence in depth: smb2_process_read_request_fixed() (smb2-cmd-read.c:
  // 360-365) already rejects an over-long read before this handler is reached.
  // Kept because the allocation below is sized from this number, and a limit
  // that is enforced in exactly one place, in someone else's code, is a limit
  // that silently disappears the day that code is updated.
  const uint32_t maxRead = smb2_get_max_read_size(smb2);
  if (maxRead != 0 && req->length > maxRead) {
    DiagLog::line("SMB read reject: len=%u exceeds max_read_size=%u offset=%llu path=%s", (unsigned)req->length,
                  (unsigned)maxRead, (unsigned long long)req->offset, slot->path);
    return -1;
  }

  if (req->length == 0) {
    rep->data = nullptr;
    rep->data_length = 0;
    rep->data_remaining = 0;
    return 0;
  }

  const uint64_t fileSize = slot->file.fileSize64();
  if (req->offset >= fileSize) {
    // END OF FILE, and as of v65 we can finally SAY so. MS-SMB2 3.3.5.12 wants
    // STATUS_END_OF_FILE for a read starting at or beyond the end; v64 could not
    // produce a specific status at all and answered a successful zero-byte read
    // instead, on the reasoning that every sequential reader understands that as
    // the end (it is what POSIX read() does) and it cannot corrupt anything.
    //
    // That reasoning was sound but the cost was not visible until it was
    // measured: a zero-byte SUCCESS is not an END, so a client that trusts
    // max_read_size keeps walking. Reading an 11-byte file through the Linux
    // kernel client took 64 READ round trips -- one for the data and 63 asking
    // for offsets from 32,768 up to 1,015,819 -- and one 100 KB copy left 123
    // "read at/after EOF" lines in diag.log. replyStatus() removes the
    // constraint that forced the choice.
    //
    // Note this is NOT the ordinary last-chunk case: a read that STRADDLES the
    // end is clamped below and returns the bytes that exist.
    DiagLog::line("SMB read at/after EOF: offset=%llu size=%llu len=%u path=%s",
                  (unsigned long long)req->offset, (unsigned long long)fileSize, (unsigned)req->length,
                  slot->path);
    return replyStatus(smb2, SMB2_READ, SMB2_STATUS_END_OF_FILE);
  }

  const uint64_t available = fileSize - req->offset;
  uint32_t want = req->length;
  if (available < static_cast<uint64_t>(want)) want = static_cast<uint32_t>(available);

  // malloc(), deliberately -- see the REPLY OWNERSHIP note above. Sized to
  // what will actually be returned, not to what was asked for, so a tail read
  // costs the tail rather than a full 32 KB block on a device where large
  // contiguous allocations are the scarce resource (CLAUDE.md hard limit 6).
  //
  // v69: HALVE AND RETRY rather than fail the read.
  //
  // diag15.log, twice, on the sleep wallpapers:
  //   SMB alloc FAILED site=read_reply bytes=16384 attempt=1 failed=1 largest=16384
  //   SMB read reject: OOM 16384 bytes offset=0 path=/sleep/..._bouguereau.bmp
  // Note the numbers: the request is EXACTLY the largest free block. That is not
  // really "out of memory" -- CLAUDE.md hard limit 6 spells it out, TLSF rounds
  // an allocation up to its size class and adds a header, so asking for exactly
  // the largest free block ALWAYS fails. v68 turned that into a failed READ,
  // which makes the file uncopyable even though there is plenty of room for
  // half of it.
  //
  // A short read is legal and ordinary: MS-SMB2 2.2.20 lets the server return
  // fewer bytes than were asked for, and every client then asks for the rest.
  // So step down by halves until the allocator is comfortable. The floor keeps
  // a pathologically fragmented heap from turning one file into thousands of
  // round trips -- below it, failing honestly is better than crawling.
  constexpr uint32_t kMinReadChunk = 2048;
  const uint32_t requested = want;
  uint8_t* buf = nullptr;
  for (;;) {
    buf = static_cast<uint8_t*>(malloc(want));
    if (buf != nullptr || want <= kMinReadChunk) break;
    want /= 2;
  }
  // Task 8: the one >= 8 KB allocation site in this file. See diagBigAlloc().
  // Recorded at the size that was actually taken, so the counters describe what
  // happened rather than what was first attempted.
  diagBigAlloc(gReadReplyAllocStats, "read_reply", want, buf != nullptr);
  if (buf == nullptr) {
    LOG_ERR(kTag, "read: OOM %u bytes", (unsigned)want);
    DiagLog::line("SMB read reject: OOM %u bytes (from %u) offset=%llu path=%s", (unsigned)want,
                  (unsigned)requested, (unsigned long long)req->offset, slot->path);
    return -1;
  }
  if (want != requested) {
    // Worth a line: it is the difference between "the card is slow" and "the
    // heap is nearly gone", and on a device with no serial port nothing else
    // would ever say so.
    DiagLog::line("SMB read: short read %u -> %u bytes (largest free block %u) offset=%llu path=%s",
                  (unsigned)requested, (unsigned)want, smbLargestFreeBlock(),
                  (unsigned long long)req->offset, slot->path);
  }

  if (!slot->file.seek64(req->offset)) {
    free(buf);
    DiagLog::line("SMB read reject: seek to %llu failed (size=%llu) path=%s", (unsigned long long)req->offset,
                  (unsigned long long)fileSize, slot->path);
    return -1;
  }

  const int got = slot->file.read(buf, want);
  if (got < 0 || static_cast<uint32_t>(got) != want) {
    // Fact 1: the end-of-file clamp is already accounted for above, so this is
    // an I/O error, not a short read to be resumed. Reported as a failure
    // rather than as a successful partial read on purpose -- a client that
    // treats a short read as end-of-file would write a silently truncated copy,
    // which is precisely the outcome this task must not produce.
    free(buf);
    DiagLog::line("SMB read reject: short read offset=%llu wanted=%u got=%d size=%llu path=%s",
                  (unsigned long long)req->offset, (unsigned)want, got, (unsigned long long)fileSize,
                  slot->path);
    return -1;
  }

  rep->data = buf;  // ownership passes to libsmb2 here; do not free below
  rep->data_length = want;
  rep->data_remaining = 0;  // MS-SMB2 2.2.20: named pipes only
  return 0;
}

// Set by smbAllocateTables() so the headroom report below is per session, not
// per boot -- a device that stays up across several file-transfer sessions
// would otherwise report only the first.
bool gReportedWriteHeadroom = false;
void smbResetWriteHeadroomReportImpl() { gReportedWriteHeadroom = false; }

int writeCmd(smb2_server*, smb2_context* smb2, smb2_write_request* req, smb2_write_reply* rep) {
  if (req == nullptr || rep == nullptr) return -1;

  OpenFileEntry* slot = findOpenFile(smb2, req->file_id);
  if (slot == nullptr) {
    DiagLog::line("SMB write reject: no such handle for ctx=%p id=%s offset=%llu len=%u", (void*)smb2,
                  fileIdBrief(req->file_id), (unsigned long long)req->offset, (unsigned)req->length);
    return -1;
  }
  if (slot->isNullStream) {
    // v74: report the whole length as written, touch nothing. This is the point
    // of the null stream -- see createCmd's named stream branch.
    rep->count = req->length;
    rep->remaining = 0;
    return 0;
  }
  if (slot->isDirectory || !slot->file) {
    DiagLog::line("SMB write reject: handle is not an open file path=%s offset=%llu len=%u", slot->path,
                  (unsigned long long)req->offset, (unsigned)req->length);
    return -1;
  }
  // Checked here, in the handler, rather than left to the filesystem: SdFat
  // returns 0 from write() both for "this handle is not writable" and for
  // "the card failed", and on a device with no serial port the diag.log line
  // is the only thing that will ever distinguish them.
  if (!slot->writable) {
    DiagLog::line("SMB write reject: handle is read-only path=%s offset=%llu len=%u", slot->path,
                  (unsigned long long)req->offset, (unsigned)req->length);
    return -1;
  }

  // Unlike READ, the vendored library does NOT check this one against the
  // negotiated maximum (smb2_process_write_request_fixed(), smb2-cmd-write.c:
  // 246-300, reads the length and never compares it), so this is the only
  // enforcement there is.
  //
  // Worth knowing before anyone re-does the memory budget: this check runs
  // AFTER libsmb2 has already malloc'd and received req->length bytes
  // (socket.c:661-673 sizes the variable tail from what the fixed stage
  // returned). It is the right defence for the filesystem -- nothing oversized
  // reaches the card -- but it is not a defence for RAM, and it cannot be one
  // from inside a handler: by the time a handler runs, the receive is done.
  // Bounding the buffer would have to happen in socket.c, i.e. a third
  // vendored divergence.
  const uint32_t maxWrite = smb2_get_max_write_size(smb2);
  if (maxWrite != 0 && req->length > maxWrite) {
    DiagLog::line("SMB write reject: len=%u exceeds max_write_size=%u offset=%llu path=%s",
                  (unsigned)req->length, (unsigned)maxWrite, (unsigned long long)req->offset, slot->path);
    return -1;
  }

  if (req->length == 0) {
    // Legal no-op; some clients use it as a touch. Nothing to log: this is a
    // success, not a rejection.
    rep->count = 0;
    rep->remaining = 0;
    return 0;
  }
  if (req->buf == nullptr) {
    DiagLog::line("SMB write reject: null payload for len=%u offset=%llu path=%s", (unsigned)req->length,
                  (unsigned long long)req->offset, slot->path);
    return -1;
  }

  // v72: one line per file-transfer session, at the first WRITE that is really
  // going to put bytes on the card. The headroom at this exact moment -- write
  // PDU received, its variable tail and signing scratch both still allocated,
  // file handle open -- is the number that decides whether the transfer sizes
  // can go back up, and no log has ever captured it.
  //
  // v75 moved it here from the top of writeCmd, below every early return above.
  // It used to fire before them, and v74 (accept Apple's named streams, discard
  // the bytes) then put a 32-byte com.apple.FinderInfo write in front of every
  // copy -- so the one-shot latch was spent on a write that allocates almost
  // nothing. diag23 duly reported `len=32 ... 20468`, which looks like an answer
  // and is not one; v73 had already raised the transfer size on that shape of
  // reasoning. An instrument that fires on the wrong event is worse than none,
  // and every rejection above is a wrong event for the same reason.
  //
  // flags is here for the other open question: WRITE_THROUGH forces an SdFat
  // sync() per chunk (below), so quartering the chunk size quadruples the FAT
  // updates -- but only if iOS actually sets it, which nothing here knows.
  if (!gReportedWriteHeadroom) {
    gReportedWriteHeadroom = true;
    DiagLog::line("SMB write first: len=%u offset=%llu flags=0x%x largest free block %u",
                  (unsigned)req->length, (unsigned long long)req->offset, (unsigned)req->flags,
                  smbLargestFreeBlock());
  }

  const uint64_t fileSize = slot->file.fileSize64();
  if (req->offset > fileSize) {
    // A hole. See kMaxWriteGapBytes and fact 3: the seek below would fail, and
    // a handler that did not notice would write these bytes at the file's
    // current position instead.
    const uint64_t gap = req->offset - fileSize;
    if (gap > kMaxWriteGapBytes) {
      DiagLog::line("SMB write reject: %llu-byte hole exceeds the %llu-byte fill budget offset=%llu size=%llu path=%s",
                    (unsigned long long)gap, (unsigned long long)kMaxWriteGapBytes,
                    (unsigned long long)req->offset, (unsigned long long)fileSize, slot->path);
      // v80: NOT a bare -1. The dispatcher turns that into STATUS_NOT_IMPLEMENTED
      // -- "this server does not implement WRITE" -- which is a lie and is what
      // diag27 shows iOS giving up on. INSUFFICIENT_RESOURCES is the honest
      // answer and is literally true: a transient server-side limit, not a
      // malformed request. (Writing past EOF is legal; MS-SMB2 3.3.5.13 requires
      // the server to extend, so INVALID_PARAMETER would also be a lie.) This is
      // exactly the defect v72 fixed for SET_INFO and did not carry across to
      // WRITE -- the second time this project has fixed an instance and left the
      // class.
      return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    const uint32_t fillStart = smbMonotonicMs();
    if (!extendWithZeros(slot->file, fileSize, req->offset)) {
      // v80: report the size AFTER the failure, not the one captured before it.
      // A fill that dies midway leaves the file permanently longer, and the old
      // line printed the pre-fill size -- understating the damage in the only
      // telemetry this device has.
      DiagLog::line("SMB write reject: zero-fill of %llu bytes failed, size was %llu now %llu offset=%llu path=%s",
                    (unsigned long long)gap, (unsigned long long)fileSize,
                    (unsigned long long)slot->file.fileSize64(), (unsigned long long)req->offset, slot->path);
      return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INSUFFICIENT_RESOURCES);
    }
    // The elapsed time is the number that decides whether the budget can stay:
    // this whole path has never once executed on hardware (`zero-fill` appears
    // in none of the ten diag logs), so its real cost is unmeasured.
    DiagLog::line("SMB write: zero-filled %llu-byte hole from %llu to %llu in %lu ms path=%s",
                  (unsigned long long)gap, (unsigned long long)fileSize, (unsigned long long)req->offset,
                  (unsigned long)(smbMonotonicMs() - fillStart), slot->path);
  }

  if (!slot->file.seek64(req->offset)) {
    DiagLog::line("SMB write reject: seek to %llu failed (size=%llu) path=%s", (unsigned long long)req->offset,
                  (unsigned long long)fileSize, slot->path);
    return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }

  const size_t written = slot->file.write(req->buf, req->length);
  if (written != static_cast<size_t>(req->length)) {
    // Fact 2: this is all-or-nothing on both filesystems, so `written` is 0
    // here in practice. Reported as a failure, and rep->count is left alone --
    // NEVER a success carrying a smaller count. That shape is what CLAUDE.md's
    // v53 entry records as having already cost this project silent data loss
    // once, in a different subsystem; a client told "I wrote 4 KB of your
    // 32 KB" mostly just advances by 4 KB and carries on.
    DiagLog::line("SMB write FAILED: offset=%llu len=%u written=%zu size=%llu path=%s",
                  (unsigned long long)req->offset, (unsigned)req->length, written,
                  (unsigned long long)fileSize, slot->path);
    return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }

  // MS-SMB2 2.2.21: WRITE_THROUGH means the data must be on stable storage
  // BEFORE the response. Only when asked -- doing it unconditionally would put
  // a FAT update behind every 32 KB chunk.
  //
  // Its result is checked, and checked BEFORE rep->count is filled in: the
  // reply is making a durability promise, so reporting success on a failed
  // write-back is precisely the "looks like it worked, the bytes are not
  // there" outcome this whole task is written against. sync(), not flush() --
  // FsFile::flush() throws sync()'s bool away (see HalStorage.h).
  if ((req->flags & SMB2_WRITEFLAG_WRITE_THROUGH) != 0 && !slot->file.sync()) {
    DiagLog::line("SMB write reject: WRITE_THROUGH sync failed after %u bytes at offset=%llu path=%s",
                  (unsigned)req->length, (unsigned long long)req->offset, slot->path);
    return replyStatus(smb2, SMB2_WRITE, SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }

  rep->count = req->length;
  rep->remaining = 0;
  return 0;
}

int flushCmd(smb2_server*, smb2_context* smb2, smb2_flush_request* req) {
  if (req == nullptr) return -1;

  OpenFileEntry* slot = findOpenFile(smb2, req->file_id);
  if (slot == nullptr) {
    DiagLog::line("SMB flush reject: no such handle for ctx=%p id=%s", (void*)smb2,
                  fileIdBrief(req->file_id));
    return -1;
  }
  if (slot->isNullStream) return 0;  // nothing was written, so nothing is unwritten
  if (!slot->file) {
    DiagLog::line("SMB flush reject: handle has no open file path=%s", slot->path);
    return -1;
  }

  // Directory handles are NOT rejected: MS-SMB2 3.3.5.11 makes FLUSH valid on
  // any open, clients do send it on directories, and FatFile::sync() is a
  // no-op for a clean directory entry.
  //
  // sync(), not flush(). Both reach the card -- FsFile::flush() IS
  // `void flush() { sync(); }` (FsLib/FsFile.h:262) -- but flush() throws away
  // the bool that says whether the write-back worked (FsFile.h:809-810), which
  // would leave this handler reporting a success it never verified. A client
  // flushes before closing precisely to be told whether its data is safe; a
  // card that fails here would otherwise hand iOS a clean "copied", leave a
  // corrupt book behind, and put nothing in diag.log. HalFile::sync() was
  // added for this (see lib/hal/HalStorage.h).
  if (!slot->file.sync()) {
    DiagLog::line("SMB flush reject: sync failed path=%s", slot->path);
    return -1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Task 7: set_info -- rename, delete-on-close, set size.
//
// THIS HANDLER IS ONLY REACHABLE BECAUSE OF A PATCH TO THE VENDORED LIBRARY.
// Upstream's smb2_process_set_info_request_variable() refused every SET_INFO
// carrying a buffer, from a DECODE function, which tore the connection down
// rather than failing the request -- so this function's `return -1` had never
// once executed. See docs/third-party/libsmb2-vendoring.md, "The third patch",
// which is pinned by scripts/verify_libsmb2_patch.py. If that patch is ever
// dropped, everything below becomes dead code again and a client renaming a
// file loses its session.
//
// PAYLOADS ARE RAW WIRE BYTES. The patch hands over `iov->buf` unparsed (that
// is all upstream's passthrough branch ever did), so each class below decodes
// its own MS-FSCC structure. Two consequences worth stating out loud:
//   * every length is validated against req->buffer_length before any read --
//     these bytes come straight off the network;
//   * multi-byte fields are read with memcpy, never by casting the uint8_t* to
//     a wider pointer. ESP32-C3 is RISC-V and faults on unaligned wide loads
//     (CLAUDE.md's platform pitfalls). It is little-endian, so memcpy alone is
//     the whole conversion.
//
// THREE FILESYSTEM FACTS shape what is possible here, all read from the SdFat
// sources rather than assumed, and all three differ from POSIX:
//
//   1. SdFat's rename is CREATE-EXCLUSIVE. FatFile::rename() builds the new
//      directory entry with O_CREAT|O_EXCL (files) or mkdir(..., pFlag=false)
//      (directories) -- FatFile.cpp:970-984 -- so it FAILS if the target name
//      exists, where POSIX rename(2) silently replaces. MS-FSCC's
//      ReplaceIfExists therefore cannot be one call; see renameOpenHandle().
//   2. The file must be renamed THROUGH ITS OPEN HANDLE. FatVolume::rename()'s
//      own doc comment (FatVolume.h:193-195) says "The file to be renamed must
//      not be open. The directory entry may be moved and file system
//      corruption could occur if the file is accessed by a file object that
//      was opened before the rename() call." In SMB2 the handle IS open --
//      that is how the client names the source -- so Storage.rename() is
//      unusable here and HalFile::rename() (which moves the entry and updates
//      the live handle, FatFile.cpp:986-1010) is the only correct call.
//   3. truncate() CANNOT GROW a file. Both implementations are
//      `seekSet(length) && truncate()` and seekSet refuses to move past EOF
//      (FatFile.h:957 / FatFile.cpp:1184-1188). POSIX ftruncate() grows and
//      zero-fills; this does not. See setEndOfFile().

// MS-FSCC 2.4.34.2, FileRenameInformation for SMB2:
//   0  ReplaceIfExists (1)   1  Reserved (7)   8  RootDirectory (8)
//   16 FileNameLength (4)   20 FileName (UTF-16LE, FileNameLength bytes)
constexpr size_t kRenameInfoHeaderBytes = 20;
// MS-FSCC 2.4.7, FileBasicInformation: four 8-byte times then FileAttributes.
constexpr size_t kBasicInfoMinBytes = 36;
// MS-FSCC 2.4.7 again: a timestamp of 0 means "do not change this field", and
// so does an all-ones value. Both spellings are seen in the wild.
constexpr uint64_t kTimestampNoChange = 0;
constexpr uint64_t kTimestampNoChangeAllOnes = 0xFFFFFFFFFFFFFFFFULL;

// The holding name a ReplaceIfExists rename parks the destination under while
// the source takes its place. See renameOpenHandle() for the ordering and why
// it is not dot-prefixed.
//
// PER-OPERATION, not fixed. A fixed name meant an interrupted replace left
// `crossmosa-replace.tmp` behind and that leftover then blocked EVERY LATER
// REPLACE IN THAT DIRECTORY, permanently, with one diag.log line as the only
// notice. A visible leftover file the user can delete is a nuisance; a
// directory that silently stops accepting replaces is undiagnosable from the
// outside. The counter makes a leftover unable to block, and each one found is
// logged as it is stepped over.
//
// The counter is not persisted, so it restarts at 0 after a reboot -- which is
// exactly why the caller TRIES SEVERAL, rather than assuming uniqueness.
constexpr char kReplaceTempPrefix[] = "crossmosa-replace-";
constexpr char kReplaceTempSuffix[] = ".tmp";
// Longest name this can generate: prefix + 10 digits (uint32 max) + suffix.
constexpr size_t kReplaceTempMaxLen = sizeof(kReplaceTempPrefix) - 1 + 10 + sizeof(kReplaceTempSuffix);
// How many candidates to step over before giving up. Only ever >1 when a
// previous replace was interrupted (or the counter wrapped past a leftover
// after a reboot); 8 is far more leftovers than one directory should ever have
// and still bounds the work at 8 exists() calls.
constexpr unsigned kReplaceTempAttempts = 8;
uint32_t gReplaceTempCounter = 0;

// Second path buffer, live only during a ReplaceIfExists rename: that sequence
// is the one place in this file where two full paths must exist at once
// (gPathScratch holds the destination, this holds the holding name, and the
// source is in the slot's own storage). 512 B of BSS, and the reason it is
// worth paying: it is what turns the only irreversible multi-step operation
// here into one where both files are on the card under some name at every
// intermediate point. Same one-request-in-flight invariant as gPathScratch.
char gRenameTempScratch[kMaxSmbPathLen];

// memcpy, not a cast: see the banner. ESP32-C3 is little-endian, so no swap.
uint32_t readLe32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
uint64_t readLe64(const uint8_t* p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}


// FILE_RENAME_INFORMATION.
int renameOpenHandle(OpenFileEntry* slot, const uint8_t* buf, uint32_t len) {
  if (buf == nullptr || len < kRenameInfoHeaderBytes) {
    DiagLog::line("SMB set_info rename reject: payload %u bytes < %zu path=%s", (unsigned)len,
                  kRenameInfoHeaderBytes, slot->path);
    return -1;
  }

  const bool replaceIfExists = buf[0] != 0;
  // RootDirectory: MS-FSCC 2.4.34.2 requires 0 for SMB2 (the name is
  // share-relative). A non-zero value means "relative to this OTHER open
  // handle", which this server does not implement -- honouring it wrongly
  // would rename into a directory nobody asked for.
  const uint64_t rootDirectory = readLe64(buf + 8);
  if (rootDirectory != 0) {
    DiagLog::line("SMB set_info rename reject: RootDirectory=0x%llx unsupported path=%s",
                  (unsigned long long)rootDirectory, slot->path);
    return -1;
  }

  const uint32_t nameLen = readLe32(buf + 16);
  if (nameLen == 0 || (nameLen % 2) != 0 || nameLen > len - kRenameInfoHeaderBytes) {
    DiagLog::line("SMB set_info rename reject: FileNameLength=%u invalid for a %u-byte payload path=%s",
                  (unsigned)nameLen, (unsigned)len, slot->path);
    return -1;
  }
  // smb2_utf16_to_utf8() takes a uint16_t*, so the name must be 2-byte
  // aligned. It is, by construction -- socket.c malloc()s this buffer
  // (socket.c:661-673) and 20 is a multiple of 4 -- but this is a RISC-V
  // target where being wrong means a fault, not a slow path, so it is checked
  // rather than argued.
  const uint8_t* namePtr = buf + kRenameInfoHeaderBytes;
  if ((reinterpret_cast<uintptr_t>(namePtr) % 2) != 0) {
    DiagLog::line("SMB set_info rename reject: UTF-16 name is not 2-byte aligned path=%s", slot->path);
    return -1;
  }

  // gPathScratch holds the DESTINATION; the source stays in slot->path, which
  // is the slot's own storage and is not scratch. One buffer, two paths, no
  // overlap -- see gPathScratch's one-request-in-flight invariant.
  char* const newPath = gPathScratch;
  if (!smbPathFromSmb(reinterpret_cast<const uint16_t*>(namePtr), nameLen / 2, newPath, sizeof(gPathScratch))) {
    DiagLog::line("SMB set_info rename reject: cannot decode destination name path=%s", slot->path);
    return -1;
  }

  // PROTECTION APPLIES IN BOTH DIRECTIONS. Renaming a file OUT of /.crossmosa
  // exfiltrates it exactly as effectively as renaming one in: the Wi-Fi
  // credentials become readable the moment they are called something else. The
  // source can only be protected if a protected path was somehow opened, which
  // createCmd forbids -- so this is defence in depth on that half and the
  // primary check on the other.
  if (smbIsProtectedPath(slot->path)) {
    DiagLog::line("SMB set_info rename reject: SOURCE is a protected path src=%s dst=%s", slot->path, newPath);
    return -1;
  }
  if (smbIsProtectedPath(newPath)) {
    DiagLog::line("SMB set_info rename reject: DESTINATION is a protected path src=%s dst=%s", slot->path, newPath);
    return -1;
  }
  if (isShareRootPath(slot->path)) {
    DiagLog::line("SMB set_info rename reject: cannot rename the share root dst=%s", newPath);
    return -1;
  }

  // Source and destination naming the same object. Two different cases, and
  // conflating them is how a file gets deleted:
  //   * byte-identical  -> a legal no-op. Windows answers success; so do we.
  //   * differing only in case -> FAT is case-insensitive, so the destination
  //     "already exists" and IS the source. SdFat's create-exclusive rename
  //     cannot express a case-only rename at all, and the replace path below
  //     would remove the destination -- i.e. delete the very file being
  //     renamed, then fail. Refused explicitly, before any of that can run.
  if (strcasecmp(slot->path, newPath) == 0) {
    if (strcmp(slot->path, newPath) == 0) return 0;
    DiagLog::line("SMB set_info rename reject: case-only rename not possible on FAT src=%s dst=%s", slot->path,
                  newPath);
    return -1;
  }

  // v76: a handle whose entry was already deleted or moved by someone else must
  // not rename. Storage.rename() works BY PATH, so if the name has since been
  // taken by something else, this moves THAT -- an object this connection never
  // opened. Measured on the desktop harness before this check existed: B holds a
  // listing handle, A deletes the directory, a replacement appears at the name,
  // B renames, and the replacement moves.
  //
  // FatFile::rename() also has no isWritable() guard of its own (its only
  // preconditions are isFile()||isSubDir(), LFN mode and same volume), so there
  // is nothing downstream that would stop it either.
  if (slot->stale) {
    DiagLog::line("SMB set_info rename reject: stale handle, entry was removed under it src=%s", slot->path);
    return -1;
  }

  // v76: same narrowing as setDisposition, and for the same reason -- rename
  // MOVES the source entry and (on the replace path below) FREES the
  // destination one, so the hazard is identical and so is the answer: only a
  // handle that could flush stale state over an entry can corrupt it.
  //
  // Included here rather than left for later on purpose. The reported bug was a
  // folder that would not delete, but the predicate is one class, and renaming
  // a folder while iOS holds its listing handle is the same wall one step to
  // the left -- the user renamed two files in the very log that produced this
  // fix. Fixing only the instance that was reported is a mistake this project
  // has made before.
  if (const OpenFileEntry* other = otherWritebackHandleOn(slot, slot->path)) {
    DiagLog::line("SMB set_info rename reject: writable handle on source [%s] src=%s", handleBrief(other),
                  slot->path);
    return kSetInfoSharingViolation;
  }
  if (const OpenFileEntry* other = otherWritebackHandleOn(slot, newPath)) {
    DiagLog::line("SMB set_info rename reject: writable handle on destination [%s] dst=%s", handleBrief(other),
                  newPath);
    return kSetInfoSharingViolation;
  }

  // ReplaceIfExists, on a filesystem whose rename cannot replace (fact 1).
  // The destination name has to be freed before the source can take it, so
  // this is the one MULTI-STEP operation in this file -- and deletes are
  // irreversible on this device, so the steps are ordered to keep BOTH files
  // recoverable at every point:
  //
  //   1. dest -> temp   (nothing destroyed; if it fails, nothing has changed)
  //   2. src  -> dest   (if it fails, roll temp back to dest and report)
  //   3. remove temp    (if it fails, the client's operation still succeeded)
  //
  // The obvious ordering -- remove dest, then rename -- destroys the
  // destination and then has nothing to put in its place if step 2 fails.
  // Every intermediate state here has both files on the card under some name.
  //
  // A DIRECTORY destination is still refused outright: replacing a folder
  // means destroying everything under it, MS-FSCC does not ask for that, and
  // no ordering makes it recoverable.
  bool usingTemp = false;
  char* const tempPath = gRenameTempScratch;
  if (Storage.exists(newPath)) {
    if (!replaceIfExists) {
      DiagLog::line("SMB set_info rename reject: destination exists and ReplaceIfExists=0 src=%s dst=%s",
                    slot->path, newPath);
      return -1;
    }
    HalFile probe = Storage.open(newPath, O_RDONLY);
    const bool destIsDirectory = probe && probe.isDirectory();
    probe = HalFile();
    if (destIsDirectory) {
      DiagLog::line("SMB set_info rename reject: ReplaceIfExists destination is a DIRECTORY src=%s dst=%s",
                    slot->path, newPath);
      return -1;
    }

    // The holding name, in the DESTINATION'S OWN DIRECTORY -- a rename within
    // one directory is the cheapest thing FAT does, and it keeps the temporary
    // next to the file it belongs to if anything is ever left behind.
    // Deliberately NOT dot-prefixed: ProtectedPath hides dotfiles, so a
    // leftover would be invisible AND unopenable over SMB. This one shows up
    // in a listing and the user can delete it.
    const char* const lastSlash = strrchr(newPath, '/');  // never null: newPath starts with '/'
    const size_t dirLen = static_cast<size_t>(lastSlash - newPath);
    if (dirLen + 1 + kReplaceTempMaxLen > kMaxSmbPathLen) {
      DiagLog::line("SMB set_info rename reject: no room for a holding name beside dst=%s", newPath);
      return -1;
    }
    memcpy(tempPath, newPath, dirLen);
    tempPath[dirLen] = '/';

    // Step over any leftovers rather than refusing because of them -- see
    // kReplaceTempPrefix. Each one is logged as it is found, so a directory
    // slowly collecting them is visible in diag.log rather than only in a
    // listing.
    bool haveName = false;
    for (unsigned attempt = 0; attempt < kReplaceTempAttempts; ++attempt) {
      const unsigned suffix = static_cast<unsigned>(gReplaceTempCounter++);
      // snprintf into the remaining space; the bound above guarantees it fits.
      snprintf(tempPath + dirLen + 1, kMaxSmbPathLen - dirLen - 1, "%s%u%s", kReplaceTempPrefix, suffix,
               kReplaceTempSuffix);
      if (!Storage.exists(tempPath)) {
        haveName = true;
        break;
      }
      DiagLog::line("SMB set_info rename: holding name %s already exists (leftover from an interrupted "
                    "replace?), trying the next one",
                    tempPath);
    }
    if (!haveName) {
      DiagLog::line("SMB set_info rename reject: %u holding names in a row are taken beside dst=%s -- delete the "
                    "crossmosa-replace-*.tmp files there",
                    kReplaceTempAttempts, newPath);
      return -1;
    }
    if (!Storage.rename(newPath, tempPath)) {
      // Nothing has been destroyed -- the destination is still under its own
      // name and the source is untouched.
      DiagLog::line("SMB set_info rename reject: could not move destination aside dst=%s tmp=%s src=%s", newPath,
                    tempPath, slot->path);
      return -1;
    }
    usingTemp = true;
    DiagLog::line("SMB set_info rename: ReplaceIfExists moved destination aside dst=%s -> tmp=%s (src=%s)",
                  newPath, tempPath, slot->path);
  }

  // Through the OPEN HANDLE (fact 2), never Storage.rename().
  if (!slot->file.rename(newPath)) {
    // Step 2 failed. Put the destination back under its own name -- this is
    // the whole reason for the holding name. These log lines have to be exact
    // rather than merely informative: they are the only record of which files
    // exist under which names afterwards.
    if (usingTemp) {
      const bool rolledBack = Storage.rename(tempPath, newPath);
      DiagLog::line("SMB set_info rename FAILED: src=%s dst=%s rollback=%s", slot->path, newPath,
                    rolledBack ? "ok (destination restored)"
                               : "FAILED -- the destination is still under the holding name");
      if (!rolledBack) {
        DiagLog::line("SMB set_info rename: MANUAL RECOVERY NEEDED, rename %s back to %s", tempPath, newPath);
      }
    } else {
      DiagLog::line("SMB set_info rename FAILED: src=%s dst=%s (nothing had to be moved aside)", slot->path,
                    newPath);
    }
    return -1;
  }

  if (usingTemp && !Storage.remove(tempPath)) {
    // Step 3. The client's operation SUCCEEDED -- its file is at the name it
    // asked for -- so this is not a failure to report back; it is a stray file
    // to record. Returning -1 here would tell the client its rename did not
    // happen, which would be false.
    DiagLog::line("SMB set_info rename: replaced ok but the holding copy could not be removed, tmp=%s remains",
                  tempPath);
  }

  DiagLog::line("SMB set_info rename ok: %s -> %s", slot->path, newPath);
  // v76: any other handle still on the OLD name is now holding a moved entry.
  // Before slot->path is overwritten, while it still names the old path.
  // Source first, while slot->path still names it.
  markOtherHandlesStale(slot, slot->path);
  // The destination too. Belt and braces with the CURRENT predicate: the replace
  // path above refuses a directory destination outright, and a FILE handle is
  // never exempted by otherWritebackHandleOn, so no surviving handle can be
  // sitting on a destination this function freed. That is a property of two
  // other pieces of code, not of this one -- and the replace path DOES free an
  // entry, so if either of them ever relaxes, this line is what keeps the
  // consequence from being a silent write into a reissued directory slot.
  markOtherHandlesStale(slot, newPath);
  // slot->path is char[kMaxSmbPathLen] and smbPathFromSmb() already bounded
  // newPath within that size, so this cannot overrun.
  memcpy(slot->path, newPath, strlen(newPath) + 1);
  return 0;
}

// FILE_DISPOSITION_INFORMATION -- sets a FLAG consumed at close, not an
// immediate delete. MS-FSCC 2.4.11: the object is removed when the last handle
// closes, and DeletePending can be cleared again before then.
int setDisposition(OpenFileEntry* slot, const uint8_t* buf, uint32_t len) {
  if (buf == nullptr || len < 1) {
    DiagLog::line("SMB set_info disposition reject: payload %u bytes < 1 path=%s", (unsigned)len, slot->path);
    return -1;
  }
  const bool pending = buf[0] != 0;

  if (!pending) {
    slot->deletePending = false;
    DiagLog::line("SMB set_info disposition: delete-on-close CLEARED path=%s", slot->path);
    return 0;
  }

  // v76: do not ARM delete-on-close on a handle whose entry is already gone.
  // deleteOnClose() deletes BY PATH and runs after releaseSlot() has cleared the
  // slot (it must -- the file has to be closed before Storage.remove()), so the
  // flag alone cannot carry this knowledge to it. closeCmd and destructionEvent
  // capture `stale` and drop the delete as well; that covers the opposite order,
  // where the path disappears AFTER the flag is set. Both are needed.
  if (slot->stale) {
    DiagLog::line("SMB set_info disposition reject: stale handle, entry was removed path=%s", slot->path);
    return -1;
  }

  // Defence in depth: createCmd already refuses to open a protected path at
  // all, so no handle should exist for one. If that ever stops being true, a
  // delete is not the place to find out.
  if (smbIsProtectedPath(slot->path)) {
    DiagLog::line("SMB set_info disposition reject: protected path path=%s", slot->path);
    return -1;
  }
  if (isShareRootPath(slot->path)) {
    DiagLog::line("SMB set_info disposition reject: cannot delete the share root path=%s", slot->path);
    return -1;
  }
  // Same rule as rename, and for the same SdFat reason: deleting frees the
  // directory entry (and the cluster chain) that a second open FatFile is
  // still caching -- "a given file must not be opened by more than one
  // FatFile object or file corruption may occur" (FatFile.h:570-571).
  //
  // This is STRICTER than MS-FSCC 2.4.11, which defers deletion until the LAST
  // handle closes rather than refusing. Still deliberate: the deferral needs
  // DeletePending to live on the PATH rather than the handle, which inverts the
  // invariant recorded at the top of this file (a flag keyed on less than the
  // owning handle lets one connection mark another connection's file for
  // deletion), and it turns "the user pressed delete" into something that
  // happens minutes later, irreversibly, on a card holding a real library.
  //
  // ⚠️ v76 NARROWED WHAT COUNTS AS A CONFLICT, because the sentence that used
  // to be here -- "real clients delete with exactly one handle, so this is not
  // a path anything ordinary takes" -- was FALSE. diag24.log, deleting an empty
  // folder from the iOS Files app:
  //
  //   query_directory done: 2 responses, buffer 4096, path=/Test_go好
  //   set_info disposition reject: path also open on ctx=... path=/Test_go好
  //
  // iOS enumerates the folder to confirm it is empty, keeps that handle, then
  // opens a second one to delete. Perfectly ordinary. The guard now asks the
  // question SdFat actually cares about -- can the other handle FLUSH over the
  // entry? -- instead of merely whether one exists. See otherWritebackHandleOn.
  //
  // ⚠️ AND THE OLD JUSTIFICATION FOR NOT CHECKING IN setEndOfFile() IS WRONG,
  // in the opposite direction. It claimed truncation "does not move or free the
  // directory entry". FatFile::truncate() calls freeChain() (FatFile.cpp:1346),
  // and unlike remove() it leaves the entry LIVE, so sync()'s only safety net
  // (the FAT_NAME_DELETED check at FatFile.cpp:1239-1241) does not apply there
  // at all. So setEndOfFile and truncating CREATE are the two paths that really
  // can cross-link clusters, and they are the two that are unguarded.
  // DELIBERATELY NOT FIXED HERE: they sit on the copy-a-book path, which only
  // started working on hardware in v75. Separate version, separate evidence.
  if (const OpenFileEntry* other = otherWritebackHandleOn(slot, slot->path)) {
    DiagLog::line("SMB set_info disposition reject: writable handle open [%s] path=%s", handleBrief(other),
                  slot->path);
    return kSetInfoSharingViolation;
  }

  slot->deletePending = true;
  DiagLog::line("SMB set_info disposition: delete-on-close SET path=%s (directory=%d)", slot->path,
                slot->isDirectory ? 1 : 0);
  return 0;
}

// FILE_END_OF_FILE_INFORMATION -- set the file's length.
int setEndOfFile(OpenFileEntry* slot, const uint8_t* buf, uint32_t len) {
  if (buf == nullptr || len < 8) {
    DiagLog::line("SMB set_info end_of_file reject: payload %u bytes < 8 path=%s", (unsigned)len, slot->path);
    return -1;
  }
  if (slot->isDirectory) {
    DiagLog::line("SMB set_info end_of_file reject: handle is a directory path=%s", slot->path);
    return -1;
  }
  if (!slot->writable) {
    DiagLog::line("SMB set_info end_of_file reject: handle is read-only path=%s", slot->path);
    return -1;
  }

  const uint64_t wanted = readLe64(buf);
  const uint64_t current = slot->file.fileSize64();

  if (wanted == current) return 0;  // legal no-op, not worth a log line

  if (wanted < current) {
    if (!slot->file.truncate(wanted)) {
      DiagLog::line("SMB set_info end_of_file FAILED: truncate %llu -> %llu path=%s", (unsigned long long)current,
                    (unsigned long long)wanted, slot->path);
      return -1;
    }
    return 0;
  }

  // GROWING. SdFat cannot (fact 3): truncate() to a larger length returns
  // false and changes nothing, because it is seekSet-then-truncate and seekSet
  // refuses to pass EOF. POSIX ftruncate() would zero-extend, which is why the
  // desktop stub had to be taught to refuse -- otherwise the harness would
  // certify a path the X3 cannot execute.
  //
  // So growth is done the only way this device can do it: by writing the
  // zeros, with the same helper and the same 64 KiB budget write_cmd already
  // uses for a write that starts past EOF. That budget is a NON-BLOCKING
  // constraint, not a storage one -- this runs inside a network callback that
  // SmbServer::tick() expects to return promptly, and zero-filling a
  // book-sized pre-allocation would stall the e-ink UI for seconds.
  //
  // Refusing beyond the budget is the deliberate choice, and it is the safe
  // direction: a client that pre-sizes a file gets a clean per-request
  // failure and its subsequent writes still create and extend the file
  // normally. THE LOG LINE IS THE POINT -- if a real iPhone is ever seen
  // hitting it, the answer is to make the fill incremental across ticks, not
  // to raise the budget.
  const uint64_t gap = wanted - current;
  if (gap > kMaxWriteGapBytes) {
    DiagLog::line("SMB set_info end_of_file reject: growing by %llu bytes exceeds the %llu-byte fill budget "
                  "(%llu -> %llu) path=%s",
                  (unsigned long long)gap, (unsigned long long)kMaxWriteGapBytes, (unsigned long long)current,
                  (unsigned long long)wanted, slot->path);
    return -1;
  }
  if (!extendWithZeros(slot->file, current, wanted)) {
    DiagLog::line("SMB set_info end_of_file FAILED: zero-fill of %llu bytes (%llu -> %llu) path=%s",
                  (unsigned long long)gap, (unsigned long long)current, (unsigned long long)wanted, slot->path);
    return -1;
  }
  DiagLog::line("SMB set_info end_of_file: grew %llu -> %llu by zero-fill path=%s", (unsigned long long)current,
                (unsigned long long)wanted, slot->path);
  return 0;
}

// FILE_BASIC_INFORMATION -- timestamps are APPLIED, not accepted and dropped.
//
// This is the class macOS and iOS send most: having copied a file, they stamp
// it. There is exactly one honest way to answer that, and it is to write the
// stamps -- accepting the request and doing nothing is the "report unverified
// success" antipattern this project has already paid for twice. So the setter
// went into the HAL (HalFile::setTimestamp(), wrapping FatFile::timestamp())
// the same way sync(), getModifyDateTime() and truncate() did.
//
// This has NOTHING TO DO with the missing FsDateTime callback recorded in
// test/host/README.md: a setter takes its value from the client, so it needs
// no clock. The two are independent.
//
// FIELD MAPPING, and the one place FAT cannot represent what MS-FSCC asks for:
//
//   CreationTime   -> T_CREATE
//   LastAccessTime -> T_ACCESS
//   LastWriteTime  -> T_WRITE
//   ChangeTime     -> **FAT HAS NO SUCH FIELD**. It is mapped onto the same
//                     single modify stamp as LastWriteTime, which is what
//                     every FAT-backed server does and what query_info here
//                     already reports (all four fields come back as the modify
//                     time, see modifyTimeOf()). If BOTH are present and they
//                     differ, LastWriteTime wins -- it is the more specific of
//                     the two -- and the coalescing is logged. The result is
//                     self-consistent and observable: a following query_info
//                     reports exactly what was stored.
//
// A field of 0 or all-ones means "do not change" (MS-FSCC 2.4.7), so a request
// in which every field says that is a genuine no-op and succeeds without
// touching the card.
int setBasicInfo(OpenFileEntry* slot, const uint8_t* buf, uint32_t len) {
  if (buf == nullptr || len < kBasicInfoMinBytes) {
    DiagLog::line("SMB set_info basic reject: payload %u bytes < %zu path=%s", (unsigned)len, kBasicInfoMinBytes,
                  slot->path);
    return -1;
  }

  // v76: the entry this handle names was freed or moved by a delete/rename on
  // another handle. This is the one path that would still write to it --
  // FatFile::timestamp() caches the dir entry FOR WRITE and stamps the date
  // bytes itself, without the FILE_FLAG_DIR_DIRTY gate or the FAT_NAME_DELETED
  // check that make everything else on a read-only handle harmless. See
  // OpenFileEntry::stale.
  if (slot->stale) {
    DiagLog::line("SMB set_info basic reject: stale handle, entry was removed path=%s", slot->path);
    return -1;
  }

  // Attributes first: it is the cheap check, and refusing after having already
  // written timestamps would leave the request half-applied.
  const uint32_t attributes = readLe32(buf + 32);
  const uint32_t currentAttributes =
      slot->isDirectory ? SMB2_FILE_ATTRIBUTE_DIRECTORY : SMB2_FILE_ATTRIBUTE_NORMAL;
  // Bits this server does not model and therefore cannot be asked to change.
  //
  //   NORMAL  (0x80) means "no other attributes are set" -- it is the absence
  //           of flags, not a flag.
  //   ARCHIVE (0x20) is set by FAT itself on every write. query_info here
  //           never reports it, so a client cannot be asking to change it
  //           relative to anything we told it; every client sets it on a file
  //           it has just copied. Refusing it made an ordinary post-copy
  //           request fail even when every timestamp said "do not change" --
  //           found in review, and the harness could not see it because it
  //           only ever sent attributes = 0.
  //
  // Everything else (READ_ONLY, HIDDEN, SYSTEM, ...) would really change
  // behaviour, is not implemented, and is still refused with a log.
  constexpr uint32_t kUnmodelledAttributes = SMB2_FILE_ATTRIBUTE_NORMAL | SMB2_FILE_ATTRIBUTE_ARCHIVE;
  if (attributes != 0 && (attributes & ~kUnmodelledAttributes) != (currentAttributes & ~kUnmodelledAttributes)) {
    DiagLog::line("SMB set_info basic reject: FileAttributes 0x%x vs current 0x%x differ outside the bits this "
                  "server models (setting attributes is not implemented) path=%s",
                  (unsigned)attributes, (unsigned)currentAttributes, slot->path);
    return -1;
  }

  const uint64_t creation = readLe64(buf + 0);
  const uint64_t access = readLe64(buf + 8);
  const uint64_t write = readLe64(buf + 16);
  const uint64_t change = readLe64(buf + 24);
  auto wants = [](uint64_t t) { return t != kTimestampNoChange && t != kTimestampNoChangeAllOnes; };

  if (!wants(creation) && !wants(access) && !wants(write) && !wants(change)) {
    return 0;  // every field said "do not change" -- nothing to do, honestly
  }

  // ChangeTime folded onto the write stamp; see the mapping above.
  uint64_t writeFiletime = write;
  if (wants(change)) {
    if (!wants(write)) {
      writeFiletime = change;
    } else if (change != write) {
      DiagLog::line("SMB set_info basic: ChangeTime 0x%llx coalesced into LastWriteTime 0x%llx (FAT stores one "
                    "modification stamp) path=%s",
                    (unsigned long long)change, (unsigned long long)write, slot->path);
    }
  }

  // Each requested field is converted and applied separately, because SdFat's
  // setter takes ONE calendar instant plus a flag mask -- two different
  // instants cannot go in one call.
  //
  // VALIDATE EVERYTHING FIRST, THEN APPLY. This used to validate each field
  // inside the apply loop, which meant a request like
  // {LastAccessTime = valid, LastWriteTime = year 2100} wrote the access stamp
  // and THEN returned -1: a request reported as rejected but partially
  // applied. It also contradicted the invariant stated forty lines above --
  // "refusing after having already written timestamps would leave the request
  // half-applied" -- which is why the attribute check is done before any of
  // this. The comment was right and the loop below it was not.
  //
  // The second half of the damage was the log. It said "reject", and on a
  // device with no serial port diag.log is the only evidence that will ever
  // exist; a line that says "reject" for a partially-applied request ends the
  // investigation in the wrong place. A wrong log is worse than no log.
  struct Prepared {
    bool wanted;
    uint8_t flag;
    const char* name;
    FatTimestamp::Fields fields;
  };
  Prepared prepared[3] = {
      {wants(creation), T_CREATE, "CreationTime", {}},
      {wants(access), T_ACCESS, "LastAccessTime", {}},
      {wants(writeFiletime), T_WRITE, "LastWriteTime", {}},
  };
  const uint64_t filetimes[3] = {creation, access, writeFiletime};

  for (size_t i = 0; i < 3; ++i) {
    if (!prepared[i].wanted) continue;

    // FILETIME (100 ns ticks since 1601) -> Unix seconds. This is the ONE
    // place this file does the conversion itself: the library's
    // smb2_timeval_to_win() runs on the way OUT, and there is no inbound
    // counterpart on the set_info path because the payload arrives as raw
    // bytes. Sub-second precision is discarded, which FAT could not store
    // anyway (two-second resolution).
    constexpr uint64_t kFiletimeEpochDelta = 116444736000000000ULL;  // 1601-01-01 -> 1970-01-01
    // v72: A DATE FAT CANNOT HOLD IS NOT A BAD REQUEST. Skip the field, keep
    // the rest, and report success.
    //
    // This is what stopped the first real file copy. diag19.log:118304:
    //   set_info basic reject: CreationTime 0x153b281e0fb4000 predates 1970,
    //                          nothing applied path=/api
    // 0x153b281e0fb4000 decodes to 1904-01-01 -- **the Mac/HFS epoch**. That is
    // what iOS sends for a file whose source has no creation date: not a
    // corrupt value, a "there isn't one" sentinel, and Apple's zero is 1904
    // where Microsoft's is 1601. Refusing the whole request over it (and, worse,
    // having libsmb2 report that refusal as STATUS_NOT_IMPLEMENTED -- see
    // setInfoCmd) is what the iPhone gave up on: the log shows it CREATE the
    // file, open it FILE_WRITE_DATA|FILE_APPEND_DATA, and then never send a
    // single WRITE, sitting idle for 144 seconds before logging off.
    //
    // The all-or-nothing rule this replaces was introduced for a real defect --
    // a request answered "reject" after two of three stamps had already been
    // written -- and that rule still holds for everything that IS applied:
    // validation still happens entirely before the first write. What changes is
    // the definition of "valid": a field the medium cannot represent is now
    // dropped rather than treated as an error, because the alternative is
    // failing a file copy over a date. The log names every dropped field, so
    // this is not silent, and the contract is honest: "I stored every date this
    // card can hold."
    if (filetimes[i] < kFiletimeEpochDelta) {
      DiagLog::line("SMB set_info basic: %s 0x%llx predates 1970 (Mac epoch is 1904) -- field skipped path=%s",
                    prepared[i].name, (unsigned long long)filetimes[i], slot->path);
      prepared[i].wanted = false;
      continue;
    }
    const int64_t seconds = static_cast<int64_t>((filetimes[i] - kFiletimeEpochDelta) / 10000000ULL);
    if (!FatTimestamp::fromUnixSeconds(seconds, prepared[i].fields)) {
      DiagLog::line("SMB set_info basic: %s = %lld s is outside FAT's range 1980-2099 -- field skipped path=%s",
                    prepared[i].name, (long long)seconds, slot->path);
      prepared[i].wanted = false;
      continue;
    }
  }

  // Past this point every requested field is known-good, so the only way to
  // fail is the card itself. That failure IS still potentially partial -- two
  // stamps can land before a third write fails -- and there is no way to make
  // three separate directory-entry writes atomic, so the log says so rather
  // than pretending otherwise.
  for (size_t i = 0; i < 3; ++i) {
    if (!prepared[i].wanted) continue;
    const FatTimestamp::Fields& f = prepared[i].fields;
    if (!slot->file.setTimestamp(prepared[i].flag, f.year, f.month, f.day, f.hour, f.minute, f.second)) {
      DiagLog::line("SMB set_info basic FAILED: could not write %s (%04u-%02u-%02u %02u:%02u:%02u) path=%s%s"
                    " -- earlier fields in this request may already be written",
                    prepared[i].name, (unsigned)f.year, (unsigned)f.month, (unsigned)f.day, (unsigned)f.hour,
                    (unsigned)f.minute, (unsigned)f.second, slot->path,
                    // SdFat's timestamp() starts with isFileOrSubDir(), and the
                    // FAT root is neither (it carries FILE_ATTR_ROOT_FIXED /
                    // ROOT32, never FILE_ATTR_SUBDIR -- FatFile.h:454-460,
                    // :1014-1022), so it refuses outright. Naming that here
                    // saves the next reader a source dive.
                    isShareRootPath(slot->path) ? " (the share root has no directory entry to write)" : "");
      return -1;
    }
  }
  return 0;
}

int setInfoCmd(smb2_server*, smb2_context* smb2, smb2_set_info_request* req) {
  if (req == nullptr) return -1;

  // MUST BE FIRST, and it is the same upstream hazard Task 5 found in
  // query_directory. smb2_process_set_info_request_fixed()
  // (smb2-cmd-set-info.c:355-394) malloc()s the request struct and assigns
  // every field EXCEPT input_data; input_data is only ever written by the
  // _variable stage, which socket.c skips entirely when buffer_length == 0
  // (socket.c:640-676). So a zero-length SET_INFO leaves input_data holding
  // whatever the recycled heap block held, and every decoder below would
  // dereference it. Nothing in the library frees it (checked: no free of
  // input_data anywhere outside this handler's reach), so normalising it here
  // is sufficient and costs one compare.
  if (req->buffer_length == 0) req->input_data = nullptr;

  OpenFileEntry* slot = findOpenFile(smb2, req->file_id);
  if (slot == nullptr) {
    DiagLog::line("SMB set_info reject: no such handle for ctx=%p id=%s type=%u class=%u", (void*)smb2,
                  fileIdBrief(req->file_id), (unsigned)req->info_type, (unsigned)req->file_info_class);
    // v72 (round 2): FILE_CLOSED, the same status close_cmd has answered for an
    // invalid FileId since v65 and what MS-SMB2 prescribes. This path was left
    // on the bare -1 when the class path below was upgraded -- the fix named
    // one instance of the defect and missed its three siblings, which is the
    // pattern this project already has a name for. Caught in review.
    return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_FILE_CLOSED);
  }
  if (slot->isNullStream) {
    // v74: timestamps and sizes on a discarded stream are no-ops that succeed;
    // rename and delete-on-close are refused, because those are the two that
    // would otherwise be asked to act on a path with a colon in it -- and a
    // client marking the STREAM for deletion must never take the file with it.
    const bool harmless = req->info_type == SMB2_0_INFO_FILE &&
                          (req->file_info_class == SMB2_FILE_BASIC_INFORMATION ||
                           req->file_info_class == SMB2_FILE_END_OF_FILE_INFORMATION);
    if (harmless) return 0;
    DiagLog::line("SMB set_info reject: not supported on a discarded stream type=%u class=%u path=%s",
                  (unsigned)req->info_type, (unsigned)req->file_info_class, slot->path);
    return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_NOT_SUPPORTED);
  }
  if (!slot->file) {
    DiagLog::line("SMB set_info reject: handle has no open file path=%s type=%u class=%u", slot->path,
                  (unsigned)req->info_type, (unsigned)req->file_info_class);
    // The handle exists but carries nothing to act on. Same family as above.
    return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_FILE_CLOSED);
  }

  const auto* buf = static_cast<const uint8_t*>(req->input_data);
  const uint32_t len = req->buffer_length;

  trace("set_info type=%u class=%u path=%s", (unsigned)req->info_type,
        (unsigned)req->file_info_class, slot->path);
  if (req->info_type != SMB2_0_INFO_FILE) {
    // SMB2_0_INFO_FILESYSTEM / SECURITY / QUOTA. Numerically, because this
    // device has no serial port and diag.log is the only evidence that will
    // ever exist about what a real iPhone asked for.
    DiagLog::line("SMB set_info unsupported type=%u (0x%02x) class=%u (0x%02x) len=%u path=%s",
                  (unsigned)req->info_type, (unsigned)req->info_type, (unsigned)req->file_info_class,
                  (unsigned)req->file_info_class, (unsigned)len, slot->path);
    // Same reasoning as the unsupported-CLASS path at the end of this function:
    // "I do not support that information TYPE" is a far smaller claim than
    // "I do not implement SET_INFO", and only the smaller one leaves a client
    // willing to carry on.
    return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_NOT_SUPPORTED);
  }

  // v72: a handler that refuses must say WHY. Every one of these returns -1 on
  // failure, and libsmb2 turns that into STATUS_NOT_IMPLEMENTED
  // (libsmb2.c:3885) -- which does not mean "that request was wrong", it means
  // "this server does not implement SET_INFO at all". A client that believes
  // that stops trying to write metadata, and iOS appears to stop the copy with
  // it (diag19.log: the write-mode handle was open and no WRITE ever followed a
  // refused SET_INFO). STATUS_INVALID_PARAMETER is the honest answer for a
  // request this server understood and could not carry out, and it leaves the
  // client free to continue.
  {
    int rc = -1;
    switch (req->file_info_class) {
      case SMB2_FILE_RENAME_INFORMATION:
        rc = renameOpenHandle(slot, buf, len);
        break;
      case SMB2_FILE_DISPOSITION_INFORMATION:
        rc = setDisposition(slot, buf, len);
        break;
      case SMB2_FILE_END_OF_FILE_INFORMATION:
        rc = setEndOfFile(slot, buf, len);
        break;
      case SMB2_FILE_BASIC_INFORMATION:
        rc = setBasicInfo(slot, buf, len);
        break;
      default:
        rc = -2;  // not one of ours -- falls through to the class report below
        break;
    }
    if (rc >= 0) return rc;
    if (rc == kSetInfoSharingViolation) return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_SHARING_VIOLATION);
    if (rc == -1) return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
  }

  // FILE_ALLOCATION_INFORMATION (0x13), FILE_LINK_INFORMATION (0x0B),
  // FILE_POSITION_INFORMATION (0x0E), FILE_DISPOSITION_INFORMATION_EX (0x40)
  // and everything else. Reported numerically, always -- a class this server
  // has never seen has no name here to print.
  //
  // v72: STATUS_NOT_SUPPORTED, matching what query_info's default already
  // answers. "I do not support that information class" is a different and much
  // smaller claim than NOT_IMPLEMENTED's "I do not implement SET_INFO", and a
  // client that hears the smaller one keeps going.
  DiagLog::line("SMB set_info unsupported type=%u (0x%02x) class=%u (0x%02x) len=%u path=%s",
                (unsigned)req->info_type, (unsigned)req->info_type, (unsigned)req->file_info_class,
                (unsigned)req->file_info_class, (unsigned)len, slot->path);
  return replyStatus(smb2, SMB2_SET_INFO, SMB2_STATUS_NOT_SUPPORTED);
}

smb2_server_request_handlers gHandlers = {
    .destruction_event = destructionEvent,
    .authorize_user = authorizeUser,
    .session_established = sessionEstablished,
    .logoff_cmd = logoffCmd,
    .tree_connect_cmd = treeConnectCmd,
    .tree_disconnect_cmd = treeDisconnectCmd,
    .create_cmd = createCmd,
    .close_cmd = closeCmd,
    .flush_cmd = flushCmd,
    .read_cmd = readCmd,
    .write_cmd = writeCmd,
    .oplock_break_cmd = oplockBreakCmd,
    .lease_break_cmd = leaseBreakCmd,
    .lock_cmd = lockCmd,
    .ioctl_cmd = ioctlCmd,
    .cancel_cmd = cancelCmd,
    .echo_cmd = echoCmd,
    .query_directory_cmd = queryDirectoryCmd,
    .change_notify_cmd = changeNotifyCmd,
    .query_info_cmd = queryInfoCmd,
    .set_info_cmd = setInfoCmd,
};

}  // namespace

smb2_server_request_handlers* smbGetRequestHandlers() { return &gHandlers; }

bool smbAllocateTables() {
  // Per file-transfer session: begin() calls this once, so the trace covers the
  // mount and first browse of each session rather than only the first ever.
  gTraceBudget = kTraceBudget;
  smbResetWriteHeadroomReportImpl();
  if (gTables) return true;  // idempotent; begin() is too
  gTables = makeUniqueNoThrow<SmbTables>();
  if (!gTables) {
    LOG_ERR(kTag, "OOM allocating handler tables (%u bytes)", (unsigned)sizeof(SmbTables));
    // Logged to the card as well as the ring buffer: this is the one failure
    // that stops SMB from starting at all while HTTP/WebDAV carry on, so the
    // symptom a user reports ("the smb:// lines are not on the screen") has to
    // be traceable to a number.
    DiagLog::line("SMB tables OOM: %u bytes wanted, largest free block %u -- SMB will not start",
                  (unsigned)sizeof(SmbTables), smbLargestFreeBlock());
    return false;
  }
  DiagLog::line("SMB tables allocated: %u bytes, largest free block now %u", (unsigned)sizeof(SmbTables),
                smbLargestFreeBlock());
  // v76: one line, zero risk, and the only way to answer a question that is
  // otherwise unanswerable from here -- does this device know what DAY it is?
  //
  // Nothing in this firmware registers an SdFat FsDateTime callback, so every
  // file the X3 itself creates is stamped FS_DEFAULT_DATE ("1 January <compile
  // year>") and its modify time is never updated on write. Registering one is
  // the real fix, but the DS3231 helper only reads and writes hours/minutes/
  // seconds -- the device has a TIME and no DATE -- and SNTP runs once in a
  // device's life. So the honest question before that work is: at the moment
  // SMB starts, what does the wall clock actually say?
  //
  //   a plausible 2026 date -> SNTP has run this boot, or the boot time
  //                            survived in RTC scratch: a callback would work
  //   1970                   -> time() is returning uptime; a callback would
  //                            stamp every file with an obviously-broken date,
  //                            which is WORSE than today's single wrong-but-
  //                            uniform one, because the files would no longer
  //                            sort together in the Files app
  {
    const time_t now = time(nullptr);
    struct tm utc {};
    gmtime_r(&now, &utc);
    // v77: cb= says whether SdFat's date/time callback was live for THIS
    // session, so one line answers both "did the device know the date" and
    // "did that reach the card".
    DiagLog::line("SMB clock at start: epoch=%lld -> %04d-%02d-%02d %02d:%02d:%02d UTC cb=%d", (long long)now,
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                  SdDateTime::isRegistered() ? 1 : 0);
  }
  gOpenFiles = gTables->openFiles;
  gDirEntries = gTables->dirEntries;
  gDirNameArena = gTables->dirNameArena;
  return true;
}

void smbReleaseTables() {
  if (!gTables) return;
  // Null the aliases FIRST: ~SmbTables runs ~OpenFileEntry, which closes (and
  // syncs) any HalFile a client left open, and nothing may observe a dangling
  // alias in between.
  gOpenFiles = nullptr;
  gDirEntries = nullptr;
  gDirNameArena = nullptr;
  gTables.reset();
  DiagLog::line("SMB tables released: %u bytes, largest free block now %u", (unsigned)sizeof(SmbTables),
                smbLargestFreeBlock());
}
