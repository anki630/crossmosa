#pragma once
#include <cstdint>

// The resolved text-rendering configuration a reader hands to the layout
// engine. Section-cache validation keys on every field: a section file built
// with a different spec is discarded and rebuilt.
//
// Build one via CrossPointSettings::readerRenderSpec(width, height), which
// fills every field: the settings-derived ones from the store, the viewport
// from the caller. Taking the viewport as arguments is what keeps a spec from
// existing in a half-filled state — the 0 defaults below are a last-resort
// backstop (a 0x0 viewport lays out nothing), not an invitation to omit it.
struct ReaderRenderSpec {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
  bool boldBodyText = false;  // v31/v41 → v187：粗體內文在排版階段烤進去（ParsedText::addWord），進 section 檔頭
  // ── 直排（縦書き）──────────────────────────────────────────────────
  // ⭐ 這兩個**進 section 檔頭**，不走檔名變體。原因見 Section.cpp 檔頭常數區的註解：
  //    檔頭的回數 seek 全部錨定在【尾端】，在前段插欄位是安全的；而檔名變體要靠一個
  //    全域「目前是哪個變體」，一旦設晚了就會【把橫排快取當直排讀】——錯得無聲無息。
  bool verticalLayout = false;
  uint8_t columnPitchTier = 1;  // 0=緊 1.35em／1=標準 1.50em／2=寬 1.75em
};
