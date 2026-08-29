#include <Arduino.h>
#include <cstdarg>
#include "ImageBlock.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

char ImageBlock::lastFailPath[112] = {0};

// v24/v148 低記憶體紓解 hook（見標頭）。
// 門檻 48KB：抽取的最大單塊是 32,768 的 LZ77 window，解碼器物件另需 20–44KB（先後配置，
// 非同時）；48KB 涵蓋「window ＋ 小配置」與「解碼器 ＋ 邊際」兩種形狀。實測 v147 在
// maxAlloc=32,160 時 window 差 608 bytes 配不到 —— 卸載字型可騰回 43K+ 的連續塊。
// 過度觸發的代價只是一次字型重載（約 300ms SD 讀取），且只發生在「首次看到這張圖」。
static constexpr size_t IMAGE_RENDER_RELIEF_MAX_ALLOC = 48 * 1024;
// v54 的教訓：解碼器自己另有【總量】門檻（MIN_FREE_HEAP_FOR_PNG ≈ 60KB / JPEG ≈ 36KB，
// 都是對 getFreeHeap）。只看連續塊時，總量落在中間帶 relief 不觸發、卻被解碼器自己擋下
// → 圖片變佔位框，而且 rememberImageFailure 讓它【整個 session 不再重試】。
// 兩側都檢查把這個縫隙補起來；76KB = PNG 的 60KB 門檻 + 16KB 邊際。
static constexpr size_t IMAGE_RENDER_RELIEF_MIN_FREE = 76 * 1024;
static ImageBlock::MemoryReliefFn g_imageReliefFn = nullptr;
static ImageBlock::MemoryReliefFn g_imageRestoreFn = nullptr;
static void* g_imageReliefCtx = nullptr;

void ImageBlock::setMemoryReliefHooks(MemoryReliefFn reliefFn, MemoryReliefFn restoreFn, void* ctx) {
  g_imageReliefFn = reliefFn;
  g_imageRestoreFn = restoreFn;
  g_imageReliefCtx = ctx;
}

void ImageBlock::noteFailure(const char* fmt, ...) {
  if (lastFailPath[0] != '\0') return;  // 先到先得，見標頭
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(lastFailPath, sizeof(lastFailPath), fmt, ap);
  va_end(ap);
}

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void* ImageBlock::extractCtx = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;

void ImageBlock::setExtractor(void* ctx, ExtractFn fn) {
  extractCtx = ctx;
  extractFn = fn;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Pages are deserialized afresh on each visit. Keep a bounded, allocation-free
// record so an image that failed renders its placeholder directly for the rest
// of the reader session instead of paying another placeholder refresh and
// decode. The reader clears this on entry so transient memory/storage failures
// are retried.
constexpr size_t MAX_SESSION_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_SESSION_IMAGE_FAILURES];
// v190：render-remembered 累計；閱讀器取差值綁定 spine/page，不在這裡清表。
uint32_t rememberedPlaceholderCount_ = 0;
// v193：延後解碼旗標與計數。旗標由閱讀器整輪 renderContents 以 RAII 固定，
// 計數只加、身分由呼叫端綁（與 rememberedPlaceholderCount_ 同一手法）。
bool deferHeavyDecode_ = false;
uint32_t deferredDecodeCount_ = 0;
// v176：0 = 永久失敗（整個 session 不重試）；非 0 = 暫時性（記憶體）失敗當時的最大連續塊，
// 只在堆積明顯好轉（+8KB）時才重試 —— 否則每次重繪都重試會變成效能懸崖（codex 複查）。
uint32_t failedImageMaxAlloc[MAX_SESSION_IMAGE_FAILURES];
size_t failedImageCount = 0;
// v191：1 專屬於「下一頁再試」哨兵，避免開檔失敗誤走 +8KB 記憶體規則。
// ⚠️ 解碼失敗記的是 std::max(2u, maxAlloc)（下面），刻意讓 1 不可能由記憶體路徑產生——
// 原本的 max(1u, …) 在 maxAlloc==0 時會寫出 1，被 clearRetryableFailures 當哨兵清掉（複查抓到）。
constexpr uint32_t IMAGE_FAILURE_RETRY_NEXT_RENDER = 1;
// v191：失敗表 16 格滿了就記不進去，而同一頁的 BW／灰階帶會重入 render 十幾次 —— 沒有備援就是
// 一次翻頁抽十幾次（正是這一版要避免的效能懸崖）。這一格只記「本次 render 已經失敗過的那張」。
uint64_t currentRenderFailHash = 0;
bool currentRenderFailValid = false;
// v191：n= 要跨頁累計才看得出暫時性 SD 失誤有沒有自己好，所以不能跟哨兵項一起清。
// 16×(8+1) BSS、固定大小；不用 heap，因為這是診斷計數、上限已知。
uint64_t openFailHashes[MAX_SESSION_IMAGE_FAILURES];
uint8_t openFailN[MAX_SESSION_IMAGE_FAILURES];
size_t openFailEntries = 0;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

