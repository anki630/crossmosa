#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  // words/rubyTexts are std::deque, not std::vector: a paragraph can hold thousands
  // of tokens (CJK splits every character), and a vector grows by reallocating its
  // whole element array into one contiguous block (32 B/std::string -> 64-128 KB at
  // a few thousand tokens). On the ESP32-C3 that single large contiguous request
  // fails under a fragmented, BLE-resident heap and the throwing operator new
  // abort()s the firmware (fresh-open CJK crash). A deque grows in fixed ~512 B nodes
  // (largest contiguous alloc stays ~2 KB regardless of token count), so it never
  // triggers that. The per-token parallel arrays below stay vectors: 1 byte / 1 bit
  // each, they never approach the contiguous-block ceiling.
  // v6/v149：sticky 低記憶體旗標。addWord 的守衛拒絕之後 latch 住，
  // 解析器透過 hasOom() 看到並中止本章（而不是讓 throwing new 去 abort 整台機器）。
  // ⚠️ 只有 addWord 側有守衛。【不要】在 layout 側加早退 —— v139 在
  //    layoutAndExtractLines 開頭加 `if (oom_) return;` 跳過了尾端的
  //    「Remove consumed words」，一次暫時性拒絕變成正回饋迴圈，每本書都打不開。
  bool oom_ = false;

  std::deque<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;      // true = word attaches to previous with no break
  std::vector<bool> wordNoSpaceBefore;  // true = may break before token, but no synthetic space when joined
  std::vector<bool> wordIsFocusSuffix;  // true = token is the regular tail of a focus bold-prefix split
  // Zero-based visible Unicode-codepoint offsets in the spine body, stored as
  // uint16_t deltas from a shared base to keep this layout-only metadata small.
  // Pathological spans wider than uint16_t use sparse rebases; rendered
  // TextBlocks do not carry any of this metadata.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  std::deque<std::string> rubyTexts;
  BlockStyle blockStyle;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool isNaturalAlign;
  bool hasRtlWord;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<uint16_t> reorderedWidthsScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<bool> reorderedFocusSuffixScratch;
  std::vector<uint16_t> visualOrderScratch;

  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& noSpaceBeforeVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool hyphenationEnabled = false,
                      const bool focusReadingEnabled = false, const BlockStyle& blockStyle = BlockStyle())
      : blockStyle(blockStyle),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        isNaturalAlign(false),
        hasRtlWord(false) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               uint32_t visibleTextOffset = 0);
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  // 建置期間是否曾因低記憶體拒絕過 token（sticky）。解析器據此中止本章。
  bool hasOom() const { return oom_; }

  // v151：最後一次守衛拒絕的描述（靜態、先到先得、由 src 端讀走寫進 diag.log ——
  // LOG_ERR 在這台沒有序列埠的機器上等於丟掉，v150 的「索引失敗」因此零證據）。
  static char lastRefusal[96];
  // v190：按鍵盲區探針（只量、不讓路、不碰 ADC）。site 是插點小整數。
  static uint32_t buildGapMaxUs;
  static uint8_t buildGapSite;
  static uint32_t buildProbeCount;
  static void noteBuildProbe(uint8_t site);
  static void resetBuildProbeClock();
  static void stopBuildProbeClock();
  // v175：守衛拒絕【當下】的回呼（src 端掛 DiagLog 池快照）。fg-lowmem 的快照是在 suspendBuild
  // 釋放建置脈絡【之後】拍的，看不到吃掉記憶體的主嫌；這裡在拒絕的那一刻拍。函式指標維持
  // lib 不依賴 app 的分層（同 ImageBlock 的 relief hook）。
  using RefusalHook = void (*)(void*);
  static RefusalHook refusalHook_;
  static void* refusalCtx_;
  static void setRefusalHook(RefusalHook fn, void* ctx) {
    refusalHook_ = fn;
    refusalCtx_ = ctx;
  }
  // v31/v41 → v187：粗體閱讀。排版時把每個內文字升成粗體字面（REGULAR→BOLD、ITALIC→BOLD_ITALIC）；
  // 標題本來就粗的不變；沒有粗體字面的字型由 resolveStyle 退回。狀態是全域的，因為它跟
  // section 快取一起烤進去（在檔頭比對），開閱讀器與改設定時都要重新設。
  static void setBoldBodyText(bool enabled);
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true);
};
