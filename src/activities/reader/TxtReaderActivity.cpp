#include "TxtReaderActivity.h"

#include "TxtEngineLayout.h"

#include "Epub/ParsedText.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/VerticalEm.h"
#include "Epub/VerticalText.h"

#include "ReaderFontSizes.h"

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
#include "QrDisplayActivity.h"  // v289：顯示 QR（與 EPUB 共用）
#include "ReaderBookmarksActivity.h"  // v290：與 EPUB 共用的書籤清單
#include <SdCardFont.h>

#include "SdCardFontSystem.h"

#include "ReaderUtils.h"
#include "TxtReaderMenuActivity.h"
#include "activities/settings/TextSettingsActivity.h"  // v286：txt 也要有文字設定入口
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DiagLog.h"
#include "util/NvsStore.h"
#include "util/BookmarkFile.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"  // v289：選單觸發的截圖

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

// v290：把一段【任意切出來的】位元組修成合法 UTF-8。
// 頁界是排版切的、不是字元切的，所以頭尾都可能落在一個字的中間。
// ⚠️ 複查抓過一次：**只有不完整的那一個序列該砍，完整的字必須留著**
//    （第一版寫成先剝續接位元組再剝起始位元組 →「abc中」變「abc」）。
void trimToUtf8Boundaries(std::string& text) {
  size_t start = 0;
  while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) ++start;
  if (start > 0) text.erase(0, start);

  size_t i = text.size();  // 退到最後一個起始位元組
  while (i > 0 && (static_cast<unsigned char>(text[i - 1]) & 0xC0) == 0x80) --i;
  if (i > 0) {
    const unsigned char lead = static_cast<unsigned char>(text[i - 1]);
    size_t need = 1;
    if ((lead & 0xF8) == 0xF0) need = 4;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xE0) == 0xC0) need = 2;
    if (text.size() - (i - 1) < need) text.resize(i - 1);  // 續接位元組在下一頁
  }
}
}  // namespace

void TxtReaderActivity::onEnter() {
  // ⭐ 軸向照 CrossPointSettings.h 的紀律「開文件的人解析、寫進唯一權威欄位」。
  //    v242：txt 沒有出版社訊號，**視為直排出版** → 傳 true。全域「依出版社」（預設）與「直排」時 txt 直排，
  //    只有全域「橫排」時 txt 橫排（維護者 2026-09-14 拍板：CrossMosa 是繁中韌體，txt 大宗是中文小說）。
  //    ⚠️ 代價：英文 txt／程式碼／log 預設也直排；想看橫排只能把全域改成橫排（EPUB 會一起變）。
  //       真正的解法是「每本書各自覆寫方向」，**延後、而且要一次推廣到所有格式**（帳本記著），不做 txt 專用版。
  //    （v241 曾經傳 false ＝「依出版社」時 txt 橫排。）
  //    ⚠️ 仍然【必須每次寫】，不可以只在直排時寫：EPUB 設的值是 settings 單例上的執行期欄位、onExit 不清，
  //      讀完一本直排 EPUB 再開 .txt 若不覆寫，按鍵方向會整組反過來（複查抓到過）。
  SETTINGS.activeDocumentVertical = SETTINGS.resolveVerticalFor(/*publisherRtl=*/true) ? 1 : 0;

  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // v290：載入這本書的書籤。⚠️ 必須在 setupCacheDir 之後 —— 書籤檔不在快取目錄下
  //   （它在 /.crossmosa/bookmarks/），但載入失敗時我們要的是「空清單」而不是舊的殘留。
  if (!BookmarkFile::load(txt->getPath(), bookmarks_)) {
    bookmarks_.clear();
  }

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.save();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  if (progressDirty_) saveProgressNow("exit");  // v329：欠的進度在離開時寫掉（ActivityManager 持 RenderLock）
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
  APP_STATE.saveDurable();  // v332：離開書＝沒人等的時刻，NVS＋state.json 都寫（SD 那份是降版／換卡的保險）
  txt.reset();
}




// ---------------------------------------------------------------------------
// v239：txt 走 EPUB 排版引擎
// ---------------------------------------------------------------------------
//
// 頁游標仍是單一位元組位移（理由見 TxtEngineLayout.h）。多讀【前一個位元組】：
// 它決定這一頁是不是從段落中間開始 —— 是的話續排，不縮排。
bool TxtReaderActivity::loadPageAtOffset(const size_t offset, std::vector<std::shared_ptr<TextBlock>>& outUnits,
                                         size_t& nextOffset) {
  outUnits.clear();
  const size_t fileSize = txt->getFileSize();
  if (offset >= fileSize) {
    return false;
  }

  const size_t readFrom = offset > 0 ? offset - 1 : 0;
  const size_t lead = offset - readFrom;  // 0 或 1
  const size_t want = std::min(CHUNK_SIZE + lead, fileSize - readFrom);
  auto* buffer = static_cast<char*>(malloc(want));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", want);
    return false;
  }

  const uint32_t readStartMs = millis();
  if (!txt->readContent(reinterpret_cast<uint8_t*>(buffer), readFrom, want)) {
    free(buffer);
    return false;
  }
  segReadMs_ = millis() - readStartMs;
  // advance 表的預熱改由 `ParsedText::layoutAndExtractLines` 自己做（ensureSdCardFontReady，
  // 只灌這一段實際用到的碼位），所以 `wrp=` 在新引擎下【包含】預熱，`fnt=` 固定為 0。
  segFontMs_ = 0;

  const bool midParagraph = lead == 1 && buffer[0] != '\n';
  size_t skip = lead;
  // UTF-8 BOM 只可能出現在檔頭。舊引擎會把它當成一個字畫出來。
  if (offset == 0 && want >= 3 && static_cast<unsigned char>(buffer[0]) == 0xEF &&
      static_cast<unsigned char>(buffer[1]) == 0xBB && static_cast<unsigned char>(buffer[2]) == 0xBF) {
    skip = 3;
  }

  txtengine::Params tp;
  tp.fontId = cachedFontId;
  tp.extent = static_cast<uint16_t>(vertical_ ? viewportHeight_ : viewportWidth);
  tp.maxUnits = unitsPerPage_;
  tp.vertical = vertical_;
  tp.alignment = cachedParagraphAlignment;

  // ⚠️ `g_boldBodyText` 是全域，EPUB 閱讀器與文字設定頁會留下它的值。v239 的橫排要與舊引擎
  //    【同行為】才能逐頁對照，而舊引擎從不加粗 —— 每次排版前明確關掉，不賭它剛好是乾淨的。
  //    （txt 要不要吃「粗體內文」設定是之後的產品決定，帳本記著。）
  ParsedText::setBoldBodyText(false);

  const uint32_t wrapStartMs = millis();
  txtengine::Result r = txtengine::layoutPage(buffer + skip, want - skip, readFrom + want >= fileSize, midParagraph,
                                              renderer, tp);
  segWrapMs_ = millis() - wrapStartMs;
  free(buffer);

  diagRemapMiss_ = r.remapMiss;
  diagVerifyMiss_ = r.verifyMiss;
  diagFlushes_ = r.flushes;
  diagWords_ = r.words;
  diagEngOom_ = r.oom ? 1 : 0;
  diagChunkCut_ = r.chunkCut ? 1 : 0;
  diagGlue_ = static_cast<uint16_t>(r.glueMoved + r.glueForced * 100);

  outUnits = std::move(r.units);
  nextOffset = readFrom + skip + r.nextOffset;
  return !outUnits.empty();
}

