#pragma once

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
// BEFORE any store/settings load. Safe to call again (no-op).
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

}  // namespace DataDir
