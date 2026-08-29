#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "ParsedText.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// v28: text decoration bits now include line-through in serialized wordStyles.
// v29: TextBlock word data stored as one flat arena (offset table + NUL-terminated
// text blob) instead of length-prefixed strings and per-field arrays.
// v30: Arabic shaping changed both drawing and measurement (getTextAdvanceX now
//      measures the shaped visual text); cached word positions from v29 no longer
//      match what drawText renders.
// v32: ImageBlock serializes the book-internal source href after the cache path
//      (lazy extraction: images are header-probed at build time and extracted on
//      first render).
// v33: Support <ruby> and <rt> tags. Skip <rp> tags
// v34: Word gaps are only suppressed for tokens glued in the source, so spaces between
//      Hangul words survive again; ruby element boundaries carry the continuation flag
//      instead. Invalidates v33 caches, whose word positions have the spaces collapsed.

// v34: <br> handling changed layout — a <br> after text is now a margin-stripped
//      line break (browser-like) and only a <br> whose block stays empty injects
//      the scene-break gap, so cached pages laid out by older versions no longer
//      match. Keeps <br>-per-paragraph books (common CJK formatting) from
//      re-adding container spacing at every paragraph.
// v35: Persist a uint32_t visible-text start offset for every page.
// CrossMosa：跳號到 100，與上游【脫鉤】。
// 我們的 36 與上游的 36 是【同號不同義】，檔頭佈局不相容：
//   我們 36 = 上游 35 + v126（MAX_SOURCE_PIXELS 放寬，圖片重進版面）+ boldBodyText 欄位
//   上游 36 = ruby / CJK justification（另有我們沒有的 ReaderRenderSpec）
// 從 v130 升上這個新基底時，若沿用 36 -> 版本檢查【會通過】但佈局對不上 -> 排版錯亂，
// 而壞掉的快取住在 SD 卡上，重刷韌體清不掉。
// 為什麼是 100 不是 37/50：上游現在 41、每個 minor +2~3，跳 50 大約半年就被追上，
// 而那時已經改不動了。跳 100 讓未來合併時這一行【必定衝突】，強迫人工判讀。
// v144：100 → 101。v123 的圖片溢出縮放【改變了排版結果】，而已排好的 section 快取
// 已經把溢出座標固化進去了 —— 不 bump 的話舊快取照樣記著「這張圖在螢幕外」，
// 使用者看起來就像沒修好（教訓 A-11，v24/v123/v126 踩過三次）。
// 代價：每本書下次開會重排一次，【閱讀進度不受影響】。
// ⚠️ 重排視窗是記憶體壓力最高的時候，所以這一版特別要看實機的 alloc_fail 與 p2/p3 數字。
// v145：101 → 102。v24 的「讀檔頭取寬高」讓記憶體吃緊時不再於版面階段丟圖，
// 但已經固化成【無圖排版】的舊快取不會自己重排 —— 不 bump 就等於沒修。
// ⚠️ 這是連續第二次 bump（v144 已 100 → 101）。代價是書再重排一次，
//    而我在 v144 刻意不綁其他排版改動，這就是那個決定的帳單。進度仍不受影響。
// v187：102 → 103（bump 批次，四件事一次跳號）：①上游 #2959 圖片上邊界夾限改變圖片頁的版面；
// ②註腳 href 96→256（FootnoteEntry 存在頁資料裡）；③粗體閱讀 boldBodyText 進檔頭（v31/v41 回歸）；
// ④CSS 載入狀態進檔頭——在記憶體地板下被截斷的規則集排出來的版面不再被當成永久正確（v163 複查）。
constexpr uint8_t SECTION_FILE_VERSION = 103;
// v187 檔頭的 cssState 欄位：0 = 沒用 CSS（embeddedStyle 關或載入失敗）、1 = 規則全載、
// 2 = 撞記憶體地板被截斷（樣式打折的版面）。loadSectionFile 看到 2 且此刻記憶體寬裕就重排。
constexpr uint8_t CSS_STATE_NONE = 0;
constexpr uint8_t CSS_STATE_FULL = 1;
constexpr uint8_t CSS_STATE_TRUNCATED = 2;
// 3 = 已經為了截斷重排過一次、還是截斷（或載入失敗）：版面就這樣了，不再重排（收斂）。
constexpr uint8_t CSS_STATE_TRUNCATED_FINAL = 3;

// Written into the version field while a build is in progress; patched to
// SECTION_FILE_VERSION only when the build is finalized. An abandoned /
// crash-interrupted .bin therefore carries version 0, which loadSectionFile rejects
// as unknown and clears -- so an incomplete file is never mistaken for a valid one.
constexpr uint8_t SECTION_FILE_INCOMPLETE_VERSION = 0;
// Written when a build is suspended partway (reader exited or device slept mid-build).
// The file carries valid pages 0..pageCount-1, all LUTs, and a trailer with the parse
// watermark (bytesConsumed, totalBytes) appended after the li LUT. loadSectionFile
// accepts it so a resume shows those pages instantly; the reader extends it by
// rebuilding in the background. Uses the same header layout as SECTION_FILE_VERSION,
// so finalized files are untouched by this feature; older firmware treats the sentinel
// as an unknown version and rebuilds, which is a safe downgrade.
// MUST change in lockstep with SECTION_FILE_VERSION: the sentinel IS the partial's
// format version, so a stale-format partial otherwise passes the header check and
// only fails (noisily, via the block-decode error path) when a page is loaded.
// Derived so the pairing can't be forgotten: 0xFE for v28, 0xFD for v29, ...
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xFE - (SECTION_FILE_VERSION - 28);
// CrossMosa：哨兵是【反向】遞減的，與 SECTION_FILE_VERSION 在 V=141 撞號。
// 撞號後「建到一半」的 section 會被當成完整檔讀入，而失敗只在載入某一頁時才浮現。
// 原本完全沒有護欄。純編譯期，不影響執行期行為。
static_assert(SECTION_FILE_PARTIAL_VERSION != SECTION_FILE_VERSION,
              "SECTION_FILE_PARTIAL_VERSION collides with SECTION_FILE_VERSION (they meet at 141)");
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(bool) /*boldBodyText*/ +
                                 sizeof(uint8_t) /*cssState*/ + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
