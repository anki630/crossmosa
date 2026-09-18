#include "PngToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PNGdec.h>

#include <cstdlib>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "DecodeFile.h"
#include "PixelCache.h"

namespace {

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the DecodeFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};  // Track last rendered destination Y to avoid duplicates
  uint32_t lastYieldMs{0};  // yieldDuringDecode() 的節流狀態
  bool inputAborted{false};  // v260：回呼因按鍵中止解碼

  PixelCache cache;
  bool caching{false};

  uint8_t* grayLineBuffer{nullptr};
};

// File I/O callbacks use pFile->fHandle to access the DecodeFile* (HalFile ＋ v247 預讀緩衝),
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) { return openDecodeFile("PNG", filename, size); }

void pngCloseWithHandle(void* handle) { closeDecodeFile(handle); }

// v247：讀／跳走預讀層（語意與直接呼叫 HalFile 相同，桌機比對見 ReadAheadCore.h）。
int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* d = static_cast<DecodeFile*>(pFile->fHandle);
  if (!d || len <= 0) return 0;
  int32_t bytesRead;
  {
    DecodeStatTimer t(g_decodeStats.readUs);  // v246 儀器
    bytesRead = d->read(pBuf, static_cast<size_t>(len));
  }
  if (d->hadError()) g_decodeStats.ioError = 1;
  g_decodeStats.readCalls++;
  if (bytesRead > 0) g_decodeStats.readBytes += static_cast<uint32_t>(bytesRead);
  return bytesRead;
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  auto* d = static_cast<DecodeFile*>(pFile->fHandle);
  if (!d || pos < 0) return -1;
  const bool ok = d->seek(static_cast<size_t>(pos));
  if (d->hadError()) g_decodeStats.ioError = 1;
  return ok;
}

// The PNG decoder (PNGdec) is heap-allocated on demand rather than a static instance, so this memory
// is only consumed while actually decoding PNG images.
// v245：原註解寫「~42 KB」是過期的 —— 裝置上 sizeof(PNG) 實測 59,456 B（PNG_MAX_BUFFERED_PIXELS=16416），
//   而且是【一整塊】，比內部堆讀幾章後的連續塊天花板（約 53KB，CLAUDE.md 硬限制第 6 條）還大 →
//   p2 一被切開，每張 PNG 都 png-alloc-decoder 失敗直到重開機（diag244-2；同一張圖 v191 就失敗過）。
//   scripts/patch_pngdec.py 把 zlib 視窗（PNG_ZLIB_BUF_SIZE，約 40KB）與列緩衝（16,416）搬出物件，
//   這裡分三塊配。【總量】不變，所以總量門檻沿用舊值 60KB（行為不變；它本來就只比真實總量多約 0.5KB）。
constexpr size_t MIN_FREE_HEAP_FOR_PNG = 60 * 1024;

// PNGdec keeps TWO scanlines in its internal ucPixels buffer (current + previous)
// and each scanline includes a leading filter byte.
// Required storage is therefore approximately: 2 * (pitch + 1) + alignment slack.
// If PNG_MAX_BUFFERED_PIXELS is smaller than this requirement for a given image,
// PNGdec can overrun its internal buffer before our draw callback executes.
int bytesPerPixelFromType(int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return 1;
  }
}

int packedRowBytes(int srcWidth, int bitsPerSample) { return (srcWidth * bitsPerSample + 7) / 8; }

int requiredPngInternalBufferBytes(int srcWidth, int pixelType, int bitsPerSample) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  if ((pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED) && bitsPerSample < 8) {
    pitch = packedRowBytes(srcWidth, bitsPerSample);
  }
  return ((pitch + 1) * 2) + 32;
}

bool isSupportedBitDepth(int pixelType, int bitsPerSample) {
  if (bitsPerSample == 8) return true;
  if (bitsPerSample != 1 && bitsPerSample != 2 && bitsPerSample != 4) return false;
  return pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED;
}

uint8_t readPackedSample(const uint8_t* pixels, int x, int bitsPerSample) {
  if (bitsPerSample == 8) return pixels[x];

  const int bitOffset = x * bitsPerSample;
  const int shift = 8 - bitsPerSample - (bitOffset & 7);
  const uint8_t mask = (1U << bitsPerSample) - 1;
  return (pixels[bitOffset >> 3] >> shift) & mask;
}