// 回傳 0 = 記不下了（診斷欄位印 n=0 表示未知，不謊報成 1；複查抓到）。
unsigned bumpOpenFailN(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < openFailEntries; i++) {
    if (openFailHashes[i] != hash) continue;
    if (openFailN[i] < 255) openFailN[i]++;
    return openFailN[i];
  }
  if (openFailEntries < MAX_SESSION_IMAGE_FAILURES) {
    openFailHashes[openFailEntries] = hash;
    openFailN[openFailEntries] = 1;
    openFailEntries++;
    return 1;
  }
  return 0;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  if (currentRenderFailValid && currentRenderFailHash == hash) return true;  // v191：滿表備援
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] != hash) continue;
    // v191：同一頁 BW／灰階帶會重入 render，哨兵必須仍擋下來，否則一次翻頁抽十幾次。
    if (failedImageMaxAlloc[i] == IMAGE_FAILURE_RETRY_NEXT_RENDER) return true;
    if (failedImageMaxAlloc[i] == 0) return true;                                      // 永久
    if (ESP.getMaxAllocHeap() < failedImageMaxAlloc[i] + 8 * 1024) return true;         // 還沒好轉
    // 好轉了：移除紀錄讓它重試；再失敗會以新水位重新記錄。
    failedImageHashes[i] = failedImageHashes[failedImageCount - 1];
    failedImageMaxAlloc[i] = failedImageMaxAlloc[failedImageCount - 1];
    failedImageCount--;
    return false;
  }
  return false;
}

// transientMaxAlloc = 0 → 永久失敗；否則記錄當時的最大連續塊，作為重試門檻。
void rememberImageFailure(const std::string& path, const uint32_t transientMaxAlloc = 0) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES) return;
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return;
  }
  failedImageHashes[failedImageCount] = hash;
  failedImageMaxAlloc[failedImageCount] = transientMaxAlloc;
  failedImageCount++;
}

// --- Per-page-render RAM slot for the pixel cache ----------------------------
// The tiled grayscale flow re-renders an image page once for the BW
// double-refresh and again for every band of both gray planes, and each pass
// re-read the whole .pxc off SD (~100 ms for a full-page image, ~13 passes).
// Column clipping cannot reduce the SD traffic: the row stride (~100 B) is
// smaller than an SD sector, so every sector is touched regardless of the band
// window. Instead the first pass loads the payload into RAM and later passes
// render from it. Chunked allocation because a single full-image block (up to
// 96 KB) rarely fits the fragmented mid-render heap; each chunk is heap-gated
// and any failure falls back to the streaming path unchanged. The reader
// releases the slot when the page render completes, so nothing stays resident
// across page turns.
constexpr size_t PXC_CHUNK_SHIFT = 14;  // 16 KB chunks
constexpr size_t PXC_CHUNK_SIZE = 1u << PXC_CHUNK_SHIFT;
constexpr size_t PXC_MAX_CHUNKS = 6;  // 96 KB: a full-screen 2bpp image
constexpr size_t PXC_HEAP_RESERVE = 24 * 1024;
constexpr size_t PXC_MAX_ALLOC_RESERVE = 8 * 1024;
// Rows can straddle a chunk boundary; they are reassembled into a stack
// buffer. (screenWidth + 3) / 4 caps at 200 B for an 800px panel.
constexpr int PXC_MAX_BYTES_PER_ROW = 208;

