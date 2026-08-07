#include "QrUtils.h"

#include <Utf8.h>
#include <qrcode.h>

#include <algorithm>
#include <memory>

#include "Logging.h"

void QrUtils::drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload) {
  // Dynamically calculate the QR code version based on text length
  // Version 4 holds ~114 bytes, Version 10 ~395, Version 20 ~1066, up to 40
  // qrcode.h max version is 40.
  // Formula: approx version = size / 26 + 1 (very rough estimate, better to find best fit)
  size_t len = textPayload.length();

  // Truncate to max QR capacity at a UTF-8 safe boundary to avoid splitting multi-byte sequences.
  // 兩個硬前提(v39 實機回報「掃不到」的雙重根因):
  // ①這個 QRCode 函式庫【不做容量檢查】,payload 超過該 version 的 byte-mode 容量時會
  //   「成功」產出格式合法但解碼必然失敗的壞 QR(qrcode.c padding 下溢被 clamp、
  //   interleaver 靜默丟尾巴)——所以 cap 與 ladder 門檻必須用【真實容量】:
  //   ECC_LOW byte mode:v4=78、v10=271、v20=858(ISO 18004:資料碼字 − 檔頭 2.5B)。
  //   原上游 ladder 的 114/395/1066/2110 全是超載門檻,長 payload 一律產壞 QR。
  // ②上限刻意停在 version 20(97×97 模組):再大的版本在 ~488px 顯示區每模組 ≤3px,
  //   手機相機掃不動。v20 = 每模組 5px(0.63mm)可穩定掃描。中文一頁 ~2KB → 截前 ~286 字。
  static constexpr size_t MAX_QR_CAPACITY = 858;  // Version 20, ECC_LOW, byte mode 真實容量
  std::string truncated;
  const char* payload = textPayload.c_str();
  if (len > MAX_QR_CAPACITY) {
    len = utf8SafeTruncateBuffer(textPayload.c_str(), static_cast<int>(MAX_QR_CAPACITY));
    truncated = textPayload.substr(0, len);
    payload = truncated.c_str();
  }

  int version = 4;
  if (len > 78) version = 10;
  if (len > 271) version = 20;

  // Make sure we have a large enough buffer on the heap to avoid blowing the stack
  uint32_t bufferSize = qrcode_getBufferSize(version);
  auto qrcodeBytes = std::make_unique<uint8_t[]>(bufferSize);

  QRCode qrcode;
  // Initialize the QR code. We use ECC_LOW for max capacity.
  int8_t res = qrcode_initText(&qrcode, qrcodeBytes.get(), version, ECC_LOW, payload);

  if (res == 0) {
    // Determine the optimal pixel size.
    const int maxDim = std::min(bounds.width, bounds.height);

    int px = maxDim / qrcode.size;
    if (px < 1) px = 1;

    // Calculate centering X and Y
    const int qrDisplaySize = qrcode.size * px;
    const int xOff = bounds.x + (bounds.width - qrDisplaySize) / 2;
    const int yOff = bounds.y + (bounds.height - qrDisplaySize) / 2;

    // Draw the QR Code
    for (uint8_t cy = 0; cy < qrcode.size; cy++) {
      for (uint8_t cx = 0; cx < qrcode.size; cx++) {
        if (qrcode_getModule(&qrcode, cx, cy)) {
          renderer.fillRect(xOff + px * cx, yOff + px * cy, px, px, true);
        }
      }
    }
  } else {
    // If it fails (e.g. text too large), log an error
    LOG_ERR("QR", "Text too large for QR Code version %d", version);
  }
}
