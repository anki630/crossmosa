#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <EpdFontData.h>  // v118:fp4::toPixel(單趟斷行的定點累加)
#include <FontCacheManager.h>
#include <GfxRenderer.h>

#include <cstring>  // v118:memcpy
#include <optional>  // v121:預取讓 PrewarmScope 只在冷路徑存在(內建字型備援路徑的單碼位緩衝)
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "EpubReaderPercentSelectionActivity.h"
#include <SdCardFont.h>

#include "SdCardFontSystem.h"

#include "ReaderUtils.h"
#include "TxtReaderMenuActivity.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DiagLog.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
// v118 進度檔格式(12 bytes)。位元組 0-1 維持舊語意(頁碼),所以刷回舊韌體讀到的仍是
// 合理的頁碼而不是垃圾;位元組 2-3 是新格式識別碼 —— 所有出貨過的舊版都把它們寫成 0,
// 所以「等於 TX」可以無歧義地分辨新舊。位元組 4-7 是真正的錨點(位元組位移),8-11 是
// 平均每頁位元組數(讓重開機後的估計頁數立刻穩定,不必等累積)。
constexpr uint8_t PROGRESS_MAGIC0 = 'T';
constexpr uint8_t PROGRESS_MAGIC1 = 'X';
constexpr size_t PROGRESS_SIZE = 12;
// 舊 index.bin 的檔頭大小(只在一次性遷移時用來把舊頁碼換算成位移)
constexpr size_t LEGACY_INDEX_HEADER_V3 = 30;
constexpr size_t LEGACY_INDEX_HEADER_V4 = 35;
constexpr uint32_t LEGACY_INDEX_MAGIC = 0x54585449;  // "TXTI"

// v116 索引節奏常數。
// 讓步改成「以時間為準」而非以頁數為準:舊條件是 pageOffsets.size() % 20,而整章不換行的
// 中文 txt 整份只產生三四頁 → 全程一次都沒讓出。
constexpr uint32_t INDEX_YIELD_INTERVAL_MS = 40;
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  currentPageLines.clear();

  // v118 順手補上:txt 從來沒有呼叫過 setProgress,所以主畫面續讀卡與最近閱讀清單上
  // 它一直沒有百分比(EPUB 從 v31 起就有)。位移進度讓這件事變成兩行。
  if (txt) {
    const size_t fileSize = txt->getFileSize();
    const int pct = fileSize != 0 ? static_cast<int>(static_cast<double>(pageStartOffset_) * 100.0 / fileSize + 0.5) : 0;
    RECENT_BOOKS.setProgress(txt->getPath(), static_cast<uint8_t>(pct > 100 ? 100 : pct));
  }

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}




bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  const uint32_t readStartMs = millis();
  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  segReadMs_ = millis() - readStartMs;

  // v120:只餵「這一頁大概會用到」的位元組,不是整個 8KB 分塊。
  // 舊行為把 2,730 個碼位灌進 advance 表,而一頁只用到約 112 個 —— 約 24 倍的浪費,
  // 而那正是 v119 量到 layout=232ms 的頭號嫌疑。
  // 取三倍的平均頁長當上限(實測平均約 334 位元組,所以約 1KB),留足餘裕;
  // 萬一這一頁真的更長,超出的字會落到 getGlyph 的 SD 路徑,結果【逐位元組相同】,
  // 只是那幾個字慢一點 —— 所以這個裁切不可能改變任何斷點。
  const uint32_t fontStartMs = millis();
  if (renderer.isSdCardFont(cachedFontId)) {
    const size_t budget = avgBytesPerPage_ != 0 ? static_cast<size_t>(avgBytesPerPage_) * 3 : 1024;
    const size_t warmLen = std::min(chunkSize, std::max<size_t>(budget, 512));
    const uint8_t saved = buffer[warmLen];
    buffer[warmLen] = '\0';  // 暫時截斷(buffer 有 chunkSize+1 的空間)
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
    buffer[warmLen] = saved;
  }
  segFontMs_ = millis() - fontStartMs;
  const uint32_t wrapStartMs = millis();

  // Parse lines from buffer
  size_t pos = 0;

  // v116 修正(實機回報「取消沒有作用」):這個計數器【必須】活在整頁的範圍,
  // 不能宣告在下面每一條可視行的迴圈裡 —— 一般中文段落的回溯迴圈只跑一百多次,
  // 每行歸零的計數器永遠到不了門檻,於是讓步與取消的檢查一次都不會執行。
  // 門檻取 16:回溯的每一次迭代都含一次 substr(配置+複製)加一次走訪整條候選行的量寬,
  // 是微秒等級,對照之下每 16 次問一次 millis() 可以忽略。
  uint32_t abortTick = 0;

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // v118:單趟前向斷行,取代原本的回溯搜尋。
    //
    // 舊做法每退一個候選斷點就 substr 一份前綴、再把整條從頭量一次;中文沒有空格,
    // rfind(' ') 永遠找不到,所以每次只退一個字 —— 對來源行長度是平方級。
    // 用使用者的真實檔案(745,717 位元組、2,225 頁)模擬:每頁 406,342 次字寬查詢、
    // 1,475 次子字串配置、1.19 MB 記憶體複製。本版每頁約 103 次查詢、0 次子字串配置。
    //
    // 關鍵在 cand*:同時記住「最後一個合法斷點在哪」與「累加到那裡的定點寬度」,
    // 所以超寬時跳回斷點是【相減】而不是【重量】,每個碼位一輩子只查一次寬度。
    // 寬度語意與 getTextAdvanceX 完全一致:整條累加成定點,最後才 toPixel 一次
    // (逐字捨入再相加會與繪製不符,見 GfxRenderer::getCodepointAdvanceFP 的說明)。
    const uint8_t* const lineBuf = buffer + pos;

    size_t scan = 0;       // 已走訪的位元組(相對 lineBuf),永不後退
    size_t lineStart = 0;  // 目前尚未輸出的可視行起點,恆為碼位邊界
    int32_t lineFP = 0;    // [lineStart, scan) 的 12.4 定點寬度總和
    uint32_t prevCp = 0;
    bool hasPrev = false;
    bool candValid = false;
    size_t candEnd = 0;   // 可視行輸出到這裡(不含)
    size_t candNext = 0;  // 下一條可視行從這裡開始(空格斷點會跨過那個空格)
    int32_t candFP = 0;   // 從 lineStart 累加到 candNext 的寬度

    auto emitLine = [&](const size_t from, const size_t to) {
      outLines.emplace_back(reinterpret_cast<const char*>(lineBuf + from), to - from);
    };

    if (displayLen == 0) {
      outLines.emplace_back();  // 空的來源行仍要產生一條空的可視行(原行為)
    }

    while (scan < displayLen && static_cast<int>(outLines.size()) < linesPerPage) {
      const size_t cpStart = scan;
      const unsigned char* p = lineBuf + scan;
      const uint32_t cp = utf8NextCodepoint(&p);
      const size_t cpEnd = static_cast<size_t>(p - lineBuf);
      if (cp == 0 || cpEnd <= cpStart || cpEnd > displayLen) {
        break;  // 壞的 UTF-8 或內嵌 NUL:停在這裡,已輸出的行仍然有效
      }

      // 讓步:v118 之後排一頁只有約 101 次字寬查詢,這裡實務上不會觸發,但留著讓
      // 病態輸入(單一超長來源行)不會獨佔 render task。
      if ((++abortTick & 0x0F) == 0) {
        const uint32_t now = millis();
        if (now - lastYieldMs_ >= INDEX_YIELD_INTERVAL_MS) {
          vTaskDelay(1);
          lastYieldMs_ = now;
        }
      }

      // 逐碼位取寬。內建備援字型有字距對,沒有正確的逐碼位答案,所以退回整字量寬並
      // 轉成定點 —— 只在「SD 字型不可用」這個已經降級的狀態才會走到,行距誤差有界。
      int32_t advFP = renderer.getCodepointAdvanceFP(cachedFontId, cp, EpdFontFamily::REGULAR);
      if (advFP == GfxRenderer::kAdvanceUnavailable) {
        char one[8] = {0};
        memcpy(one, lineBuf + cpStart, cpEnd - cpStart);
        advFP = static_cast<int32_t>(renderer.getTextAdvanceX(cachedFontId, one, EpdFontFamily::REGULAR)) << 4;
      }
      const int32_t fpBefore = lineFP;
      lineFP += advFP;

      // 候選斷點。禁則走既有的 hasCjkBreakOpportunityBetween(v118 搬進 lib/Utf8),
      // 與 EPUB 同一份規則:左字元禁下、右字元禁上都會讓該處不成立。
      if (hasPrev && hasCjkBreakOpportunityBetween(prevCp, cp)) {
        candValid = true;
        candEnd = cpStart;
        candNext = cpStart;
        candFP = fpBefore;
      }
      if (cp == static_cast<uint32_t>(' ') && cpStart > lineStart) {
        candValid = true;
        candEnd = cpStart;
        candNext = cpEnd;
        candFP = lineFP;  // 含被略過的空格寬度,否則下一行會永久多算一個空格
      }

      scan = cpEnd;
      prevCp = cp;
      hasPrev = true;

      if (fp4::toPixel(lineFP) <= viewportWidth) {
        continue;
      }

      if (candValid) {
        emitLine(lineStart, candEnd);
        lineFP -= candFP;
        lineStart = candNext;
        candValid = false;
        if (lineStart == scan) {  // 斷點略過的正是剛掃到的空格,下一行目前是空的
          lineFP = 0;
          hasPrev = false;
          prevCp = 0;
        }
      } else if (cpStart > lineStart) {
        // 沒有任何合法斷點(超長英文單字、整行不可斷標點):在造成超寬的碼位【之前】強制斷。
        emitLine(lineStart, cpStart);
        lineStart = cpStart;
        lineFP = advFP;
      } else {
        // 單一碼位本身就超寬:仍要輸出完整碼位,絕不切在 UTF-8 續接位元組中間,
        // 也絕不產生零長度的可視行(舊版的 `breakPos = 1` 是位元組不是字元,會切出豆腐)。
        emitLine(cpStart, cpEnd);
        lineStart = cpEnd;
        lineFP = 0;
        hasPrev = false;
        prevCp = 0;
      }
    }

    // 掃完了但還有尾巴沒輸出
    if (lineStart < displayLen && static_cast<int>(outLines.size()) < linesPerPage) {
      emitLine(lineStart, displayLen);
      lineStart = displayLen;
    }

    // Determine how much of the source buffer we consumed
    if (lineStart >= displayLen) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      pos = pos + lineStart;
      break;
    }
  }

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  segWrapMs_ = millis() - wrapStartMs;

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

  return !outLines.empty();
}


