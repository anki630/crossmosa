#include "PersistableStore.h"
#include <DataDir.h>

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>
#include <string>
#include <limits>

bool PersistableStoreBase::writeDocAtomic(const char* path, const JsonDocument& doc) {
  const std::string finalPath = path;
  const std::string tmpPath = finalPath + ".tmp";

  const size_t expected = measureJson(doc);
  size_t written = 0;
  {
    HalFile f;
    if (!Storage.openFileForWrite("PERSIST", tmpPath.c_str(), f)) {
      LOG_ERR("PERSIST", "Could not open temp file for write: %s", tmpPath.c_str());
      return false;
    }
    written = serializeJson(doc, f);
    f.flush();
    // f 於 scope 結束時關閉（DESTRUCTOR_CLOSES_FILE=1）；SdFat 不可 rename 仍開啟的路徑。
  }

  if (written != expected || expected == 0) {
    LOG_ERR("PERSIST", "Short write (%u/%u), keeping previous %s", static_cast<unsigned>(written),
            static_cast<unsigned>(expected), finalPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  Storage.remove(finalPath.c_str());  // SdFat 的 rename 不覆蓋既有目標
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PERSIST", "Failed to rename temp into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir(DataDir::path());
  // 所有 JSON 落地一律走原子路徑 —— 呼叫端不必改。
  return writeDocAtomic(path, doc);
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  if (!Storage.exists(path)) {
    return false;  // Expected on first boot — not an error.
  }
  String json = Storage.readFile(path);
  if (json.isEmpty()) {
    LOG_ERR("PERSIST", "Failed to read %s (empty)", path);
    return false;
  }
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return true;
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave) {
  bool valid = false;
  return extractPassword(doc, needsResave, std::numeric_limits<size_t>::max(), valid);
}

std::string PersistableStoreBase::extractPassword(JsonVariantConst doc, bool& needsResave, const size_t maxLength,
                                                  bool& valid) {
  valid = true;
  bool ok = false;
  bool tooLong = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", maxLength, &ok, &tooLong);
  if (tooLong) {
    valid = false;
    return "";
  }
  if (!ok) {
    // Deobfuscation failed — fall back to legacy plaintext password.
    const char* legacyPassword = doc["password"] | "";
    const size_t legacyLength = strlen(legacyPassword);
    if (legacyLength > maxLength) {
      valid = false;
      return "";
    }
    pass.assign(legacyPassword, legacyLength);
    if (!pass.empty()) needsResave = true;
  }
  // A successfully decoded empty string is a legitimate value; preserve as-is.
  return pass;
}
