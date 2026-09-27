#pragma once

#include <EpdFontFamily.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

namespace zhuyin {
class ZhuyinTxtCursor;
}  // namespace zhuyin

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
  bool greedyLineBreaks_ = false;  // 見 setGreedyLineBreaks
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
  // v272／v273：這一段可不可以做約物擠壓（兩條斷行路徑與定位共用同一個判準）。
  //   v271 曾在這裡做行尾懸掛，v273 移除（橫排維持方格，見 .cpp 的說明）。
  [[nodiscard]] bool punctFittingAllowed() const;
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
  // ---- 注音（見 public 的 enableZhuyin）----
  struct ZhuyinState;
  std::unique_ptr<ZhuyinState> zy_;
  bool zhuyinLive();
  void zhuyinStop(bool degraded);
  void zhuyinFeed(const std::string& word, bool attachToPrevious);
  size_t zhuyinCoveredUnits(const std::vector<size_t>& breaks, size_t count);
  // 直排：段落結束 → 全部送出、回 false；還沒結束而注音在跑 → 回 true（呼叫端要用 zhuyinCoveredColumns 限制欄數）
  bool zhuyinPrepareBatch(bool final);
  size_t zhuyinCoveredColumns(const std::vector<uint16_t>& unitSrcWord, const std::vector<uint16_t>& unitByteBegin,
                              const std::vector<uint16_t>& unitByteLen, const std::vector<uint16_t>& columnStarts,
                              size_t columnCount, size_t unitCount);
  void zhuyinAbandon();  // 字詞被整批丟掉（直排的 bail）：佇列裡那些字不會再有人取 → 這一段之後不標
  // 一行要了清單、TextBlock 卻因為配不到而沒掛上（SwapStats::listOom 在建構時加一）→ 這一段記成降級（章節提交成「沒注音」、
  // 之後記憶體夠時重排），但不停止：後面的行照常標（codex 整合複查 A1：否則那一行在「開」的快取裡永遠沒有注音）。
  void zhuyinNoteBuilt(const zhuyin::SwapBatch& requested, uint32_t listOomBefore);
  bool zhuyinTake(uint32_t cp, uint16_t* out);
  // 一行（或一欄）照順序的每個漢字拿讀音、組清單（放在引擎的緩衝裡，建 TextBlock 之前用完）。
  // annotated(ctx, i) ＝ 第 i 個字詞是出版社標注的（ruby）→ 讀音照拿（保持對齊）但不換。
  using ZhuyinAnnotatedFn = bool (*)(const void* ctx, size_t wordIndex);
  zhuyin::SwapBatch zhuyinLineSwaps(const std::vector<std::string>& lineWords, ZhuyinAnnotatedFn annotated,
                                    const void* ctx);

  void extractLine(size_t breakIndex, int pageWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  // 建構與解構都在 .cpp：注音狀態（ZhuyinState）在這裡是不完整型別
  explicit ParsedText(bool extraParagraphSpacing, bool hyphenationEnabled = false, bool focusReadingEnabled = false,
                      const BlockStyle& blockStyle = BlockStyle());
  ~ParsedText();

  // 注音（P2 設計第 2 節）：這一段的破音字要不要換成注音字形。在第一個 addWord 之前呼叫；
  // fontId ＝ 排版與繪製用的字型，必須就是引擎登記的那一個（否則不開）。
  //   - EPUB：enableZhuyin —— 字詞進來時餵給 session，取出行時照漢字順序拿讀音；
  //   - TXT：useZhuyinCursor —— 讀音來源是整頁共用的游標（它自己往前、往後讀檔），不餵、也不必等。
  //     generation ＝ 建游標時引擎的世代（游標拿的是那個引擎的資料與暫存）。⚠️ 游標只核對碼位，所以這一段只要有
  //     任何一個漢字不會交給它（接不上、中途停止標注）就讓整個游標作廢 —— 否則下一段的字會對到這一段剩下的讀音。
  // 任何失敗都只讓這一段之後不標（v2 字型下＝破音字不標）；資源或 I/O 的失敗另外記成降級（SwapStats::degradedEvents）。
  // docAnnotated（v342，B 路線）：這一段屬於預先標注的章節（<head> 有 zhuyin-ivs 標記）→ 沒有 bpmfvs 選擇符號的破音字
  //   用第一個讀音、不採用解析器的判斷（ZhuyinSession::setDocumentAnnotated）。
  void enableZhuyin(int fontId, bool docAnnotated = false);
  // v342：這一段屬於預先標注的章節（章節 <head> 的 bpmfvs 標記）。解析器在讀到任何標籤之前就建好第一個區塊
  //   （beginParse），空區塊之後會被第一個段落重用、不會再 enableZhuyin → 認到標記的當下要補設給當前這一個。
  void setZhuyinDocAnnotated(bool on);
  void useZhuyinCursor(zhuyin::ZhuyinTxtCursor* cursor, int fontId, uint32_t generation);
  bool zhuyinActive() const;  // 這一段還在標注（排版端用它把軟性分批壓到 200 詞）
  bool zhuyinDegraded() const;  // 這一段因資源或 I/O 停止標注、或有一行的清單配不到（章節身分要寫「沒注音」）
  // 排版端每加一個字詞問一次：session 佇列裡等著被取出的漢字超過這個數 → 現在就排一批（軟性分批，不含最後一行）。
  // 真正的上界靠這個，不靠字詞數：一個字詞可以有好幾個漢字、一次字元回呼可以進好幾百個（codex 整合複查 A2）。
  // 佇列 512 ＋ 窗口 128 才會失敗；這裡 256 ＋ 一個字詞（≤ 200 B ＝ 66 字）＋ 留下的最後一行，離那裡很遠。
  static constexpr size_t kZhuyinFlushQueued = 256;
  bool zhuyinWantsFlush() const;

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
  // v240：txt 閱讀器專用。開啟時橫排改走 greedy 斷行（computeHyphenatedLineBreaks 那條迴圈），
  // 但【不斷字】。預設關閉 → EPUB 的兩條既有分支（DP／斷字 greedy）逐字不變。
  // 為什麼 txt 需要 greedy：DP（最小參差）不具前綴穩定性 —— 同一段從不同行首開始排，斷點就不同，
  // 所以分頁鏈依起點而定、往前翻頁永遠對不準。greedy 從任一行首重排，後面的斷點都與整段排相同。
  void setGreedyLineBreaks(const bool v) { greedyLineBreaks_ = v; }

  // v151：最後一次守衛拒絕的描述（靜態、先到先得、由 src 端讀走寫進 diag.log ——
  // LOG_ERR 在這台沒有序列埠的機器上等於丟掉，v150 的「索引失敗」因此零證據）。
  static char lastRefusal[96];
  // v190：按鍵盲區探針（只量、不讓路、不碰 ADC）。site 是插點小整數。
  static uint32_t buildGapMaxUs;
  static uint8_t buildGapSite;
  static uint32_t buildProbeCount;
  static void noteBuildProbe(uint8_t site);

  // v252：EPUB 建置分項計時（累計 µs，閱讀器在 noteBuildStart 歸零、BUILD end 印 BUILDPROF）。
  // txt 每頁排版中位 39ms（TXTPAGE），EPUB 約 190ms，而兩者用同一個 layoutAndExtractColumns ——
  // 差距在引擎外面，這組數字回答「在哪」。全部是每段落／每頁／每次 SD 讀才計一次，不在每個字上計時。
  // 巢狀關係：xml ⊃ {cd, el}；cd／el ⊃ lay（layCd＝發生在文字回呼裡的那份）；lay ⊃ {adv, proc}；proc ⊃ ser。
  struct BuildProf {
    uint64_t rdUs = 0;     // parseStep 讀章節 HTML（SD）
    uint64_t xmlUs = 0;    // XML_ParseBuffer（含所有回呼）
    uint64_t cdUs = 0;     // characterData 回呼
    uint64_t elUs = 0;     // start／endElement 回呼
    uint64_t layUs = 0;    // layoutAndExtractColumns／Lines 整段
    uint64_t layCdUs = 0;  // 其中發生在 characterData 裡的
    uint64_t advUs = 0;    // ensureSdCardFontReady（字寬預載）
    uint64_t procUs = 0;   // 欄／行交給頁面（addColumnToPage／addLineToPage，含換頁序列化）
    uint64_t serUs = 0;    // Section::onPageComplete（頁序列化寫 SD）
    uint64_t imgUs = 0;    // 圖片檔頭探測
    uint32_t words = 0;    // addWord 次數
    uint32_t layCalls = 0;
    uint32_t cdCalls = 0;
  };
  static BuildProf buildProf;
  static bool buildProfInCd;
  static int64_t profNowUs();

  // v252：背景建置期間的按鍵輪詢。閱讀器只在【主任務的建置 tick 內、持 RenderLock】時設定，離開 tick 前清掉
  // （render task 的同步建置也會走探針點，但那時 hook 一定是 nullptr —— 兩者被 RenderLock 串行）。
  // 探針點每 ≥15ms 呼叫一次（有了事件也繼續，時間戳才準）；hook 回 true＝有待處理的輸入事件，
  // buildInputPending() 讓 Section 在這一步結束時讓路。
  static void setBuildInputPollHook(bool (*fn)());
  static bool buildInputPending();

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
  // 幾何證人。⚠️ **原本是 one-shot bool —— 那讓它只印「開機後第一個字型／字級」，**
  //    而要證明的主張（每個字型 × 每個字級的 rotcross 都對）恰恰要靠換字型才驗得到
  //    （2026-09-11 對抗複查抓到，B-22 的變形：儀器裝得到，但覆蓋不到要證明的事）。
  //    改成「(fontId, em) 變了就再印一次」，上限 8 筆免得洗版。
  static inline uint8_t vertGeoLogged = 0;
  static inline int vertGeoKey = -1;
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
