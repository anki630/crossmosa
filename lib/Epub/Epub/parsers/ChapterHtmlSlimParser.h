#pragma once

#include <HalStorage.h>
#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/ParsedText.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include <Logging.h>

#include "Epub/VerticalText.h"
#include "Epub/css/CssParser.h"
#include "Epub/css/CssStyle.h"

class Page;
class GfxRenderer;
class Epub;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  std::shared_ptr<Epub> epub;
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)> completePageFn;
  std::function<void()> popupFn;  // Popup callback
  bool imagePopupFired = false;   // popupFn fired for the first image probe (single-shot)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  bool nextWordContinues = false;  // true when next flushed word attaches to previous (inline element boundary)
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  // Ruby text state
  bool inRuby = false;
  int rubyStartWordIndex = -1;
  bool collectingRubyText = false;
  std::string rubyTextBuffer;
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  // 直排：欄的位置，從右緣往【左】遞減（欄由右往左）。橫排不使用。
  int16_t currentPageNextX = 0;
  int fontId;
  // v284：橫排行距，**已解析好的像素值**（＝量到的字身框 × 檔位係數）。
  // ⚠️ 刻意不是「em 倍數 ＋ 每次現算」：`probeEmFP` 有兩條精度不同的路（advance 表建好前
  //    走整數 px 的備援、建好後走 12.4 定點），**同一章前後可能差 1px** → 頁界不一致，
  //    而快取身分只記使用者設定，看不出差別（codex 複查抓到）。所以由 Section 解析一次、
  //    連同這個像素值一起寫進 section 檔頭，整章凍結。
  int lineHeightPx;
  // v284：建置凍結的字身框（12.4 定點）。直排欄距也由它算 —— parser 內不再 probe。
  int32_t frozenEmFP_ = -1;
  // 直排的欄距係數（em 的倍數）。
  // ⚠️ **v284 更正**：本欄原本寫著「不能從行距反推 —— 它是 advanceY 的壓縮率，
  //    值域還隨字型家族重疊（同一個 1.0 在兩邊是不同檔位）」。**那個前提已經不成立**：
  //    v284 之後兩者同源（都是量到的字身框 × 檔位係數），只是這欄已先乘好成像素。
  //    仍然分開送，是因為**它們是兩個獨立的使用者設定**（橫排 `lineSpacing`／直排
  //    `readerColumnPitch`），不是因為推不出來。
  // ⚠️ 而它【必須進 section 檔頭】，否則換檔位會讀到用舊幾何排的快取。
  //    已在 Section.cpp 的 columnPitchTier 欄位（v104）。曾規劃摺進側檔檔名，已放棄。
  float columnPitchFactor = 1.50f;
  mutable int columnPitchCache_ = -1;  // v284：由 frozenEmFP_ 算一次就記住（見 .cpp columnPitchPx）
  bool vertColLogged = false;  // VERTCOL 每次建置只印一行
  uint8_t vertImgDropLogged = 0;  // VERTIMGDROP 上限 3 筆
  uint8_t imgPlaceLogged = 0;     // v255：IMGPLACE 每章上限 6 筆
  // 直排欄頂。由 layoutCurrentBlock 夾限後寫入，addColumnToPage 直接用 ——
  // 兩處各自讀 BlockStyle 就可能不一致，而 colTop + colLen <= viewportHeight 靠它們一致。
  int16_t verticalColTop = 0;
  // 直排的段落間距（跨欄軸）。由 CSS margin／padding 換算：先表達成「幾行」，
  // 再套成「幾個欄距」——一行空白 → 一欄空白。見 .cpp layoutCurrentBlock 的註解。
  int16_t verticalLeadGap = 0;
  int16_t verticalTrailGap = 0;
  // 上一個區塊的下緣間距，**還沒套用**——要跟下一個區塊的上緣間距【摺疊】
  // （CSS margin collapsing）。見 .cpp addColumnToPage。
  int16_t verticalPendingTrailGap = 0;
  bool verticalBlockFirstColumn = false;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  const CssParser* cssParser;
  bool embeddedStyle;
  uint8_t imageRendering;
  std::string contentBase;
  std::string imageBasePath;
  int imageCounter = 0;

  // Style tracking (replaces depth-based approach)
  struct StyleStackEntry {
    int depth = 0;
    bool hasBold = false, bold = false;
    bool hasItalic = false, italic = false;
    bool hasTextDecoration = false;
    CssTextDecoration textDecoration = CssTextDecoration::None;
    bool hasDirection = false;
    CssTextDirection direction = CssTextDirection::Ltr;
    bool hasSup = false, sup = false;
    bool hasSub = false, sub = false;
  };
  std::vector<StyleStackEntry> inlineStyleStack;
  std::vector<BlockStyle> blockStyleStack;  // accumulated block styles from open ancestor elements
  CssStyle currentCssStyle;
  bool effectiveBold = false;
  bool effectiveItalic = false;
  CssTextDecoration effectiveTextDecoration = CssTextDecoration::None;
  bool effectiveDirectionDefined = false;
  CssTextDirection effectiveDirection = CssTextDirection::Ltr;
  bool effectiveSup = false;
  bool effectiveSub = false;
  int tableDepth = 0;
  int tableRowIndex = 0;
  int tableColIndex = 0;
  bool listItemBulletOnly = false;  // true when currentTextBlock has only the <li> bullet

  // Anchor-to-page mapping: tracks which page each HTML id attribute lands on
  int completedPageCount = 0;
  std::vector<std::pair<std::string, uint16_t>> anchorData;
  std::string pendingAnchorId;          // deferred until after previous text block is flushed
  std::vector<std::string> tocAnchors;  // the list of anchors that are TOC chapter boundaries
  uint16_t xpathParagraphIndex = 0;
  uint16_t xpathListItemIndex = 0;
  // Canonical reading-position counter: zero-based Unicode codepoints in visible
  // <body> text. Token offsets flow through line breaking so every completed page
  // records the first source character it renders.
  uint32_t visibleTextOffset = 0;
  uint32_t partWordVisibleOffset = 0;
  uint32_t currentPageVisibleOffset = 0;
  bool currentPageVisibleOffsetSet = false;
  bool insideBody = false;
  bool syntheticCharacterData = false;
  uint16_t nonVisibleTextDepth = 0;

  // Footnote link tracking
  bool insideFootnoteLink = false;
  int footnoteLinkDepth = -1;
  FootnoteEntry currentFootnote = {};
  int currentFootnoteLinkTextLen = 0;
  // 連結文字被長度上限截掉了嗎（不是「剛好填滿」）。收尾時據此補省略號。
  // ⚠️ 這個 handler 會被 expat 分多次呼叫，所以省略號不能在累積的當下補 ——
  //    補了之後下一次呼叫又會接著寫。只有收尾時才知道文字真的結束了。
  bool currentFootnoteTruncated = false;
  std::vector<std::pair<int, FootnoteEntry>> pendingFootnotes;  // <wordIndex, entry>
  int wordsExtractedInBlock = 0;

  // Resumable parse state. The one-shot parseAndBuildPages() drives these
  // internally; the incremental section builder drives them across render ticks
  // so a large single chapter can yield between pages instead of blocking the UI
  // until the whole thing is laid out. parseFile_ and the expat parser stay alive
  // for the lifetime of the parse so it can be paused and resumed at buffer
  // boundaries.
  XML_Parser xmlParser_ = nullptr;
  HalFile parseFile_;
  uint32_t parseStartTime_ = 0;

  void updateEffectiveInlineStyle();
  // v194：失敗立刻回 false。latch 不等於離開 callback，呼叫點必須同步 return。
  bool startNewTextBlock(const BlockStyle& blockStyle);
  void flushPendingAnchor();
  void flushPartWordBuffer();
  void setCurrentPageVisibleOffset(uint32_t offset);
  void makePages();
  static EpdFontFamily::Style fontStyleForTextDecoration(CssTextDecoration decoration);
  static void applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css);
  static void applyTextDecorationToEntry(StyleStackEntry& entry, const CssStyle& css);
  void pushDecorationStyleEntry(CssTextDecoration defaultDecoration, const CssStyle& cssStyle);
  void emitHorizontalRule(const BlockStyle& blockStyle);
  // v194：nothrow 配 Page；失敗就 latch，走既有 lowmem／不提交路徑。
  bool ensureCurrentPage(const char* where);
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL defaultHandlerExpand(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  // v258：預排接手時補上 popup callback（預排的 startBuild 沒有給；同 showBuildPopup 自己的 buildPopupPending 閘門）。
  void setPopupFn(std::function<void()> fn) { popupFn = std::move(fn); }
  explicit ChapterHtmlSlimParser(
      std::shared_ptr<Epub> epub, const std::string& filepath, GfxRenderer& renderer, const int fontId,
      const int lineHeightPx, const int32_t frozenEmFP, const bool extraParagraphSpacing,
      const uint8_t paragraphAlignment,
      const uint16_t viewportWidth, const uint16_t viewportHeight, const bool hyphenationEnabled,
      const bool focusReadingEnabled,
      const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>& completePageFn,
      const bool embeddedStyle, const std::string& contentBase, const std::string& imageBasePath,
      const uint8_t imageRendering = 0, std::vector<std::string> tocAnchors = {},
      const std::function<void()>& popupFn = nullptr, const CssParser* cssParser = nullptr,
      // 直排的欄距檔位。
      //
      // ⚠️ **沒有 `verticalLayout` 參數，這是刻意的。** 軸向分流一律讀
      //    `renderer.isVerticalLayout()` —— 排版（layoutCurrentBlock）與繪製
      //    （TextBlock::render）必須看同一個旗標，存第二份成員遲早分岔。
      //    我一度想在這裡加「spec 與 renderer 是否一致」的斷言，但那是同義反覆：
      //      · 建置期：Section::startBuild 與 buildSomeMore 的旗標同源於 buildVertical_
      //      · 繪製期：檔頭比對（spec.verticalLayout != fileVerticalLayout）保證
      //        用另一個軸向排過的快取會被丟掉重排
      //    不變量已由結構保證，不值得為此把 GfxRenderer.h 拉進這個標頭。
      const uint8_t columnPitchTier = 1)

      : epub(epub),
        filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineHeightPx(lineHeightPx),
        frozenEmFP_(frozenEmFP),
        columnPitchFactor(vtext::columnPitchForTier(columnPitchTier)),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        completePageFn(completePageFn),
        popupFn(popupFn),
        cssParser(cssParser),
        embeddedStyle(embeddedStyle),
        imageRendering(imageRendering),
        contentBase(contentBase),
        imageBasePath(imageBasePath),
        tocAnchors(std::move(tocAnchors)) {}

  ~ChapterHtmlSlimParser();

  // One-shot parse: builds every page before returning (begin + step* + finish).
  bool parseAndBuildPages();

  // Resumable parse, for the incremental section builder. Drive as:
  //   if (!beginParse()) fail;
  //   loop: switch (parseStep()) { More: keep going / yield; Done: finishParse(); Error: abortParse(); }
  // Pages are emitted via completePageFn as they complete during parseStep(), so
  // the caller can stop once enough pages are built and resume on a later tick.
  enum class ParseStatus { More, Done, Error };
  bool beginParse();
  // v6/v149：建置期間曾因低記憶體被 ParsedText 的 addWord 守衛拒絕過 token（sticky）。
  // 一旦成立就中止本章 —— 掉過字的章節【不可】提交成快取（教訓 A-20：壞快取住在
  // SD 卡上，重刷韌體清不掉）。
  bool buildAborted_ = false;
  void latchBuildAborted();
  bool hasBuildAborted() const { return buildAborted_; }

  ParseStatus parseStep();
  bool finishParse();  // flush the trailing page and tear down; returns true
  void abortParse();   // tear down without flushing (error / abandon)

  void addLineToPage(std::shared_ptr<TextBlock> line, uint32_t visibleOffset);
  // 直排：一欄放進頁面（欄由右往左）。
  void addColumnToPage(std::shared_ptr<TextBlock> column, uint32_t visibleOffset, int tokensInColumn);
  // 兩個排版呼叫點共用的分派：橫排走 layoutAndExtractLines，直排走 layoutAndExtractColumns。
  // ⚠️ 兩個呼叫點都要走這裡 —— 教訓 v149：「只守一處等於沒守」。
  void layoutCurrentBlock(bool includeLast);
  // 直排：讓圖片／<hr> 獨占一頁（它們仍走橫排游標，會與欄互疊）。見 .cpp 的註解。
  bool verticalBeginIsolated(uint32_t visibleTextOffset);
  void verticalEndIsolated();
  int columnPitchPx() const;
  // v275：**巢狀區塊的左右內縮會累加，而累加沒有上限** —— `getCombinedBlockStyle` 是父子相加，
  //   `fromCssStyle` 只夾了【單一元素】的 2em。實例（2026-09-17 實機回報）：
  //   `div{margin:1em;padding:1em}` ＋ `p{margin-left:2em}` ＝ 左 4em ＋ 右 2em，
  //   512px 的版心只剩 188px ＝ **一行 4.1 個字**。這種 CSS 是為寬螢幕寫的，在 528px 上就是災難。
  //   → 對**累加後**的總內縮設上限，兩側等比例縮（左右比例保住，縮排的視覺線索還在）。
  //   ⚠️ **只在橫排做**：直排的欄幾何另有一套，而且目前直排的表現是好的，不要順手動它。
  [[nodiscard]] int16_t maxTotalHorizontalInset() const;
  void clampHorizontalInsets(BlockStyle& style) const;
  mutable int16_t maxTotalInsetCache_ = -1;  // 版心與字級在一次解析裡不變 → 只算一次
  // v276：**垂直空間儀器**（純觀測，不改任何排版行為）。
  //   要查的事：某本中文書的一章，一頁 11 行只用 4–5 行——標題各佔一整頁、段落之間空 268px，
  //   而那本書的 CSS 上 `p{margin:0}`、`h1{margin:0}`，找不到來源。**推理已經錯兩次，改用量的。**
  //   裝在 `makePages()`（區塊級上下間距唯一的套用點）＋ `addLineToPage`（行數）。
  //   ⚠️ 額度要夾住：diag.log 有大小上限，撞到會**無聲停寫**（memory
  //      `diagnostics-need-their-own-failure-audit`）。每章 24 行、每次開機 120 行封頂。
  uint16_t vspaceBlockSeq_ = 0;   // 本章第幾個區塊
  uint16_t vspaceLines_ = 0;      // 這個區塊吐了幾行（addLineToPage 累加）
  static uint16_t vspaceBudget_;  // 每次開機的總額度
  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorData; }

  // Byte progress of the in-flight parse, used to estimate a still-building section's total page
  // count (a giant single-spine book never fully lays out, so its real count is unknown). Valid
  // between beginParse() and finishParse()/abortParse().
  size_t parseBytesConsumed() { return parseFile_ ? parseFile_.position() : 0; }
  size_t parseTotalBytes() { return parseFile_ ? parseFile_.size() : 0; }
};
