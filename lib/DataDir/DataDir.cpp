#include "DataDir.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

namespace {
constexpr const char* NEW_DIR = "/.crossmosa";
constexpr const char* LEGACY_DIR = "/.crosspoint";
const char* activeDir = NEW_DIR;

// True when dir contains any store file a used data dir acquires within its
// first sessions (state/settings are saved on every sleep or settings exit;
// .bin variants cover pre-JSON stock-firmware cards). Distinguishes a real
// migrated dir from junk left behind by a failed SdFat rename.
bool looksLikeRealDataDir(const char* dir) {
  static constexpr const char* SENTINELS[] = {"state.json",   "settings.json", "recent.json", "wifi.json",
                                              "opds.json",    "state.bin",     "settings.bin"};
  char p[44];
  for (const char* s : SENTINELS) {
    snprintf(p, sizeof(p), "%s/%s", dir, s);
    if (Storage.exists(p)) return true;
  }
  return false;
}

// Remove a junk NEW_DIR left by a failed/interrupted SdFat rename — SdFat
// creates the new name FIRST (empty dir on FAT, zero-byte plain file on
// exFAT) and only then moves/removes, so a failure can strand that stub.
// Refuses to touch a non-empty directory (rmdir fails on non-empty by
// design). Returns true when NEW_DIR no longer exists.
bool removeJunkNewDir() {
  auto f = Storage.open(NEW_DIR);
  if (!f) {
    return !Storage.exists(NEW_DIR);
  }
  const bool isDir = f.isDirectory();
  f.close();
  return isDir ? Storage.rmdir(NEW_DIR) : Storage.remove(NEW_DIR);
}
}  // namespace

namespace DataDir {

void resolve() {
  const bool legacyExists = Storage.exists(LEGACY_DIR);
  bool newExists = Storage.exists(NEW_DIR);

  if (newExists && legacyExists) {
    if (looksLikeRealDataDir(NEW_DIR)) {
      // Real migrated dir plus a later-recreated legacy dir (typical cause:
      // an OTA-slot rollback session on pre-v36 firmware). New wins; never
      // delete the other automatically — after a mid-rename power cut both
      // entries can even alias the same FAT clusters, so deleting one could
      // corrupt the other. Needs a PC disk check before manual cleanup.
      LOG_ERR("MAIN", "Both %s and %s exist; using %s (data written by old-firmware sessions stays in %s — do not delete it from the device)",
              NEW_DIR, LEGACY_DIR, NEW_DIR, LEGACY_DIR);
      activeDir = NEW_DIR;  // reset for re-call safety
      return;
    }
    // NEW is a stub from a failed/interrupted rename: clear it and retry the
    // migration below.
    if (!removeJunkNewDir()) {
      LOG_ERR("MAIN", "Unidentifiable %s blocks migration; staying on %s", NEW_DIR, LEGACY_DIR);
      activeDir = LEGACY_DIR;
      return;
    }
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
      activeDir = NEW_DIR;  // reset for re-call safety
      return;               // Migrated (or fresh dir already created).
    }
    if (!removeJunkNewDir()) {
      LOG_ERR("MAIN", "Stub %s could not be removed; saves may fail until the SD is cleaned", NEW_DIR);
      return;
    }
  }
  if (!legacyExists) {
    return;  // Fresh SD: stores mkdir the new dir on first save.
  }
  if (Storage.rename(LEGACY_DIR, NEW_DIR)) {
    LOG_INF("MAIN", "Migrated %s -> %s", LEGACY_DIR, NEW_DIR);
    activeDir = NEW_DIR;  // reset for re-call safety
    return;
  }
  // Best-effort: clear any stub the failed rename just created so the next
  // boot retries from a clean state, then keep using the legacy dir this
  // session — zero data loss.
  removeJunkNewDir();
  LOG_ERR("MAIN", "Data dir migration failed; staying on %s", LEGACY_DIR);
  activeDir = LEGACY_DIR;
}

const char* path() { return activeDir; }

}  // namespace DataDir