void TxtReaderActivity::renderPage(const size_t pageOffset, const size_t pageEndOffset) {
  const int lineHeight = cachedLineHeight_;
  const int contentWidth = viewportWidth;

  // v239：對齊、兩端對齊、RTL 都由排版引擎算進 TextBlock 的字位置裡（extent＝viewportWidth），
  // 這裡只負責逐行往下畫。`TextBlock::render` 的 y 與舊的 `drawText` 同語意（行頂）。
  (void)contentWidth;
  auto renderLines = [&]() {
    // ⚠️ `TextBlock::render` 會看 `renderer.isVerticalLayout()` 分流到直排繪製，所以每一趟（掃描／BW／AA）
    //    都要【明確宣告】這本書的軸向（v241 起 txt 可能是直排），不賭上一個閱讀器留下的值。
    const GfxRenderer::VerticalScope axis(renderer, vertical_);
    if (vertical_) {
      // v241 直排：欄由右往左，照 ChapterHtmlSlimParser 的放欄規則 —— 第一欄左緣＝視窗右緣減欄距，
      // 之後每欄再減一個欄距，餘數留在左邊；欄頂＝版心頂。TextBlock::render 在直排範圍內分流到
      // renderVertical，x 是欄的左緣、y 是欄頂。空白單位（nullptr）照樣佔一欄。
      int x = cachedOrientedMarginLeft + viewportWidth;
      for (const auto& unit : currentPageLines) {
        x -= columnPitch_;
        if (unit) {
          unit->render(renderer, cachedFontId, x, cachedOrientedMarginTop);
        }
      }
      return;
    }
    int y = cachedOrientedMarginTop;
    for (const auto& unit : currentPageLines) {
      if (unit) {
        unit->render(renderer, cachedFontId, cachedOrientedMarginLeft, y);
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
  renderStatusBar(pageOffset, pageEndOffset);
  segBwMs_ = millis() - bwStartMs;

  const uint32_t dispStartMs = millis();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  segDispMs_ = millis() - dispStartMs;
  dispDoneMs_ = std::max<uint32_t>(1, millis());  // v243：黑白這一趟上了面板 ＝ 使用者看到換頁的時刻（0 ＝ 這次沒上面板）

  segAaMs_ = 0;
  if (SETTINGS.textAntiAliasing) {
    const uint32_t aaStartMs = millis();
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
    segAaMs_ = millis() - aaStartMs;
  }
  // scope destructor clears font cache via FontCacheManager

  // v289：選單觸發的截圖。⭐ **放在最後一趟繪製之後** —— 這時 framebuffer 才是面板上
  //   真正會看到的畫面（抗鋸齒開著時前面幾趟是中間狀態）。同 EpubReaderActivity 的紀律。
  if (pendingScreenshot_) {
    pendingScreenshot_ = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}







// ---------------------------------------------------------------------------
// v118:串流導覽
// ---------------------------------------------------------------------------

void TxtReaderActivity::markPress() {
  // 只記最早那一次：連按時要量的是「第一次按下到真的換頁」。
  // CAS 而不是 load＋store：兩步之間 render 可能剛好把舊值取走，新的這次就會被吃掉（codex 複查）。
  // ⚠️ 盡力而為的證人：連按時後面幾次併進同一筆、兩筆同時在途時後一筆會漏記 —— 只少樣本，不會錯記。
  uint32_t expected = 0;
  pressMs_.compare_exchange_strong(expected, std::max<uint32_t>(1, millis()));
}

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

// v240：一次排到目標為止、取最後 N 個單位（排版引擎的收集模式）。
//
// v239 以前：從較早的行首往前逐頁推，希望「剛好接上」目標。DP 斷行不具前綴穩定性，幾乎接不上；
// 實機 9 次裡 4 次要推到 k=64（往回試排約 85 頁、估約 3 秒），3 次沒接上而重疊 1–3 行。
// v240：橫排改 greedy。greedy 對「非段落首行、零縮排」具後綴穩定性（codex 複查把原本的主張縮小到這裡）：
// 從段落起點排到目標，目標之前第 N 個單位的起點，往後排 N 個單位會剛好停在目標 ——
// 【前提是目標本身是 greedy 的行首】。不成立的情況：升級後第一次（進度是 v239 的 DP 頁首）、
// NFC 會變的文字裡的 glue 群組、段落超過 8KB（canon=0）。這些情況 TXTBACKCHK 會記 exact=0。
// ⭐ 但任何情況都【不會跳字】：算出的上一頁起點一定在目標之前，往後翻看到的是從那裡開始的連續分頁，
//    最壞只是與原頁重疊幾行。
//
// 段落長到目標與段落起點相距超過一塊（8KB，整章一行的檔）時，只能從段落中間開始（canon=0），
// 那時的上一頁可能與這一頁差一行，不會跳字。
size_t TxtReaderActivity::findPreviousPageOffset(const size_t offset, BackStats& st) {
  if (offset == 0) {
    return 0;
  }
  const size_t avg = avgBytesPerPage_ != 0 ? avgBytesPerPage_ : 512;
  size_t span = std::min<size_t>(CHUNK_SIZE, std::max<size_t>(1024, avg * 3));
  // ⚠️ 初值是 offset（＝原地不動），不是 0：第一次配置或讀檔就失敗時回傳 0 會把讀者直接丟回書首
  //    （codex 複查抓到）。失敗時寧可這一次按鍵沒有作用。
  size_t best = offset;

  for (int attempt = 0; attempt < 3; ++attempt) {
    st.span = span;
    const size_t from = offset > span ? offset - span : 0;
    size_t start = alignToLineStart(from);
    bool canonical = true;
    if (offset - start > CHUNK_SIZE) {
      start = offset - CHUNK_SIZE;
      canonical = false;
    }

    const size_t readFrom = start > 0 ? start - 1 : 0;
    const size_t lead = start - readFrom;
    const size_t want = offset - readFrom;
    auto* buffer = static_cast<char*>(malloc(want));
    if (!buffer) {
      LOG_ERR("TRS", "back: failed to allocate %zu bytes", want);
      st.oom = true;
      return best;
    }
    if (!txt->readContent(reinterpret_cast<uint8_t*>(buffer), readFrom, want)) {
      free(buffer);
      return best;
    }
    size_t skip = lead;
    if (start == 0 && want > 3 && static_cast<unsigned char>(buffer[0]) == 0xEF &&
        static_cast<unsigned char>(buffer[1]) == 0xBB && static_cast<unsigned char>(buffer[2]) == 0xBF) {
      skip = 3;
    }
    if (!canonical) {
      while (skip < want && (static_cast<unsigned char>(buffer[skip]) & 0xC0) == 0x80) ++skip;  // 對齊碼位
    }
    const bool midParagraph = lead == 1 && buffer[0] != '\n';

    txtengine::Params tp;
    tp.fontId = cachedFontId;
    tp.extent = static_cast<uint16_t>(vertical_ ? viewportHeight_ : viewportWidth);
    tp.maxUnits = unitsPerPage_;
    tp.vertical = vertical_;
    tp.alignment = cachedParagraphAlignment;
    tp.collectOnly = true;
    tp.collectKeep = static_cast<size_t>(unitsPerPage_ > 0 ? unitsPerPage_ : 1);
    ParsedText::setBoldBodyText(false);  // 與 loadPageAtOffset 同一個理由

    txtengine::Result r;
    if (skip < want) {
      r = txtengine::layoutPage(buffer + skip, want - skip, /*atEof=*/true, midParagraph, renderer, tp);
    }
    free(buffer);
    ++st.passes;
    st.units = r.unitCount;
    st.canonical = canonical;
    if (r.oom) {
      st.oom = true;  // 收集不完整：退回視窗起點（寧可重疊，不跳字）
      return start;
    }

    const size_t base = readFrom + skip;
    best = base + (r.lastStarts.empty() ? 0 : r.lastStarts.front());
    // 書首有 BOM 時第一個單位的起點是 3。回傳 3 的話，再按上一頁會從 [0,3) 收集到零個單位、
    // 算出的還是 3 → 永遠卡在「不是書首、也退不回去」（codex 複查抓到）。正規化成 0。
    if (start == 0 && skip == 3 && best == 3) best = 0;
    if (r.unitCount >= static_cast<size_t>(unitsPerPage_)) {
      return best;  // lastStarts 恰好是最後 N 個單位，front ＝ 目標之前第 N 個
    }
    if (start == 0 || !canonical || span >= CHUNK_SIZE) {
      return best;  // 書首，或視窗已經最大：不滿一頁就從最前面那個單位開始
    }
    span = std::min<size_t>(CHUNK_SIZE, span * 3);
  }
  return best;
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

  // v291：**長按確認鍵 → 設定→操作→「長按選單功能」**（目前唯一的選項是「書籤」）。
  //   EPUB 一直都有，txt 沒有 —— 實機回報。照 EpubReaderActivity 的同一段搬過來。
  //   ⚠️ `confirmHoldConsumed_` 同時扮演兩個角色，兩個都不能少：
  //     ① **閂鎖**：loop 每圈都會跑，沒有它按著不放會一路「加→刪→加…」切換下去。
  //     ② **吃掉這次放開**：否則手放開時下面那段又會開選單（EPUB 用 ignoreNextConfirmRelease
  //        做同一件事）。
  //   ℹ️ `getHeldTime()` 是全域計時、不是單鍵計時（memory: x3-opds-nav-gotchas）。這裡沿用
  //      EPUB 已在真機跑過很多版的同一種用法，不另外發明。
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmPressedAtMs_ = millis();  // 這一鍵自己的按下時刻（見標頭的說明）
    confirmHoldConsumed_ = false;    // 新的一次按壓 → 閂鎖重新開始
  }
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // ⚠️ 自癒：放開的邊緣有可能沒被這個活動收到（例如按著確認鍵時用觸控開了選單，
    //    在選單裡才放開）。沒有這一段，旗標會卡在 true，下一次短按會被白吃掉。
    confirmHoldConsumed_ = false;
    confirmPressedAtMs_ = 0;
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && !confirmHoldConsumed_ &&
      confirmPressedAtMs_ != 0 && SETTINGS.longPressMenuFunction == CrossPointSettings::LP_MENU_BOOKMARK &&
      (millis() - confirmPressedAtMs_) >= ReaderUtils::BOOKMARK_HOLD_MS) {
    confirmHoldConsumed_ = true;
    toggleBookmark();  // 回饋就是狀態列的書籤圖示亮／滅（同 v290 的設計）
  }

  // v119:短按確認鍵開閱讀選單。這正是公開 repo 的 issue #1 —— 在此之前 txt 閱讀器
  // 從頭到尾沒有查詢過 Button::Confirm,使用者按下去不是「選單壞了」,是根本沒人在聽。
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmReleased) confirmPressedAtMs_ = 0;
  if (confirmReleased && confirmHoldConsumed_) {
    confirmHoldConsumed_ = false;  // 這次放開是長按加書籤的尾巴，不開選單
    return;
  }
  if (confirmReleased || ReaderUtils::isTouchMenuGesture(mappedInput)) {
    openReaderMenu();
    return;
  }

  // v161：觸控翻頁區（新樹能力，照 XtcReaderActivity 的 OR 合併形狀；左 1/3=上一頁、右 2/3=下一頁）
  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // v291：**長按翻頁鍵 → 設定→操作→「長按按鍵行為」**。EPUB 有兩種：跳章與換方向。
  //   ⛔ **跳章對 txt 不適用**（沒有章節）—— 不硬湊一個「跳 10%」之類的替代品，
  //      那是發明新語意，不是補一致性。選了跳章的人在 txt 上就是長按沒作用，與現況相同。
  //   ⚠️ 傾斜翻頁不算長按（`fromTilt`），與 EPUB 同判準。
  //   ⚠️ **已知缺口，照實記著**：這裡的按鍵長按用的是**全域** `getHeldTime()`，所以
  //      「按著確認鍵不放、再按翻頁鍵」有機會被誤判成長按。**EPUB 同一段有一模一樣的洞**，
  //      這裡刻意維持同行為（要修就該兩邊一起改成單鍵計時，那是另一版的事）。
  //      確認鍵那條已經改成單鍵計時了 —— 因為它會**直接改掉書籤**，誤觸的代價高得多。
  {
    const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
    if (!fromTilt && heldMs > ReaderUtils::SKIP_HOLD_MS &&
        SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
      const uint8_t newOrientation =
          nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                        : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
      SETTINGS.orientation = newOrientation;
      SETTINGS.saveToFile();
      {
        // 同「選單改方向」那條已驗證的路：改 renderer 方向與重算幾何必須持 RenderLock。
        RenderLock lock(*this);
        ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
        recomputeGeometry();
        backCount_ = 0;  // 幾何變了，回溯環的舊頁首不在同一條分頁鏈上
        backHead_ = 0;
      }
      requestUpdate();
      return;
    }
  }

  if (prevTriggered) {
    if (pageStartOffset_ == 0) {
      return;  // 已在書首
    }
    size_t prev = 0;
    if (popBackOffset(prev)) {
      markPress();
      pageStartOffset_ = prev;
      requestUpdate();
      return;
    }
    // v240：ring 空了 → 要用排版引擎往回排。那會碰字型系統與常駐的 SD 檔柄，而繪製任務此刻可能
    // 正在預取下一頁（同一套快取、同一個檔柄）—— v239 以前直接在主任務算，是沒拿鎖的競態。
    // 交給 render()：requestUpdate 讓預取中止（它看 isRenderPending），render 在鎖內算完再畫。
    // 計次而不是旗標：ring 空時連按兩次上一頁要退兩頁（grok 複查指出旗標會吃掉第二次）。
    if (pendingBackSteps_.load() < 8) pendingBackSteps_.fetch_add(1);
    markPress();
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
    markPress();  // 排隊的翻頁：等待時間算進延遲，那正是使用者感受到的
    pendingForward_ = true;
    return;
  }
  markPress();
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
  // v284：**解析一次就存起來**（同 EPUB 走 Section::resolvedLineHeightPx 的理由）——
  //   probeEmFP 有兩條精度不同的路，每次現算可能讓同一份文字前後差 1px。
  cachedLineHeight_ = renderer.getReaderLineHeight(cachedFontId, SETTINGS.getReaderLinePitchEm());
  if (cachedLineHeight_ < 1) cachedLineHeight_ = 1;  // 這裡是 linesPerPage 的除數
  const int lineHeight = cachedLineHeight_;

  // v284：橫排的行距與直排的欄距現在是同一把尺（em × 檔位係數，見下方 vertical_ 分支）。
  //   在此之前這裡用的是字型宣告的 advanceY，所以 txt 的行距**完全不受「行距」設定影響**，
  //   而且換字型就換行距（原俠正楷／RoundTC 是零行距）。
  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  // v241：直排的每頁單位數是欄數。欄距公式與 EPUB 相同（ChapterHtmlSlimParser::columnPitchPx）：
  // round(em × 欄距檔位係數)，em 用 probeEmFP 實際量。欄長＝版心高。
  viewportHeight_ = viewportHeight;
  vertical_ = SETTINGS.documentIsVertical();
  if (vertical_) {
    const float em = static_cast<float>(vtext::probeEmFP(renderer, cachedFontId)) / 16.0f;
    const int pitch = static_cast<int>(em * vtext::columnPitchForTier(SETTINGS.readerColumnPitch) + 0.5f);
    columnPitch_ = pitch > 0 ? pitch : 1;
    unitsPerPage_ = viewportWidth / columnPitch_;
    if (unitsPerPage_ < 1) unitsPerPage_ = 1;
  } else {
    columnPitch_ = 0;
    unitsPerPage_ = linesPerPage;
  }

  // 幾何變了,每頁的位元組數也會變 —— 估計值重新累積,免得沿用舊字級的平均值。
  avgBytesPerPage_ = 0;

  LOG_DBG("TRS", "Viewport: %dx%d, units per page: %d (vertical=%d pitch=%d)", viewportWidth, viewportHeight,
          unitsPerPage_, vertical_ ? 1 : 0, columnPitch_);
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
  // v243：按鍵到換頁的延遲證人。在 render() 最前面取，才能量到「等繪製任務／鎖」的時間
  // （上一輪的存進度＋預取還沒做完時，這一頁要排隊）。
  const uint32_t renderStartMs = millis();
  const uint32_t pressMs = pressMs_.exchange(0);
  const uint32_t prevDlogMs = lastRenderDlogMs_;  // v329：TXTPAGE dlog= 報【上一次】render 的（含那次 TXTPAGE 自己的 append）
  lastRenderDlogMs_ = 0;
  const int32_t prevNvsUs = lastNvsUs_;  // v331：同 dlog，報上一次的
  lastNvsUs_ = -1;
  dlogStartAtRender_ = DiagLog::writeMsTotal();
  dispDoneMs_ = 0;  // 這次 render 沒走到面板就不報 lat，不沿用上一頁的時刻
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
  // v240：往前翻頁（ring 空了）在這裡、鎖內做。見 loop()。
  while (pendingBackSteps_.load() > 0) {
    pendingBackSteps_.fetch_sub(1);
    const size_t target = pageStartOffset_;
    if (target > 0) {
      BackStats bs;
      const uint32_t backStartMs = millis();
      const size_t prev = findPreviousPageOffset(target, bs);
      DiagLog::line("TXTBACK off=%u prev=%u units=%u passes=%u span=%u canon=%u oom=%u ms=%u",
                    static_cast<unsigned>(target), static_cast<unsigned>(prev), static_cast<unsigned>(bs.units),
                    bs.passes, static_cast<unsigned>(bs.span), bs.canonical ? 1 : 0, bs.oom ? 1 : 0,
                    static_cast<unsigned>(millis() - backStartMs));
      backCheckTarget_ = target;
      backCheckPrev_ = prev;
      pageStartOffset_ = prev;
    }
  }

  const size_t pageOffset = pageStartOffset_;

  const uint32_t layoutStartMs = millis();
  currentPageLines.clear();
  size_t nextOffset = pageOffset;
  loadPageAtOffset(pageOffset, currentPageLines, nextOffset);
  const uint32_t layoutMs = millis() - layoutStartMs;

  // v240：往前翻頁的證人 —— 往回排出來的上一頁，往後排是不是剛好停在原頁（分頁唯一性的直接證據）。
  if (backCheckTarget_ != 0 && pageOffset == backCheckPrev_) {
    DiagLog::line("TXTBACKCHK prev=%u next=%u target=%u exact=%d", static_cast<unsigned>(pageOffset),
                  static_cast<unsigned>(nextOffset), static_cast<unsigned>(backCheckTarget_),
                  nextOffset == backCheckTarget_ ? 1 : 0);
    backCheckTarget_ = 0;
  }

  nextPageOffset_ = nextOffset;
  atLastPage_ = (nextOffset >= fileSize) || (nextOffset <= pageOffset);
  if (nextOffset > pageOffset) {
    updatePageSizeEstimate(nextOffset - pageOffset);
  }

  renderer.clearScreen();
  renderPage(pageOffset, nextOffset);
  // v240：`afail`／`dropped` 改成【近似這一頁】的值。字型統計在 PrewarmScope 建構時歸零：
  // 冷頁讀到的是它自己 prewarm 的結果；暖頁（renderPage 不建 scope、不歸零）讀到的是上一輪預取
  // 替它準備時的結果，外加這一頁繪製期間可能的增量（grok 複查指出「一定是預取」太強）。
  // 不是 EPUB 那種嚴格的逐次差分，但已經不是累計值。
  // v239 以前是在預取裡 `+=` 而且從不歸零 ＝ 開書以來的累計，還漏掉冷頁自己的失敗（讀 log 時被它騙過一次）。
  diagAllocFail_ = 0;
  diagDropped_ = 0;
  diagRescue_ = 0;
  if (diagWarmHit_ && prefetchStatValid_ && prefetchStatOffset_ == pageOffset) {
    // 暖頁：用預取當時替【這一頁】記下的數字（綁定位移，消費一次）。
    diagAllocFail_ = prefetchAllocFail_;
    diagDropped_ = prefetchDropped_;
    diagRescue_ = prefetchRescue_;
  } else if (const auto* font = sdFontSystem.currentReaderFont()) {
    // 冷頁：renderPage 自己建了 PrewarmScope（建構時歸零），讀到的就是它的 prewarm。
    const auto& st = font->getStats();
    diagAllocFail_ = st.bitmapAllocFailures;
    diagDropped_ = st.bitmapGlyphsDropped;
    diagRescue_ = st.bitmapExactRescues;
  }
  prefetchStatValid_ = false;

  // v329：進度不在翻頁路徑寫（見 .h）。save= 現在只在真的寫那一頁才非零；只數位置真的變了的 render。
  uint32_t saveMs = 0;
  {
    const bool firstObs = lastObservedOffset_ == SIZE_MAX;
    const bool moved = !firstObs && pageOffset != lastObservedOffset_;
    lastObservedOffset_ = pageOffset;
    if (firstObs || moved) {
      progressDirty_ = true;  // progress.bin 過期：離開時補寫（第一次 render 也算，同 EPUB）
      nvsProgDirty_ = true;
    }
    if (moved) {
      if (sdProgLen_ == 0) saveProgressSd(pageOffset, "anchor");  // v332：沒有 progress.bin 先寫一次當錨（同 EPUB）
      // v332：閱讀位置的主檔＝NVS，每一次真的翻頁寫（同 EPUB）。失敗 → 當場退回寫 progress.bin（save= 就是那次的毫秒）。
      lastNvsUs_ = writeProgressNvs(pageOffset);
      if (lastNvsUs_ == -2 && ++nvsFailStreak_ >= 10) {  // 退路：第一次失敗立刻、之後每 10 次（v329 的節奏；codex）
        nvsFailStreak_ = 0;
        const uint32_t saveStartMs = millis();
        saveProgressSd(pageOffset, "nvsfail");
        saveMs = millis() - saveStartMs;
      }
    }
  }

  // v119 分段儀器:v118 只量到 layout,其餘各段都還是從 EPUB 換算的估計值。
  // 只在 SD 根目錄有 /diag.on 時才會真的寫入。
  // 超過 10 秒的按鍵視為不是這一次繪製的（被吸收的按鍵留下的殘值），不報。
  // 無號減法本身就跨得過 millis() 繞回，所以只比「經過多久」，不比大小（codex 複查）。
  const bool pressValid = pressMs != 0 && static_cast<uint32_t>(renderStartMs - pressMs) < 10000;
  const uint32_t latWait = pressValid ? renderStartMs - pressMs : 0;
  const uint32_t latTotal = pressValid && dispDoneMs_ != 0 ? dispDoneMs_ - pressMs : 0;
  DiagLog::line("TXTPAGE off=%u next=%u layout=%u rd=%u fnt=%u wrp=%u prewarm=%u bw=%u disp=%u aa=%u save=%u warm=%u afail=%u dropped=%u lines=%u est=%d/%d eng=2 remap=%u vmiss=%u flush=%u words=%u eoom=%u cut=%u glue=%u vert=%u pitch=%u rescue=%u wait=%u lat=%u dlog=%u nvs=%d",
                static_cast<unsigned>(pageOffset), static_cast<unsigned>(nextOffset), layoutMs, segReadMs_, segFontMs_, segWrapMs_, segPrewarmMs_,
                segBwMs_, segDispMs_, segAaMs_, saveMs, diagWarmHit_, diagAllocFail_, diagDropped_,
                static_cast<unsigned>(currentPageLines.size()),
                estimatedCurrentPage(), estimatedTotalPages(), diagRemapMiss_, diagVerifyMiss_, diagFlushes_, diagWords_, diagEngOom_,
                diagChunkCut_, diagGlue_, vertical_ ? 1u : 0u, static_cast<unsigned>(columnPitch_),
                diagRescue_, static_cast<unsigned>(latWait), static_cast<unsigned>(latTotal),
                static_cast<unsigned>(prevDlogMs), static_cast<int>(prevNvsUs));
  // v240：txt 也寫 SDCFFAIL（EPUB 閱讀器一直有寫）。v239 看得到 afail 卻不知道差多少位元組，
  // 分不出是新引擎把記憶體切碎、還是某頁剛好用到比較多字。預取的失敗會出現在【下一頁】的這一行之後。
  DiagLog::crumb("SDCFFAIL", SdCardFont::lastAllocFail, sizeof(SdCardFont::lastAllocFail));
  DiagLog::crumb("ADVSCAN", SdCardFont::lastAdvScan, sizeof(SdCardFont::lastAdvScan));  // v253：txt 也走同一條字寬路徑

  // v121:預取下一頁的字。面板刷新那 441ms CPU 是空的(waitBusy 走 vTaskDelay 會讓出),
  // 加上使用者停留時間 —— 把 SD 讀字圖那一段塞進去。中止條件是「有新的繪製在等」。
  if (!atLastPage_ && nextOffset > pageOffset) {
    prefetchNextPage(nextOffset);
  }
  lastRenderDlogMs_ = DiagLog::writeMsTotal() - dlogStartAtRender_;  // v329：下一次 TXTPAGE 的 dlog=

  // v120:消化在這次繪製期間被按下、但當時還不知道要去哪裡的那一次翻頁。
  // 只消化一次(旗標立刻清掉),所以連按多次不會變成無限前進。
  if (pendingForward_ && !atLastPage_ && nextPageOffset_ > pageOffset) {
    pendingForward_ = false;
    pushBackOffset(pageOffset);
    pageStartOffset_ = nextPageOffset_;
    requestUpdate();
  } else {
    // 排隊的翻頁沒被消化（已到檔尾）→ 它記下的按鍵時刻也不該留給之後某一次繪製。
    if (pendingForward_) pressMs_.store(0);
    pendingForward_ = false;
  }
}

// v290：把目前這一頁加入／移出書籤。錨點是**這一頁的起始位元組位移**。
// ⚠️ 「同一頁」的判準刻意用【位移相等】而不是「落在這一頁的範圍內」——
//    範圍會隨字級／行距／方向改變，而位移不會。代價是改了排版之後舊書籤可能不再
//    「剛好等於某一頁的頁首」，但它仍然跳得到正確的位置（跳轉是用位移重排那一頁）。
void TxtReaderActivity::toggleBookmark() {
  if (!txt) return;
  const uint32_t anchor = static_cast<uint32_t>(pageStartOffset_);
  const uint32_t hi = static_cast<uint32_t>(nextPageOffset_ > pageStartOffset_ ? nextPageOffset_ : pageStartOffset_ + 1);
  // ⚠️ 存檔失敗要能還原 —— 不可以「記憶體改了、SD 沒寫成功」卻讓使用者以為成功
  //    （這個專案的老毛病：把 I/O 失敗當成 UI 成功）。
  const std::vector<BookmarkEntry> snapshot = bookmarks_;
  const size_t before = bookmarks_.size();
  {
    // ⚠️ `bookmarks_` 會被繪圖任務在畫狀態列時走訪 —— insert／erase 與它並行是
    //    資料競爭＋迭代器失效（複查指出）。改動一律在鎖內。
    RenderLock lock(*this);
    bookmarks_.erase(std::remove_if(bookmarks_.begin(), bookmarks_.end(),
                                    [anchor, hi](const BookmarkEntry& b) {
                                      return b.hasByteOffset && b.byteOffset >= anchor && b.byteOffset < hi;
                                    }),
                     bookmarks_.end());
  }
  if (bookmarks_.size() == before) {
    BookmarkEntry entry;
    const size_t fileSize = txt->getFileSize();
    entry.percentage = fileSize != 0 ? static_cast<float>(static_cast<double>(anchor) / fileSize) : 0.0f;
    entry.hasByteOffset = true;
    entry.byteOffset = anchor;
    // 摘要：這一頁開頭的幾十個位元組。直接讀檔 —— TextBlock 沒有取回純文字的 API，
    // 而位元組位移本來就是 txt 的頁游標，這是最短的路。
    std::string head;
    const size_t want = std::min<size_t>(96, fileSize > anchor ? fileSize - anchor : 0);
    if (want > 0) {
      head.resize(want);
      if (txt->readContent(reinterpret_cast<uint8_t*>(&head[0]), anchor, want)) {
        trimToUtf8Boundaries(head);
      } else {
        head.clear();
      }
    }
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(head);
    RenderLock lock(*this);
    bookmarks_.insert(bookmarks_.begin(), entry);
  }
  // 使用者的回饋就是狀態列那個書籤圖示會亮／滅 —— 不另外做提示彈窗。
  if (!BookmarkFile::save(txt->getPath(), bookmarks_)) {
    LOG_ERR("TRS", "Failed to save bookmarks — reverting in-memory change");
    RenderLock lock(*this);
    bookmarks_ = snapshot;  // 記憶體與 SD 保持一致：寧可「按了沒變」，不要「畫面說成功、重開就沒了」
  }
  requestUpdate();
}

// ⚠️ **判準是「落在這一頁的範圍內」，不是「位移剛好相等」。**
// 我第一版寫成相等並且在註解裡說那是刻意的 —— 複查證明那會壞掉：書籤存的是**當時**
// 那一頁的頁首，換了字級／行距之後分頁鏈不同，那個位移可能**永遠不等於任何一頁的頁首**。
// 後果不只是圖示不亮：從清單跳過去之後圖示仍然不亮，使用者再按「切換書籤」就會
// **新增第二個幾乎同位置的書籤**，而不是移除原來那個。EPUB 用的本來就是範圍判準。
bool TxtReaderActivity::isBookmarked(const size_t from, const size_t to) const {
  const uint32_t lo = static_cast<uint32_t>(from);
  const uint32_t hi = static_cast<uint32_t>(to > from ? to : from + 1);
  for (const auto& b : bookmarks_) {
    if (b.hasByteOffset && b.byteOffset >= lo && b.byteOffset < hi) return true;
  }
  return false;
}

void TxtReaderActivity::renderStatusBar(const size_t offset, const size_t endOffset) const {
  const size_t fileSize = txt->getFileSize();
  const float progress = fileSize != 0 ? static_cast<float>(static_cast<double>(offset) * 100.0 / fileSize) : 0.0f;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = txt->getTitle();
  }
  // v122:txt 預設【不顯示頁數】—— 它是用平均頁長外推的估計值,對長文沒有意義;
  // 而位元組位移算出來的百分比是精確的。EPUB 維持頁數為預設(它的頁碼是真的排出來的)。
  // 百分比取兩位小數:長文的整數百分比幾乎不動(2,225 頁的書一頁只佔 0.045%)。
  GUI.drawStatusBar(renderer, progress, estimatedCurrentPage(), estimatedTotalPages(), title, 0, 0, true,
                    isBookmarked(offset, endOffset),  // v290：從參數算，不讀易變成員（教訓 A-24）
                    /*pageCountEstimated=*/true, /*progressDecimals=*/2, /*hidePageCount=*/true);
}

