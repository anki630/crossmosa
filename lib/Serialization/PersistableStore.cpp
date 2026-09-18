#include "PersistableStore.h"
#include <DataDir.h>

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <limits>

void (*PersistableStoreBase::diagHook)(const char* line) = nullptr;

namespace {

// v283：PERSISTW 這行 log 只印【固定名稱】的白名單檔名，其餘一律印 "other"。
// ⚠️ BookmarkFile 也走這條路，而書籤檔的路徑帶書名 —— 書名是私人資料，不得進 diag.log
//    （CLAUDE.md 的公開紅線：書庫的組成剖面不外流）。白名單是唯一擋得住的寫法，
//    因為「這條路只會被那幾個 store 呼叫」本身就是要被查證的主張，而它是假的。
const char* persistTag(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const char* base = slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
  static const char* const kKnown[] = {"settings.json", "state.json", "recent.json", "opds.json", "wifi.json"};
  for (const char* k : kKnown) {
    if (strcmp(base, k) == 0) return k;
  }
  return "other";
}

// 鉤子沒接就直接 return —— 連 snprintf 都不做（桌面測試與早於 DiagLog::begin 的存檔）。
void persistDiag(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void persistDiag(const char* fmt, ...) {
  if (!PersistableStoreBase::diagHook) return;
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  PersistableStoreBase::diagHook(buf);
}

}  // namespace

bool PersistableStoreBase::writeDocAtomic(const char* path, const JsonDocument& doc, const unsigned long mkdirMs) {
  // v283：把這條路拆成分段計時。v282 量到喚醒時這一次存檔要 ~860ms，而檔案內容只有
  //   一百多個位元組 —— 也就是【成本不在資料量，在動目錄的次數】。這裡不猜哪一個貴，
  //   直接把 open / write / close / rm / rename 各自量出來，一次刷機定案。
  //   log 寫在全部計時取樣【之後】，所以它自己的 SD I/O 不會污染任何一個數字（v278 的教訓）。
  // ⚠️ t0 放在函式【第一行】：複查指出原本放在 measureJson 之後，`total=` 就不能拿去跟
  //   v282 量到的 860ms 對比（那是整個 saveToFile）。序列化與字串組裝也要算進去。
  const unsigned long t0 = millis();
  const std::string finalPath = path;
  const std::string tmpPath = finalPath + ".tmp";

  const size_t expected = measureJson(doc);
  size_t written = 0;
  const unsigned long tPrep = millis();
  unsigned long tOpen = tPrep;
  unsigned long tWrite = tPrep;
  unsigned long tClose = tPrep;
  bool reopened = false;
  bool remkdir = false;
  {
    HalFile f;
    if (!Storage.openFileForWrite("PERSIST", tmpPath.c_str(), f)) {
      // v283 自癒：呼叫端可能因為 DataDir::existedAtResolve() 而跳過了 mkdir。目錄若在本次
      //   開機途中被刪掉（網頁檔案管理／清快取），這裡補做一次再試 —— 所以跳過 mkdir
      //   是【最佳化】，不是正確性的前提。
      //   ⚠️ mkdir 的結果要記下來（複查：不記就無法分辨「目錄補不回來」與「不是缺目錄」）。
      remkdir = Storage.mkdir(DataDir::path());
      reopened = true;
      if (!Storage.openFileForWrite("PERSIST", tmpPath.c_str(), f)) {
        LOG_ERR("PERSIST", "Could not open temp file for write: %s", tmpPath.c_str());
        persistDiag("PERSISTW %s FAILED at=open mkdir=%lu remkdir=%d", persistTag(finalPath), mkdirMs, remkdir ? 1 : 0);
        return false;
      }
    }
    tOpen = millis();
    written = serializeJson(doc, f);
    f.flush();
    tWrite = millis();
    // f 於 scope 結束時關閉（DESTRUCTOR_CLOSES_FILE=1）；SdFat 不可 rename 仍開啟的路徑。
  }
  tClose = millis();

  if (written != expected || expected == 0) {
    LOG_ERR("PERSIST", "Short write (%u/%u), keeping previous %s", static_cast<unsigned>(written),
            static_cast<unsigned>(expected), finalPath.c_str());
    Storage.remove(tmpPath.c_str());
    persistDiag("PERSISTW %s FAILED at=write got=%u want=%u", persistTag(finalPath), static_cast<unsigned>(written),
                  static_cast<unsigned>(expected));
    return false;
  }

  // 舊檔【本來就可能不存在】（第一次存檔），所以 remove 回 false 不一定是錯誤 —— 但它要
  // 印出來，否則無從分辨「刪很快」與「根本沒刪到」（這個專案吃過「失敗得很快」的虧）。
  const bool removed = Storage.remove(finalPath.c_str());  // SdFat 的 rename 不覆蓋既有目標
  const unsigned long tRm = millis();
  const bool renamed = Storage.rename(tmpPath.c_str(), finalPath.c_str());
  const unsigned long tRename = millis();

  // ⚠️ 這些是【端到端等待時間】，不是純 SdFat 成本：HalStorage 用一把遞迴 mutex 序列化所有
  //   操作，所以網頁伺服器任務同時在動 SD 時，等鎖的時間會算進來。喚醒路徑上沒有那個任務，
  //   所以喚醒那幾筆樣本是乾淨的；其餘筆數判讀時要記得這件事。
  persistDiag("PERSISTW %s n=%u mkdir=%lu prep=%lu open=%lu write=%lu close=%lu rm=%lu rename=%lu total=%lu "
              "ok=%d rmok=%d re=%d",
              persistTag(finalPath), static_cast<unsigned>(expected), mkdirMs,
              static_cast<unsigned long>(tPrep - t0), static_cast<unsigned long>(tOpen - tPrep),
              static_cast<unsigned long>(tWrite - tOpen), static_cast<unsigned long>(tClose - tWrite),
              static_cast<unsigned long>(tRm - tClose), static_cast<unsigned long>(tRename - tRm),
              static_cast<unsigned long>(tRename - t0) + mkdirMs, renamed ? 1 : 0, removed ? 1 : 0,
              reopened ? 1 : 0);

  if (!renamed) {
    LOG_ERR("PERSIST", "Failed to rename temp into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  // v283：每次存檔都無條件 mkdir 一次是多餘的 —— 開機的 DataDir::resolve() 已經【證明】
  //   過目錄存在（見 DataDir::existedAtResolve() 逐條的依據）。SdFat 的 mkdir 即使目錄已
  //   存在，仍要走完整路徑解析；而 v282 量到這條路上任何一個動目錄的操作都以百毫秒計。
  //   ⚠️ 這是最佳化不是保證：writeDocAtomic 開檔失敗時會自己補 mkdir 再試一次。
  unsigned long mkdirMs = 0;
  if (!DataDir::existedAtResolve()) {
    const unsigned long m0 = millis();
    Storage.mkdir(DataDir::path());
    mkdirMs = millis() - m0;
  }
  // 所有 JSON 落地一律走原子路徑 —— 呼叫端不必改。
  return writeDocAtomic(path, doc, mkdirMs);
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
