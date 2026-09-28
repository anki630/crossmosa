#include "ZhuyinSwaps.h"

#include <cstring>

#include "ZhuyinData.h"
#include "ZhuyinFormat.h"
#include "ZhuyinUtf8.h"

namespace zhuyin {

uint16_t crc16(const uint8_t* p, size_t len, uint16_t crc) {
  for (size_t i = 0; i < len; i++) {
    crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(p[i]) << 8));
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000u) ? static_cast<uint16_t>((crc << 1) ^ 0x1021u) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

bool locateSwap(const LineText& line, uint16_t word, uint8_t cp, uint32_t* codepoint, uint16_t* byteOff) {
  if (!line.off || !line.text || word >= line.words) return false;
  const uint16_t start = line.off[word];
  const uint16_t stop = (word + 1 < line.words) ? line.off[word + 1] : line.textBytes;  // 含 NUL
  if (stop <= start || stop > line.textBytes) return false;
  const auto* base = reinterpret_cast<const uint8_t*>(line.text);
  const uint8_t* p = base + start;
  const uint8_t* end = base + stop - 1;  // 不含 NUL
  for (uint16_t k = 0; p < end; k++) {
    const uint8_t* at = p;
    const uint32_t c = decodeUtf8(p, end);
    if (k == cp) {
      *codepoint = c;
      *byteOff = static_cast<uint16_t>(at - base);
      return true;
    }
  }
  return false;
}

SwapCheck checkSwaps(const LineText& line, const Swap* swaps, uint16_t n, const ZhuyinData& data) {
  if (n == 0 || !swaps) return SwapCheck::Empty;
  if (n > kMaxLineSwaps) return SwapCheck::TooMany;
  if (!data.loaded()) return SwapCheck::NoEngine;
  for (uint16_t i = 0; i < n; i++) {
    const Swap& s = swaps[i];
    if (i > 0) {
      const Swap& q = swaps[i - 1];
      if (!(q.word < s.word || (q.word == s.word && q.cp < s.cp))) return SwapCheck::Unsorted;
    }
    if (s.word >= line.words) return SwapCheck::BadWord;
    uint32_t c = 0;
    uint16_t at = 0;
    if (!locateSwap(line, s.word, s.cp, &c, &at)) return SwapCheck::BadCp;
    if (!isIdeograph(c)) return SwapCheck::NotIdeograph;
    if (s.pua < 0xE000 || s.pua > 0xF8FF) return SwapCheck::NotPua;
    // 單音字只擁有 0 → 任何 pua 都不屬於它；破音字只擁有自己的預設與替代讀音
    if (!data.outputOwned(c, s.pua)) return SwapCheck::NotOwned;
  }
  return SwapCheck::Ok;
}

void encodeBinding(const SwapBinding& b, uint8_t out[kSwapBindingBytes]) {
  for (int i = 0; i < 8; i++) out[i] = static_cast<uint8_t>(b.dataset >> (8 * i));
  out[8] = static_cast<uint8_t>(b.semantics);
  out[9] = static_cast<uint8_t>(b.semantics >> 8);
  const auto f = static_cast<uint32_t>(b.fontId);
  for (int i = 0; i < 4; i++) out[10 + i] = static_cast<uint8_t>(f >> (8 * i));
  out[14] = static_cast<uint8_t>(b.place.spine);
  out[15] = static_cast<uint8_t>(b.place.spine >> 8);
  out[16] = static_cast<uint8_t>(b.place.page);
  out[17] = static_cast<uint8_t>(b.place.page >> 8);
}

uint16_t swapCrcBegin(const uint8_t binding[kSwapBindingBytes], const uint16_t n, const uint16_t words,
                      const uint8_t focusFlag, const uint16_t textBytes, const uint8_t* arena,
                      const size_t arenaBytes) {
  uint16_t c = crc16(binding, kSwapBindingBytes);
  const uint8_t head[7] = {static_cast<uint8_t>(n),
                           static_cast<uint8_t>(n >> 8),
                           static_cast<uint8_t>(words),
                           static_cast<uint8_t>(words >> 8),
                           focusFlag,
                           static_cast<uint8_t>(textBytes),
                           static_cast<uint8_t>(textBytes >> 8)};
  c = crc16(head, sizeof(head), c);
  if (arena && arenaBytes) c = crc16(arena, arenaBytes, c);
  return c;
}

void encodeSwap(const Swap& s, uint8_t rec[kSwapDiskBytes]) {
  rec[0] = static_cast<uint8_t>(s.word);
  rec[1] = static_cast<uint8_t>(s.word >> 8);
  rec[2] = s.cp;
  rec[3] = static_cast<uint8_t>(s.pua);
  rec[4] = static_cast<uint8_t>(s.pua >> 8);
}

Swap decodeSwap(const uint8_t rec[kSwapDiskBytes]) {
  return Swap{static_cast<uint16_t>(rec[0] | (rec[1] << 8)), static_cast<uint16_t>(rec[3] | (rec[4] << 8)), rec[2], 0};
}

bool applyWordSwaps(const char* word, size_t len, const Swap* swaps, uint16_t n, char* out, size_t cap) {
  if (!word || !swaps || n == 0 || !out || len + 1 > cap) return false;
  std::memcpy(out, word, len);
  out[len] = '\0';
  const auto* base = reinterpret_cast<const uint8_t*>(word);
  const uint8_t* p = base;
  const uint8_t* end = base + len;
  uint16_t next = 0;
  for (uint16_t k = 0; p < end && next < n; k++) {
    const uint8_t* at = p;
    const uint32_t c = decodeUtf8(p, end);
    if (k != swaps[next].cp) continue;
    // 掛上時 checkSwaps 驗過；這裡再確認「3 位元組換 3 位元組」的前提，不成立就整個字詞畫原字
    const uint16_t u = swaps[next].pua;
    if (p - at != 3 || !isIdeograph(c) || u < 0xE000 || u > 0xF8FF) return false;
    auto* o = reinterpret_cast<uint8_t*>(out) + (at - base);
    o[0] = static_cast<uint8_t>(0xE0 | (u >> 12));
    o[1] = static_cast<uint8_t>(0x80 | ((u >> 6) & 0x3F));
    o[2] = static_cast<uint8_t>(0x80 | (u & 0x3F));
    next++;
  }
  return next == n;  // 每一筆都找到了位置
}

}  // namespace zhuyin