// v329：閒置／離開／休眠時真的寫（翻頁時每 N 頁那次在 render 裡、有 save= 證人）。
bool TxtReaderActivity::saveProgressNow(const char* why) { return saveProgressSd(pageStartOffset_, why); }

// v332：真的寫 progress.bin（離開／檢查點／NVS 退路）：成功就記指紋並讓 NVS 那格指向這份（配對）。
bool TxtReaderActivity::saveProgressSd(const size_t offset, const char* why) {
  if (!txt) return false;
  const uint32_t t0 = millis();
  uint32_t fp = 0, len = 0;
  const bool ok = saveProgress(offset, &fp, &len);
  if (ok) {
    progressDirty_ = false;  // 失敗留著 dirty（codex：不可吞掉）
    sdProgHash_ = fp;
    sdProgLen_ = len;
    writeProgressNvs(offset);
  }
  DiagLog::line("PROGRESS save why=%s ok=%d ms=%lu", why, ok ? 1 : 0, static_cast<unsigned long>(millis() - t0));
  return ok;
}

int TxtReaderActivity::flushProgress() {
  if (!txt) return 0;
  if (!nvsProgDirty_) return 0;  // v332：NVS 已是目前位置（每次翻頁都寫）
  const int32_t us = writeProgressNvs(pageStartOffset_);
  if (us != -2) {
    DiagLog::line("PROGRESS nvs why=flush us=%ld", static_cast<long>(us));
    return 1;
  }
  return saveProgressNow("flush") ? 1 : -1;  // 退路：SD
}

