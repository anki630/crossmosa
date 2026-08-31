#include "DeviceInfo.h"

#include <HalGPIO.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>

#include <cctype>
#include <cstring>

bool deviceSerial(char* out, size_t outLen) {
  if (!out || outLen == 0) return false;
  out[0] = '\0';
  if (outLen < 33) return false;
  char snBuf[33] = {0};
#if !CONFIG_IDF_TARGET_ESP32
  // Classic ESP32 的 efuse 表沒有 USER_DATA 區塊（只有 C3／S3 有）
  if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, snBuf, 256) != ESP_OK) {
    return false;
  }
  if (snBuf[0] == '\0' || snBuf[0] == static_cast<char>(0xFF)) {
    return false;
  }
  for (int i = 0; i < 32 && snBuf[i] != '\0'; i++) {
    if (!std::isprint(static_cast<unsigned char>(snBuf[i]))) {
      return false;
    }
  }
  snBuf[32] = '\0';
  std::strncpy(out, snBuf, outLen - 1);
  out[outLen - 1] = '\0';
  return true;
#else
  (void)snBuf;
  return false;
#endif
}

const char* displayControllerName() {
  // v196：與 main.cpp 開機 log 的 displayIsUc8279() 分支一致。
  return gpio.displayIsUc8279() ? "UC8279" : "UC8253";
}
