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
  // v284：行距＝字身框的倍數。⚠️ 預設**不可以是 1.0** —— 1.0 em 正是這一版要消滅的「零行距」，
  //   而 default-constructed 的 spec 若沒經過 readerRenderSpec() 填值就會靜默用它（codex 複查）。
  float lineHeightEm = 1.50f;
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
  // ℹ️ v271 曾有 `hangMarginPx`（行尾懸掛可吊進頁邊多少），v273 隨橫排懸掛一起移除。
  //    約物擠壓在行內進行，不需要任何頁邊參數 —— 不要為了它再把設定送進排版引擎。
};

// v258：預排接手的前提是「規格逐欄相同」。⚠️ 新增欄位時這裡要一起加 —— 下面的 sizeof 檢查就是為了讓你想起來
//   （codex 複查：比對漏欄位會讓不同的排版被當成相同而接手）。大小變了先補比對，再改數字。
inline bool operator==(const ReaderRenderSpec& a, const ReaderRenderSpec& b) {
  return a.fontId == b.fontId && a.lineHeightEm == b.lineHeightEm &&
         a.extraParagraphSpacing == b.extraParagraphSpacing && a.paragraphAlignment == b.paragraphAlignment &&
         a.viewportWidth == b.viewportWidth && a.viewportHeight == b.viewportHeight &&
         a.hyphenationEnabled == b.hyphenationEnabled && a.embeddedStyle == b.embeddedStyle &&
         a.imageRendering == b.imageRendering && a.focusReadingEnabled == b.focusReadingEnabled &&
         a.boldBodyText == b.boldBodyText && a.verticalLayout == b.verticalLayout &&
         a.columnPitchTier == b.columnPitchTier;
}
inline bool operator!=(const ReaderRenderSpec& a, const ReaderRenderSpec& b) { return !(a == b); }
static_assert(sizeof(ReaderRenderSpec) == 24, "ReaderRenderSpec changed: update operator== above, then this size");