// v332：淺睡眠入口、桌布已上面板之後（沒人等）→ SD 檢查點（同 EPUB）。
int TxtReaderActivity::flushProgressDurable() {
  if (!txt || !progressDirty_) return 0;
  return saveProgressSd(pageStartOffset_, "checkpoint") ? 1 : -1;
}

// v332：NVS 進度寫入（render 尾段與 flush 共用）。回傳微秒；-2＝失敗。
int32_t TxtReaderActivity::writeProgressNvs(const size_t offset) {
  if (!txt) return -2;
  if (nvsBookHash_ == 0) nvsBookHash_ = NvsStore::fnv1a(txt->getPath().c_str()) | 1u;
  NvsStore::ProgBlob pb{};
  pb.magic = 'P';
  pb.version = 2;
  pb.kind = 2;
  pb.flags = NvsStore::PROG_HAS_OFFSET;
  pb.bookHash = nvsBookHash_;
  pb.seq = NvsStore::nextSeq();
  pb.page = static_cast<uint16_t>(std::clamp<long>(estimatedCurrentPage(), 0, 0xFFFE));
  pb.pageCount = static_cast<uint16_t>(std::clamp<long>(estimatedTotalPages(), 0, 0xFFFE));
  pb.offset = static_cast<uint32_t>(offset);
  pb.sdHash = sdProgHash_;
  pb.sdLen = static_cast<uint16_t>(std::min<uint32_t>(sdProgLen_, 0xFFFF));
  pb.identity = static_cast<uint32_t>(txt->getFileSize());  // 同名不同檔的保險
  uint32_t us = 0;
  int err = 0;
  const bool ok = NvsStore::putProg(pb, &us, &err);
  if (ok) {
    nvsProgDirty_ = false;
    nvsFailStreak_ = 10;  // 成功就重設（codex 第二輪）
  }
  return ok ? static_cast<int32_t>(us) : -2;
}