// v187：五個「只讀檔頭尾段」的讀取點（loadPageAt／anchor／paragraph／li）原本不驗版號——跳號後
// 還沒重開的舊 .bin 會被用舊偏移讀，拿到的是別的欄位（KOReader 同步的 ProgressMapper 會經過這條路）。
bool headerVersionOk(HalFile& f) {
  uint8_t version = 0;
  serialization::readPod(f, version);
  return version == SECTION_FILE_VERSION || version == SECTION_FILE_PARTIAL_VERSION;
}
}  // namespace

// Out-of-line so the unique_ptr<ChapterHtmlSlimParser> in BuildContext can be
// constructed/destroyed where the parser's full definition is visible.
Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}

// Suspend any in-progress build so every section.reset() / navigation / sleep path
// persists the pages already laid out as a partial .bin instead of discarding them
// (no-op once a build has completed or never started).
Section::~Section() { suspendBuild(); }

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", builtPageCount_);
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", builtPageCount_);
    return 0;
  }
  ParsedText::noteBuildProbe(3);  // v190：頁序列化到 SD，parseStep 內另一段不返回
  LOG_DBG("SCT", "Page %d processed", builtPageCount_);

  builtPageCount_++;
  // pageCount is the pages available to read: a rebuild over a partial only raises it
  // once it has laid out more pages than the partial already covers.
  if (builtPageCount_ > pageCount) {
    pageCount = builtPageCount_;
  }
  return position;
}

void Section::writeSectionFileHeader(const ReaderRenderSpec& spec) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(spec.fontId) + sizeof(spec.lineCompression) +
                                   sizeof(spec.extraParagraphSpacing) + sizeof(spec.paragraphAlignment) +
                                   sizeof(spec.viewportWidth) + sizeof(spec.viewportHeight) + sizeof(pageCount) +
                                   sizeof(spec.hyphenationEnabled) + sizeof(spec.embeddedStyle) +
                                   sizeof(spec.imageRendering) + sizeof(spec.focusReadingEnabled) +
                                   sizeof(spec.boldBodyText) + sizeof(uint8_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  // Written as the incomplete sentinel; finalizeBuild() patches it to
  // SECTION_FILE_VERSION as the last step, committing the file.
  serialization::writePod(file, SECTION_FILE_INCOMPLETE_VERSION);
  serialization::writePod(file, spec.fontId);
  serialization::writePod(file, spec.lineCompression);
  serialization::writePod(file, spec.extraParagraphSpacing);
  serialization::writePod(file, spec.paragraphAlignment);
  serialization::writePod(file, spec.viewportWidth);
  serialization::writePod(file, spec.viewportHeight);
  serialization::writePod(file, spec.hyphenationEnabled);
  serialization::writePod(file, spec.embeddedStyle);
  serialization::writePod(file, spec.imageRendering);
  serialization::writePod(file, spec.focusReadingEnabled);
  serialization::writePod(file, spec.boldBodyText);
  serialization::writePod(file, CSS_STATE_NONE);  // 佔位，commitBuildFile 補成這次建置的實際狀態
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for visible-offset LUT (patched later)
}