void TxtReaderActivity::renderPage(const size_t pageOffset) {
  const int lineHeight = renderer.getLineHeight(cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  // v121:字型預取。上一次 render 尾端已經把【這一頁】的字灌進快取並保留,
  // 身分逐欄位相符才算數(任何 clearCache/prewarmCache 都會讓身分失效,機制內建)。
  // 相符就整個跳過掃描與 prewarm —— 那是 v120 量到的 286ms、目前最大的單筆軟體成本。
  const uint32_t prewarmStartMs = millis();
  const bool warmHit = fcm->warmIdentity().matches(buildWarmIdentity(pageOffset));
  diagWarmHit_ = warmHit ? 1 : 0;
  // scope 必須活到 renderPage 結束(它的解構子才清快取),所以用 optional 而不是內層區塊。
  std::optional<FontCacheManager::PrewarmScope> scope;
  if (!warmHit) {
    scope.emplace(fcm->createPrewarmScope());
    renderLines();  // scan pass — text accumulated, no drawing
    scope->endScanAndPrewarm();
  }
  segPrewarmMs_ = millis() - prewarmStartMs;

  // BW rendering
  const uint32_t bwStartMs = millis();
  renderLines();
  renderStatusBar(pageOffset);
  segBwMs_ = millis() - bwStartMs;

  const uint32_t dispStartMs = millis();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  segDispMs_ = millis() - dispStartMs;

  segAaMs_ = 0;
  if (SETTINGS.textAntiAliasing) {
    const uint32_t aaStartMs = millis();
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
    segAaMs_ = millis() - aaStartMs;
  }
  // scope destructor clears font cache via FontCacheManager
}







// ---------------------------------------------------------------------------
// v118:串流導覽
// ---------------------------------------------------------------------------

void TxtReaderActivity::pushBackOffset(const size_t offset) {
  backRing_[backHead_] = static_cast<uint32_t>(offset);
  backHead_ = static_cast<uint16_t>((backHead_ + 1) % kBackRingSize);
  if (backCount_ < kBackRingSize) {
    backCount_++;
  }
}

bool TxtReaderActivity::popBackOffset(size_t& outOffset) {
  if (backCount_ == 0) {
    return false;
  }
  backHead_ = static_cast<uint16_t>((backHead_ + kBackRingSize - 1) % kBackRingSize);
  backCount_--;
  outOffset = backRing_[backHead_];
  return true;
}

// 環空了(關機後重開、或往回超過 256 頁)才走這裡:從 offset 之前一段距離找一個
// 對齊到行首的起點,往前逐頁推進到剛好接上 offset。推得【剛好】才是精確答案;
// 越過了代表這個起點不在同一條分頁鏈上,換更遠的起點再試一次。
// 往回找上一個換行的下一個位元組。每次只讀 128 bytes(專案的堆疊預算是 256)。
size_t TxtReaderActivity::alignToLineStart(size_t p) {
  constexpr size_t kWin = 128;
  uint8_t win[kWin];
  for (int step = 0; step < 32 && p > 0; ++step) {
    const size_t from = p > kWin ? p - kWin : 0;
    const size_t len = p - from;
    if (!txt->readContent(win, from, len)) {
      return from;
    }
    for (size_t i = len; i-- > 0;) {
      if (win[i] == '\n') {
        return from + i + 1;
      }
    }
    p = from;
  }
  return 0;
}

size_t TxtReaderActivity::findPreviousPageOffset(const size_t offset) {
  if (offset == 0) {
    return 0;
  }
  const uint32_t avg = avgBytesPerPage_ != 0 ? avgBytesPerPage_ : 512;

  size_t fallback = 0;
  for (int k = 4; k <= 64; k *= 4) {
    const size_t span = static_cast<size_t>(avg) * static_cast<size_t>(k);
    size_t start = offset > span ? offset - span : 0;
    start = alignToLineStart(start);

    size_t cur = start;
    size_t prev = start;
    std::vector<std::string> scratch;
    int guard = 0;
    while (cur < offset && guard++ < 512) {
      scratch.clear();
      size_t next = cur;
      if (!loadPageAtOffset(cur, scratch, next) || next <= cur) {
        break;
      }
      prev = cur;
      cur = next;
    }
    if (cur == offset) {
      return prev;  // 剛好接上 = 這條分頁鏈是對的
    }
    fallback = prev;
    if (start == 0) {
      break;  // 已經從檔頭推過來了,不會有更好的答案
    }
  }
  return fallback;
}

void TxtReaderActivity::updatePageSizeEstimate(const size_t pageBytes) {
  if (pageBytes == 0) {
    return;
  }
  // 指數移動平均(權重 1/8):單頁長短受段落切分影響很大,不平滑的話估計頁數會跳。
  avgBytesPerPage_ = avgBytesPerPage_ != 0
                         ? static_cast<uint32_t>((static_cast<uint64_t>(avgBytesPerPage_) * 7 + pageBytes) / 8)
                         : static_cast<uint32_t>(pageBytes);
}

int TxtReaderActivity::estimatedTotalPages() const {
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  if (fileSize == 0 || avgBytesPerPage_ == 0) {
    return 1;
  }
  const size_t n = (fileSize + avgBytesPerPage_ - 1) / avgBytesPerPage_;
  return n > 0 ? static_cast<int>(n) : 1;
}

int TxtReaderActivity::estimatedCurrentPage() const {
  if (avgBytesPerPage_ == 0) {
    return 1;
  }
  const int total = estimatedTotalPages();
  const int p = static_cast<int>(pageStartOffset_ / avgBytesPerPage_) + 1;
  return p > total ? total : p;
}

void TxtReaderActivity::loop() {
  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, txt ? txt->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<TxtReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  // v119:短按確認鍵開閱讀選單。這正是公開 repo 的 issue #1 —— 在此之前 txt 閱讀器
  // 從頭到尾沒有查詢過 Button::Confirm,使用者按下去不是「選單壞了」,是根本沒人在聽。
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openReaderMenu();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered) {
    if (pageStartOffset_ == 0) {
      return;  // 已在書首
    }
    size_t prev = 0;
    if (!popBackOffset(prev)) {
      prev = findPreviousPageOffset(pageStartOffset_);
    }
    pageStartOffset_ = prev;
    requestUpdate();
    return;
  }

  if (atLastPage_) {
    onGoHome();
    return;
  }
  // v120:下一頁的起點只有在「這一頁排完」之後才知道。繪製要約一秒,期間再按一次翻頁,
  // 舊版會用同一個 nextPageOffset_ 做一次【指派同值的空操作】然後照樣 requestUpdate,
  // 結果同一頁又重畫一遍 —— 畫面閃一下、還在原地,也就是使用者說的「按了沒反應」。
  // v119 的 log 裡 11/48 是這種浪費。現在把那次按鍵【排隊】,等這一頁畫完自動前進。
  if (nextPageOffset_ <= pageStartOffset_) {
    pendingForward_ = true;
    return;
  }
  pushBackOffset(pageStartOffset_);
  pageStartOffset_ = nextPageOffset_;
  pendingForward_ = false;
  requestUpdate();
}

