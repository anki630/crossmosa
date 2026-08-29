#include "DataDir.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

namespace {
constexpr const char* NEW_DIR = "/.crossmosa";
constexpr const char* LEGACY_DIR = "/.crosspoint";
const char* activeDir = NEW_DIR;
DataDir::Outcome outcome_ = DataDir::Outcome::Unresolved;
bool stubWasRemoved_ = false;

constexpr const char* SENTINELS[] = {"state.json",   "settings.json", "recent.json", "wifi.json",
                                     "opds.json",    "state.bin",     "settings.bin"};

// ⚠️ No "which dir is newer" heuristic on purpose (v186 review)：v194 起時鐘已知
// 才會掛 FsDateTime callback，我們寫的檔才有真實時戳；但目錄誰贏仍不看 FAT 時間
// （教訓 20a：這台機器上「哪個檔比較新」是假的）。規則維持 v36：NEW 名贏、
// 輸家不動，決定寫進 diag.log。

// True when dir is a directory holding at least one entry. A failed-rename
// stub is EMPTY by construction (FAT: mkdir first; exFAT: zero-byte file), so
// any non-empty directory is real data even without a store file (e.g. a card
// that only ever got epub_<hash>/ caches before a power cut).
bool dirHasEntries(const char* dir) {
  auto d = Storage.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) d.close();
    return false;
  }
  bool any = false;
  for (auto f = d.openNextFile(); f; f = d.openNextFile()) {
    f.close();
    any = true;
    break;
  }
  d.close();
  return any;
}

// True when dir contains any store file a used data dir acquires within its
// first sessions (state/settings are saved on every sleep or settings exit;
// .bin variants cover pre-JSON stock-firmware cards). Distinguishes a real
// migrated dir from junk left behind by a failed SdFat rename.
bool looksLikeRealDataDir(const char* dir) {
  char p[44];
  for (const char* s : SENTINELS) {
    snprintf(p, sizeof(p), "%s/%s", dir, s);
    if (Storage.exists(p)) return true;
  }
  return false;
}

// Remove a junk NEW_DIR left by a failed/interrupted SdFat rename. SdFat
// creates the new name FIRST and only then moves/removes, so a failure can
// strand a stub: an EMPTY directory (or, transiently, a zero-byte file) on
// FAT, a zero-byte plain file on exFAT. Refuses to touch a non-empty
// directory (rmdir fails on non-empty by design) or a non-empty file.
// Returns true when NEW_DIR no longer exists.
bool removeJunkNewDir() {
  auto f = Storage.open(NEW_DIR);
  if (!f) {
    return !Storage.exists(NEW_DIR);
  }
  const bool isDir = f.isDirectory();
  const bool emptyFile = !isDir && f.fileSize() == 0;
  f.close();
  if (isDir) return Storage.rmdir(NEW_DIR);
  return emptyFile ? Storage.remove(NEW_DIR) : false;
}
}  // namespace

