#include "CrossPointState.h"

#include <Logging.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "util/NvsStore.h"

namespace {

// 呼叫端持 storeMutex。
void fillBlob(const CrossPointState& s, NvsStore::StateBlob& b, const uint32_t sdNonce) {
  b.magic = 'S';
  b.version = 2;
  b.recentPos = s.recentSleepPos;
  b.recentFill = s.recentSleepFill;
  b.wakeFrameToken = s.wakeFrameToken;
  b.readerLoadCount = s.readerActivityLoadCount;
  b.deepSleepStamp = s.deepSleepStamp ? 1 : 0;
  b.lastSleepFromReader = s.lastSleepFromReader ? 1 : 0;
  memcpy(b.recent, s.recentSleepImages, sizeof(b.recent));
  b.pathTrunc = s.openEpubPath.size() >= sizeof(b.path) ? 1 : 0;
  strncpy(b.path, s.openEpubPath.c_str(), sizeof(b.path) - 1);  // 最後一個 byte 永遠是 0
  b.sdNonce = sdNonce;
}

// 呼叫端持 storeMutex。blob 已由 readState 驗過長度／magic／版本並補了結尾 0。不動 sdNonce_（那是 SD 那份的）。
void applyBlob(CrossPointState& s, const NvsStore::StateBlob& b) {
  s.openEpubPath.assign(b.path, strnlen(b.path, sizeof(b.path)));
  memcpy(s.recentSleepImages, b.recent, sizeof(s.recentSleepImages));
  s.recentSleepPos = b.recentPos < CrossPointState::SLEEP_RECENT_COUNT ? b.recentPos : 0;
  s.recentSleepFill = std::min<uint8_t>(b.recentFill, CrossPointState::SLEEP_RECENT_COUNT);
  s.readerActivityLoadCount = b.readerLoadCount;
  s.wakeFrameToken = b.wakeFrameToken;
  s.deepSleepStamp = b.deepSleepStamp != 0;
  s.lastSleepFromReader = b.lastSleepFromReader != 0;
}

}  // namespace

// 一次 storeMutex 取兩份（JSON 與 blob 來自同一份欄位；codex）。nonce ＝ 這份要配對的 state.json nonce。
void CrossPointState::snapshot(JsonDocument* doc, NvsStore::StateBlob* b, const uint32_t nonce) const {
  std::lock_guard<std::mutex> lock(storeMutex);
  if (doc) {
    toJson(*doc);
    (*doc)["nonce"] = nonce;
  }
  if (b) fillBlob(*this, *b, nonce);
}

// 換一個新 nonce（保證跟目前不同）、序列化、寫 state.json；【只有寫成功才】把 sdNonce_ 換成新的 —— codex 第二輪：
//   沒寫成就換，之後的 NVS 會配對到一個卡上不存在的 nonce，開機反而選到舊的 SD。呼叫端持 saveMutex_。
bool CrossPointState::writeSdLocked() {
  uint32_t cand;
  do {
    cand = esp_random() | 1u;  // 非 0：0 保留給「沒有 nonce」
  } while (cand == sdNonce_);
  JsonDocument doc;
  snapshot(&doc, nullptr, cand);
  if (!writeDocToFile(getFilePath(), doc)) return false;
  std::lock_guard<std::mutex> lock(storeMutex);
  sdNonce_ = cand;
  return true;
}

// v332：熱路徑 —— 只寫 NVS（實測 3–4ms、GC 時 33ms）。NVS 不可用／寫失敗／路徑放不進 blob → 當場退回寫 state.json
//   （v330 的行為）。退路寫成功的 SD 帶新 nonce，而 NVS 那份（舊）記的是舊 nonce → 下次開機對不上 → 選 SD（新的）。
bool CrossPointState::save() {
  std::lock_guard<std::mutex> op(saveMutex_);
  NvsStore::StateBlob b{};
  snapshot(nullptr, &b, sdNonce_);
  b.seq = NvsStore::nextSeq();  // 鎖外配號（別在 storeMutex 裡等 render task 的 GC）
  int err = 0;
  if (NvsStore::putState(b, nullptr, &err) && b.pathTrunc == 0) return true;
  return writeSdLocked();
}

// v332：沒人等的時刻（離開書、淺睡眠入口桌布之後、真關機出口）—— state.json 與 NVS 都寫，同一份快照。
//   SD 先、NVS 後：中間斷電 → SD 新、NVS 舊且 nonce 對不上 → 開機選 SD（新的那份）。
//   SD 沒寫成 → NVS 那份改配對到還在卡上的舊 nonce：狀態仍是最新的，開機仍會採信它（codex 第二輪）。
bool CrossPointState::saveDurable() {
  std::lock_guard<std::mutex> op(saveMutex_);
  uint32_t cand;
  do {
    cand = esp_random() | 1u;
  } while (cand == sdNonce_);
  JsonDocument doc;
  NvsStore::StateBlob b{};
  snapshot(&doc, &b, cand);
  const bool sdOk = writeDocToFile(getFilePath(), doc);
  if (sdOk) {
    std::lock_guard<std::mutex> lock(storeMutex);
    sdNonce_ = cand;
  } else {
    b.sdNonce = sdNonce_;
  }
  b.seq = NvsStore::nextSeq();
  NvsStore::putState(b);  // 失敗無妨：SD 那份（新或舊）就是開機會選的
  return sdOk;
}

// v332：開機 —— 先讀 state.json（含 nonce），NVS 配對得上才蓋過去。
bool CrossPointState::load() {
  std::lock_guard<std::mutex> op(saveMutex_);
  const bool sdOk = loadFromFile();  // 讀不到 → 欄位維持預設、sdNonce_ 維持 0
  NvsStore::StateBlob b{};
  int err = 0;
  if (NvsStore::readState(&b, &err) && b.pathTrunc == 0 && b.sdNonce != 0 && b.sdNonce == sdNonce_) {
    {
      std::lock_guard<std::mutex> lock(storeMutex);
      applyBlob(*this, b);
    }
    loadSource_ = 1;
    return true;
  }
  loadSource_ = sdOk ? 2 : 0;
  return sdOk;
}

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

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentArr.add(recentSleepImages[i]);
  doc["recentSleepPos"] = recentSleepPos;
  doc["recentSleepFill"] = recentSleepFill;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["wakeFrameToken"] = wakeFrameToken;  // v293
  doc["deepSleepStamp"] = deepSleepStamp;          // v312
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["nonce"] = sdNonce_;  // v332：與 NVS 配對用；v330 重寫時會丟掉它（→ 開機時 NVS 對不上，選 SD）
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";
  memset(recentSleepImages, 0, sizeof(recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount =
      recentArr.isNull() ? 0 : std::min(static_cast<int>(recentArr.size()), static_cast<int>(SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (recentSleepPos >= SLEEP_RECENT_COUNT) recentSleepPos = actualCount > 0 ? recentSleepPos % SLEEP_RECENT_COUNT : 0;
  recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(recentSleepFill), actualCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  wakeFrameToken = doc["wakeFrameToken"] | static_cast<uint32_t>(0);  // v293
  deepSleepStamp = doc["deepSleepStamp"] | false;                              // v312（舊檔沒有 → false → 照畫 logo）
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  sdNonce_ = doc["nonce"] | static_cast<uint32_t>(0);  // v332
  return true;
}
