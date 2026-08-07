#pragma once

// Task 4: real connection-layer handlers (authentication, tree connect,
// create/close, plus the eleven trivial-but-not-optional handlers) backed by
// HalStorage/HalFile. Tasks 5-7 replace the remaining stub bodies
// (query_directory/query_info, read/write/flush, set_info) -- see this
// file's .cpp for the per-handler breakdown and task-4-report.md for the
// verification record.
//
// Never include lib/smb2's own headers directly -- go through
// "CrossPointSmb2.h", the project's single funnel for that dependency (see
// that header's own comment for why).
#include "CrossPointSmb2.h"

struct smb2_server_request_handlers* smbGetRequestHandlers();

// The handlers' three large tables (open-file table, directory-entry table,
// directory-name arena -- 12,606 B together) live on the heap for exactly as
// long as the server does, instead of permanently in .bss.
//
// Why the lifetime matters more than the size: .bss sits immediately below
// pool p3, the ~143,728 B pool malloc hits first, and CLAUDE.md records that
// background chapter reflow already fills p3 and overflows ~65 KB into p2 --
// the path v61 and v62 were shipped to stop crashing. Leaving these resident
// would cut p3 by ~10% for the whole session, including while reading, and
// invalidate v59's measured alloc_fail=0 baseline. The SMB server lives inside
// one activity; its tables now do too.
//
// smbAllocateTables() must succeed before any handler can be reached, and
// smbReleaseTables() must not run until every smb2_context is destroyed --
// both guaranteed by the only caller, SmbServer::begin()/end(). Releasing also
// closes (and therefore syncs) any handle a client left open, which the .bss
// version never did before the reboot that follows.
bool smbAllocateTables();
void smbReleaseTables();