bool TxtReaderActivity::saveProgress(const size_t offset, uint32_t* fingerprintOut, uint32_t* lenOut) const {
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
    return false;
  }
  if (fingerprintOut) *fingerprintOut = NvsStore::fnv1aBytes(data, sizeof(data));  // v332：NVS 配對用
  if (lenOut) *lenOut = sizeof(data);
  return true;
}

// v332：先讀 progress.bin（舊格式遷移都在裡面），再讓 NVS 蓋過去（同一本書時 NVS 永遠不比 SD 舊）。
//   avgBytesPerPage_ 留 SD 那份（NVS 沒存；只影響頁數估計）。offset 超出檔案大小就不信（換過卡、同名不同檔）。
void TxtReaderActivity::loadProgress() {
  sdProgHash_ = NvsStore::FNV1A_BASIS;
  sdProgLen_ = 0;
  loadProgressSd();  // 順便記下 progress.bin 的指紋（讀到幾個 byte 就是幾個）
  nvsBookHash_ = NvsStore::fnv1a(txt->getPath().c_str()) | 1u;
  const uint32_t fileSize = static_cast<uint32_t>(txt->getFileSize());
  NvsStore::ProgBlob pb{};
  bool used = false;
  // NVS 贏的條件（同 EPUB，codex）：同一個檔（路徑 hash＋大小）＋ 它記的 progress.bin 指紋／長度＝現在讀到的 ＋ offset 在檔內。
  if (sdProgLen_ != 0 && NvsStore::readProg(&pb) && pb.kind == 2 && pb.bookHash == nvsBookHash_ && pb.identity == fileSize &&
      pb.sdHash == sdProgHash_ && pb.sdLen == sdProgLen_ && (pb.flags & NvsStore::PROG_HAS_OFFSET) != 0 &&
      pb.page != UINT16_MAX && pb.offset < fileSize) {
    pageStartOffset_ = pb.offset;
    used = true;
  }
  DiagLog::line("PROG src=%s off=%u sdlen=%u", used ? "nvs" : "sd", static_cast<unsigned>(pageStartOffset_),
                static_cast<unsigned>(sdProgLen_));
}