bool Section::loadSectionFile(const ReaderRenderSpec& spec) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  bool filePartial = false;
  {
    uint8_t version;
    serialization::readPod(file, version);
    lastLoadReject_ = 0;
    if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      lastLoadReject_ = 1;
      clearCache();
      return false;
    }
    filePartial = (version == SECTION_FILE_PARTIAL_VERSION);

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    bool fileBoldBodyText;
    uint8_t fileCssState;
    serialization::readPod(file, fileFontId);
    serialization::readPod(file, fileLineCompression);
    serialization::readPod(file, fileExtraParagraphSpacing);
    serialization::readPod(file, fileParagraphAlignment);
    serialization::readPod(file, fileViewportWidth);
    serialization::readPod(file, fileViewportHeight);
    serialization::readPod(file, fileHyphenationEnabled);
    serialization::readPod(file, fileEmbeddedStyle);
    serialization::readPod(file, fileImageRendering);
    serialization::readPod(file, fileFocusReadingEnabled);
    serialization::readPod(file, fileBoldBodyText);
    serialization::readPod(file, fileCssState);

    if (spec.fontId != fileFontId || spec.lineCompression != fileLineCompression ||
        spec.extraParagraphSpacing != fileExtraParagraphSpacing || spec.paragraphAlignment != fileParagraphAlignment ||
        spec.viewportWidth != fileViewportWidth || spec.viewportHeight != fileViewportHeight ||
        spec.hyphenationEnabled != fileHyphenationEnabled || spec.embeddedStyle != fileEmbeddedStyle ||
        spec.imageRendering != fileImageRendering || spec.focusReadingEnabled != fileFocusReadingEnabled ||
        spec.boldBodyText != fileBoldBodyText) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      lastLoadReject_ = 2;
      clearCache();
      return false;
    }
    // v187：這份版面是在 CSS 撞地板、規則集被截斷（或根本載不到）的情況下排的（樣式打折）。當時是為了
    // 「書開得起來」，不是永久正確。現在記憶體寬裕（門檻高於過濾模式地板 20KB 很多）就丟掉重排【一次】：
    // 重建若又截斷會寫 CSS_STATE_TRUNCATED_FINAL，之後不再為此重排（複查：沒有這條會每次開章都重排）。
    // ⚠️ partial 不因 CSS 丟掉（丟掉＝整章從第 0 頁重排，而延伸建置沒有 framebuffer 借用、更容易再截斷 →
    //    驗證者抓到的乒乓）；partial 由延伸建置決定最終狀態，且延伸時若再截斷就寫 FINAL。
    if (!filePartial && spec.embeddedStyle && fileCssState == CSS_STATE_TRUNCATED &&
        heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) >= 64 * 1024) {
      file.close();
      LOG_INF("SCT", "Cached layout was built with a truncated CSS rule set; rebuilding once now that memory allows");
      lastLoadReject_ = 3;
      cssRetry_ = true;
      clearCache();
      return false;
    }
    // 載入成功：partial 的延伸建置、或已標 FINAL 的章，之後若再截斷一律寫 FINAL——狀態只能往「定案」走，
    // 不能被延伸建置降回 2 再被下次開啟丟掉。
    cssRetry_ = filePartial || fileCssState == CSS_STATE_TRUNCATED_FINAL;
  }

  serialization::readPod(file, pageCount);

  if (filePartial) {
    // A partial's pageCount is the watermark of a suspended build. Read the watermark
    // trailer (appended after the visible-offset LUT) so estimatedTotalPages can extrapolate.
    uint32_t liLutOffset = 0;
    file.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
    serialization::readPod(file, liLutOffset);
    uint32_t visibleLutOffset = 0;
    file.seek(HEADER_SIZE - sizeof(uint32_t));
    serialization::readPod(file, visibleLutOffset);
    const uint32_t trailerOffset = visibleLutOffset + static_cast<uint32_t>(pageCount) * sizeof(uint32_t);
    const bool trailerValid = pageCount > 0 && liLutOffset >= HEADER_SIZE && visibleLutOffset > liLutOffset &&
                              trailerOffset + 2 * sizeof(uint32_t) <= file.size();
    if (!trailerValid) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: malformed partial section");
      lastLoadReject_ = 4;
      clearCache();
      pageCount = 0;
      return false;
    }
    file.seek(trailerOffset);
    serialization::readPod(file, partialBytesConsumed_);
    serialization::readPod(file, partialTotalBytes_);
    partial_ = true;
    partialPageCount_ = pageCount;
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages%s", pageCount, filePartial ? " (partial)" : "");
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  const std::string tmpBin = binTmpPath();
  if (Storage.exists(tmpBin.c_str())) {
    Storage.remove(tmpBin.c_str());
  }
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  // One-shot build: start, then lay out the whole section in a single pass.
  if (!startBuild(spec, popupFn)) {
    return false;
  }
  if (!buildSomeMore(0)) {  // 0 = build to completion
    return false;
  }
  return buildComplete_;
}