void TxtReaderActivity::recomputeGeometry() {
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  // 幾何變了,每頁的位元組數也會變 —— 估計值重新累積,免得沿用舊字級的平均值。
  avgBytesPerPage_ = 0;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }
  recomputeGeometry();
  // v118:不再建索引。只把閱讀位置(位元組位移)讀回來,第一頁在 render() 當場排。
  loadProgress();
  initialized = true;
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }
  if (!initialized) {
    initializeReader();
  }

  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }
  if (pageStartOffset_ >= fileSize) {
    pageStartOffset_ = 0;  // 防呆:檔案被換掉或位移壞掉時回到書首,而不是畫出空白
  }

  // v119 競態修正:【只讀一次】pageStartOffset_,之後整個函式都用這個區域變數。
  // 主任務的 loop() 會在 render 進行中(面板刷新約一秒)把它改成 nextPageOffset_,
  // 而本函式後面每一處重讀都會拿到下一頁的值 —— v118 的 log 有 38/54 筆印出
  // next==off 就是這樣來的,更嚴重的是 atLastPage_ 會被誤設成 true、下一次翻頁
  // 直接跳回主畫面。這是 CLAUDE.md v110「pageNo 捕捉一次」記過的同一類錯誤。
  const size_t pageOffset = pageStartOffset_;

  const uint32_t layoutStartMs = millis();
  currentPageLines.clear();
  size_t nextOffset = pageOffset;
  loadPageAtOffset(pageOffset, currentPageLines, nextOffset);
  const uint32_t layoutMs = millis() - layoutStartMs;

  nextPageOffset_ = nextOffset;
  atLastPage_ = (nextOffset >= fileSize) || (nextOffset <= pageOffset);
  if (nextOffset > pageOffset) {
    updatePageSizeEstimate(nextOffset - pageOffset);
  }

  renderer.clearScreen();
  renderPage(pageOffset);

  const uint32_t saveStartMs = millis();
  saveProgress(pageOffset);
  const uint32_t saveMs = millis() - saveStartMs;

  // v119 分段儀器:v118 只量到 layout,其餘各段都還是從 EPUB 換算的估計值。
  // 只在 SD 根目錄有 /diag.on 時才會真的寫入。
  DiagLog::line("TXTPAGE off=%u next=%u layout=%u rd=%u fnt=%u wrp=%u prewarm=%u bw=%u disp=%u aa=%u save=%u warm=%u afail=%u dropped=%u lines=%u est=%d/%d",
                static_cast<unsigned>(pageOffset), static_cast<unsigned>(nextOffset), layoutMs, segReadMs_, segFontMs_, segWrapMs_, segPrewarmMs_,
                segBwMs_, segDispMs_, segAaMs_, saveMs, diagWarmHit_, diagAllocFail_, diagDropped_,
                static_cast<unsigned>(currentPageLines.size()),
                estimatedCurrentPage(), estimatedTotalPages());

  // v121:預取下一頁的字。面板刷新那 441ms CPU 是空的(waitBusy 走 vTaskDelay 會讓出),
  // 加上使用者停留時間 —— 把 SD 讀字圖那一段塞進去。中止條件是「有新的繪製在等」。
  if (!atLastPage_ && nextOffset > pageOffset) {
    prefetchNextPage(nextOffset);
  }

  // v120:消化在這次繪製期間被按下、但當時還不知道要去哪裡的那一次翻頁。
  // 只消化一次(旗標立刻清掉),所以連按多次不會變成無限前進。
  if (pendingForward_ && !atLastPage_ && nextPageOffset_ > pageOffset) {
    pendingForward_ = false;
    pushBackOffset(pageOffset);
    pageStartOffset_ = nextPageOffset_;
    requestUpdate();
  } else {
    pendingForward_ = false;
  }
}

