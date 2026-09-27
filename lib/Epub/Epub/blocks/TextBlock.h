#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>
#include <ZhuyinSwaps.h>

#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

namespace zhuyin {
struct ActiveEngine;
}  // namespace zhuyin

// Represents a line of text on a page.
//
// All per-word data lives in ONE flat heap allocation (the arena) instead of
// six parallel vectors: a resident page holds ~25-30 of these blocks, and the
// vector-of-string layout cost ~250 throwing allocations per page load, which
// was the primary driver of heap fragmentation on the ESP32-C3.
//
// Arena layout, in order (2-byte alignment holds by construction: all 16-bit
// arrays come first and the arena base is allocator-aligned; RISC-V faults on
// unaligned multi-byte access):
//   uint16_t textOff[wordCount]        byte offset of word i's text in text[]
//   int16_t  xpos[wordCount]
//   uint16_t focusSuffixX[wordCount]   present only when focusPresent
//   uint8_t  styles[wordCount]
//   uint8_t  focusBoundary[wordCount]  present only when focusPresent
//   char     text[textBytes]           all words back to back, NUL-terminated
//
// Each word is stored NUL-terminated so render() can hand `text + textOff[i]`
// straight to C APIs (drawText) with no std::string materialization.
//
// Focus split semantics (unchanged from the vector layout): boundary N > 0
// means the first N bytes of word i render bold, the remainder in the base
// style. N is bounded to 9 codepoints (<= 36 UTF-8 bytes) by the clamp in
// ParsedText::addWord. focusSuffixX is the pre-computed pixel offset from the
// word start to the regular suffix. Both arrays are omitted from the arena
// entirely when no word on the line has a split (zero per-word RAM cost when
// focus reading is disabled).
//
// 注音替換清單（P2 ③，lib/Zhuyin/ZhuyinSwaps.h）：同一次 arena 配置的尾端，從 4 位元組對齊的位置開始：
//   [u32 引擎世代][u16 筆數][u16 0][zhuyin::Swap × 筆數]
// 只有注音字型、引擎可用時排出來的行才有；其他行 swapTail 是 null → arena 大小與章節格式都跟以前一樣
// （章節檔的標籤 1 由 test/zhuyin_cache 的黃金檔守著）。arena＋清單 ≤ zhuyin::kMaxLineArenaBytes，超過就這一行不換。
class TextBlock final : public Block {
 private:
  BlockStyle blockStyle;
  uint16_t numWords = 0;
  uint16_t textBytes = 0;  // total size of the text region, including NULs
  bool focusPresent = false;
  bool isValid = true;
  // The ONLY allocation: makeUniqueNoThrow, so OOM yields an invalid block
  // instead of abort() (bare new is not nothrow with -fno-exceptions).
  std::unique_ptr<uint8_t[]> arena;
  // Typed views into the arena, bound once after the arena is filled. All
  // 16-bit bases sit at even offsets, so direct dereference is alignment-safe.
  const uint16_t* textOffArr = nullptr;
  const int16_t* xposArr = nullptr;
  const uint16_t* focusSuffixXArr = nullptr;  // null when !focusPresent
  const uint8_t* stylesArr = nullptr;
  const uint8_t* focusBoundaryArr = nullptr;  // null when !focusPresent
  const char* textArr = nullptr;
  std::vector<std::string> rubyTexts;
  const uint8_t* swapTail = nullptr;  // 注音替換清單（見上）；沒有 → null

  TextBlock() = default;  // deserialize() fills the fields directly
  static size_t arenaSize(uint16_t wordCount, bool hasFocus, uint16_t textBytes);
  static size_t swapTailOffset(size_t arenaBytes) { return (arenaBytes + 3) & ~static_cast<size_t>(3); }
  static size_t swapTailBytes(uint16_t n) { return 8 + static_cast<size_t>(n) * sizeof(zhuyin::Swap); }
  void bindArenaPointers();
  // 驗過就寫進 arena 尾端、設定 swapTail（arena 要已經配到 swapTailOffset(base) + swapTailBytes(n)）
  bool attachSwaps(size_t baseArenaBytes, const zhuyin::Swap* swaps, uint16_t n, uint32_t generation);
  // 繪製（P2 ④）：閘門開著（引擎可用、世代跟這一行建立時相同、交給字型的 fontId 就是引擎的那個）才回清單，
  // 否則 nullptr（整行畫原字）
  const zhuyin::Swap* drawableSwaps(int fontId, uint16_t* n) const;
  // 第 i 個字詞要交給字型的字串：有替換就換好放進 buf（next 往前走），否則原字串
  const char* drawnWord(uint16_t i, const zhuyin::Swap* swaps, uint16_t n, uint16_t& next, char* buf,
                        size_t cap) const;

