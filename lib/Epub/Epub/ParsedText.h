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
  // ⚠️ **兩軸共用。** `isNaturalAlign` 先前只在 `layoutAndExtractLines`（橫排）裡賦值，
  //    而它的建構子初始值是 false → 直排讀到的永遠是 false。
  //    v215 的縮排閘門因此恆不成立，把直排的段首縮排整個拿掉了（實機未上線前抓到）。
  //    抽成一個函式讓兩條路徑不可能再分岔。
  void updateNaturalAlign();
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

  // 直排的診斷輸出。
  // ⚠️ `lib/Epub` 看不到 `src/util/DiagLog.h`，而這台機器**沒有序列埠 → LOG_ERR 等於丟掉**
  //    （v207 的 VERTGEO 就是這樣一行都沒進 diag.log）。所以走 hook：src 端接上 DiagLog。
  //    沒接的話是靜默 no-op —— 這是刻意的，桌面測試不需要它。
  static void (*vertDiagHook)(const char* line);
  static void vertDiag(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
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
  // 直排的禁則 delta（分隔號 / ／、連接號、〞 等橫排表沒有的）。
  // ⚠️ 影響【切詞】→ 影響版面 → 改了必須跳 SECTION_FILE_VERSION。
  static void setVerticalKinsoku(bool enabled);
  static bool verticalKinsoku();
  // ⚠️ **必須用 RAII，不能設一次就算了。**（複查抓到）
  //    它的孿生旗標 `Section::buildVertical_` 用的是 `GfxRenderer::VerticalScope`，
  //    每個 tick 重設、離開自動還原；這個卻只在 startBuild 設一次、永不還原 →
  //    讀完直排書之後，設定頁預覽（TextSettingsPreview 直接呼叫
  //    layoutAndExtractLines，不經過 startBuild）會用**直排的禁則**切橫排的詞。
  //    兩個旗標必須同一種生命週期，否則遲早分岔。
  class VerticalKinsokuScope {
   public:
    explicit VerticalKinsokuScope(const bool v) : prev_(verticalKinsoku()) { setVerticalKinsoku(v); }
    ~VerticalKinsokuScope() { setVerticalKinsoku(prev_); }
    VerticalKinsokuScope(const VerticalKinsokuScope&) = delete;
    VerticalKinsokuScope& operator=(const VerticalKinsokuScope&) = delete;

   private:
    bool prev_;
  };
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  // ── 直排（縦書き）——定義在 ParsedTextVertical.cpp ────────────────────────
  // ⚠️ 刻意放另一個 .cpp：橫排的 ParsedText.cpp【一行都沒改】，複查時 diff 乾淨。
  //    走平行迴圈而不是在 extractLine 加分支的理由見帳本「V1 插入點測繪」：
  //    extractLine 已 421 行、三個互斥定位分支，而它的 DP 目標函式（remainingSpace²）
  //    是為兩端對齊設計的，直排不做兩端對齊 → 那個最佳化目標在直排沒有意義。
  // 幾何證人只印第一次（每次開機一次就夠了，它不隨頁面變）。
  static inline bool vertGeoLogged = false;
  static inline bool vertHangLogged = false;
  // 縮排閘門的證人：**只在閘門真的擋下時才印**（非自然對齊的區塊）。
  // 這樣才證明得了「閘門會分辨」，而不只是「常數改對了」。上限 3 筆免得洗版。
  static inline uint8_t vertIndGateLogged = 0;
  // v219：縦中横的墨水置中、以及 UAX #50 旋轉表的證人（各自上限，避免刷爆 diag.log）
  // ⭐ 首行縮排只能套用一次，而「一次」的範圍是**整個區塊**不是一次呼叫。
  //    ⚠️ 長段落會被 soft flush 排【好幾次】（`layoutCurrentBlock(false)`），
  //      每次都是新的一趟 `layoutAndExtractColumns`、`firstChunk` 都是 true →
  //      第二趟起又縮兩格 ＝ **段落中間出現假的段落起頭**（複查抓到）。
  //    ⚠️ 這個旗標必須是【成員】而不是區域變數：`ParsedText` 物件在 soft flush
  //      之間是活的（words 被消耗掉但物件留著），新區塊才會 `reset()` 出新物件。
  bool verticalIndentApplied = false;
  static inline uint8_t vertTcyLogged = 0;
  static inline uint8_t vertRotLogged = 0;
  // v220：西文整串旋轉、以及懸掛的實際落點（兩者都只在實機看得到）
  static inline uint8_t vertWordRotLogged = 0;
  static inline uint8_t vertHungLogged = 0;
  void layoutAndExtractColumns(const GfxRenderer& renderer, int fontId, uint16_t columnLength,
                               // ⚠️ 第三個參數是**這一欄用掉幾個 token**，不是幾個格子。
                               //    註腳歸頁的佇列是用 token 索引排的，而直排一個 token
                               //    可以吐出好幾個格（禁則黏合、縮略詞逐字、混合 token 切段）
                               //    —— 拿格數去比會提前跨過門檻，註腳早一頁出現（複查抓到）。
                               const std::function<void(std::shared_ptr<TextBlock>, uint32_t, int)>& processColumn,
                               bool includeLastColumn = true);

  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                             bool includeLastLine = true);
};