void TxtReaderActivity::renderStatusBar(const size_t offset) const {
  const size_t fileSize = txt->getFileSize();
  const float progress = fileSize != 0 ? static_cast<float>(static_cast<double>(offset) * 100.0 / fileSize) : 0.0f;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  // v122:txt 預設【不顯示頁數】—— 它是用平均頁長外推的估計值,對長文沒有意義;
  // 而位元組位移算出來的百分比是精確的。EPUB 維持頁數為預設(它的頁碼是真的排出來的)。
  // 百分比取兩位小數:長文的整數百分比幾乎不動(2,225 頁的書一頁只佔 0.045%)。
  GUI.drawStatusBar(renderer, progress, estimatedCurrentPage(), estimatedTotalPages(), title, 0, 0, true, false,
                    /*pageCountEstimated=*/true, /*progressDecimals=*/2, /*hidePageCount=*/true);
}

void TxtReaderActivity::saveProgress(const size_t offset) const {
  const int page = estimatedCurrentPage();
  const uint32_t off = static_cast<uint32_t>(offset);
  uint8_t data[PROGRESS_SIZE];
  data[0] = static_cast<uint8_t>(page & 0xFF);
  data[1] = static_cast<uint8_t>((page >> 8) & 0xFF);
  data[2] = PROGRESS_MAGIC0;
  data[3] = PROGRESS_MAGIC1;
  data[4] = static_cast<uint8_t>(off & 0xFF);
  data[5] = static_cast<uint8_t>((off >> 8) & 0xFF);
  data[6] = static_cast<uint8_t>((off >> 16) & 0xFF);
  data[7] = static_cast<uint8_t>((off >> 24) & 0xFF);
  data[8] = static_cast<uint8_t>(avgBytesPerPage_ & 0xFF);
  data[9] = static_cast<uint8_t>((avgBytesPerPage_ >> 8) & 0xFF);
  data[10] = static_cast<uint8_t>((avgBytesPerPage_ >> 16) & 0xFF);
  data[11] = static_cast<uint8_t>((avgBytesPerPage_ >> 24) & 0xFF);
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data))) {
    LOG_ERR("TRS", "Failed to save progress: offset %u", static_cast<unsigned>(off));
  }
}

