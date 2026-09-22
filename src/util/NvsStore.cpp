#include "util/NvsStore.h"

#include <esp_err.h>
#include <esp_timer.h>
#include <nvs.h>

#include <mutex>

namespace NvsStore {
namespace {

constexpr char NAMESPACE[] = "cmosa";  // ≤15 字元；HalGPIO 用 cphw、WiFi 用 nvs.net80211，不會撞

// 葉鎖：只保護下面這些，鎖內【不做】任何 SD／log。NVS 本身另有自己的鎖（在 nvs_set_blob／commit 裡面）。
//   render task（進度影子，持 RenderLock）與主任務（state 影子，storeMutex 已釋放）都會來。
std::mutex gMutex;
nvs_handle_t gHandle = 0;
bool gOpened = false;
bool gFailed = false;  // 開過一次失敗就不再試（這一段開機 NVS 不可用）
Stats gStats = {};
uint32_t gSeq = 0;

// 呼叫端已持 gMutex。
bool ensureOpen(int* errOut) {
  if (gOpened) return true;
  if (gFailed) {
    if (errOut) *errOut = gStats.openErr;
    return false;
  }
  const esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &gHandle);
  if (err != ESP_OK) {
    gFailed = true;
    gStats.openErr = static_cast<int>(err);
    if (errOut) *errOut = static_cast<int>(err);
    return false;
  }
  gOpened = true;
  return true;
}

}  // namespace

bool putBlob(const char* key, const void* data, const size_t len, uint32_t* usOut, int* errOut) {
  const int64_t t0 = esp_timer_get_time();  // 從進入就計時：等鎖也是呼叫端尾段真的付的時間（codex）
  esp_err_t e = ESP_OK;
  uint32_t us = 0;
  {
    std::lock_guard<std::mutex> lock(gMutex);
    int err = 0;
    if (!ensureOpen(&err)) {
      us = static_cast<uint32_t>(esp_timer_get_time() - t0);  // 開不了也算時間（codex：慢又失敗的那次不能只剩錯誤碼）
      gStats.fails++;
      gStats.lastErr = err;
      gStats.usTotal += us;
      if (us > gStats.usMax) gStats.usMax = us;
      if (errOut) *errOut = err;
      if (usOut) *usOut = us;
      return false;
    }
    e = nvs_set_blob(gHandle, key, data, len);
    if (e == ESP_OK) e = nvs_commit(gHandle);
    us = static_cast<uint32_t>(esp_timer_get_time() - t0);
    gStats.writes++;
    gStats.usTotal += us;
    if (us > gStats.usMax) gStats.usMax = us;
    if (e != ESP_OK) {
      gStats.fails++;
      gStats.lastErr = static_cast<int>(e);
    }
  }
  if (usOut) *usOut = us;
  if (errOut) *errOut = static_cast<int>(e);
  return e == ESP_OK;
}

bool getBlob(const char* key, void* data, size_t* lenInOut, int* errOut) {
  // 唯讀 handle：namespace 不存在就直接失敗（NVS_READONLY 不會建立它），不動 flash。
  nvs_handle_t h = 0;
  esp_err_t e = nvs_open(NAMESPACE, NVS_READONLY, &h);
  if (e == ESP_OK) {
    e = nvs_get_blob(h, key, data, lenInOut);
    nvs_close(h);
  }
  if (errOut) *errOut = static_cast<int>(e);
  return e == ESP_OK;
}

// v332：型別化入口。
bool putState(const StateBlob& b, uint32_t* usOut, int* errOut) { return putBlob("state", &b, sizeof(b), usOut, errOut); }

bool readState(StateBlob* out, int* errOut) {
  StateBlob b{};
  size_t len = sizeof(b);
  if (!getBlob("state", &b, &len, errOut)) return false;
  if (len != sizeof(b) || b.magic != 'S' || b.version != 2) {
    // 長度／magic／版本不合：當成沒有（呼叫端走 SD）。v1（v331 的影子）刻意不採信：它沒有配對資訊。
    if (errOut)
      *errOut = len != sizeof(b) ? ESP_ERR_NVS_INVALID_LENGTH : b.magic != 'S' ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_VERSION;
    return false;
  }
  b.path[sizeof(b.path) - 1] = '\0';
  *out = b;
  return true;
}

bool putProg(const ProgBlob& b, uint32_t* usOut, int* errOut) { return putBlob("prog", &b, sizeof(b), usOut, errOut); }

bool readProg(ProgBlob* out, int* errOut) {
  ProgBlob b{};
  size_t len = sizeof(b);
  if (!getBlob("prog", &b, &len, errOut)) return false;
  if (len != sizeof(b) || b.magic != 'P' || b.version != 2) {
    if (errOut)
      *errOut = len != sizeof(b) ? ESP_ERR_NVS_INVALID_LENGTH : b.magic != 'P' ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_VERSION;
    return false;
  }
  *out = b;
  return true;
}

Stats takeStats() {
  std::lock_guard<std::mutex> lock(gMutex);
  const Stats s = gStats;
  const int openErr = gStats.openErr;  // 開啟失敗是這一段開機的常態，不歸零
  gStats = {};
  gStats.openErr = openErr;
  return s;
}

bool usage(uint32_t* used, uint32_t* freeEntries, uint32_t* available, uint32_t* total, uint32_t* namespaces) {
  nvs_stats_t st = {};
  if (nvs_get_stats(nullptr, &st) != ESP_OK) return false;
  if (used) *used = st.used_entries;
  if (freeEntries) *freeEntries = st.free_entries;
  if (available) *available = st.available_entries;
  if (total) *total = st.total_entries;
  if (namespaces) *namespaces = st.namespace_count;
  return true;
}

uint32_t nextSeq() {
  std::lock_guard<std::mutex> lock(gMutex);
  return ++gSeq;
}

}  // namespace NvsStore