 public:
  // Flatten-on-construct: copies the layout-time vectors into the arena; the
  // vectors die with the caller. On arena OOM the block is empty and valid()
  // is false -- callers must check and fail the line instead of using it.
  // 注音：swaps 由排版給（清單照 (word, cp) 排好＋解析那一段時的世代＋給哪個字型）。
  // 引擎不可用、世代或字型不是登記的那個、太大、配不到記憶體、驗不過 → 這一行不換（行本身照常建立）。
  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSuffixX, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<std::string> rubyTexts = {}, const zhuyin::SwapBatch& swaps = {});
  ~TextBlock() override = default;
  TextBlock(const TextBlock&) = delete;
  TextBlock& operator=(const TextBlock&) = delete;

  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  bool isEmpty() override { return numWords == 0; }
  bool valid() const { return isValid; }
  uint16_t wordCount() const { return numWords; }
  // NUL-terminated by construction; safe to pass to C APIs directly.
  const char* wordText(const uint16_t i) const { return textArr + textOffArr[i]; }
  uint16_t wordTextLen(const uint16_t i) const {
    const uint16_t end = (i + 1 < numWords) ? textOffArr[i + 1] : textBytes;
    return end - textOffArr[i] - 1;  // exclude the NUL
  }
  int16_t wordXpos(const uint16_t i) const { return xposArr[i]; }
  EpdFontFamily::Style wordStyle(const uint16_t i) const { return static_cast<EpdFontFamily::Style>(stylesArr[i]); }
  uint8_t focusBoundary(const uint16_t i) const { return focusPresent ? focusBoundaryArr[i] : 0; }
  uint16_t focusSuffixX(const uint16_t i) const { return focusPresent ? focusSuffixXArr[i] : 0; }
  bool hasRuby() const;
  int getRubyShift(int ascender) const { return hasRuby() ? (ascender / 2) : 0; }
  const std::vector<std::string>& getRubyTexts() const { return rubyTexts; }

  void render(const GfxRenderer& renderer, int fontId, int x, int y) const;
  // 直排的繪製（轉置編碼；由 render() 在 renderer.isVerticalLayout() 時分流）。
  void renderVertical(const GfxRenderer& renderer, int fontId, int x, int y) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(HalFile& file) const;
  // zhuyinSwaps／binding ＝ 章節檔標籤 4 的 PageLine 在本體之前寫的筆數與綁定（Page.cpp 驗過筆數）：本體之後接著讀那麼多筆＋CRC。
  // 0 ＝ 標籤 1（跟以前一模一樣）。完整性不過（讀不到、CRC 不符）→ nullptr（這一頁壞了）；
  // 完整性過了但掛不上（沒有引擎、綁定不同、太大、配不到記憶體、歸屬不過）→ 這一行不換。
  // place ＝ 要載入的是章節裡的哪一頁（綁定要跟它相同）。
  static std::unique_ptr<TextBlock> deserialize(HalFile& file, uint16_t zhuyinSwaps = 0,
                                                const uint8_t* binding = nullptr, const zhuyin::PagePlace& place = {});

  // 注音替換清單（沒有 → 0 筆）
  uint16_t swapCount() const;
  const zhuyin::Swap* swapList() const;
  uint32_t swapGeneration() const;
  zhuyin::LineText lineText() const { return {textOffArr, textArr, numWords, textBytes}; }
  // 序列化前判一次：清單在這個引擎快照下還有效嗎（可用、世代相同）。PageLine 取一次快照，
  // 標籤、內容、綁定都用同一份（codex 複查 ③ F4、第二輪 F6）。無參數版本取當下的快照（查詢用）。
  bool swapsPersistable(const zhuyin::ActiveEngine& eng) const;
  bool swapsPersistable() const;
  // 標籤 4 的前段（本體之前：筆數、補數、綁定）與後段（本體之後：每一筆＋CRC）。binding ＝ PageLine 算一次、兩段共用。
  bool serializeSwapHeader(HalFile& file, const uint8_t* binding) const;
  bool serializeSwaps(HalFile& file, const uint8_t* binding) const;
};
