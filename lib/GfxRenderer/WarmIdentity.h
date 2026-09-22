#pragma once
#include <cstdint>
#include <cstdio>
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
  // v284：記的是**解析後的實際行距（像素）**，不是使用者設定的 em 倍數。
  // ⚠️ 設定一樣但字身框量測結果不同時，版面其實不同 —— 只比設定會沿用錯的暖頁
  //    （codex 複查）。欄位名保留 Bits 是因為仍走 floatBits 精確比對。
  uint32_t lineHeightEmBits = 0;
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
           viewportHeight == cur.viewportHeight && lineHeightEmBits == cur.lineHeightEmBits &&
           paragraphAlignment == cur.paragraphAlignment && imageRendering == cur.imageRendering &&
           extraParagraphSpacing == cur.extraParagraphSpacing && hyphenationEnabled == cur.hyphenationEnabled &&
           embeddedStyle == cur.embeddedStyle && focusReadingEnabled == cur.focusReadingEnabled &&
           boldBodyText == cur.boldBodyText;
  }
  void invalidate() { valid = false; }

  // v313 證人用：哪些欄位不同。0 ⇔ matches() 為真。
  //   bit 0=self.valid 1=cur.valid 2=book 3=spine 4=page 5=font 6=vw 7=vh 8=lh
  //       9=align 10=imgR 11=extraSp 12=hyph 13=embed 14=focus 15=bold
  uint16_t diffMask(const WarmIdentity& cur) const {
    uint16_t m = 0;
    if (!valid) m |= 1u << 0;
    if (!cur.valid) m |= 1u << 1;
    if (bookHash != cur.bookHash) m |= 1u << 2;
    if (spineIndex != cur.spineIndex) m |= 1u << 3;
    if (pageNumber != cur.pageNumber) m |= 1u << 4;
    if (fontId != cur.fontId) m |= 1u << 5;
    if (viewportWidth != cur.viewportWidth) m |= 1u << 6;
    if (viewportHeight != cur.viewportHeight) m |= 1u << 7;
    if (lineHeightEmBits != cur.lineHeightEmBits) m |= 1u << 8;
    if (paragraphAlignment != cur.paragraphAlignment) m |= 1u << 9;
    if (imageRendering != cur.imageRendering) m |= 1u << 10;
    if (extraParagraphSpacing != cur.extraParagraphSpacing) m |= 1u << 11;
    if (hyphenationEnabled != cur.hyphenationEnabled) m |= 1u << 12;
    if (embeddedStyle != cur.embeddedStyle) m |= 1u << 13;
    if (focusReadingEnabled != cur.focusReadingEnabled) m |= 1u << 14;
    if (boldBodyText != cur.boldBodyText) m |= 1u << 15;
    return m;
  }
  // v313 證人用：緊湊字串（約 55 字元）。o = align(2b) | imgR(2b)<<2 | extra<<4 | hyph<<5 | embed<<6 | focus<<7 | bold<<8。
  int format(char* out, size_t n) const {
    const unsigned o = (paragraphAlignment & 3u) | ((imageRendering & 3u) << 2) | (extraParagraphSpacing ? 1u << 4 : 0) |
                       (hyphenationEnabled ? 1u << 5 : 0) | (embeddedStyle ? 1u << 6 : 0) |
                       (focusReadingEnabled ? 1u << 7 : 0) | (boldBodyText ? 1u << 8 : 0);
    return snprintf(out, n, "b:%08lx/s:%ld/p:%ld/f:%ld/wh:%ux%u/lh:%08lx/o:%03x/v:%d", static_cast<unsigned long>(bookHash),
                    static_cast<long>(spineIndex), static_cast<long>(pageNumber), static_cast<long>(fontId),
                    static_cast<unsigned>(viewportWidth), static_cast<unsigned>(viewportHeight),
                    static_cast<unsigned long>(lineHeightEmBits), o, valid ? 1 : 0);
  }
};