// v175（diag174）：startBuild 有三個出口（inflate 失敗、bin 開檔、beginParse）沒有掛 lastBuildWasLowMemory_
// latch —— XML_ParserCreate 與 inflate 的 zlib 狀態在最大連續塊只剩 7KB 時會失敗，呼叫端卻只看得到
// 「無效的書籍檔」（diag174：一次 lowmem 之後連續 8 次「索引失敗」，每次 300ms 就退）。
// 這裡不改失敗語意，只在出口當下堆積真的瀕死（<16KB）時補 latch，讓呼叫端分流成「記憶體不足」。
bool Section::startBuild(const ReaderRenderSpec& spec, const std::function<void()>& popupFn) {
  lastBuildWasLowMemory_ = false;
  if (build_) {
    LOG_ERR("SCT", "startBuild called while a build is already active");
    return false;
  }
  buildComplete_ = false;
  builtPageCount_ = 0;
  // Pages from a loaded partial stay readable (from filePath) while this build writes
  // to the tmp .bin, so availability never drops below the partial's watermark.
  pageCount = partial_ ? partialPageCount_ : 0;

  // Remove a stale tmp .bin from a crash-interrupted build; this build recreates it.
  {
    const std::string staleTmp = binTmpPath();
    if (Storage.exists(staleTmp.c_str())) {
      Storage.remove(staleTmp.c_str());
    }
  }

  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto htmlDir = epub->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Reuse the previously unzipped HTML if we already have it. The unzipped HTML is keyed only on the
  // book (it lives in the per-book cache dir), not on render settings, so it survives the invalidation
  // that wipes the layout (.bin) caches when font/margin/orientation change -- rebuilds then skip zip
  // inflation entirely. It's promoted by an atomic rename as soon as the inflate succeeds (below), so
  // even a window-only giant spine -- whose .bin never finalizes -- still caches its HTML, letting a
  // reopen skip the multi-second inflate. If htmlPath exists it is known-complete.
  const bool reusedHtml = Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml;
  if (reusedHtml) {
    LOG_DBG("SCT", "Reusing cached HTML %s", htmlPath.c_str());
  } else {
    Storage.mkdir(htmlDir.c_str());

    // Retry logic for SD card timing issues
    bool streamed = false;
    uint32_t fileSize = 0;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      // Larger chunks mean far fewer SD writes inflating the HTML; a 1KB chunk turned a 584KB
      // single-spine novel into ~570 tiny writes (multi-second). 8KB keeps the transient buffers
      // small while cutting the write count 8x.
      streamed = epub->readItemContentsToStream(localPath, tmpHtml, 8192);
      fileSize = tmpHtml.size();
      // Explicitly close() file before calling Storage.remove()
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 16 * 1024) lastBuildWasLowMemory_ = true;  // v175：見下
      return false;
    }

    LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

    // Promote to the persistent HTML cache immediately -- the inflate is complete and the bytes are
    // valid regardless of whether the layout build finishes, so reopening (even a window-only spine
    // that never finalizes its .bin) skips re-inflation. If the rename fails we just parse the temp.
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }

  if (!Storage.openFileForWrite("SCT", binTmpPath(), file)) {
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 16 * 1024) lastBuildWasLowMemory_ = true;  // v175：見下
    return false;
  }
  // Header is written with the incomplete-version sentinel; finalizeBuild() commits it.
  writeSectionFileHeader(spec);

  auto ctx = makeUniqueNoThrow<BuildContext>();
  if (!ctx) {
    LOG_ERR("SCT", "OOM: BuildContext");
    lastBuildWasLowMemory_ = true;  // v165：讓呼叫端分流成「記憶體不足」而非「無效的書籍檔」
    file.close();
    Storage.remove(binTmpPath().c_str());
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  // htmlCached == "htmlPath is the live cache" (reused, or just promoted). finalizeBuild/abandonBuild
  // then leave the cached HTML alone; only an un-promoted temp (rename failed) is theirs to clean up.
  ctx->reusedHtml = htmlCached;
  ctx->htmlPath = htmlPath;
  ctx->tmpHtmlPath = tmpHtmlPath;
  ctx->parsePath = htmlCached ? htmlPath : tmpHtmlPath;

  // Derive the content base directory and image cache path prefix for the parser
  const size_t lastSlash = localPath.find_last_of('/');
  ctx->contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  ctx->imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  if (spec.embeddedStyle) {
    ctx->cssParser = epub->getCssParser();
    // v176：按章過濾 —— parsePath 此刻已是完整的章節 HTML（inflate 在上面完成）。
    // 載入失敗與截斷同等對待（都是「樣式打折的版面」，複查：NONE 不能同時代表「沒開 CSS」與「載不到」）；
    // 已經為此重排過一次就標 FINAL，不再重排。
    if (ctx->cssParser && !ctx->cssParser->loadFromCache(ctx->parsePath.c_str())) {
      LOG_ERR("SCT", "Failed to load CSS from cache");
      ctx->cssState = cssRetry_ ? CSS_STATE_TRUNCATED_FINAL : CSS_STATE_TRUNCATED;
    } else if (ctx->cssParser) {
      ctx->cssState = !ctx->cssParser->lastLoadTruncated_ ? CSS_STATE_FULL
                      : (cssRetry_ ? CSS_STATE_TRUNCATED_FINAL : CSS_STATE_TRUNCATED);
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  // The parser stores the path/contentBase/imageBasePath by reference, so they must
  // live in the BuildContext (which outlives the parser). The page-complete callback
  // captures the BuildContext pointer to append to its in-RAM LUT; build_ owns the
  // context for the parser's whole lifetime.
  BuildContext* ctxPtr = ctx.get();
  ctx->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      epub, ctxPtr->parsePath, renderer, spec.fontId, spec.lineCompression, spec.extraParagraphSpacing,
      spec.paragraphAlignment, spec.viewportWidth, spec.viewportHeight, spec.hyphenationEnabled,
      spec.focusReadingEnabled,
      [this, ctxPtr](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
                     const uint32_t visibleTextOffset) {
        ctxPtr->lut.push_back(
            {this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex, visibleTextOffset});
      },
      spec.embeddedStyle, ctxPtr->contentBase, ctxPtr->imageBasePath, spec.imageRendering, std::move(tocAnchors),
      popupFn, ctxPtr->cssParser);
  if (!ctx->parser) {
    LOG_ERR("SCT", "OOM: ChapterHtmlSlimParser");
    lastBuildWasLowMemory_ = true;  // v165：同 BuildContext——分流成「記憶體不足」
    if (ctx->cssParser) ctx->cssParser->clear();
    file.close();
    Storage.remove(binTmpPath().c_str());
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  Hyphenator::setPreferredLanguage(epub->getLanguage());
  build_ = std::move(ctx);

  if (!build_->parser->beginParse()) {
    LOG_ERR("SCT", "Failed to begin parse");
    abandonBuild();
    if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 16 * 1024) lastBuildWasLowMemory_ = true;  // v175：見下
    return false;
  }
  build_->totalBytes = build_->parser->parseTotalBytes();
  // v189：頁表一次配到位。push_back 的倍增成長（12→24→…→3,072B）是「先配新的、複製、再刪舊」
  // 的搬遷，每一步都在 p2 中段留一個洞（CLAUDE.md 硬限制 6）；排到底之後整章的 LUT 都在爆發期內
  // 長出來，洞會排成一串。估計值：這台中文書約 2KB HTML／頁（含標記），寧可多估（上限 320 筆＝3.8KB，
  // 巨型章超過再讓它倍增一次）也不要少估；partial 延伸時 watermark 是已知下界。
  // 守衛：reserve 走會丟例外的 operator new，-fno-exceptions 下配不到就 abort——地板之下不 reserve，
  // 退回倍增（原本的行為）。
  {
    size_t want = build_->totalBytes / 2048;
    if (want < 64) want = 64;
    if (want > 320) want = 320;
    if (partial_ && want < static_cast<size_t>(partialPageCount_) + 16) want = partialPageCount_ + 16;
    const size_t bytes = want * sizeof(PageLutEntry);
    if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) > bytes + 8 * 1024) build_->lut.reserve(want);
  }
  return true;
}

