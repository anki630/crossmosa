#pragma once

#include <Txt.h>

#include <WarmIdentity.h>

#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

// v118:純文字閱讀器改為【串流】—— 不再預先排版整本書。
//
// 舊設計:開書前把整份檔案排完,得到一個「每頁從第幾個位元組開始」的陣列(index.bin),
// 然後【每翻一頁又重排一次】,因為排好的行一個字都沒留。實測 745,717 位元組的中文檔要
// 4,379,996 ms(73 分鐘)才排完 2,225 頁,而那 73 分鐘沒有買到任何「顯示一頁」需要的東西。
//
// 新設計:只排「當下這一頁」。整份索引唯一買到的三樣東西各自用更便宜的方式取得:
//   1. 總頁數  → 不顯示精確值,改用位元組比例外推的估計值,前面加 `~` 誠實標示
//   2. 是不是最後一頁 → 比對位元組位移是否到檔尾
//   3. 往回翻  → 只記錄【真的造訪過】的頁首(下面的 backRing_),成本與閱讀量成正比,
//                而不是與書的長度成正比
// 閱讀位置因此改存【位元組位移】而非頁碼:位移不是字型的函數,所以改字級、改邊界、
// 轉螢幕方向都不會讓進度失效,也不需要重建任何東西。
class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int pagesUntilFullRefresh = 0;

  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // --- 串流狀態 ---
  size_t pageStartOffset_ = 0;  // 這一頁從檔案的第幾個位元組開始
  size_t nextPageOffset_ = 0;   // 下一頁的起點(由 loadPageAtOffset 算出,舊版算了卻丟掉)
  bool atLastPage_ = false;     // 這一頁排完就到檔尾了

  // 往回翻:一次配滿、永不成長的環形緩衝,存造訪過的頁首位移。
  // 256 格 × 4 bytes = 1 KB,以每頁約 335 位元組計可回溯約 85 KB(約 256 頁)。
  // 環空了(例如關機後重開)才走 findPreviousPageOffset() 從錨點往前重推。
  static constexpr uint16_t kBackRingSize = 256;
  uint32_t backRing_[kBackRingSize] = {};
  uint16_t backCount_ = 0;  // 環內有效筆數
  uint16_t backHead_ = 0;   // 下一個要寫入的位置

  // 估計頁數:用「已排頁面的平均位元組數」外推,指數移動平均平滑。
  // 只用於顯示(前面會加 `~`),任何邏輯判斷都不得依賴它。
  uint32_t avgBytesPerPage_ = 0;

  uint32_t lastYieldMs_ = 0;  // 讓步節流,由斷行迴圈內部使用

  // v119 分段儀器:v118 只量到排版,其餘各段都還是從 EPUB 換算來的估計值。
  uint32_t segPrewarmMs_ = 0;
  uint32_t segBwMs_ = 0;
  uint32_t segDispMs_ = 0;
  uint32_t segAaMs_ = 0;
  // v120:把 layout 再拆三段,回答 v119 量到的 232ms 到底花在哪
  uint32_t segReadMs_ = 0;  // 讀 8KB(含開檔/seek)
  uint32_t segFontMs_ = 0;  // 灌 advance 表
  uint32_t segWrapMs_ = 0;  // 實際斷行
  bool pendingForward_ = false;  // 繪製期間被按下、待消化的翻頁
  // v121 預取
  uint8_t diagWarmHit_ = 0;
  uint32_t diagAllocFail_ = 0;
  uint32_t diagDropped_ = 0;  // 繪製期間被按下、待消化的翻頁

  // Cached settings (幾何改變時要重算,但【不再】讓閱讀位置失效)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage(size_t pageOffset);
  // v119:offset 由呼叫端傳入而不是讀成員 —— 主任務會在 render 進行中改 pageStartOffset_
  // (v118 的 log 有 38/54 筆因此印出 next==off),狀態列必須畫「這一頁」的進度。
  void renderStatusBar(size_t offset) const;

  void initializeReader();
  void recomputeGeometry();  // 幾何(視窗寬、每頁行數、字型 ID);改字級或方向後重跑
  // 對齊到行首。'\n' 是 ASCII,所以下一個位元組必定是碼位的前導位元組 ——
  // 這同時解決了「按百分比跳轉會切在 UTF-8 序列中間」的問題。
  size_t alignToLineStart(size_t offset);

  WarmIdentity buildWarmIdentity(size_t offset) const;
  static bool prefetchShouldAbort(void* ctx);
  void prefetchNextPage(size_t nextOffset);

  void openReaderMenu();
  void jumpToPercent(int percent);
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset);

  void pushBackOffset(size_t offset);
  bool popBackOffset(size_t& outOffset);
  // 環空了才用:從 offset 之前一段距離找一個安全起點,往前推到剛好接上 offset。
  // 推得剛好 = 精確;越過了 = 使用者跳過位置,退回最後一個沒越過的頁首。
  size_t findPreviousPageOffset(size_t offset);

  void updatePageSizeEstimate(size_t pageBytes);
  int estimatedTotalPages() const;
  int estimatedCurrentPage() const;

  void saveProgress(size_t offset) const;
  void loadProgress();

 public:
  // v161：第 4 參數為新樹慣例（ReaderActivity 讓刷新節奏跨書延續）
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             int initialRefreshCountdown)
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