std::unique_ptr<uint8_t[]> pxcChunks[PXC_MAX_CHUNKS];
uint64_t pxcSlotHash = 0;
uint16_t pxcSlotWidth = 0;
uint16_t pxcSlotHeight = 0;

void releasePxcSlot() {
  for (auto& chunk : pxcChunks) chunk.reset();
  pxcSlotHash = 0;
  pxcSlotWidth = 0;
  pxcSlotHeight = 0;
}

const uint8_t* pxcRowPtr(size_t rowStart, int bytesPerRow, uint8_t* tempRow) {
  const size_t chunk = rowStart >> PXC_CHUNK_SHIFT;
  const size_t offset = rowStart & (PXC_CHUNK_SIZE - 1);
  if (offset + bytesPerRow <= PXC_CHUNK_SIZE) {
    return pxcChunks[chunk].get() + offset;
  }
  const size_t firstPart = PXC_CHUNK_SIZE - offset;
  memcpy(tempRow, pxcChunks[chunk].get() + offset, firstPart);
  memcpy(tempRow + firstPart, pxcChunks[chunk + 1].get(), bytesPerRow - firstPart);
  return tempRow;
}

// cacheFile is positioned just past the header. True when the slot holds the
// full pixel payload for this cache path afterward.
bool loadPxcSlot(uint64_t cacheHash, HalFile& cacheFile, uint16_t cachedWidth, uint16_t cachedHeight, int bytesPerRow) {
  releasePxcSlot();
  if (bytesPerRow > PXC_MAX_BYTES_PER_ROW) {
    return false;
  }
  size_t remaining = (size_t)bytesPerRow * cachedHeight;
  const size_t chunkCount = (remaining + PXC_CHUNK_SIZE - 1) >> PXC_CHUNK_SHIFT;
  if (chunkCount == 0 || chunkCount > PXC_MAX_CHUNKS) {
    return false;
  }
  for (size_t i = 0; i < chunkCount; i++) {
    const size_t want = remaining < PXC_CHUNK_SIZE ? remaining : PXC_CHUNK_SIZE;
    if (ESP.getFreeHeap() < remaining + PXC_HEAP_RESERVE || ESP.getMaxAllocHeap() < want + PXC_MAX_ALLOC_RESERVE) {
      releasePxcSlot();
      return false;
    }
    pxcChunks[i] = makeUniqueNoThrow<uint8_t[]>(want);
    if (!pxcChunks[i] || cacheFile.read(pxcChunks[i].get(), want) != static_cast<int>(want)) {
      releasePxcSlot();
      return false;
    }
    remaining -= want;
  }
  pxcSlotHash = cacheHash;
  pxcSlotWidth = cachedWidth;
  pxcSlotHeight = cachedHeight;
  return true;
}