uint32_t Section::buildStepMaxMs = 0;
uint32_t Section::buildStepTotalUs = 0;
uint32_t Section::buildStepCount = 0;
int Section::lastPoisonAvoidedSpine = -1;

bool Section::buildSomeMore(const int maxPages, bool (*shouldYield)(void*), void* yieldCtx) {
  if (!build_ || !build_->parser) {
    LOG_ERR("SCT", "buildSomeMore with no active build");
    return false;
  }
  // Pace on pages laid out by THIS build, not pageCount: during a rebuild over a partial,
  // pageCount stays pinned at the partial's watermark until the build passes it, which
  // would otherwise turn one "small" chunk into a blocking rebuild of the whole watermark.
  const int startCount = builtPageCount_;
  for (;;) {
    // v189：每一步之前問一次（第一步也問：按鍵已經按著就一步都別做）。
    if (shouldYield && shouldYield(yieldCtx)) {
      build_->bytesConsumed = build_->parser->parseBytesConsumed();
      return true;
    }
    // v189 儀器：一步多長。一步＝1KB HTML 餵 expat，但 callback 裡會排完整個段落（可能好幾頁）、
    // 序列化到 SD、圖片探頭——這才是按鍵盲區的真實上界，不是「1KB」。只有量到才能說它小。
    const int64_t stepT0 = esp_timer_get_time();
    ParsedText::resetBuildProbeClock();  // v190：與 stepmax 同一處起算，不含步間讓路
    const auto status = build_->parser->parseStep();
    ParsedText::stopBuildProbeClock();  // v190：步外的排版（設定頁預覽）不得計入盲區
    const auto noteStep = [&]() {
      const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - stepT0);
      if (us / 1000 > buildStepMaxMs) buildStepMaxMs = us / 1000;
      buildStepTotalUs += us;
      buildStepCount++;
    };
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      // 收尾（最後一頁的排版＋序列化＋commit 改名）也在同一個 tick 裡、也不能讓路——算進最後一步。
      const bool fin = finalizeBuild();
      noteStep();
      return fin;
    }
    noteStep();
    if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
      // v149：低記憶體中止與「XHTML 真的壞了」要分開處置。
      // abandonBuild() 連既有 partial 與 filePath 一起刪 —— 對會重現的 parse error 是對的，
      // 對暫時性 OOM 是災難：使用者每重試一次就更貴，還配上無退避的重建迴圈。
      // OOM 走 suspendBuild()：保留舊 partial、丟掉這輪的 tmp，等記憶體寬鬆時再試。
      if (build_->parser->hasBuildAborted()) {
        LOG_ERR("SCT", "Low-memory abort during incremental build, keeping partial");
        lastBuildWasLowMemory_ = true;
        suspendBuild();
      } else {
        LOG_ERR("SCT", "Parse error during incremental build");
        abandonBuild();
      }
      return false;
    }
    // ParseStatus::More: yield once we've laid out the requested number of pages.
    if (maxPages > 0 && (builtPageCount_ - startCount) >= maxPages) {
      build_->bytesConsumed = build_->parser->parseBytesConsumed();
      return true;
    }
  }
}