void TxtReaderActivity::loadProgress() {
  pageStartOffset_ = 0;
  avgBytesPerPage_ = 0;
  const size_t fileSize = txt->getFileSize();

  uint8_t data[PROGRESS_SIZE] = {};
  size_t got = 0;
  {
    HalFile f;
    if (!Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
      return;
    }
    got = f.read(data, PROGRESS_SIZE);
  }
  if (got < 4) {
    return;
  }

  if (got >= PROGRESS_SIZE && data[2] == PROGRESS_MAGIC0 && data[3] == PROGRESS_MAGIC1) {
    const uint32_t off = static_cast<uint32_t>(data[4]) | (static_cast<uint32_t>(data[5]) << 8) |
                         (static_cast<uint32_t>(data[6]) << 16) | (static_cast<uint32_t>(data[7]) << 24);
    if (off < fileSize) {
      pageStartOffset_ = off;
    }
    avgBytesPerPage_ = static_cast<uint32_t>(data[8]) | (static_cast<uint32_t>(data[9]) << 8) |
                       (static_cast<uint32_t>(data[10]) << 16) | (static_cast<uint32_t>(data[11]) << 24);
    LOG_DBG("TRS", "Loaded progress: offset %u", static_cast<unsigned>(pageStartOffset_));
    return;
  }

  // --- 舊格式(只有頁碼)的一次性遷移 ---
  // 判準是 維護者定的「進度找得回來」:用還躺在卡上的 index.bin 把頁碼換算成位移。
  // 檔頭驗證通過就精確換算;只讀得到頁數就按比例估;兩者都不行才回到書首。
  const int page = data[0] + (data[1] << 8);
  if (page <= 0) {
    return;
  }
  const std::string indexPath = txt->getCachePath() + "/index.bin";
  uint32_t numPages = 0;
  size_t headerSize = 0;
  {
    HalFile f;
    if (Storage.openFileForRead("TRS", indexPath, f)) {
      uint8_t head[LEGACY_INDEX_HEADER_V4] = {};
      if (f.read(head, LEGACY_INDEX_HEADER_V4) >= LEGACY_INDEX_HEADER_V3) {
        const uint32_t magic = static_cast<uint32_t>(head[0]) | (static_cast<uint32_t>(head[1]) << 8) |
                               (static_cast<uint32_t>(head[2]) << 16) | (static_cast<uint32_t>(head[3]) << 24);
        const uint8_t version = head[4];
        if (magic == LEGACY_INDEX_MAGIC && (version == 3 || version == 4)) {
          headerSize = version == 4 ? LEGACY_INDEX_HEADER_V4 : LEGACY_INDEX_HEADER_V3;
          const size_t np = headerSize - 4;
          numPages = static_cast<uint32_t>(head[np]) | (static_cast<uint32_t>(head[np + 1]) << 8) |
                     (static_cast<uint32_t>(head[np + 2]) << 16) | (static_cast<uint32_t>(head[np + 3]) << 24);
        }
      }
    }
  }

  if (numPages > 0 && static_cast<uint32_t>(page) <= numPages) {
    HalFile f;
    if (Storage.openFileForRead("TRS", indexPath, f) &&
        f.seekSet(static_cast<uint32_t>(headerSize + static_cast<size_t>(page - 1) * 4))) {
      uint8_t buf[4] = {};
      if (f.read(buf, 4) == 4) {
        const uint32_t off = static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
                             (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
        if (off < fileSize) {
          pageStartOffset_ = off;
          avgBytesPerPage_ = numPages > 0 ? static_cast<uint32_t>(fileSize / numPages) : 0;
          LOG_DBG("TRS", "Migrated progress: page %d -> offset %u (exact)", page, static_cast<unsigned>(off));
        }
      }
    }
  }
  if (pageStartOffset_ == 0 && numPages > 0) {
    // 索引在但那一頁讀不出來:按比例估,落點差幾頁,不會回到書首。
    pageStartOffset_ = static_cast<size_t>(static_cast<uint64_t>(fileSize) * (page - 1) / numPages);
    avgBytesPerPage_ = static_cast<uint32_t>(fileSize / numPages);
    LOG_DBG("TRS", "Migrated progress: page %d -> offset %u (proportional)", page,
            static_cast<unsigned>(pageStartOffset_));
  }
  // 索引已無用武之地,順手回收(2,225 頁約 8.9 KB)。
  Storage.remove(indexPath.c_str());
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = estimatedCurrentPage();
  info.totalPages = estimatedTotalPages();
  const size_t fileSize = txt ? txt->getFileSize() : 0;
  info.progressPercent = fileSize != 0 ? static_cast<int>(static_cast<double>(pageStartOffset_) * 100.0 / fileSize + 0.5)
                                       : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}

// v119:閱讀選單(公開 repo issue #1)。
void TxtReaderActivity::openReaderMenu() {
  const size_t fileSize = txt->getFileSize();
  const float pct =
      fileSize != 0 ? static_cast<float>(static_cast<double>(pageStartOffset_) * 100.0 / fileSize) : 0.0f;

  startActivityForResult(
      std::make_unique<TxtReaderMenuActivity>(renderer, mappedInput, txt->getTitle(), pct, SETTINGS.orientation),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);

        // 方向與字級是 pending 語意:彈窗選定即生效,即使整個選單被取消(與 EPUB 同語意)。
        bool geometryChanged = false;
        if (menu.orientation != SETTINGS.orientation) {
          SETTINGS.orientation = menu.orientation;
          SETTINGS.saveToFile();
          ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
          geometryChanged = true;
        }
        if (menu.fontSize != SETTINGS.fontSize) {
          SETTINGS.fontSize = menu.fontSize;
          SETTINGS.saveToFile();
          geometryChanged = true;
        }
        if (geometryChanged) {
          // v121:比照 EpubReaderActivity::applyOrientation —— 改 renderer 方向與重算幾何
          // 必須持 RenderLock,否則 render task 可能正拿著舊幾何在畫。
          RenderLock lock(*this);
          // v118 買到的東西在這裡兌現:位元組位移不是字型的函數,所以改字級或轉方向
          // 只要重算幾何、用同一個位移重排當前頁 —— 不必重建索引(舊版是 73 分鐘),
          // 閱讀位置也不會漂掉(舊版存頁碼,總頁數一變就錯位)。
          recomputeGeometry();
          backCount_ = 0;  // 分頁鏈變了,回溯環裡的舊頁首不再落在同一條鏈上
          backHead_ = 0;
        }

        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        switch (static_cast<TxtReaderMenuActivity::MenuAction>(menu.action)) {
          case TxtReaderMenuActivity::MenuAction::GO_HOME:
            onGoHome();
            return;
          case TxtReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
            const size_t size = txt->getFileSize();
            const int initial = size != 0 ? static_cast<int>(pageStartOffset_ * 100 / size) : 0;
            startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initial),
                                   [this](const ActivityResult& r) {
                                     if (!r.isCancelled) {
                                       jumpToPercent(std::get<PercentResult>(r.data).percent);
                                     } else {
                                       requestUpdate();
                                     }
                                   });
            return;
          }
          default:
            requestUpdate();
            return;
        }
      });
}