void renderRowsFromPxcSlot(GfxRenderer& renderer, int x, int y) {
  const int bytesPerRow = (pxcSlotWidth + 3) / 4;
  uint8_t tempRow[PXC_MAX_BYTES_PER_ROW];

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < pxcSlotHeight; row++) {
    const uint8_t* rowBuffer = pxcRowPtr((size_t)row * bytesPerRow, bytesPerRow, tempRow);
    pw.beginRow(y + row);
    int colStart, colEnd;
    pw.bandColRange(x, pxcSlotWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      const uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
      pw.writePixel(x + col, pixelValue);
    }
  }
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  // A later pass of the same page render: the payload is already in RAM, skip
  // the file entirely.
  const uint64_t cacheHash = imagePathHash(cachePath);
  if (pxcSlotHash == cacheHash && pxcSlotWidth != 0) {
    renderRowsFromPxcSlot(renderer, x, y);
    return true;
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte

  // First pass of a page render: try to pull the payload into the RAM slot so
  // the remaining ~12 passes skip SD entirely. Only an EMPTY slot is claimed:
  // the slot lives until the page render completes, so a populated slot with a
  // different hash means another image on this same page owns it. Evicting it
  // here would make 2+ image pages reload each other from SD on every pass
  // (all the SD traffic of streaming plus the slot alloc churn); instead later
  // images take the streaming path below, unchanged from pre-cache behavior.
  if (pxcSlotHash == 0 && loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, bytesPerRow)) {
    renderRowsFromPxcSlot(renderer, x, y);
    LOG_DBG("IMG", "Cache render complete (payload now in RAM)");
    return true;
  }

  // Streaming fallback (slot didn't fit). A failed slot load may have consumed
  // part of the payload; rewind to just past the header.
  cacheFile.seek(4);

  // Read several rows per SD access. A one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat; batching
  // rows into a ~4KB buffer cuts that to ~20 reads per pass without holding the
  // whole image.
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    // v194：LOG_ERR 在這台等於丟掉。走既有 noteFailure，閱讀器每圈倒進 diag.log。
    ImageBlock::noteFailure("cache-rowbuf bytes=%u max=%u", static_cast<unsigned>(bytesPerRow),
                            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

}  // namespace