bool Section::hasHtmlCache() const {
  const std::string htmlPath = epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

std::optional<uint16_t> Section::findAnchorDuringBuild(const std::string& anchor) const {
  if (!build_ || !build_->parser) return std::nullopt;
  for (const auto& [key, page] : build_->parser->getAnchors()) {
    if (key == anchor) return page;
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::findAnchor(const std::string& anchor) const {
  if (const auto page = findAnchorDuringBuild(anchor)) {
    return page;
  }
  // Fall back to the on-disk anchor map: a finalized section, or a partial whose map
  // covers everything up to its watermark (nullopt past it -- build further and retry).
  return getPageForAnchor(anchor);
}

uint16_t Section::estimatedTotalPages() const {
  // Extrapolation from a suspended session's watermark trailer. A static snapshot, so no EMA
  // damping is needed. Also the best guess while a rebuild is running but hasn't laid out
  // enough pages yet to extrapolate from its own progress.
  const auto partialEstimate = [this]() -> uint16_t {
    if (!partial_ || partialBytesConsumed_ == 0 || partialTotalBytes_ <= partialBytesConsumed_) {
      return pageCount;
    }
    const uint64_t est = static_cast<uint64_t>(partialPageCount_) * partialTotalBytes_ / partialBytesConsumed_;
    if (est <= pageCount) return pageCount;
    return est > 60000 ? 60000 : static_cast<uint16_t>(est);
  };

  if (!build_) {
    return partial_ ? partialEstimate() : pageCount;  // partial -> extrapolate, finalized -> exact
  }
  const uint32_t consumed = build_->bytesConsumed;
  const uint32_t total = build_->totalBytes;
  if (builtPageCount_ == 0 || consumed == 0 || total <= consumed) return partialEstimate();

  // Raw extrapolation: scale the pages built so far by the fraction of HTML still unparsed. This
  // re-derives from a growing, non-uniform sample, so it jitters up and down as the build crosses
  // dense vs sparse regions of the chapter.
  const uint64_t raw = static_cast<uint64_t>(builtPageCount_) * total / consumed;

  // Damp that jitter with an exponential moving average. Step it once per build advance (keyed on
  // bytesConsumed) rather than per status-bar redraw, so the smoothing rate doesn't depend on how
  // often we repaint. As the build nears the end, consumed -> total and raw -> the built count, so
  // the average settles onto the true count (and finalizeBuild then returns the exact pageCount).
  constexpr float ALPHA = 0.25f;  // weight of each new sample; lower = steadier but slower to settle
  if (build_->smoothedEstimate <= 0) {
    build_->smoothedEstimate = static_cast<float>(raw);  // seed on the first estimate
  } else if (consumed != build_->smoothedAtConsumed) {
    build_->smoothedEstimate += ALPHA * (static_cast<float>(raw) - build_->smoothedEstimate);
  }
  build_->smoothedAtConsumed = consumed;

  const uint64_t est = static_cast<uint64_t>(build_->smoothedEstimate + 0.5f);
  if (est <= pageCount) return pageCount;  // never fewer than the pages already available
  return est > 60000 ? 60000 : static_cast<uint16_t>(est);
}

// Write the LUTs and anchor map into the open tmp .bin, patch the header with the built
// page count and table offsets, stamp `version` as the commit point, then swap the tmp
// file over filePath. For SECTION_FILE_PARTIAL_VERSION a watermark trailer
// (bytesConsumed, totalBytes) is appended after the li LUT so a later open can estimate
// the total page count. The parser must still be alive (anchors are read from it).
// On failure the tmp is removed and any pre-existing file at filePath is left intact.
bool Section::commitBuildFile(const uint8_t version, const uint32_t bytesConsumed, const uint32_t totalBytes) {
  const bool asPartial = (version == SECTION_FILE_PARTIAL_VERSION);

  const auto failCommit = [this]() {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
    return false;
  };

  const uint32_t lutOffset = file.position();
  for (const auto& entry : build_->lut) {
    if (entry.fileOffset == 0) {
      LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
      return failCommit();
    }
    serialization::writePod(file, entry.fileOffset);
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets). For a
  // partial, skip anchors that landed on the incomplete trailing page the suspend drops.
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = build_->parser->getAnchors();
  uint16_t anchorCount = 0;
  for (const auto& [anchor, page] : anchors) {
    if (!asPartial || page < builtPageCount_) anchorCount++;
  }
  serialization::writePod(file, anchorCount);
  for (const auto& [anchor, page] : anchors) {
    if (asPartial && page >= builtPageCount_) continue;
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(build_->lut.size()));
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  const uint32_t visibleLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.visibleTextOffset);
  }

  if (asPartial) {
    // Watermark trailer, located on load immediately after the visible-offset LUT.
    serialization::writePod(file, bytesConsumed);
    serialization::writePod(file, totalBytes);
  }

  // Patch header with the CSS state (v187), the built page count and section offsets...
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(builtPageCount_) - sizeof(uint8_t));
  serialization::writePod(file, build_->cssState);
  serialization::writePod(file, builtPageCount_);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  serialization::writePod(file, visibleLutFileOffset);
  // ...then commit by overwriting the sentinel version with the real one. Writing the
  // version last makes it the commit point: a crash before here leaves version 0.
  file.seek(0);
  serialization::writePod(file, version);
  // Explicit close() required: member variable persists beyond function scope
  file.close();

  // Swap into place. A crash between remove and rename loses the old file but keeps a
  // fully-committed tmp; the next build just removes it and rebuilds.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!Storage.rename(binTmpPath().c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to move built section into place");
    Storage.remove(binTmpPath().c_str());
    return false;
  }
  return true;
}

bool Section::finalizeBuild() {
  // Flush the trailing page (emits the last page via the completePageFn into the LUT).
  // v149：⚠️ 回傳值要接。低記憶體中止的半截章節不可提交（教訓 A-20）。
  // 不走 abandonBuild() —— 它會連既有 partial 一起刪（它的理由「parse error 會重現」
  // 對暫時性 OOM 不成立）；suspendBuild() 配下面的 aborted 檢查 = 不提交這份壞的、
  // 保留舊的、丟掉 tmp。
  if (!build_->parser->finishParse()) {
    LOG_ERR("SCT", "finalizeBuild: low-memory abort, keeping existing partial");
    lastBuildWasLowMemory_ = true;
    suspendBuild();
    return false;
  }

  if (!build_->reusedHtml) {
    // Parse succeeded: promote the freshly unzipped HTML to the persistent cache so future
    // rebuilds skip zip inflation. If promotion fails, drop the temp -- the build still succeeded.
    if (!Storage.rename(build_->tmpHtmlPath.c_str(), build_->htmlPath.c_str())) {
      LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
      Storage.remove(build_->tmpHtmlPath.c_str());
    }
  }

  const bool committed = commitBuildFile(SECTION_FILE_VERSION, 0, 0);
  if (build_->cssParser) build_->cssParser->clear();
  build_.reset();
  if (!committed) {
    // commitBuildFile removed filePath before the failed swap, so nothing valid remains.
    partial_ = false;
    partialPageCount_ = 0;
    pageCount = 0;
    builtPageCount_ = 0;
    return false;
  }
  buildComplete_ = true;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = builtPageCount_;
  return true;
}

