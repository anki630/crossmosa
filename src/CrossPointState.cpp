#include "CrossPointState.h"

#include <HalStorage.h>
#include <DataDir.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <mutex>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 4;
// v36: paths hang off the boot-resolved data dir; built on first use —
// DataDir::resolve() has run by then (boot order).
const char* stateFileBin() {
  static char p[36] = "";
  if (!p[0]) snprintf(p, sizeof(p), "%s/state.bin", DataDir::path());
  return p;
}
const char* stateFileJson() {
  static char p[36] = "";
  if (!p[0]) snprintf(p, sizeof(p), "%s/state.json", DataDir::path());
  return p;
}
const char* stateFileBak() {
  static char p[36] = "";
  if (!p[0]) snprintf(p, sizeof(p), "%s/state.bin.bak", DataDir::path());
  return p;
}
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

bool CrossPointState::saveToFile() const {
  std::lock_guard<std::mutex> lock(_mutex);
  Storage.mkdir(DataDir::path());
  return JsonSettingsIO::saveState(*this, stateFileJson());
}

bool CrossPointState::loadFromFile() {
  // Try JSON first
  if (Storage.exists(stateFileJson())) {
    String json = Storage.readFile(stateFileJson());
    if (!json.isEmpty()) {
      std::lock_guard<std::mutex> lock(_mutex);
      return JsonSettingsIO::loadState(*this, json.c_str());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(stateFileBin())) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(stateFileBin(), stateFileBak());
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
  }

  return false;
}

bool CrossPointState::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", stateFileBin(), inputFile)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(_mutex);

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  return true;
}
