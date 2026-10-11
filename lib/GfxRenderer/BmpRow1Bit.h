#pragma once

#include <cstdint>

// v387（diag386：封面每列「讀列」約 113 µs，一點約 125 個週期）：1-bit BMP 列轉成 readNextRow 的 2-bit 輸出。
//   原本逐點走 packPixel（查色盤、判斷抖色器、移位），封面縮圖全是 1-bit → 改成一次轉 8 點。
//   輸出跟逐點版逐位元組相同：2-bit 由高位排起、超出寬度的位置是 0（主機測試 test/bmp_row 窮舉比對）。
namespace BmpRow1Bit {

// 半位元組（4 點，高位是第一點）→ 每點一個 2-bit 格子的最低位：bit3→bit6、bit2→bit4、bit1→bit2、bit0→bit0
inline uint8_t spreadNibble(const uint8_t n) {
  static constexpr uint8_t kSpread[16] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15,
                                          0x40, 0x41, 0x44, 0x45, 0x50, 0x51, 0x54, 0x55};
  return kSpread[n & 0x0F];
}

// c0／c1：色盤 0／1 的 2-bit 顏色（0–3）。格子乘上顏色不會溢到隔壁格子。
inline uint8_t packNibble(const uint8_t n, const uint8_t c0, const uint8_t c1) {
  return static_cast<uint8_t>(spreadNibble(n) * c1 + spreadNibble(static_cast<uint8_t>(~n)) * c0);
}

// src：BMP 的一列（1 bit／點，高位是第一點）；out 至少 (width+3)/4 位元組。
inline void convertRow(const uint8_t* src, const int width, const uint8_t c0, const uint8_t c1, uint8_t* out) {
  const int fullBytes = width >> 3;
  for (int i = 0; i < fullBytes; i++) {
    const uint8_t b = src[i];
    *out++ = packNibble(static_cast<uint8_t>(b >> 4), c0, c1);
    *out++ = packNibble(static_cast<uint8_t>(b & 0x0F), c0, c1);
  }
  const int rest = width & 7;  // 剩下不滿 8 點：照逐點版只寫用到的位元組，沒用到的格子是 0
  if (rest == 0) return;
  const uint8_t b = src[fullBytes];
  uint8_t cur = 0;
  int shift = 6;
  for (int k = 0; k < rest; k++) {
    const uint8_t c = (b & (0x80 >> k)) ? c1 : c0;
    cur = static_cast<uint8_t>(cur | (c << shift));
    if (shift == 0) {
      *out++ = cur;
      cur = 0;
      shift = 6;
    } else {
      shift -= 2;
    }
  }
  if (shift != 6) *out = cur;
}

}  // namespace BmpRow1Bit
