#include "DeviceInfo.h"

#include <BoardConfig.h>
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
  // v333：讀開機探測實際填好的控制器（HalGPIO::begin 的 applyXteinkDisplayController）。
  //   原本 `displayIsUc8279() ? "UC8279" : "UC8253"` 只分辨 X3 的兩種，X4 不管實際是哪顆一律落到 "UC8253" ——
  //   而 X4 只會是 SSD1677／UC8179／UC8279(800×480)，UC8253 只存在於 X3。X3 不變：設定檔 XTEINK_X3＝UC8253、
  //   XTEINK_X3_UC8279＝UC8279，跟原本顯示的一模一樣。
  switch (BoardConfig::ACTIVE.displayController) {
    case BoardConfig::DisplayController::SSD1677: return "SSD1677";
    case BoardConfig::DisplayController::UC8253: return "UC8253";
    case BoardConfig::DisplayController::UC8279: return "UC8279";
    case BoardConfig::DisplayController::UC8179: return "UC8179";
    default: return "?";
  }
}