void Section::suspendBuild() {
  if (!build_) return;

  // Only worth persisting if this build produced pages a pre-existing partial doesn't
  // already cover; otherwise keep the older (bigger) partial and just drop the tmp.
  // v149：低記憶體中止的建置不可落地 —— 它的頁數看起來變多，但內容掉過字。
  // ~Section()（使用者離開書、裝置休眠）也走這裡，少了這條，半截章節就以 PARTIAL 進 SD。
  const bool aborted = build_->parser && build_->parser->hasBuildAborted();
  if (aborted) {
    // v194：低記憶體中止的排版不可寫成看起來合法的 SD 快取（教訓 A-20）。
    lastPoisonAvoidedSpine = spineIndex;
    LOG_ERR("SCT", "SECTPOISON avoided spine=%d", spineIndex);
  }
  const bool worthKeeping =
      !aborted && builtPageCount_ > 0 && (!partial_ || builtPageCount_ > partialPageCount_);

  bool committed = false;
  if (worthKeeping) {
    // Capture the parse watermark and commit before tearing the parser down (the anchor
    // map is read from it). The incomplete trailing page is intentionally not flushed:
    // only fully laid-out pages are persisted, and the rebuild re-derives the rest.
    const uint32_t consumed = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
    committed = commitBuildFile(SECTION_FILE_PARTIAL_VERSION, consumed, build_->totalBytes);
    if (committed) {
      partial_ = true;
      partialPageCount_ = builtPageCount_;
      partialBytesConsumed_ = consumed;
      partialTotalBytes_ = build_->totalBytes;
      LOG_INF("SCT", "Suspended build: %u pages persisted", builtPageCount_);
    }
  }

  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (!committed && file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  pageCount = partial_ ? partialPageCount_ : 0;
  builtPageCount_ = 0;
}

void Section::abandonBuild() {
  if (!build_) return;
  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
  }
  // A parse error would recur against the same HTML, so drop any partial too -- resuming
  // from it would just re-enter the failing build every open.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = 0;
  builtPageCount_ = 0;
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const int page) {
  if (!build_ || page < 0 || page >= static_cast<int>(build_->lut.size()) || !file) {
    return nullptr;
  }
  const uint32_t pos = build_->lut[page].fileOffset;
  if (pos == 0) {
    return nullptr;
  }
  // The .bin is open O_RDWR for the build. Read the already-written page, then restore
  // the write cursor so the next onPageComplete keeps appending where it left off.
  const uint32_t writePos = file.position();
  file.seek(pos);
  auto p = Page::deserialize(file);
  file.seek(writePos);
  if (p) {
    p->visibleTextOffset = build_->lut[page].visibleTextOffset;
  } else if (Page::lastAllocFail[0] != '\0') {
    // v194：反序列化配不到 Page → 暫時性 OOM，不是壞檔。
    lastLoadWasLowMemory_ = true;
  }
  return p;
}

// Read a page from the committed file at filePath (finalized section or partial from a
// previous session). Uses a local handle so it is safe while a build holds the member
// `file` open on the tmp .bin.
std::unique_ptr<Page> Section::loadPageAt(const int page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return nullptr;
  }
  if (!headerVersionOk(f)) return nullptr;  // v187

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5);
  uint32_t lutOffset;
  serialization::readPod(f, lutOffset);
  f.seek(lutOffset + sizeof(uint32_t) * page);
  uint32_t pagePos;
  serialization::readPod(f, pagePos);

  // Read this page's visible-codepoint start offset from the visible-offset LUT (last header slot)
  // in the same open handle, so the reader can persist progress without reopening the section file
  // on every page turn (see Page::visibleTextOffset). A malformed/old file leaves it at 0.
  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  uint32_t visibleTextOffset = 0;
  const uint32_t visibleEntry = visibleLutOffset + sizeof(uint32_t) * page;
  if (visibleLutOffset >= HEADER_SIZE && visibleEntry + sizeof(uint32_t) <= f.size()) {
    f.seek(visibleEntry);
    serialization::readPod(f, visibleTextOffset);
  }

  f.seek(pagePos);
  auto p = Page::deserialize(f);
  if (p) {
    p->visibleTextOffset = visibleTextOffset;
  } else if (Page::lastAllocFail[0] != '\0') {
    lastLoadWasLowMemory_ = true;
  }
  return p;
  // No f.close() needed -- DESTRUCTOR_CLOSES_FILE=1 handles it at scope exit
}

std::unique_ptr<Page> Section::loadPage(const int page) {
  lastLoadWasLowMemory_ = false;
  if (page < 0) {
    return nullptr;
  }
  // v152：反序列化一頁要配 PageLine/TextBlock 與它們的字詞 vector（都是 throwing）。
  // 實機 v151 的 abort：圖片頁 render 期間（pxc slot 握著大塊）loadPage 的
  // shared_ptr 控制塊（~32 bytes！）配不到 -> terminate。32B 失敗 = 那一瞬間堆積是零，
  // 事前地板是唯一擋法。4KB 遠低於任何正常狀態（CAPS 實測 boot 114K、穩態 >24K），
  // 只在瀕死時成立；回 nullptr 由呼叫端走「跳過本輪、下輪重試」的降級。
  // v187：註腳項目 96→256 後 footnotes.resize(16) 要 4,608 B 連續——那一筆改在 Page::deserialize 讀到
  // fnCount 後單獨守（守不過就這次不帶註腳），地板維持 4KB（v152 刻意設在瀕死線）。
  if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 4 * 1024) {
    LOG_ERR("SCT", "loadPage deferred: low memory (defaultMax=%u)",
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
    lastLoadWasLowMemory_ = true;
    return nullptr;
  }
  if (build_ && page < static_cast<int>(build_->lut.size())) {
    return loadPageDuringBuild(page);
  }
  // Not (yet) in the active build: serve from the file on disk -- a finalized section,
  // or a partial from a previous session whose pages the rebuild hasn't reached again.
  const int onDisk = partial_ ? partialPageCount_ : (build_ ? 0 : pageCount);
  if (page >= onDisk) {
    return nullptr;
  }
  return loadPageAt(page);
}