void TxtReaderActivity::jumpToPercent(const int percent) {
  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) {
    return;
  }
  const int p = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  size_t target = static_cast<size_t>(static_cast<uint64_t>(fileSize) * static_cast<uint64_t>(p) / 100);
  if (target >= fileSize) {
    target = fileSize - 1;
  }
  // 對齊到行首。這一步同時保證落點是 UTF-8 的前導位元組 —— 直接用比例算出來的位元組
  // 很可能切在一個漢字中間,那樣這一頁的第一個字會是垃圾。
  pageStartOffset_ = alignToLineStart(target);
  backCount_ = 0;  // 跳轉之後回溯環的舊頁首不在同一條分頁鏈上,作廢重新定錨
  backHead_ = 0;
  requestUpdate();
}

WarmIdentity TxtReaderActivity::buildWarmIdentity(const size_t offset) const {
  WarmIdentity id;
  id.bookHash = WarmIdentity::fnv1a(txt->getCachePath().c_str());
  id.spineIndex = 0;                                  // txt 沒有 spine
  id.pageNumber = static_cast<int32_t>(offset);       // 位元組位移就是 txt 的「頁身分」
  id.fontId = cachedFontId;
  id.viewportWidth = static_cast<uint16_t>(viewportWidth);
  id.viewportHeight = static_cast<uint16_t>(linesPerPage);  // 每頁行數已折入方向/邊距/狀態列
  id.paragraphAlignment = cachedParagraphAlignment;
  id.valid = true;
  return id;
}

