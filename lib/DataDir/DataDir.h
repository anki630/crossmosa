#pragma once

#include <stdint.h>

// Runtime-resolved SD data directory (v36).
//
// CrossMosa renames the legacy CrossPoint data dir /.crosspoint to
// /.crossmosa once at boot. A FAT directory rename is metadata-only:
// instant, and every content survives in place (epub_<hash> caches,
// progress.bin, bookmarks/, wifi.json, recent.json, opds.json, settings).
// The epub cache hash is computed from the book path — not the cache dir —
// so no cache is invalidated and no progress is lost.
//
// If the rename fails (unhealthy card), the legacy dir keeps being used for
// this session — zero data loss — and the migration retries next boot.
namespace DataDir {

// One-shot migration + resolution. Call after Storage.begin() succeeds and
// BEFORE any store/settings load. Call EXACTLY ONCE: every consumer bakes
// path() into a lazy static buffer on first use, so a later resolve() that
// changed activeDir would leave them on the old string (a second call is a
// no-op by design — it keeps the first decision).
//
// Concurrency note: consumers cache "<path()>/<file>" in lazy static char
// buffers guarded by a plain p[0] check (no magic-statics ordering). That is
// safe because every store's FIRST use happens single-threaded during boot
// (main.cpp setup loads them right after resolve()); keep it that way when
// adding consumers reachable from other FreeRTOS tasks.
void resolve();

// Base data dir without trailing slash, e.g. "/.crossmosa" (or
// "/.crosspoint" after a failed migration). Stable for the whole session.
const char* path();

// v186: what resolve() decided, for the diag.log witness (X3 has no serial —
// LOG_* alone would drop the single most important SD-format decision).
enum class Outcome : uint8_t {
  Unresolved,        // resolve() not run
  Fresh,             // neither dir: new dir created lazily by the first save
  AlreadyNew,        // only /.crossmosa (real dir)
  Migrated,          // renamed /.crosspoint -> /.crossmosa this boot
  MigrationFailed,   // rename failed: staying on /.crosspoint this session
  StubRemoved,       // a failed-rename stub was cleared, then handled as above
  StubBlocked,       // stub could not be cleared: staying on /.crosspoint
  StubBlockedNew,    // only /.crossmosa exists but is an unremovable non-dir stub
  BothNewWins,       // both real: /.crossmosa used by NAME (see .cpp — no recency on this FS)
  LegacyEmpty,       // /.crosspoint exists but holds nothing: nothing to migrate, /.crossmosa used
};
Outcome outcome();
const char* outcomeName();

}  // namespace DataDir