namespace {
// 中文排版裡每個漢字自成一個「詞」（CJK 逐字斷行），重組純文字時字與字之間不能補空格，
// 否則輸出變成「偉 大 力 量。」（v39/v40 實機回報：QR 掃出的書摘每字帶空格；書籤摘要同病）。
// 副作用不只難看：payload 膨脹約 33%（3 bytes 的漢字變 4 bytes），而 QR 的真實容量上限
// 是 858 bytes（ECC_LOW / version 20，見 util/QrUtils.cpp）—— 可容字數直接少四分之一。
//
// lead byte 判斷：0xE3-0xE9 = U+3000-U+9FFF（CJK 標點／假名／注音／漢字）、0xEF = 全形區。
// 兩側皆非 CJK（＝拉丁詞之間）才補空格，維持英文原樣。
bool cjkLeadByte(const uint8_t b) { return (b >= 0xE3 && b <= 0xE9) || b == 0xEF; }

uint8_t lastUtf8LeadByte(const std::string& s) {
  for (size_t i = s.size(); i > 0; --i) {
    const uint8_t b = static_cast<uint8_t>(s[i - 1]);
    if ((b & 0xC0) != 0x80) return b;  // 跳過 UTF-8 continuation bytes
  }
  return 0;
}
}  // namespace

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = loadPage(currentPage);
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const auto& block = *line.getBlock();
          for (uint16_t i = 0; i < block.wordCount(); i++) {
            const char* w = block.wordText(i);
            if (w == nullptr || w[0] == '\0') continue;
            if (!fullText.empty()) {
              const uint8_t prev = lastUtf8LeadByte(fullText);
              const uint8_t cur = static_cast<uint8_t>(w[0]);
              if (!cjkLeadByte(prev) && !cjkLeadByte(cur)) fullText += " ";
            }
            fullText += w;
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  // Only a finalized section's count is the chapter total; a partial's count is just the
  // suspended build's watermark, which would skew progress mapping. Callers fall back to
  // their own estimates.
  uint8_t version;
  serialization::readPod(f, version);
  if (version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  if (!headerVersionOk(f)) return std::nullopt;  // v187
  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 4);
  uint32_t anchorMapOffset;
  serialization::readPod(f, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  if (!headerVersionOk(f)) return std::nullopt;  // v187
  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    serialization::readPod(f, pagePIdx);
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  if (!headerVersionOk(f)) return std::nullopt;  // v187
  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t));
  uint16_t pIdx;
  serialization::readPod(f, pIdx);
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  if (!headerVersionOk(f)) return std::nullopt;  // v187
  const uint32_t fileSize = f.size();
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 2);
  uint32_t liLutOffset;
  serialization::readPod(f, liLutOffset);
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  f.seek(HEADER_SIZE - sizeof(uint32_t) * 3);
  uint32_t paragraphLutOffset;
  serialization::readPod(f, paragraphLutOffset);
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  f.seek(paragraphLutOffset);
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  f.seek(liLutOffset);
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    serialization::readPod(f, pageLiIdx);
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  if (build_ && page < build_->lut.size()) {
    return build_->lut[page].visibleTextOffset;
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f) || f.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  if (page >= count) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  const uint32_t entryOffset = visibleLutOffset + static_cast<uint32_t>(page) * sizeof(uint32_t);
  if (visibleLutOffset < HEADER_SIZE || entryOffset + sizeof(uint32_t) > f.size()) {
    return std::nullopt;
  }

  f.seek(entryOffset);
  uint32_t result;
  serialization::readPod(f, result);
  return result;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset,
                                                             const bool preferFirstAtOffset) const {
  const auto findInEntries = [offset, preferFirstAtOffset](const auto& entries) -> std::optional<uint16_t> {
    if (entries.empty()) return std::nullopt;
    uint16_t result = 0;
    for (size_t i = 0; i < entries.size(); i++) {
      const uint32_t pageStart = entries[i].visibleTextOffset;
      if (preferFirstAtOffset && pageStart == offset) {
        return static_cast<uint16_t>(i);
      }
      if (pageStart > offset) break;
      result = static_cast<uint16_t>(i);
    }
    return result;
  };

  if (build_ && !build_->lut.empty()) {
    // Resolve within the active build's known range. Later offsets may still be
    // covered by an on-disk partial that the resumed build has not reached yet.
    if (offset <= build_->lut.back().visibleTextOffset) {
      return findInEntries(build_->lut);
    }
  }

  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f) || f.size() < HEADER_SIZE) {
    return std::nullopt;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
    return std::nullopt;
  }
  const bool partial = version == SECTION_FILE_PARTIAL_VERSION;

  f.seek(HEADER_SIZE - sizeof(uint32_t) * 5 - sizeof(uint16_t));
  uint16_t count;
  serialization::readPod(f, count);
  if (count == 0) {
    return std::nullopt;
  }

  f.seek(HEADER_SIZE - sizeof(uint32_t));
  uint32_t visibleLutOffset;
  serialization::readPod(f, visibleLutOffset);
  if (visibleLutOffset < HEADER_SIZE || visibleLutOffset + static_cast<uint32_t>(count) * sizeof(uint32_t) > f.size()) {
    return std::nullopt;
  }

  f.seek(visibleLutOffset);
  uint16_t result = 0;
  uint32_t lastPageStart = 0;
  for (uint16_t page = 0; page < count; page++) {
    uint32_t pageStart;
    serialization::readPod(f, pageStart);
    lastPageStart = pageStart;
    if (preferFirstAtOffset && pageStart == offset) {
      return page;
    }
    if (pageStart > offset) break;
    result = page;
  }
  if (partial && offset > lastPageStart) {
    return std::nullopt;
  }
  return result;
}