bool TxtReaderActivity::prefetchShouldAbort(void* ctx) {
  (void)ctx;  // 契約同 EPUB:pending 旗標在全域 ActivityManager 上
  return activityManager.isRenderPending();
}

// v121:把下一頁的字圖預先讀進快取。只有【完整未中止】的預取才採用身分 ——
// 中止的預取留下的是半成品快取,採用它會讓下一次 render 假 warm 命中、整頁走 overflow ring
// 而診斷上卻顯示 prewarm=0(v110 的 Finding A,那正是最陰險的失敗形狀)。
void TxtReaderActivity::prefetchNextPage(const size_t nextOffset) {
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm || nextOffset >= txt->getFileSize()) {
    return;
  }

  std::vector<std::string> lines;
  size_t after = nextOffset;
  if (!loadPageAtOffset(nextOffset, lines, after) || lines.empty()) {
    return;
  }

  bool completed = false;
  {
    auto scope = fcm->createPrewarmScope();
    scope.setRetainCacheOnExit(true);
    // scan 模式:drawText 只 recordText 就返回,framebuffer 一個位元組都不會動 ——
    // 面板上仍是剛顯示出去的那一頁。座標傳真值只是為了誠實。
    const int lineHeight = renderer.getLineHeight(cachedFontId);
    int y = cachedOrientedMarginTop;
    for (const auto& line : lines) {
      if (!line.empty()) {
        renderer.drawText(cachedFontId, cachedOrientedMarginLeft, y, line.c_str());
      }
      y += lineHeight;
    }
    completed = scope.endScanAndPrewarmAbortable(&TxtReaderActivity::prefetchShouldAbort, this);
  }

  // v110 教訓:預取自己的 stats 必須折進累計 —— 下一次 render 的 PrewarmScope ctor 會
  // resetStats(),而 TXTPAGE 那一行印在預取【之前】⇒ 不折的話,預取造成的
  // alloc_fail / dropped 會完全隱形,而那兩個正是回退判準。
  if (const auto* font = sdFontSystem.currentReaderFont()) {
    const auto& st = font->getStats();
    diagAllocFail_ += st.bitmapAllocFailures;
    diagDropped_ += st.bitmapGlyphsDropped;
  }

  if (completed) {
    fcm->adoptWarmIdentity(buildWarmIdentity(nextOffset));
  } else {
    // 半成品快取:scope 解構已清過一次,這一行是保險(clearCache 冪等且自己會 invalidate 身分)。
    fcm->clearCache();
  }
}