bool ImageBlock::hasValidCache() const {
  const auto cachePath = getCachePath(imagePath);
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode() const { return !imageFailedThisSession(imagePath) && !hasValidCache(); }

void ImageBlock::clearSessionRenderFailures() {
  failedImageCount = 0;
  rememberedPlaceholderCount_ = 0;  // v190：與失敗表同一 session 起點，否則新 activity 的差值會吃到上一本
  deferredDecodeCount_ = 0;         // v193：同上，避免新 activity 的差值吃到上一本
  openFailEntries = 0;              // v191：n= 跟 session 走，進閱讀器時才歸零
  currentRenderFailValid = false;
}

void ImageBlock::clearRetryableFailures() {
  currentRenderFailValid = false;  // v191：備援格與哨兵同壽命——每頁 render 給一次機會
  size_t i = 0;
  while (i < failedImageCount) {
    if (failedImageMaxAlloc[i] != IMAGE_FAILURE_RETRY_NEXT_RENDER) {
      i++;
      continue;
    }
    failedImageHashes[i] = failedImageHashes[failedImageCount - 1];
    failedImageMaxAlloc[i] = failedImageMaxAlloc[failedImageCount - 1];
    failedImageCount--;
  }
}

uint32_t ImageBlock::rememberedPlaceholderCount() { return rememberedPlaceholderCount_; }

void ImageBlock::setDeferHeavyDecode(bool on) { deferHeavyDecode_ = on; }  // v193

uint32_t ImageBlock::deferredDecodeCount() { return deferredDecodeCount_; }  // v193

void ImageBlock::releaseRenderCache() { releasePxcSlot(); }

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    noteFailure("render-bounds x=%d y=%d w=%d h=%d %s", x, y, width, height, imagePath.c_str());
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  if (imageFailedThisSession(imagePath)) {
    noteFailure("render-remembered %s", imagePath.c_str());
    rememberedPlaceholderCount_++;  // v190：只加計數，身分由呼叫端綁定
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // v193：只延後「快取未命中、接下來要真的解碼」這條路。不是失敗，不准寫進失敗表。
  if (deferHeavyDecode_) {
    deferredDecodeCount_++;
    renderPlaceholder(renderer, x, y);
    return;
  }

  // v24/v148：紓解窗口涵蓋【懶抽取＋解碼】兩段（v147 實測 render 時的抽取在
  // maxAlloc=32,160 下差 608 bytes 失敗 —— 舊樹只包解碼，因為舊樹在建置期抽取）。
  // RAII 讓下面每一條 return 路徑都會 restore；restore = ensureLoaded，在同一個
  // render 內把字型載回來，圖之後的文字照常畫（字型 ID 是內容雜湊，重載後不變）。
  struct ReliefWindow {
    bool active = false;
    ~ReliefWindow() {
      if (active && g_imageRestoreFn) g_imageRestoreFn(g_imageReliefCtx);
    }
  } relief;
  if (g_imageReliefFn && (ESP.getMaxAllocHeap() < IMAGE_RENDER_RELIEF_MAX_ALLOC ||
                          ESP.getFreeHeap() < IMAGE_RENDER_RELIEF_MIN_FREE)) {
    g_imageReliefFn(g_imageReliefCtx);
    relief.active = true;
  }

  // The build only header-probed the image for dimensions; pull the actual
  // file out of the book now, on first visit to the page.
  if (!srcPath.empty() && extractFn && !Storage.exists(imagePath.c_str())) {
    LOG_DBG("IMG", "Lazy-extracting %s -> %s", srcPath.c_str(), imagePath.c_str());
    if (!extractFn(extractCtx, srcPath.c_str(), imagePath.c_str())) {
      LOG_ERR("IMG", "Lazy extraction failed: %s", srcPath.c_str());
      // v191：抽取器說失敗就是失敗，不要再去開那個檔 —— 抽到一半的殘檔【開得起來】，
      // 於是原本的寫法會完全沒有證據（而且 Storage.exists 之後永遠不再重抽）。複查抓到。
      // 印的是【來源路徑】（EPUB 內的項目名），那才是能拿去對書查的東西。
      Storage.remove(imagePath.c_str());
      const unsigned n = bumpOpenFailN(imagePath);
      noteFailure("render-extract n=%u %s", n, srcPath.c_str());
      rememberImageFailure(imagePath, IMAGE_FAILURE_RETRY_NEXT_RENDER);
      currentRenderFailHash = imagePathHash(imagePath);
      currentRenderFailValid = true;
      renderPlaceholder(renderer, x, y);
      return;
    }
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    // v191：ex= 放前面，長路徑截斷仍留得住原因；n= 看得出暫時性 SD 失誤有沒有自己好。
    const unsigned n = bumpOpenFailN(imagePath);
    noteFailure("render-open n=%u %s", n, imagePath.c_str());
    rememberImageFailure(imagePath, IMAGE_FAILURE_RETRY_NEXT_RENDER);
    currentRenderFailHash = imagePathHash(imagePath);
    currentRenderFailValid = true;
    renderPlaceholder(renderer, x, y);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    noteFailure("render-empty %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    noteFailure("render-nodecoder %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  ImageToFramebufferDecoder::clearLastError();
  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    // v176：render 階段的方框終於有證人（v125 那套在 v145 移植時掉了 —— diag175 那次實機的方框
    // 零紀錄）。記憶體類失敗（transient）不進 session 封殺名單，下一頁重試。
    noteFailure("render-decode %s tr=%u max=%u free=%u %s", ImageToFramebufferDecoder::lastError,
                ImageToFramebufferDecoder::lastErrorTransient ? 1u : 0u, static_cast<unsigned>(ESP.getMaxAllocHeap()),
                static_cast<unsigned>(ESP.getFreeHeap()), imagePath.c_str());
    rememberImageFailure(imagePath, ImageToFramebufferDecoder::lastErrorTransient
                                        ? std::max(2u, static_cast<unsigned>(ESP.getMaxAllocHeap()))
                                        : 0u);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writeString(file, srcPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  std::string src;
  serialization::readString(file, path);
  serialization::readString(file, src);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  auto block = std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, src, w, h));
  if (!block) {
    // v194：nothrow 配不到。LOG_ERR 在這台等於丟掉，走既有 noteFailure 進 diag.log。
    noteFailure("deserialize-alloc max=%u", static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return nullptr;
  }
  return block;
}
