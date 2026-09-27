#pragma once
// 嚴格的 UTF-8 解碼：不合法的位元組 → U+FFFD（硬邊界），一次吃一個位元組往前走。
// ⚠️ session（送進解析器的碼位）、替換清單（第幾個碼位）、繪製（換哪幾個位元組）都用這一個 ——
//    三者對「第 k 個碼位」的看法必須一致，否則讀音會掛到隔壁的字上。

#include <cstdint>

namespace zhuyin {

inline uint32_t decodeUtf8(const uint8_t*& p, const uint8_t* end) {
  const uint8_t b = *p;
  if (b < 0x80) {
    p++;
    return b;
  }
  int n = 0;
  uint32_t cp = 0;
  if ((b & 0xE0) == 0xC0) {
    n = 1;
    cp = b & 0x1Fu;
  } else if ((b & 0xF0) == 0xE0) {
    n = 2;
    cp = b & 0x0Fu;
  } else if ((b & 0xF8) == 0xF0) {
    n = 3;
    cp = b & 0x07u;
  } else {
    p++;
    return 0xFFFD;
  }
  if (end - p < n + 1) {
    p++;
    return 0xFFFD;
  }
  for (int i = 1; i <= n; i++) {
    if ((p[i] & 0xC0) != 0x80) {
      p++;
      return 0xFFFD;
    }
    cp = (cp << 6) | (p[i] & 0x3Fu);
  }
  static constexpr uint32_t kMin[4] = {0, 0x80, 0x800, 0x10000};
  if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    p++;
    return 0xFFFD;
  }
  p += n + 1;
  return cp;
}

}  // namespace zhuyin
