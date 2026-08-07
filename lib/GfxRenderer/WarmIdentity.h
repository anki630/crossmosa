#pragma once
#include <cstdint>
#include <cstring>

// v110 字型預取:「這份 glyph 快取屬於哪一頁」。
//
// 欄位就是 Section 快取檔頭的那 11 個排版參數(Section.cpp:108-183 的比對清單)
// 加上頁的身分(bookHash/spineIndex/pageNumber)。刻意逐欄位原值儲存、逐欄位比對,
// 不做壓縮指紋——兩份清單一定會漂移,而這裡漏一欄位的後果是拿別頁的字去畫。
//
// 不變量(spec §4/§5):
//   - valid 只能在「一次完整、未被中止的 prewarm 之後」由呼叫端設起(adopt)。
//   - 任何 clearCache()/prewarmCache()/unloadAll() 都會 invalidate()(機制內建,
//     不靠呼叫點自覺)。
//   - matches() 要求雙方 valid;預設建構 = invalid = 永不相符。
struct WarmIdentity {
  uint32_t bookHash = 0;             // fnv1a(epub->getCachePath())
  int32_t spineIndex = -1;
  int32_t pageNumber = -1;
  int32_t fontId = 0;
  uint16_t viewportWidth = 0;        // 已折入方向/邊距/狀態列/自動翻頁指示
  uint16_t viewportHeight = 0;
  uint32_t lineCompressionBits = 0;  // float 位元精確儲存(floatBits)
  uint8_t paragraphAlignment = 0;
  uint8_t imageRendering = 0;
  bool extraParagraphSpacing = false;
  bool hyphenationEnabled = false;
  bool embeddedStyle = false;
  bool focusReadingEnabled = false;
  bool boldBodyText = false;
  bool valid = false;

  static uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= static_cast<uint8_t>(*s); h *= 16777619u; }
    return h;
  }
  static uint32_t floatBits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof(b));
    return b;
  }

  bool matches(const WarmIdentity& cur) const {
    return valid && cur.valid && bookHash == cur.bookHash && spineIndex == cur.spineIndex &&
           pageNumber == cur.pageNumber && fontId == cur.fontId && viewportWidth == cur.viewportWidth &&
           viewportHeight == cur.viewportHeight && lineCompressionBits == cur.lineCompressionBits &&
           paragraphAlignment == cur.paragraphAlignment && imageRendering == cur.imageRendering &&
           extraParagraphSpacing == cur.extraParagraphSpacing && hyphenationEnabled == cur.hyphenationEnabled &&
           embeddedStyle == cur.embeddedStyle && focusReadingEnabled == cur.focusReadingEnabled &&
           boldBodyText == cur.boldBodyText;
  }
  void invalidate() { valid = false; }
};
