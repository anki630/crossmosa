#include "util/RecentCoverLoader.h"

#include <DataDir.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Xtc.h>
#include <esp_heap_caps.h>

#include <cstring>

#include "Bitmap.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "util/DiagLog.h"

namespace RecentCoverLoader {

// 2026-10-07：原 HomeActivity::loadRecentCovers 的本體，原封不動搬來（逐字，只把「丟快照＋重畫」換成 onChanged）。
//   各段的 vNNN 註解是當初修正的理由，搬的時候一起帶著。
void ensureThumbs(GfxRenderer& renderer, std::vector<RecentBook>& books, const int coverHeight,
                  const std::function<void()>& onChanged) {
  bool showingLoading = false;
  // v175（diag174）：縮圖的 JPEG 解碼要 53KB 總量、Epub::load 也要一塊；剛離開閱讀器時 SD 字型
  // （interval 表＋16KB 碼位緩衝）仍常駐，實測只剩 39KB → 每本 THUMBFAIL（why=cache-missing|cache-load-failed /
  // heap 39024<53248）。同 v5 連線前卸載：地板以下先卸字型，下次進閱讀器 ensureLoaded 自動重載。
  // 只在真的有縮圖要產時做一次（否則每次回主畫面都卸＝每次進書都重載）。
  bool reliefChecked = false;
  bool fontsUnloaded = false;  // v261：這一輪已經卸過字型（重試串流前不必再卸）
  auto reliefIfLow = [&]() {
    if (reliefChecked) return;
    reliefChecked = true;
    const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    const size_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    // v260：門檻 72K → 100K 並看最大塊。diag259：一次 free=78K 沒卸字型（>72K），而縮圖直接串流開始前要 85K
    //   （讀取器 49K＋轉換器入口 36K）、最大塊 40K → 退回抽到 SD，9.1 秒。另外 7 次都是 71–73K 觸發卸載後有 110K。
    //   卸載的代價：下次進閱讀器 ensureLoaded 重載（FONTLOAD why=ensure），而且只在「真的有縮圖要產」時才會走到這裡。
    if (freeNow < 100 * 1024 || largestNow < 40 * 1024) {
      DiagLog::line("THUMBRELIEF free=%u max=%u", static_cast<unsigned>(freeNow), static_cast<unsigned>(largestNow));
      sdFontSystem.unloadForLowMemory(renderer);
      fontsUnloaded = true;
    }
  };

  int progress = 0;
  for (RecentBook& book : books) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      // v174：縮圖比例 0.6 → 2:3。檔名只帶高度，「存在」不等於「是新比例」（A-20）——驗內容：
      // 位元圖寬度小於新目標寬度，就是舊比例裁過的，刪掉重產（自癒，使用者不必清快取）。
      // 0 byte 標記檔 parseHeaders 會失敗 → 視同存在、照舊跳過。
      bool needsThumb = !Storage.exists(coverPath.c_str());
      if (!needsThumb) {
        const int expectedWidth = (coverHeight * 2 + 1) / 3;
        HalFile existing;
        if (Storage.openFileForRead("HOME", coverPath, existing)) {
          Bitmap bmp(existing);
          const bool ok = bmp.parseHeaders() == BmpReaderError::Ok;
          const int w = ok ? bmp.getWidth() : 0;
          existing.close();
          if (ok && w < expectedWidth) {
            Storage.remove(coverPath.c_str());
            needsThumb = true;
            DiagLog::line("THUMBREGEN w=%d<%d %s", w, expectedWidth, book.path.c_str());
          }
        }
      }
      if (needsThumb) {
        reliefIfLow();
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, DataDir::path());
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            // v155（舊樹 v130 系）：純文字彈窗。逐本進度條只在「本與本之間」動一格，
            // 而單核心的縮圖產生把中間整段堵住 —— 條只是閃兩下永遠走不完，
            // 每動一格還多付一次 e-ink 部分刷新。文字一樣有告知效果，畫面安靜得多。
            GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          bool success = epub.generateThumbBmp(coverHeight, !fontsUnloaded);
          // v258：縮圖走哪條路、各段多久（diag257 開過書回主畫面 6–8 秒，當時沒有分段證人）。
          //   ms＝整個 generateThumbBmp；open＝zip 開讀取器或 sd 抽檔；conv＝轉檔器整段；dec＝其中 JPEGDEC
          //   解碼（含讀取）。 幾何：原圖/縮放分母>解出的格子>輸出。note＝串流退回 SD 的原因（open／io／mem）。
          //   沒走到解碼（例如沒開過的書 cache-missing）不印，那種情況已有 THUMBFAIL。
          const auto logThumbGen = [&](const bool ok) {
            if (strcmp(epub.thumbStats().src, "none") != 0 && strcmp(epub.thumbStats().src, "exists") != 0) {
              const Epub::ThumbStats& ts = epub.thumbStats();
              const bool jpg = ts.converted;  // 幾何只在這一次真的呼叫過 JPEG 轉檔器時才屬於這本書
              const JpegToBmpConverter::Info& ji = JpegToBmpConverter::lastInfo();
              DiagLog::line(
                  "THUMBGEN ok=%u h=%d src=%s note=%s fr=%uK mx=%uK ofr=%uK omx=%uK need=%uK err=%s ms=%u zip=%u "
                  "open=%u "
                  "conv=%u dec=%u %ux%u/%u>%ux%u>%ux%u prog=%u item=%uKB ra=%u rs=%u %s",
                  ok ? 1u : 0u, coverHeight, ts.src, ts.note[0] ? ts.note : "-", static_cast<unsigned>(ts.preFreeKb),
                  static_cast<unsigned>(ts.preMaxKb), static_cast<unsigned>(ts.openFreeKb),
                  static_cast<unsigned>(ts.openMaxKb),
                  jpg ? static_cast<unsigned>(JpegToBmpConverter::lastInfo().needBytes / 1024) : 0u,
                  jpg && JpegToBmpConverter::lastError()[0] ? JpegToBmpConverter::lastError() : "-",
                  static_cast<unsigned>(ts.totalMs), static_cast<unsigned>(ts.zipMs), static_cast<unsigned>(ts.openMs),
                  static_cast<unsigned>(ts.convMs), jpg ? static_cast<unsigned>(ji.decodeMs) : 0u, jpg ? ji.srcW : 0u,
                  jpg ? ji.srcH : 0u, jpg ? ji.scale : 0u, jpg ? ji.decW : 0u, jpg ? ji.decH : 0u, jpg ? ji.outW : 0u,
                  jpg ? ji.outH : 0u, jpg && ji.progressive ? 1u : 0u,
                  static_cast<unsigned>((ts.itemBytes + 512) / 1024), static_cast<unsigned>(ts.readAheadKb),
                  static_cast<unsigned>(ts.restarts), book.path.c_str());
            }
          };
          logThumbGen(success);
          // v261：串流因記憶體失敗、而這一輪還沒卸過字型 → 卸字型、重試一次串流（重試時才允許退回抽到 SD）。
          //   diag260：某本含大張封面的書 fr=99K（剛好沒觸發卸載門檻）→ 開讀取器後差約 1KB → 退回舊路 5.3 秒。
          if (!success && epub.thumbStats().deferredForMemory) {
            DiagLog::line("THUMBRELIEF retry free=%u max=%u",
                          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)),
                          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
            sdFontSystem.unloadForLowMemory(renderer);
            fontsUnloaded = true;
            reliefChecked = true;
            success = epub.generateThumbBmp(coverHeight, false);
            logThumbGen(success);
          }
          if (!success) {
            // v165（A-20）：【不要】抹掉 store 裡的封面路徑。失敗多半是暫時的
            // （記憶體緊、SD 忙），抹掉= 永久負快取，之後任何主題都不再嘗試——
            // 實機就是這樣全部消失的。留著路徑，下次進主畫面重試；
            // 真正永久失敗的（如 progressive JPEG 封面，JPEGDEC 不支援）每次
            // 快速失敗一次、顯示書脊佔位圖，誠實且無害。
            // v174：帶原因（Epub 出口＋JPEG 轉檔器錯誤＋當下最大連續塊）與書的路徑 —— diag173 的 12 筆
            // THUMBFAIL 只有快取雜湊，判不出是 progressive 封面還是記憶體。
            DiagLog::line("THUMBFAIL h=%d why=%s jpg=%s max=%u %s", coverHeight, epub.thumbFailReason(),
                          JpegToBmpConverter::lastError(),
                          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)),
                          book.path.c_str());
          }
          // v57：連【快照】一起丟，不能只清 coverRendered。
          // 否則下一次 render 會先 restoreCoverBuffer() 把舊快照寫回封面帶，再把新封面疊上去
          // —— 而 1-bit 縮圖只畫黑像素、白像素留原背景，舊快照的黑像素於是全數存活：
          // 舊 coverWidth 的圓角框邊線會殘留在新封面上，並在 storeCoverBuffer() 被重新快照，
          // 停留主畫面期間每次重繪都在。
          onChanged();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, DataDir::path());
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            // v57：同上 —— 兩個縮圖產生點都要丟快照，只改一處等於沒改。
            onChanged();
          }
        }
      }
    }
    progress++;
  }
}

}  // namespace RecentCoverLoader