uint8_t expandSampleToByte(uint8_t sample, int bitsPerSample) {
  if (bitsPerSample == 8) return sample;
  const uint8_t maxSample = (1U << bitsPerSample) - 1;
  return static_cast<uint8_t>((sample * 255U) / maxSample);
}

// Convert entire source line to grayscale with alpha blending to white background.
// Low-bit-depth grayscale/indexed scanlines are packed most-significant sample first.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(const uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, int bitsPerSample,
                       uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      if (bitsPerSample == 8) {
        memcpy(grayLine, pPixels, width);
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 3];
        grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
            uint8_t* p = &palette[idx * 3];
            uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            uint8_t alpha = palette[768 + idx];
            grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
            uint8_t* p = &palette[idx * 3];
            grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 4];
        uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    default:
      memset(grayLine, 128, width);
      break;
  }
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);
  // v260：同 JPEG —— 按鍵中止（只有閱讀器補圖那一遍會 arm）。
  if (ImageToFramebufferDecoder::inputAbortRequested()) {
    ctx->inputAborted = true;
    return 0;
  }

  int srcY = pDraw->y;
  int srcWidth = ctx->srcWidth;

  // Map source rows with the exact output-height ratio. During downscaling,
  // multiple source rows can select the same output row; during upscaling, one
  // source row must be repeated across every output row in its range. Emitting
  // only the first row of an upscale leaves zero-filled (black) gaps in the
  // streamed pixel cache.
  int firstDstY = (srcY * ctx->dstHeight) / ctx->srcHeight;
  int endDstY = firstDstY + 1;
  if (ctx->dstHeight > ctx->srcHeight) {
    endDstY = ((srcY + 1) * ctx->dstHeight) / ctx->srcHeight;
  }

  if (firstDstY <= ctx->lastDstY) firstDstY = ctx->lastDstY + 1;
  if (firstDstY >= endDstY || firstDstY >= ctx->dstHeight) return 1;
  if (endDstY > ctx->dstHeight) endDstY = ctx->dstHeight;

  // Convert entire source line to grayscale (improves cache locality)
  convertLineToGray(pDraw->pPixels, ctx->grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->iBpp, pDraw->pPalette,
                    pDraw->iHasAlpha);

  // Render scaled rows using Bresenham-style integer stepping (no floating-point division)
  int dstWidth = ctx->dstWidth;
  int outXBase = ctx->config->x;
  int screenWidth = ctx->screenWidth;
  bool useDithering = ctx->config->useDithering;

  // Pre-compute orientation and render-mode state once per callback.
  DirectPixelWriter pw;
  pw.init(*ctx->renderer);

  for (int dstY = firstDstY; dstY < endDstY; dstY++) {
    ctx->lastDstY = dstY;
    int outY = ctx->config->y + dstY;
    if (outY >= ctx->screenHeight) continue;

    pw.beginRow(outY);

    // The cache streams to disk one row at a time. Flushing rows below this one
    // (PNGdec delivers scanlines top to bottom) repositions the single-row band.
    // A flush failure stops caching for the rest of the decode so we never write
    // past the band buffer; finalize() then drops the partial file.
    bool caching = ctx->caching;
    DirectCacheWriter cw;
    if (caching) {
      if (!ctx->cache.advanceTo(dstY)) {
        caching = false;
        ctx->caching = false;
      } else {
        cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
        cw.beginRow(outY, ctx->config->y + ctx->cache.bandStart);
      }
    }

    int srcX = 0;
    int error = 0;

    for (int dstX = 0; dstX < dstWidth; dstX++) {
      int outX = outXBase + dstX;
      if (outX < screenWidth) {
        uint8_t gray = ctx->grayLineBuffer[srcX];

        uint8_t ditheredGray;
        if (useDithering) {
          ditheredGray = applyBayerDither4Level(gray, outX, outY);
        } else {
          ditheredGray = gray / 85;
          if (ditheredGray > 3) ditheredGray = 3;
        }
        pw.writePixel(outX, ditheredGray);
        if (caching) cw.writePixel(outX, ditheredGray);
      }

      // Bresenham-style stepping: advance srcX based on ratio srcWidth/dstWidth
      error += srcWidth;
      while (error >= dstWidth) {
        error -= dstWidth;
        srcX++;
      }
    }
  }

  return 1;
}

}  // namespace

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  // Reading a PNG's dimensions only requires the IHDR chunk, which is the first
  // chunk right after the 8-byte signature. Parse those 24 bytes directly instead
  // of allocating the ~44 KB PNGdec object. This deliberately removes the
  // 60 KB-free-heap gate from the layout/build path: getDimensions runs while the
  // parser lays out a chapter, and when it failed under memory pressure the image
  // was silently dropped and that image-less layout was persisted to the section
  // cache — losing the image permanently. Header parsing never needs that heap, so
  // an image is never dropped at build time. The pixel decode still uses PNGdec and
  // keeps its own MIN_FREE_HEAP_FOR_PNG check.
  HalFile file;
  if (!Storage.openFileForRead("PNG", imagePath, file)) {
    LOG_ERR("PNG", "Failed to open PNG for dimensions: %s", imagePath.c_str());
    return false;
  }

  // 8-byte signature + 4-byte IHDR length + "IHDR" + 4-byte width + 4-byte height.
  uint8_t header[24];
  if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    LOG_ERR("PNG", "PNG too small to read IHDR: %s", imagePath.c_str());
    return false;
  }

  static constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(header, kPngSignature, sizeof(kPngSignature)) != 0 || memcmp(header + 12, "IHDR", 4) != 0) {
    LOG_ERR("PNG", "Not a valid PNG (bad signature/IHDR): %s", imagePath.c_str());
    return false;
  }

  // Width/height are big-endian uint32 at offsets 16 and 20. Assemble byte-wise to
  // avoid an unaligned multi-byte load on the RISC-V target.
  const uint32_t width = (static_cast<uint32_t>(header[16]) << 24) | (static_cast<uint32_t>(header[17]) << 16) |
                         (static_cast<uint32_t>(header[18]) << 8) | static_cast<uint32_t>(header[19]);
  const uint32_t height = (static_cast<uint32_t>(header[20]) << 24) | (static_cast<uint32_t>(header[21]) << 16) |
                          (static_cast<uint32_t>(header[22]) << 8) | static_cast<uint32_t>(header[23]);

  if (!validateAndStoreDimensions(width, height, out, "PNG", /*applyPixelCap=*/false)) {
    LOG_ERR("PNG", "Invalid PNG dimensions %ux%u: %s", width, height, imagePath.c_str());
    return false;
  }
  return true;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("PNG", "Decoding PNG: %s", imagePath.c_str());

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_PNG) {
    LOG_ERR("PNG", "Not enough heap for PNG decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_PNG);
    setLastError(true, "png-heap %u<%u", static_cast<unsigned>(freeHeap), static_cast<unsigned>(MIN_FREE_HEAP_FOR_PNG));
    return false;
  }

  // v245：解碼器分三塊配，大的先配（先配大塊才不會被小塊切掉最大的洞）。宣告順序＝解構逆序：
  // cleanup（png->close）→ png → pixelBuf → zlibBuf，close 不碰這兩塊緩衝。
  // v251：列緩衝改到 open() 之後、依圖寬配剛好的大小（見下面 requiredInternal）。原本固定 16,416B 先配 ——
  //   268px 寬的章首圖只要約 2KB，而那 16KB 在背景排版的碎片化堆上要另找一塊 ≥17,408 的洞，常常就是配不到的那一塊。
  auto zlibBuf = makeUniqueNoThrow<uint8_t[]>(PNG_ZLIB_BUF_SIZE);
  if (!zlibBuf) {
    LOG_ERR("PNG", "Failed to allocate PNG zlib buffer (%u bytes)", static_cast<unsigned>(PNG_ZLIB_BUF_SIZE));
    setLastError(true, "png-alloc-zlib %u", static_cast<unsigned>(PNG_ZLIB_BUF_SIZE));
    lastErrorNeedBytes = static_cast<uint32_t>(PNG_ZLIB_BUF_SIZE);  // v255
    return false;
  }
  std::unique_ptr<uint8_t[]> pixelBuf;
  std::unique_ptr<PNG> png(new (std::nothrow) PNG());
  if (!png) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder");
    setLastError(true, "png-alloc-decoder %u", static_cast<unsigned>(sizeof(PNG)));
    lastErrorNeedBytes = static_cast<uint32_t>(sizeof(PNG));  // v255
    return false;
  }

  PngContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  g_decodeStats.fmt = 'P';
  const uint32_t setupStartUs = static_cast<uint32_t>(micros());  // v246 儀器：open → cache.begin
  int rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                     pngDrawCallback);
  const ScopedCleanup cleanup{[&png]() { png->close(); }};
  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Failed to open PNG: %d", rc);
    setLastError(false, "png-open rc=%d", rc);
    return false;
  }
  ImageDimensions sourceDimensions;
  if (!validateAndStoreDimensions(png->getWidth(), png->getHeight(), sourceDimensions, "PNG")) return false;

  // Calculate output dimensions
  ctx.srcWidth = sourceDimensions.width;
  ctx.srcHeight = sourceDimensions.height;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }
  ctx.lastDstY = -1;  // Reset row tracking

  const int pixelType = png->getPixelType();
  const int bitsPerSample = png->getBpp();
  LOG_DBG("PNG", "PNG %dx%d -> %dx%d (scale %.2f), type: %d, bpp: %d", ctx.srcWidth, ctx.srcHeight, ctx.dstWidth,
          ctx.dstHeight, ctx.scale, pixelType, bitsPerSample);

  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType, bitsPerSample);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR(
        "PNG",
        "PNG row buffer too small: need %d bytes for width=%d type=%d bpp=%d, configured PNG_MAX_BUFFERED_PIXELS=%d",
        requiredInternal, ctx.srcWidth, pixelType, bitsPerSample, PNG_MAX_BUFFERED_PIXELS);
    LOG_ERR("PNG", "Aborting decode to avoid PNGdec internal buffer overflow");
    setLastError(false, "png-rowbuf");
    return false;
  }

  if (!isSupportedBitDepth(pixelType, bitsPerSample)) {
    warnUnsupportedFeature(
        "bit depth (" + std::to_string(bitsPerSample) + "bpp) for pixel type " + std::to_string(pixelType), imagePath);
    return false;
  }

  // v251：列緩衝＝剛好 requiredInternal（現在列＋前一列＋各自 16B 對齊，PNGdec DecodePNG 用到 2×pitch＋32，這裡 2×pitch＋34）。
  //   放在 isSupportedBitDepth 之後：16-bit 樣本的 pitch 是 bytesPerPixelFromType 的兩倍，這個公式只對支援的深度成立。
  //   ⚠️ PNGdec 只有在 decode() 帶 PNG_FAST_PALETTE 時會用 ucPixels[PNG_MAX_BUFFERED_PIXELS-512] 當調色盤表 ——
  //   下面 decode(&ctx, 0) 沒帶；哪天要帶，這裡必須改回配滿 PNG_MAX_BUFFERED_PIXELS。
  //   ⚠️ 必須清零：PNGdec 從不清「前一列」（第一列的 Up／Average／Paeth 濾波要讀到 0）—— 以前緩衝在 PNG 物件裡、open() 會 memset，
  //   搬出來之後靠 makeUniqueNoThrow 的 value-init（new T[n]()）撐住。桌機用未清零的 malloc 會解出不同的圖（tools/decode-io-check/png_exact）。
  //   尾端多留 16B（防禦用）：PNGdec 內建 zlib 的 ALLOWS_UNALIGNED 路徑一次搬 4 bytes、最多寫出目標尾端 3 bytes，
  //   而 requiredInternal 在最壞對齊下只剩 2 bytes。⚠️ 裝置建置【沒有】定義它（用 riscv32 編譯器預處理 inffast.c／inflate.c 確認：
  //   條件是 64 位元或 HAL_ESP32_HAL_H_，而 zlib 的 include 鏈不含 Arduino 標頭）→ 裝置逐 byte 複製、不溢出；
  //   64 位元桌機測試會開，比裝置嚴苛。那條路徑在「兩列之間的對齊間隔 < 3」時理論上也會蓋到另一列（PNGdec 原本的配置，與緩衝大小無關）。
  //   桌機（tools/decode-io-check/png_exact.sh）：裝置版 zlib＋ASan 1,312 次與滿大小逐位元組相同、越界 0；
  //   一般編譯把兩塊緩衝前後填對抗內容、起點錯開，8,896 次與 PIL 逐位元組相同（滿大小對照組同樣全對）。
  constexpr int kInflateOvershootGuard = 16;
  const size_t pixelBufBytes = static_cast<size_t>(requiredInternal + kInflateOvershootGuard);
  pixelBuf = makeUniqueNoThrow<uint8_t[]>(pixelBufBytes);
  if (!pixelBuf) {
    LOG_ERR("PNG", "Failed to allocate PNG row buffer (%u bytes)", static_cast<unsigned>(pixelBufBytes));
    setLastError(true, "png-alloc-rows %u", static_cast<unsigned>(pixelBufBytes));
    lastErrorNeedBytes = static_cast<uint32_t>(pixelBufBytes);  // v255
    return false;
  }
  // open() 會把整個內部結構 memset 歸零 —— 緩衝指標必須在它之後、decode() 之前設。
  png->setBuffers(zlibBuf.get(), pixelBuf.get());

  // The converter expands each source row to 8-bit grayscale before dithering,
  // so this scratch buffer is sized by source pixels even when PNGdec reads a
  // packed 1/2/4-bit row internally.
  constexpr size_t MAX_GRAY_LINE_BUFFER_BYTES = PNG_MAX_BUFFERED_PIXELS / 2;
  const size_t grayBufSize = static_cast<size_t>(ctx.srcWidth);
  if (grayBufSize > MAX_GRAY_LINE_BUFFER_BYTES) {
    LOG_ERR("PNG", "Expanded gray row too wide: need %u bytes for width=%d, max=%u", static_cast<unsigned>(grayBufSize),
            ctx.srcWidth, static_cast<unsigned>(MAX_GRAY_LINE_BUFFER_BYTES));
    setLastError(false, "png-graywide");
    return false;
  }

  auto grayLineBuffer = makeUniqueNoThrow<uint8_t[]>(grayBufSize);
  if (!grayLineBuffer) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer");
    setLastError(true, "png-alloc-gray %u", static_cast<unsigned>(grayBufSize));
    lastErrorNeedBytes = static_cast<uint32_t>(grayBufSize);  // v255
    return false;
  }
  ctx.grayLineBuffer = grayLineBuffer.get();

  // Stream the pixel cache to disk. PNGdec delivers source scanlines top to
  // bottom and we emit at most one (downscaled) output row per callback, so the
  // band only needs a single row. Streaming keeps the working set tiny, so
  // unlike the old full-image buffer it neither competes with the ~44KB decoder
  // nor forces larger images to skip caching - which previously meant a full
  // re-decode on every one of an image page's ~14 render passes.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      // v248：同 JPEG —— 串流時配不到快取帶就退回抽到 SD 的舊路，不准沒快取硬解。
      if (g_decodeStats.streamed) {
        setLastError(true, "stream-nocache");
        return false;
      }
      LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  g_decodeStats.setupUs += static_cast<uint32_t>(micros()) - setupStartUs;
  g_decodeStats.srcW = static_cast<uint16_t>(ctx.srcWidth);
  g_decodeStats.srcH = static_cast<uint16_t>(ctx.srcHeight);
  g_decodeStats.dstW = static_cast<uint16_t>(ctx.dstWidth);
  g_decodeStats.dstH = static_cast<uint16_t>(ctx.dstHeight);
  g_decodeStats.scaleDenom = 1;

  unsigned long decodeStart = millis();
  ctx.lastYieldMs = decodeStart;
  {
    DecodeStatTimer t(g_decodeStats.decodeUs);  // v246 儀器
    rc = png->decode(&ctx, 0);
  }
  unsigned long decodeTime = millis() - decodeStart;

  ctx.grayLineBuffer = nullptr;

  // v260：按鍵中止 —— 不是失敗（同 JPEG）。
  if (ctx.inputAborted) {
    setLastError(true, "aborted-input");
    lastDecodeAborted = true;
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Decode failed: %d", rc);
    setLastError(false, "png-decode rc=%d", rc);
    if (ctx.caching) ctx.cache.abort();
    return false;
  }
  // v247：同 JPEG —— 讀檔出過錯就不寫永久快取，當暫時失敗下次重試。
  if (g_decodeStats.ioError) {
    LOG_ERR("PNG", "I/O error during decode, dropping cache: %s", imagePath.c_str());
    setLastError(true, "png-io");
    if (ctx.caching) ctx.cache.abort();
    return false;
  }
  // v248：同 JPEG —— 串流來源寫出快取前驗完整（IEND 之後的零頭、宣告大小、解壓錯誤）。
  if (!verifyActiveDecodeSourceComplete()) {
    LOG_ERR("PNG", "Streamed source incomplete/corrupt, dropping cache: %s", imagePath.c_str());
    setLastError(true, "stream-verify");
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  LOG_DBG("PNG", "PNG decoding complete - render time: %lu ms", decodeTime);

  // Finalize the streamed cache (caching may have been cleared on a flush error).
  if (ctx.caching) {
    DecodeStatTimer t(g_decodeStats.finalizeUs);  // v246 儀器
    ctx.cache.finalize();
  }

  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
