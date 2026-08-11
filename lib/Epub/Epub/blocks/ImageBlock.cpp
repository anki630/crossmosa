#include "ImageBlock.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap / getFreeHeap
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstdlib>

#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/ImageDecoderFactory.h"

char ImageBlock::lastFailPath[96] = {0};
char ImageBlock::lastDrawGeom[64] = {0};

void ImageBlock::noteRenderFailure(const int stage, const std::string& path, const int w, const int h, const int x,
                                   const int y) {
  if (ImageBlock::lastFailPath[0] != '\0') return;  // 先到先得,見標頭說明
  snprintf(ImageBlock::lastFailPath, sizeof(ImageBlock::lastFailPath), "%dx%d@%d,%d %s", w, h, x, y, path.c_str());
  ImageToFramebufferDecoder::noteFailure(stage, 0);
}

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

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
size_t failedImageCount = 0;

// Low-memory relief hooks (installed by the reader). When free heap is below this
// before a first-time decode, reliefFn is invoked to free RAM (e.g. unload the SD
// reading font) so the decoder can allocate, then restoreFn reloads it.
//
// v54:總 free 那一側的額外餘裕。解碼器自身也有一道總量門檻(PNG 60KB / JPEG 36KB),
// 若只看連續塊,總量落在中間帶時 relief 不觸發、卻被解碼器自己擋下 → 圖片變佔位框且
// 整個 session 不再重試。用「該解碼器的連續需求 + 此餘裕」把窗口補起來,兩種解碼器
// 各自得到貼合的值(PNG 64KB / JPEG 40KB),而不是共用一個對 JPEG 過度保守的固定值。
constexpr size_t IMAGE_DECODE_TOTAL_HEAP_MARGIN = 12 * 1024;
ImageBlock::MemoryReliefFn g_imageReliefFn = nullptr;
ImageBlock::MemoryReliefFn g_imageRestoreFn = nullptr;
void* g_imageReliefCtx = nullptr;

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return true;
  }
  return false;
}

void rememberImageFailure(const std::string& path) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES || imageFailedThisSession(path)) return;
  failedImageHashes[failedImageCount++] = imagePathHash(path);
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
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

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
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

void ImageBlock::clearSessionRenderFailures() { failedImageCount = 0; }

void ImageBlock::setMemoryReliefHooks(MemoryReliefFn reliefFn, MemoryReliefFn restoreFn, void* ctx) {
  g_imageReliefFn = reliefFn;
  g_imageRestoreFn = restoreFn;
  g_imageReliefCtx = ctx;
}

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
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    // v122:這條路徑【什麼都不畫、也不記失敗】,所以每次進來重演一次而診斷上完全看不見。
    // diag121 的 SEG 顯示那兩個圖片頁 dec=0(連解碼都沒開始),這裡是頭號嫌疑。
    // 把座標寫進去讓實機指名 —— 數字排在最前面,路徑被截斷也不影響歸因。
    if (ImageBlock::lastFailPath[0] == '\0') {
      snprintf(ImageBlock::lastFailPath, sizeof(ImageBlock::lastFailPath), "%dx%d@%d,%d scr=%dx%d %s", width, height, x,
               y, screenWidth, screenHeight, imagePath.c_str());
      ImageToFramebufferDecoder::noteFailure(ImageToFramebufferDecoder::FAIL_BOUNDS, 0);
    }
    return;
  }

  // v125:通過邊界檢查 = 這張圖【會】被畫下去。把版面實際給的幾何記下來,因為失敗碼答不出
  // 「畫出來了但小到看不見」。同一次 render 內會被每條灰階帶重寫,值相同,無害。
  snprintf(ImageBlock::lastDrawGeom, sizeof(ImageBlock::lastDrawGeom), "%dx%d@%d,%d", width, height, x, y);

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
    // v122:本 session 先前失敗過。只在還沒有待回報的紀錄時寫入,免得蓋掉真正的首次失敗原因。
    if (ImageBlock::lastFailPath[0] == '\0') {
      snprintf(ImageBlock::lastFailPath, sizeof(ImageBlock::lastFailPath), "%s", imagePath.c_str());
      ImageToFramebufferDecoder::noteFailure(ImageToFramebufferDecoder::FAIL_REMEMBERED, 0);
    }
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    noteRenderFailure(ImageToFramebufferDecoder::FAIL_NOT_FOUND, imagePath, width, height, x, y);
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    noteRenderFailure(ImageToFramebufferDecoder::FAIL_EMPTY, imagePath, width, height, x, y);
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
    noteRenderFailure(ImageToFramebufferDecoder::FAIL_NO_DECODER, imagePath, width, height, x, y);
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  // First-time decode allocates the ~44 KB PNG / ~20 KB JPEG decoder. Under heap
  // pressure that allocation fails and the image would fall back to a placeholder.
  // Give the reader a chance to free RAM (unload the SD reading font) for just this
  // decode, then restore it. Scoped tightly around the decode so no text drawing
  // happens while the font is unloaded; the reader's fontId comes from SETTINGS and
  // is unchanged by the reload. Once decoded the pixel cache is written, so later
  // views take the cheap renderFromCache path and never reach here again.
  // v54:雙判準,兩個都要看——
  // ①【最大連續塊】:解碼器要的是一整塊連續空間(PNG ~44KB / JPEG ~20KB),總量足夠但沒有
  //   夠大連續塊時,舊的「總 free」判斷會誤放行(專案鐵律,見 CLAUDE.md「硬限制 2」)。
  //   門檻由各解碼器自報,共用一個保守值會讓 JPEG 頁白拆字型快取。
  // ②【總 free】:解碼器自己內部還有一道總量門檻(PNG 60KB / JPEG 36KB),若只看連續塊,
  //   總量落在中間帶時 relief 不觸發、卻被解碼器自己的門檻擋下 → 圖片變佔位框且整個
  //   session 不再重試。保留一個總量下限把這個窗口補起來。
  const size_t needContiguous = decoder->minContiguousHeapForDecode();
  const bool relieve = g_imageReliefFn && (ESP.getMaxAllocHeap() < needContiguous ||
                                           ESP.getFreeHeap() < needContiguous + IMAGE_DECODE_TOTAL_HEAP_MARGIN);
  if (relieve) g_imageReliefFn(g_imageReliefCtx);
  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (relieve && g_imageRestoreFn) g_imageRestoreFn(g_imageReliefCtx);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    // v120:記下是哪一張、哪一個階段;由閱讀器那一層寫進 diag.log(見標頭說明)。
    // v125:加上幾何並改成先到先得。解碼器自己回報的階段碼優先,不覆寫。
    if (ImageBlock::lastFailPath[0] == '\0') {
      if (ImageToFramebufferDecoder::lastFailStage == ImageToFramebufferDecoder::FAIL_NONE) {
        ImageToFramebufferDecoder::noteFailure(ImageToFramebufferDecoder::FAIL_DECODE, 0);
      }
      snprintf(ImageBlock::lastFailPath, sizeof(ImageBlock::lastFailPath), "%dx%d@%d,%d %s", width, height, x, y,
               imagePath.c_str());
    }
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new ImageBlock(path, w, h));
}