void TxtReaderActivity::loadProgressSd() {
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
    sdProgLen_ = static_cast<uint32_t>(got);  // v332：指紋（NVS 配對用）
    sdProgHash_ = NvsStore::fnv1aBytes(data, got);
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
      std::make_unique<TxtReaderMenuActivity>(
          renderer, mappedInput, txt->getTitle(), pct, SETTINGS.orientation, !bookmarks_.empty()),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);

        // 方向是 pending 語意:彈窗選定即生效,即使整個選單被取消(與 EPUB 同語意)。
        // v286：字級的 pending 管線已移除 —— 字級改由「文字設定」那一頁負責（它每改一項就自己
        //   存檔並重載字型），選單裡不再有重複的入口。
        bool geometryChanged = false;
        if (menu.orientation != SETTINGS.orientation) {
          SETTINGS.orientation = menu.orientation;
          SETTINGS.saveToFile();
          ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
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
          case TxtReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
            // v286：與 EPUB 同一個入口（EpubReaderActivity 的 TEXT_SETTINGS）。
            //   回來之後走的重排路徑與「改字級」完全相同 —— 位元組位移不是字型的函數，
            //   所以只要重算幾何、用同一個位移重排當前頁，閱讀位置不會漂掉（v118 的資產）。
            startActivityForResult(
                std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                       TextSettingsActivity::Tab::Family),
                [this](const ActivityResult&) {
                  // TextSettingsActivity 每改一項就自己存檔，這裡不必再存。
                  // ⚠️ 文字方向可以在這裡被改掉，而 txt 的軸向是 onEnter 算的快照 ——
                  //    不重解的話會用舊軸向重排，看起來像「設定沒生效」（EPUB 踩過同一個坑）。
                  //    txt 沒有出版社訊號，視為直排出版 → 傳 true（與 onEnter 同一個運算式）。
                  //    ℹ️ 軸向的快照是 `vertical_`，由下面的 recomputeGeometry() 重讀，
                  //       所以這一行之後不需要再另外同步一次（複查問過）。
                  SETTINGS.activeDocumentVertical = SETTINGS.resolveVerticalFor(/*publisherRtl=*/true) ? 1 : 0;
                  {
                    // ⚠️ 鎖的範圍【只包】狀態與幾何的更新，不含 requestUpdate() ——
                    //    與上面「改字級」那條已驗證的路徑同形狀（複查指出我原本把兩者都圈進來了，
                    //    而 requestUpdate 的同步契約沒有證據支持它可以在鎖內呼叫）。
                    //    ⛔ 也**不**在這裡呼叫 applyOrientation：螢幕方向是閱讀選單的
                    //       ROTATE_SCREEN 在管，文字設定頁不會動它；EPUB 的對照組也沒有這一行。
                    RenderLock lock(*this);
                    // 換字型家族要真的重載 .cpfont（釋放 render task 可能正在讀的 SdCardFont，
                    // 所以必須在鎖內 —— 與上面改字級那條同紀律）。
                    // ⭐ 順序不可對調：`getReaderFontId()` 是惰性的，它反映【當下已載入的字面】，
                    //    先重算幾何會拿到舊字型的度量（教訓 A-3）。
                    sdFontSystem.ensureLoaded(renderer);
                    recomputeGeometry();
                    backCount_ = 0;  // 分頁鏈變了，回溯環裡的舊頁首不再落在同一條鏈上
                    backHead_ = 0;
                  }
                  requestUpdate();
                });
            return;
          }
          case TxtReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
            toggleBookmark();
            return;
          }
          case TxtReaderMenuActivity::MenuAction::BOOKMARKS: {
            // v290：與 EPUB 共用同一個清單活動（txt 傳 nullptr 當 epub）。
            startActivityForResult(
                std::make_unique<ReaderBookmarksActivity>(renderer, mappedInput, nullptr, txt->getPath()),
                [this](const ActivityResult& r) {
                  // 清單裡可能刪過書籤 —— 不論有沒有選取，都要重新載入，否則選單的
                  // 「有沒有書籤」與狀態列的指示會停在舊狀態。
                  {
                    RenderLock lock(*this);  // 同 toggleBookmark：繪圖任務會走訪這個 vector
                    if (!BookmarkFile::load(txt->getPath(), bookmarks_)) bookmarks_.clear();
                  }
                  if (!r.isCancelled && std::holds_alternative<ProgressChangeResult>(r.data)) {
                    const auto& p = std::get<ProgressChangeResult>(r.data);
                    if (p.hasByteOffset) {
                      jumpToOffset(p.byteOffset);
                      return;
                    }
                  }
                  requestUpdate();
                });
            return;
          }
          case TxtReaderMenuActivity::MenuAction::SCREENSHOT: {
            {
              RenderLock lock(*this);
              pendingScreenshot_ = true;
            }
            requestUpdate();
            return;
          }
          case TxtReaderMenuActivity::MenuAction::DISPLAY_QR: {
            // 當前頁的文字＝檔案上 [pageStartOffset_, nextPageOffset_) 這一段。
            // ⭐ txt 這邊比 EPUB 單純：位元組位移本來就是它的頁游標，不必回去讀版面快取。
            const size_t from = pageStartOffset_;
            const size_t to = nextPageOffset_ > from ? nextPageOffset_ : from;
            const size_t len = to - from;
            if (len == 0) {
              requestUpdate();
              return;
            }
            std::string payload;
            payload.resize(len);
            if (!txt->readContent(reinterpret_cast<uint8_t*>(&payload[0]), from, len)) {
              LOG_ERR("TRS", "QR: failed to read page bytes");
              requestUpdate();
              return;
            }
            trimToUtf8Boundaries(payload);
            // ℹ️ 容量不必在這裡夾：`QrUtils::drawQrCode` 已經把 payload 夾到 version 20 的
            //    真實容量（858 bytes）並在 UTF-8 邊界截斷（教訓 16 當初就是為此修的）。
            if (payload.empty()) {
              requestUpdate();
              return;
            }
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, payload),
                                   [this](const ActivityResult&) { requestUpdate(); });
            return;
          }
          case TxtReaderMenuActivity::MenuAction::DELETE_CACHE: {
            // ⚠️⚠️ **刻意不呼叫 `Txt::clearCache()`。**
            //   它是 `removeDir(cachePath)` 整包刪，而 **txt 的進度檔就住在那個資料夾裡**
            //   （`ProgressFile` 寫 `<cachePath>/progress.bin`）。照 EPUB 那樣「先備份、清掉、
            //   再寫回」會開出一個**真的會掉進度的視窗**：備份只在 RAM，而「刪目錄→重建→重寫」
            //   不是一筆原子交易，中間斷電進度就沒了（codex 複查指出，成立）。
            //   ⭐ 而且那個做法對 txt 根本沒必要：**txt 沒有版面快取**（v118 起是串流排版，
            //      每頁現排），這個資料夾裡可重建的東西只有封面縮圖與早期的索引殘骸。
            //   → 所以只刪那些，**進度檔完全不碰**。整個失敗模式就不存在了。
            {
              RenderLock lock(*this);
              Storage.remove(txt->getCoverBmpPath().c_str());  // 封面縮圖（會自動重產）
              // ⚠️ **不要刪 `index.bin`。** 它看起來是 v118 前的殘骸，但這棵樹還在用它做
              //    一次性的進度遷移（把舊頁碼換算成位元組位移，見 migrateLegacyProgress）——
              //    刪掉等於拿走還沒升級過的人的救援路徑，而留著只是一個不佔事的舊檔。
              if (menu.resetProgress != 0) {
                // 使用者明確選了「連進度一起重設」——這時才動進度，而且是【寫 0】不是刪檔，
                // 兩邊（進度檔與最近閱讀）保持同一個值。
                saveProgress(0);
                RECENT_BOOKS.setProgress(txt->getPath(), 0);
              }
            }
            onGoHome();
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

// v290：跳到書籤。與 jumpToPercent 同一條路，差別只在落點是【存下來的精確位移】
// 而不是比例算出來的。
// ⚠️ 仍然要 alignToLineStart：書籤存的是**當時那一頁的頁首**，而換了字級／行距之後
//    分頁鏈不同，那個位移不見得還是某一頁的頁首。對齊到行首保證第一個字不是半個漢字。
void TxtReaderActivity::jumpToOffset(const size_t offset) {
  const size_t fileSize = txt->getFileSize();
  if (fileSize == 0) return;
  size_t target = offset;
  if (target >= fileSize) target = fileSize - 1;
  pageStartOffset_ = alignToLineStart(target);
  backCount_ = 0;  // 同 jumpToPercent：回溯環的舊頁首不在同一條分頁鏈上
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
  // 每頁單位數已折入方向/邊距/狀態列；v241 起最高位標記軸向（直排與橫排同單位數時身分也必須不同）。
  id.viewportHeight = static_cast<uint16_t>((unitsPerPage_ & 0x7FFF) | (vertical_ ? 0x8000 : 0));
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

  std::vector<std::shared_ptr<TextBlock>> lines;
  size_t after = nextOffset;
  {
    // v255：這是閱讀停留時間（上一頁已經送上面板），CJK 字寬掃描放在這裡做，不放在翻頁當下的排版裡。
    //   掃描途中每批看一次有沒有畫面在等（使用者翻頁了），有就中止、下次再掃。
    struct CjkScanAllowScope {
      explicit CjkScanAllowScope(TxtReaderActivity* self) {
        SdCardFont::openCjkScanWindow(&TxtReaderActivity::prefetchShouldAbort, self);
      }
      ~CjkScanAllowScope() { SdCardFont::closeCjkScanWindow(); }
    } scanAllow(this);
    if (!loadPageAtOffset(nextOffset, lines, after) || lines.empty()) {
      return;
    }
  }

  bool completed = false;
  {
    auto scope = fcm->createPrewarmScope();
    scope.setRetainCacheOnExit(true);
    // scan 模式:drawText 只 recordText 就返回,framebuffer 一個位元組都不會動 ——
    // 面板上仍是剛顯示出去的那一頁。座標傳真值只是為了誠實。
    const int lineHeight = cachedLineHeight_;
    const GfxRenderer::VerticalScope axis(renderer, vertical_);
    int x = cachedOrientedMarginLeft + viewportWidth;
    int y = cachedOrientedMarginTop;
    for (const auto& unit : lines) {
      if (vertical_) x -= columnPitch_;
      if (unit) {
        unit->render(renderer, cachedFontId, vertical_ ? x : cachedOrientedMarginLeft, y);
      }
      if (!vertical_) y += lineHeight;
    }
    completed = scope.endScanAndPrewarmAbortable(&TxtReaderActivity::prefetchShouldAbort, this);
  }

  // v240：預取的 alloc_fail／dropped 綁定它準備的那一頁（下一次 render 若 warm 命中而且位移相符才用）。
  // v239 以前是 `+=` 而且從不歸零 ＝ 開書以來的累計。中止的預取不會被採用，它的失敗當場寫一行，
  // 否則下一次冷 render 的 scope 一歸零就消失了（codex 複查指出）。
  if (const auto* font = sdFontSystem.currentReaderFont()) {
    const auto& st = font->getStats();
    prefetchStatOffset_ = nextOffset;
    prefetchAllocFail_ = st.bitmapAllocFailures;
    prefetchDropped_ = st.bitmapGlyphsDropped;
    prefetchRescue_ = st.bitmapExactRescues;
    prefetchStatValid_ = completed;
    if (!completed && (st.bitmapAllocFailures != 0 || st.bitmapGlyphsDropped != 0)) {
      DiagLog::line("TXTPREFETCH off=%u abort=1 afail=%u dropped=%u", static_cast<unsigned>(nextOffset),
                    static_cast<unsigned>(st.bitmapAllocFailures), static_cast<unsigned>(st.bitmapGlyphsDropped));
    }
  }

  if (completed) {
    fcm->adoptWarmIdentity(buildWarmIdentity(nextOffset));
  } else {
    // 半成品快取:scope 解構已清過一次,這一行是保險(clearCache 冪等且自己會 invalidate 身分)。
    fcm->clearCache();
  }
}
