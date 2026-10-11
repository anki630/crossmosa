#pragma once
// 電腦端替身：語意照 ESP-IDF nvs_get_blob —— data 為 nullptr 時只回長度；緩衝區太小回 INVALID_LENGTH 並帶回需要的長度。
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nvs.h"
namespace NvsStore {
inline std::map<std::string, std::vector<uint8_t>> store;
inline int failPuts = 0;
inline int getErr = 0;  // 非 0：getBlob 一律回這個錯誤（namespace 開不了、flash 讀錯……）
inline int puts = 0;
inline bool putBlob(const char* key, const void* data, size_t len, uint32_t* usOut = nullptr, int* errOut = nullptr) {
  if (usOut) *usOut = 3000;
  ++puts;
  if (failPuts > 0) {
    --failPuts;
    if (errOut) *errOut = -1;
    return false;
  }
  const auto* p = static_cast<const uint8_t*>(data);
  store[key] = std::vector<uint8_t>(p, p + len);
  if (errOut) *errOut = 0;
  return true;
}
inline bool getBlob(const char* key, void* data, size_t* lenInOut, int* errOut = nullptr) {
  if (getErr != 0) {
    if (errOut) *errOut = getErr;
    return false;
  }
  const auto it = store.find(key);
  if (it == store.end()) {
    if (errOut) *errOut = ESP_ERR_NVS_NOT_FOUND;
    return false;
  }
  if (!data) {
    *lenInOut = it->second.size();
    if (errOut) *errOut = 0;
    return true;
  }
  if (*lenInOut < it->second.size()) {
    *lenInOut = it->second.size();
    if (errOut) *errOut = ESP_ERR_NVS_INVALID_LENGTH;
    return false;
  }
  std::memcpy(data, it->second.data(), it->second.size());
  *lenInOut = it->second.size();
  if (errOut) *errOut = 0;
  return true;
}
}  // namespace NvsStore
