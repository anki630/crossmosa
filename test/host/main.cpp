// Desktop smoke-test harness entry point (Task 3 of the smb2-server plan,
// see .superpowers/sdd/2026-07-28-smb2-server/). Task 2's version of this
// file drove libsmb2's own blocking smb2_serve_port() directly with local
// stub handlers, specifically because SmbServer (this task) and
// SmbFileHandlers (Task 4) didn't exist yet. Both now exist, so this file
// drives them instead: real SmbServer::begin()/tick(), real
// SmbFileHandlers via smbGetRequestHandlers().
//
// See README.md's "Baseline" section for the exact, currently-expected
// client-observable behavior. This file went through an intermediate state
// during Task 3 where SmbServer could accept a connection but not finish
// bootstrapping it into libsmb2's negotiate state machine (see
// task-3-report.md for that history, now resolved by an appended function in
// lib/smb2/lib/libsmb2.c) -- that state is not what's committed here; don't
// compare a real client's behavior against it.
//
// Usage: ./smbhost [port]   (default 4450 -- unprivileged, no root needed)

#include "SmbServer.h"

#include <HalStorage.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main(int argc, char** argv) {
  uint16_t port = 4450;
  if (argc > 1) {
    port = static_cast<uint16_t>(std::atoi(argv[1]));
  }

  // Task 4: SmbFileHandlers.cpp now does real Storage.*/HalFile I/O (create/
  // close), which needs the SD root directory to exist first -- on device
  // this already happened during boot (src/main.cpp calls Storage.begin()
  // long before any network service starts); this standalone harness has no
  // such boot sequence, so it must do the equivalent itself. See
  // stub_hal/HalStorage.cpp's begin() (creates SMBHOST_ROOT, default
  // ./sdroot) and README.md's "SMBHOST_ROOT" section.
  if (!Storage.begin()) {
    std::fprintf(stderr, "[smbhost] Storage.begin() failed\n");
    return 1;
  }

  // v82: the firmware leaves SMB2 signing ADAPTIVE -- it signs only for a
  // client that requires it -- because iOS does not, and AES-CMAC over every
  // PDU in both directions costs tens of seconds on a book. See the note at the
  // absent smb2_set_sign() call in SmbServer::acceptOneConnection().
  //
  // The harness defaults to the same thing, so the suite tests what the device
  // does. SMBHOST_SIGN=1 forces signing on, which is what the two INDEPENDENT
  // clients need: MEASURED A/B, Samba smbclient lists the share with signing on
  // and answers "tree connect failed: NT_STATUS_ACCESS_DENIED" without it.
  // Keeping them usable is why this switch exists -- they are how this project
  // avoids testing libsmb2 with libsmb2.
  const char* forceSign = std::getenv("SMBHOST_SIGN");
  const bool signOn = forceSign != nullptr && forceSign[0] == '1';
  SmbServer::setForceSigning(signOn);
  std::fprintf(stderr, "[smbhost] SMB2 signing: %s\n",
               signOn ? "FORCED ON (SMBHOST_SIGN=1)" : "adaptive, as on the device");

  SmbServer server(port);
  if (!server.begin()) {
    std::fprintf(stderr, "[smbhost] begin() failed\n");
    return 1;
  }

  std::fprintf(stderr, "[smbhost] listening on 127.0.0.1:%u (SmbServer + stub SmbFileHandlers)\n", port);

  // No FreeRTOS task on the device -- tick() is driven from the single
  // activity loop, once per iteration. usleep() here just stands in for
  // "whatever else the loop is doing this iteration" so this harness process
  // doesn't spin at 100% CPU; it isn't part of what's being tested.
  for (;;) {
    server.tick();
    usleep(5000);
  }
}