namespace DataDir {

void resolve() {
  if (outcome_ != Outcome::Unresolved) return;  // exactly once (see header)
  const bool legacyExists = Storage.exists(LEGACY_DIR);
  bool newExists = Storage.exists(NEW_DIR);

  if (newExists && legacyExists) {
    // v186: "real" = has a store file OR is any non-empty directory (a stub is
    // empty by construction). v36 only trusted the sentinel list, which pinned
    // a card to the legacy dir forever after "fresh card -> power cut before
    // the first save -> rollback -> back".
    const bool newReal = looksLikeRealDataDir(NEW_DIR) || dirHasEntries(NEW_DIR);
    if (newReal) {
      // Both are real data dirs. NEW wins BY NAME (no recency signal exists on
      // this FS — see the note above newestStoreStamp's grave). Cases: an
      // OTA-rollback / stock / CrossPoint session recreated /.crosspoint after
      // a migration; that session's data is shadowed, NOT deleted. Never delete
      // or rename the loser automatically — after a mid-rename power cut both
      // entries can alias the same FAT clusters, so touching one could corrupt
      // the other. Needs a PC disk check before manual cleanup; to prefer the
      // legacy data instead, remove /.crossmosa on a PC and reboot (migrates).
      LOG_ERR("MAIN", "Both %s and %s exist; using %s (data written by other-firmware sessions stays in %s — do not delete it from the device)",
              NEW_DIR, LEGACY_DIR, NEW_DIR, LEGACY_DIR);
      activeDir = NEW_DIR;
      outcome_ = Outcome::BothNewWins;
      return;
    }
    // v186 review: if LEGACY holds nothing there is nothing to migrate — and
    // clearing a NEW stub here is the ONE path that can free a cluster chain
    // shared with LEGACY (power cut inside rename of an EMPTY legacy dir
    // leaves both names on the same cluster). Use NEW as-is; stores create
    // what they need. LEGACY is left alone (harmless, empty).
    if (!dirHasEntries(LEGACY_DIR)) {
      activeDir = NEW_DIR;
      outcome_ = Outcome::LegacyEmpty;
      return;
    }
    // NEW is a stub from a failed/interrupted rename: clear it and retry the
    // migration below.
    if (!removeJunkNewDir()) {
      LOG_ERR("MAIN", "Unidentifiable %s blocks migration; staying on %s", NEW_DIR, LEGACY_DIR);
      activeDir = LEGACY_DIR;
      outcome_ = Outcome::StubBlocked;
      return;
    }
    stubWasRemoved_ = true;
    newExists = false;
  }

  if (newExists) {
    // Reaching here means the legacy dir is gone. Normally NEW is the real
    // migrated/fresh dir — but a stranded zero-byte stub FILE (exFAT failed
    // rename whose cleanup also failed, legacy later removed externally)
    // would wedge every future save (mkdir/open collide with the file).
    // Trust directories only; clear a stub and fall through as fresh SD.
    auto f = Storage.open(NEW_DIR);
    const bool isDir = f && f.isDirectory();
    if (f) f.close();
    if (isDir) {
      activeDir = NEW_DIR;
      outcome_ = Outcome::AlreadyNew;
      return;  // Migrated (or fresh dir already created).
    }
    if (!removeJunkNewDir()) {
      LOG_ERR("MAIN", "Stub %s could not be removed; saves may fail until the SD is cleaned", NEW_DIR);
      outcome_ = Outcome::StubBlockedNew;
      return;
    }
    stubWasRemoved_ = true;
  }
  if (!legacyExists) {
    outcome_ = stubWasRemoved_ ? Outcome::StubRemoved : Outcome::Fresh;
    return;  // Fresh SD: stores mkdir the new dir on first save.
  }
  if (!dirHasEntries(LEGACY_DIR)) {
    // Same guard as above for the legacy-only case: renaming an empty dir buys
    // nothing and is the only way to create the shared-cluster hazard.
    activeDir = NEW_DIR;
    outcome_ = Outcome::LegacyEmpty;
    return;
  }
  if (Storage.rename(LEGACY_DIR, NEW_DIR)) {
    LOG_INF("MAIN", "Migrated %s -> %s", LEGACY_DIR, NEW_DIR);
    activeDir = NEW_DIR;
    outcome_ = Outcome::Migrated;
    return;
  }
  // Best-effort: clear any stub the failed rename just created so the next
  // boot retries from a clean state, then keep using the legacy dir this
  // session — zero data loss.
  removeJunkNewDir();
  LOG_ERR("MAIN", "Data dir migration failed; staying on %s", LEGACY_DIR);
  activeDir = LEGACY_DIR;
  outcome_ = Outcome::MigrationFailed;
}

const char* path() { return activeDir; }

Outcome outcome() { return outcome_; }

const char* outcomeName() {
  switch (outcome_) {
    case Outcome::Unresolved: return "unresolved";
    case Outcome::Fresh: return "fresh";
    case Outcome::AlreadyNew: return "already-new";
    case Outcome::Migrated: return "migrated";
    case Outcome::MigrationFailed: return "rename-failed-legacy";
    case Outcome::StubRemoved: return "stub-removed";
    case Outcome::StubBlocked: return "stub-blocked-legacy";
    case Outcome::StubBlockedNew: return "stub-blocked-new";
    case Outcome::BothNewWins: return "both-new-wins";
    case Outcome::LegacyEmpty: return "legacy-empty";
  }
  return "?";
}

}  // namespace DataDir
