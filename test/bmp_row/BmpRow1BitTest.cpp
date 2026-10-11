// v387：1-bit 列快轉（BmpRow1Bit::convertRow）必須跟 Bitmap::readNextRow 原本逐點的 case 1 逐位元組相同。
// 參考實作照原本 packPixel 的寫法獨立重寫（不呼叫受測函式）。
// 只保證轉換本身；readNextRow 的接線（顏色順序、走不走快路）靠實機 COVERDRAW 的 row1= 與畫面確認（codex）。
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "lib/GfxRenderer/BmpRow1Bit.h"

namespace {

// 原本的逐點路徑：每點查 bit → 顏色，2-bit 由高位排起，滿 4 點寫一個位元組，最後不滿的位元組照寫。
void reference(const uint8_t* src, int width, uint8_t c0, uint8_t c1, uint8_t* out) {
  uint8_t cur = 0;
  int shift = 6;
  for (int x = 0; x < width; x++) {
    const uint8_t idx = (src[x >> 3] & (0x80 >> (x & 7))) ? 1 : 0;
    const uint8_t c = idx ? c1 : c0;
    cur |= static_cast<uint8_t>(c << shift);
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

void checkRow(const std::vector<uint8_t>& src, int width, uint8_t c0, uint8_t c1) {
  const int outBytes = (width + 3) / 4;
  std::vector<uint8_t> want(outBytes + 4, 0xA5), got(outBytes + 4, 0xA5);
  reference(src.data(), width, c0, c1, want.data());
  BmpRow1Bit::convertRow(src.data(), width, c0, c1, got.data());
  ASSERT_EQ(want, got) << "width=" << width << " c0=" << int(c0) << " c1=" << int(c1);
  for (int i = outBytes; i < outBytes + 4; i++) ASSERT_EQ(got[i], 0xA5) << "寫出界 width=" << width;
}

}  // namespace

// 每一個位元組值 × 每一組顏色 × 寬 1–8（含不滿一個位元組）
TEST(BmpRow1Bit, EveryByteEveryPaletteShortWidths) {
  for (int c0 = 0; c0 < 4; c0++)
    for (int c1 = 0; c1 < 4; c1++)
      for (int v = 0; v < 256; v++)
        for (int w = 1; w <= 8; w++) checkRow({static_cast<uint8_t>(v), static_cast<uint8_t>(~v)}, w, c0, c1);
}

// 隨機列 × 寬 1–2048（縮圖最寬 240、桌布 528；BMP 上限 2048）
TEST(BmpRow1Bit, RandomRowsAllWidths) {
  std::mt19937 rng(387);
  for (int w = 1; w <= 2048; w++) {
    std::vector<uint8_t> src((w + 31) / 32 * 4);
    for (auto& b : src) b = static_cast<uint8_t>(rng());
    for (int c0 = 0; c0 < 4; c0++)
      for (int c1 = 0; c1 < 4; c1++) checkRow(src, w, c0, c1);
  }
}

// 縮圖轉檔器的色盤：0＝黑、1＝白 → 黑點 0、白點 3
TEST(BmpRow1Bit, ThumbnailPaletteKnownValues) {
  uint8_t out[2] = {0, 0};
  const uint8_t src[1] = {0b10100000};
  BmpRow1Bit::convertRow(src, 8, 0, 3, out);
  EXPECT_EQ(out[0], 0b11001100);
  EXPECT_EQ(out[1], 0b00000000);
}
