#pragma once

#include <cstddef>
#include <cstdint>

struct smb2_server;
struct smb2_context;

// The one account and the one share this server accepts, plus its port.
//
// Declared here rather than as literals in SmbFileHandlers.cpp because Task 8
// puts them ON SCREEN: CrossPointWebServerActivity prints
// "smb://crossmosa.local/SD" and the credentials next to the existing http
// address. Two copies of "x3" in two files is exactly the kind of drift that
// ends with the device confidently displaying a password it no longer
// accepts, and no way to notice short of trying it.
inline constexpr char kSmbUser[] = "x3";
inline constexpr char kSmbPassword[] = "x3";
inline constexpr char kSmbShareName[] = "SD";
inline constexpr uint16_t kSmbPort = 445;

// Largest contiguous internal-heap block. THE number that predicts allocation
// failure on this device -- CLAUDE.md hard limit 6 is explicit that free total
// is not the constraint, contiguity is, and that the figure is "the largest of
// several pools that never merge". Carried on every SMB failure log line
// because it is the only cheap proxy for how close the path was to the edge,
// and because libsmb2's own allocation failures surface as strings from
// smb2_get_error() with no size attached.
//
// Returns 0 on the desktop harness, which has no such concept.
unsigned smbLargestFreeBlock();

// Non-blocking SMB2 server. Only alive while in file-transfer mode. All file
// operations are delegated to SmbFileHandlers (this class knows nothing
// about the filesystem).
//
// Interface deliberately mirrors the existing CrossPointWebServer
// (begin / handleClient / stop / isRunning) -- see that class for the
// project's established shape for a periodically-ticked network service.
// There is no FreeRTOS task here: this firmware is single-core and the one
// activity loop also drives the e-ink display, so tick() must return quickly
// every time it's called. Precisely: the client-service path uses a
// zero-timeout select() and returns immediately, while a PENDING CONNECTION
// costs up to 10 ms inside smb2_serve_port_async(to_msecs=10). "Never blocks"
// is true of the common case; "<= 10 ms" is the honest bound.
//
// The portion of the driver loop that hands a freshly-accepted connection
// off to libsmb2's negotiate state machine needs two things
// (`struct connect_data`, `smb2_negotiate_request_cb`) that have zero
// visibility outside lib/smb2/lib/libsmb2.c -- not implementable from this
// file using only libsmb2's public (or even private) headers. Resolved via
// a small, deliberately-scoped function appended to the end of that vendored
// file (`crossmosa_smb2_finish_accept()`, declared in `CrossPointSmb2.h`,
// called from `acceptOneConnection()` in SmbServer.cpp) -- see that
// function's own banner comment and
// docs/third-party/libsmb2-vendoring.md for the full rationale, and
// task-3-report.md for the history of why this was necessary.
class SmbServer {
 public:
  explicit SmbServer(uint16_t port = kSmbPort);
  ~SmbServer();
  SmbServer(const SmbServer&) = delete;
  SmbServer& operator=(const SmbServer&) = delete;

  // v82: force SMB2 message signing on for every connection.
  //
  // The device does NOT call this. Signing is adaptive there -- see the long
  // note at the (deliberately absent) smb2_set_sign() call in
  // acceptOneConnection(): iOS does not ask for signing, and AES-CMAC over
  // every PDU in both directions costs tens of seconds on a book.
  //
  // It exists because that trade has one cost which is NOT about security, and
  // which is cheap to avoid: a client that advertises SIGNING_ENABLED and then
  // refuses an unsigned reply stops working, and Samba's smbclient is exactly
  // such a client. MEASURED, A/B on the desktop harness: signing on -> smbclient
  // lists the share; adaptive -> "tree connect failed: NT_STATUS_ACCESS_DENIED".
  // smbclient and the Linux kernel cifs client are two of the three INDEPENDENT
  // implementations this project's suite uses so that libsmb2 is not merely
  // testing itself, and every blocker found in the SMB work came through one of
  // them. Losing that would cost far more than the seconds signing saves.
  //
  // So the harness can turn signing back on (SMBHOST_SIGN=1) and keep those
  // clients usable, while the firmware stays fast. Call before begin().
  static void setForceSigning(bool on);

  bool begin();
  // Call once per activity loop iteration. Servicing connected clients returns
  // immediately (zero-timeout select()); a PENDING CONNECTION costs up to
  // 10 ms in smb2_serve_port_async(to_msecs=10). See the class banner above.
  void tick();
  void end();
  bool isRunning() const { return running_; }

 private:
  // libsmb2's own active-context list (smb2_active_contexts()) is a private,
  // singly-linked list -- walking it needs the `->next` field, which only
  // exists in lib/smb2/include/libsmb2-private.h's full struct smb2_context
  // definition (opaque in the public header). Rather than reach into that
  // private header just for list traversal, SmbServer keeps its own
  // fixed-size registry of the contexts *it* accepted -- populated only by
  // this class, so plain liveness checks via the public smb2_get_fd()
  // accessor are sufficient; no ->next access, no per-tick allocation.
  static constexpr size_t kMaxClients = 4;
  smb2_context* clients_[kMaxClients] = {};

  // Latches "registry full" so acceptOneConnection() logs that condition
  // once per occurrence instead of once per tick. select() is level-triggered
  // and a full registry never drains the pending connection, so without this
  // latch the same LOG_ERR line would repeat on every activity-loop
  // iteration for as long as the registry stays full -- see the full
  // rationale where it's checked (acceptOneConnection()) and where it's
  // cleared (cullOneDeadClient()).
  bool registryFullLogged_ = false;

  // Same latch, same reason, but scoped to ONE failure code: -EIO, and only
  // -EIO. A failing raw accept() does not drain the pending connection
  // (lib/smb2/lib/socket.c:1483-1494 reaches its `else` only when clientfd < 0,
  // so nothing was dequeued), and select() is level-triggered, so a persistent
  // failure -- lwIP socket-table exhaustion with HTTP + WebDAV + mDNS + this
  // listener + up to four contexts is the plausible route -- reports readable
  // on every tick. That was free when the report was LOG_DBG (compiled out of
  // gh_release); it is not free now that it also writes to the SD card, and an
  // unlatched SD write once per activity-loop iteration would burn the whole
  // 192 KB diag budget inside a single session, which rotation cannot help.
  //
  // Every OTHER failure code is logged unconditionally -- see the enumeration
  // at the check itself. Cleared as soon as the condition stops holding (any
  // err == 0 return, accepted connection or idle poll), the way
  // registryFullLogged_ is cleared at the point its state flips, NOT merely on
  // the next thing that happens to succeed.
  bool acceptFailLogged_ = false;

  void acceptOneConnection();
  void cullOneDeadClient();

  smb2_server* server_ = nullptr;  // allocated by makeUniqueNoThrow in begin(), released in end()
  bool running_ = false;
  uint16_t port_;
};
