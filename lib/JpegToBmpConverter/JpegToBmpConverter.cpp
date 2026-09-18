#include "JpegToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

#include "BitmapHelpers.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
// v174：失敗原因鏡像（見 .h）。
static char sJpgLastErr[64] = "";
const char* JpegToBmpConverter::lastError() { return sJpgLastErr; }
// v258：最近一次轉檔的幾何與解碼時間（主畫面 THUMBGEN 證人用）。
static JpegToBmpConverter::Info sJpgLastInfo;
const JpegToBmpConverter::Info& JpegToBmpConverter::lastInfo() { return sJpgLastInfo; }

constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 32 * 1024;
// v259：縮圖路徑配完所有緩衝之後，至少還要留給其他 task 的總量。
constexpr size_t THUMB_RESERVE_BYTES = 16 * 1024;
constexpr uint32_t FP_ONE = 1UL << 16;

// Static source pointer for JPEGDEC open callback.
// Safe in single-threaded embedded context; never accessed concurrently.
// v258：來源改成抽象（HalFile 或書裡的項目串流），見 JpegToBmpConverter::Source。
static const JpegToBmpConverter::Source* s_jpegSource = nullptr;
static uint8_t s_jpegIoSinceYield = 0;

static void yieldToIdle() { vTaskDelay(1); }

static void yieldDuringJpegIo() {
  if (++s_jpegIoSinceYield < 4) return;
  s_jpegIoSinceYield = 0;
  yieldToIdle();
}

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegSource || !s_jpegSource->read || !s_jpegSource->seek) return nullptr;
  s_jpegIoSinceYield = 0;
  if (!s_jpegSource->seek(s_jpegSource->ctx, 0)) return nullptr;
  *size = s_jpegSource->size;
  yieldDuringJpegIo();
  // JPEGDEC 把回傳值當不透明把手存著，只拿去比 NULL 與傳回 read/seek。
  return const_cast<void*>(static_cast<const void*>(s_jpegSource));
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the source — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  const auto* src = static_cast<const JpegToBmpConverter::Source*>(pFile->fHandle);
  if (!src) return 0;
  int32_t n = src->read(src->ctx, pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  yieldDuringJpegIo();
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  const auto* src = static_cast<const JpegToBmpConverter::Source*>(pFile->fHandle);
  if (!src || pos < 0 || !src->seek(src->ctx, pos)) return -1;
  pFile->iPos = pos;
  yieldDuringJpegIo();
  return pos;
}

int32_t halFileSourceRead(void* ctx, uint8_t* buf, int32_t len) {
  return static_cast<HalFile*>(ctx)->read(buf, static_cast<size_t>(len));
}

bool halFileSourceSeek(void* ctx, int32_t pos) { return static_cast<HalFile*>(ctx)->seek(static_cast<size_t>(pos)); }

JpegToBmpConverter::Source halFileSource(HalFile& file) {
  JpegToBmpConverter::Source src;
  src.ctx = &file;
  src.read = &halFileSourceRead;
  src.seek = &halFileSourceSeek;
  // v258（codex）：Source::size 是 int32。超過的檔當成 0（JPEGDEC 開檔即失敗），不讓它變成負數。
  const size_t sz = file ? file.size() : 0;
  src.size = sz > 0x7FFFFFFFu ? 0 : static_cast<int32_t>(sz);
  return src;
}

// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  int bytesPerRow;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;
  bool smoothUpscale;
  uint32_t smoothScaleX_fp;
  uint32_t smoothScaleY_fp;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  std::unique_ptr<uint8_t[]> mcuBuf;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint32_t[]> rowCount;

  int smoothNextOutY;
  int smoothPrevY;
  std::unique_ptr<uint8_t[]> smoothRows;
  uint8_t* smoothPrevRow;
  uint8_t* smoothCurrRow;
  uint8_t* smoothOutRow;

  std::unique_ptr<uint8_t[]> bmpRow;

  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  uint8_t rowsSinceYield;
  uint8_t blocksSinceYield;
  bool error;
};

static void yieldDuringDecode(BmpConvertCtx* ctx) {
  if (++ctx->rowsSinceYield < 8) return;
  ctx->rowsSinceYield = 0;
  yieldToIdle();
}

static void yieldDuringDecodeBlock(BmpConvertCtx* ctx) {
  if (++ctx->blocksSinceYield < 16) return;
  ctx->blocksSinceYield = 0;
  yieldToIdle();
}

// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      ctx->bmpRow[x] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel(srcRow[x]);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow);
  yieldDuringDecode(ctx);
}

// Matches the progressive-JPEG smoothing used by JpegToFramebufferConverter, but stays
// local because cover generation streams dithered BMP rows instead of framebuffer pixels.
static uint32_t interpolationStep(const int srcSize, const int outSize) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  return (static_cast<uint32_t>(srcSize - 1) << 16) / static_cast<uint32_t>(outSize - 1);
}

static uint32_t interpolatedSourceFp(const int outIndex, const int outSize, const int srcSize, const uint32_t step) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  if (outIndex >= outSize - 1) return static_cast<uint32_t>(srcSize - 1) << 16;
  return static_cast<uint32_t>(outIndex) * step;
}

static void scaleRowLinear(BmpConvertCtx* ctx, const uint8_t* srcRow, uint8_t* dstRow) {
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const uint32_t srcX_fp = interpolatedSourceFp(outX, ctx->outWidth, ctx->srcWidth, ctx->smoothScaleX_fp);
    const int x0 = srcX_fp >> 16;
    const int x1 = (x0 + 1 < ctx->srcWidth) ? (x0 + 1) : x0;
    const uint32_t fx = srcX_fp & (FP_ONE - 1);
    dstRow[outX] = static_cast<uint8_t>((srcRow[x0] * (FP_ONE - fx) + srcRow[x1] * fx) >> 16);
  }
}

static void writeBlendedRow(BmpConvertCtx* ctx, const uint8_t* row0, const uint8_t* row1, const uint32_t fy,
                            const int outY) {
  const uint32_t invFy = FP_ONE - fy;
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    ctx->smoothOutRow[outX] = static_cast<uint8_t>((row0[outX] * invFy + row1[outX] * fy) >> 16);
  }
  writeOutputRow(ctx, ctx->smoothOutRow, outY);
}

static void processSmoothSourceRow(BmpConvertCtx* ctx, const uint8_t* srcRow, const int srcY) {
  scaleRowLinear(ctx, srcRow, ctx->smoothCurrRow);

  if (ctx->smoothPrevY < 0) {
    uint8_t* tmp = ctx->smoothPrevRow;
    ctx->smoothPrevRow = ctx->smoothCurrRow;
    ctx->smoothCurrRow = tmp;
    ctx->smoothPrevY = srcY;
    if (ctx->srcHeight <= 1) {
      while (ctx->smoothNextOutY < ctx->outHeight) {
        writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
        ctx->smoothNextOutY++;
      }
      return;
    }
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    const uint32_t srcY_fp =
        interpolatedSourceFp(ctx->smoothNextOutY, ctx->outHeight, ctx->srcHeight, ctx->smoothScaleY_fp);
    const int y0 = srcY_fp >> 16;
    const int y1 = (y0 + 1 < ctx->srcHeight) ? (y0 + 1) : y0;
    if (y1 > srcY) break;

    const uint8_t* row0 = (y0 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    const uint8_t* row1 = (y1 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    writeBlendedRow(ctx, row0, row1, srcY_fp & (FP_ONE - 1), ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }

  uint8_t* tmp = ctx->smoothPrevRow;
  ctx->smoothPrevRow = ctx->smoothCurrRow;
  ctx->smoothCurrRow = tmp;
  ctx->smoothPrevY = srcY;
}

static void finishSmoothUpscale(BmpConvertCtx* ctx) {
  if (ctx->smoothPrevY < 0) {
    LOG_ERR("JPG", "No progressive rows decoded for smoothing");
    ctx->error = true;
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpConvertCtx* ctx) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->bmpRow[x] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow);
  ctx->currentOutY++;
  yieldDuringDecode(ctx);
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;
  yieldDuringDecodeBlock(ctx);

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Guard against unexpected callback geometry so we never index past row buffers.
  if (blockX < 0 || blockY < 0 || blockX >= ctx->srcWidth || blockY >= ctx->srcHeight) {
    LOG_ERR("JPG", "Unexpected JPEG block origin (%d,%d) for decode grid %dx%d", blockX, blockY, ctx->srcWidth,
            ctx->srcHeight);
    ctx->error = true;
    return 0;
  }

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf.get() + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row
  const int endRow = blockY + blockH;

  for (int y = blockY; y < endRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf.get() + (y - blockY) * ctx->srcWidth;

    if (ctx->smoothUpscale) {
      processSmoothSourceRow(ctx, srcRow, y);
    } else if (!ctx->needsScaling) {
      // 1:1 — outWidth == srcWidth, write directly
      writeOutputRow(ctx, srcRow, y);
    } else {
      // Fixed-point area averaging on X axis
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;
        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += srcRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = srcRow[srcXStart];
          count = 1;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Flush output row(s) whose Y boundary we've crossed
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;
      while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        flushScaledRow(ctx);
        ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
        if (srcY_fp >= ctx->nextOutY_srcStart) continue;
        memset(ctx->rowAccum.get(), 0, ctx->outWidth * sizeof(uint32_t));
        memset(ctx->rowCount.get(), 0, ctx->outWidth * sizeof(uint32_t));
      }
    }
  }

  return ctx->error ? 0 : 1;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(const Source& source, Print& bmpOut, int targetWidth,
                                                     int targetHeight, bool oneBit, bool crop, bool allowDctScale) {
  sJpgLastErr[0] = '\0';
  sJpgLastInfo = Info{};
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);

  // v259：總量門檻。MIN_FREE_HEAP＝解碼器 20KB＋32KB，那 32KB 是給【全解析度】的 MCU 列緩衝（16 列 × 2048 寬）。
  //   允許 DCT 縮放的呼叫端（主畫面縮圖）改成兩段：這裡只要求「解碼器＋16KB 保留」；讀完檔頭、知道實際格子大小之後，
  //   再依【實際】要配的緩衝＋16KB 保留量一次（下面 THUMB_RESERVE_BYTES）。
  //   codex（v259）：原本想直接降到 36KB，但那等於在門檻邊緣配完緩衝就剩 0 —— 解碼中途 vTaskDelay 讓出時別的 task
  //   若做會 abort 的配置就出事。舊的全解析度路徑其實也會掉到 0，那不是「安全」的證據。
  //   diag258：一本書的縮圖因為串流前的總量檢查（109KB）沒過而退回舊路（6.7 秒）。
  const size_t minFreeHeap = allowDctScale ? (JPEG_DECODER_SIZE + THUMB_RESERVE_BYTES) : MIN_FREE_HEAP;
  if (ESP.getFreeHeap() < minFreeHeap) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(), minFreeHeap);
    sJpgLastInfo.memFail = true;
    snprintf(sJpgLastErr, sizeof(sJpgLastErr), "heap %u<%u", static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(minFreeHeap));
    return false;
  }

  s_jpegSource = &source;
  // 回傳前清掉：s_jpegSource 指向呼叫端堆疊上的物件，不能留著。
  const ScopedCleanup clearSource{[]() { s_jpegSource = nullptr; }};

  const auto jpeg = makeUniqueNoThrow<JPEGDEC>();
  if (!jpeg) {
    LOG_ERR("JPG", "OOM: JPEG decoder");
    sJpgLastInfo.memFail = true;
    snprintf(sJpgLastErr, sizeof(sJpgLastErr), "oom:%s", "JPEG decoder");
    return false;
  }

  int rc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
    snprintf(sJpgLastErr, sizeof(sJpgLastErr), "open err=%d", jpeg->getLastError());
    return false;
  }

  const ScopedCleanup cleanup{[&jpeg]() { jpeg->close(); }};

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();
  const bool progressiveDecode = (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE);
  // JPEGDEC forces progressive streams to JPEG_SCALE_EIGHTH in DecodeJPEG,
  // so callback coordinates and MCU buffering must use the reduced decode grid.
  // v258：scaleShift 之後可能被下面的 DCT 縮放改大（只有 allowDctScale 的呼叫端）。
  int scaleShift = progressiveDecode ? 3 : 0;
  int decodedSrcWidth = progressiveDecode ? ((srcWidth + 7) >> 3) : srcWidth;
  int decodedSrcHeight = progressiveDecode ? ((srcHeight + 7) >> 3) : srcHeight;

  LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);
  if (progressiveDecode) {
    LOG_DBG("JPG", "Progressive JPEG decode uses 1/8 source: %dx%d", decodedSrcWidth, decodedSrcHeight);
  }

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d), max supported: %dx%d", srcWidth, srcHeight, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    return false;
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = srcWidth;
  int outHeight = srcHeight;
  if (targetWidth <= 0 || targetHeight <= 0) {
    // Without an explicit target, keep decoder-native dimensions.
    outWidth = decodedSrcWidth;
    outHeight = decodedSrcHeight;
  }

  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (srcWidth != targetWidth || srcHeight != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / srcHeight;
    float scale = 1.0f;
    if (crop) {
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(srcWidth * scale);
    outHeight = static_cast<int>(srcHeight * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    LOG_DBG("JPG", "Scaling source %dx%d (decode grid %dx%d) -> %dx%d (target %dx%d)", srcWidth, srcHeight,
            decodedSrcWidth, decodedSrcHeight, outWidth, outHeight, targetWidth, targetHeight);
  }

  // v258（主畫面縮圖，diag257：1443×2048 封面 → 159×226 花 8 秒）：原本一律全解析度解碼（2048 列全部 IDCT、
  //   逐像素面積平均），再縮到 1/9。JPEGDEC 可以在 DCT 域直接出 1/2、1/4、1/8 的格子
  //   （1/8＝只取 DC＝每個 8×8 區塊的平均值，1/2＝2×2 平均），再從那個格子面積平均到輸出——
  //   挑【解出來的格子仍 ≥ 輸出的 2 倍】的最大縮放（永遠不從小格子放大）。
  //   為什麼是 2 倍（v258 桌機 48 張封面，1-bit 抖色後 4×4 區塊平均對 PIL BOX 參考圖的平均絕對差）：
  //   全解析度 15.0；格子 ≥1 倍（多半 1/8）16.2、最差 +4.7 —— 格子只比輸出大 1.1 倍時每個輸出像素只平均 1–2 格，
  //   權重不均；≥2 倍（多半 1/4）15.2、最差 +1.6，桌機時間 580→361ms。取後者。
  //   只給 allowDctScale 的呼叫端（主畫面 1-bit 縮圖）；待機封面等其他路徑不變。
  //   ⚠️ 必須在上面算出 outWidth／outHeight【之後】（v258 桌機 harness 抓到：第一版放在前面，比到的是原圖尺寸＝永遠不縮放）。
  //   格子大小與 JPEGDEC 自己算的一致：(w + 2^s − 1) >> s（jpeg.inl DecodeJPEG 的 iCurW／iCurH）。
  int jpegScaleOption = 0;
  if (allowDctScale && !progressiveDecode && targetWidth > 0 && targetHeight > 0) {
    for (int shift = 3; shift >= 1; --shift) {
      const int adj = (1 << shift) - 1;
      const int w = (srcWidth + adj) >> shift;
      const int h = (srcHeight + adj) >> shift;
      if (w >= 2 * outWidth && h >= 2 * outHeight) {
        scaleShift = shift;
        decodedSrcWidth = w;
        decodedSrcHeight = h;
        jpegScaleOption = shift == 3 ? JPEG_SCALE_EIGHTH : shift == 2 ? JPEG_SCALE_QUARTER : JPEG_SCALE_HALF;
        break;
      }
    }
  }

  const int scaleSrcWidth = decodedSrcWidth;
  const int scaleSrcHeight = decodedSrcHeight;
  sJpgLastInfo.srcW = static_cast<uint16_t>(srcWidth);
  sJpgLastInfo.srcH = static_cast<uint16_t>(srcHeight);
  sJpgLastInfo.decW = static_cast<uint16_t>(decodedSrcWidth);
  sJpgLastInfo.decH = static_cast<uint16_t>(decodedSrcHeight);
  sJpgLastInfo.outW = static_cast<uint16_t>(outWidth);
  sJpgLastInfo.outH = static_cast<uint16_t>(outHeight);
  sJpgLastInfo.scale = static_cast<uint8_t>(1 << scaleShift);
  sJpgLastInfo.progressive = progressiveDecode;

  if (scaleSrcWidth != outWidth || scaleSrcHeight != outHeight) {
    scaleX_fp = (static_cast<uint32_t>(scaleSrcWidth) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(scaleSrcHeight) << 16) / outHeight;
    needsScaling = true;
  }

  const bool smoothUpscale =
      progressiveDecode && needsScaling && scaleSrcWidth <= outWidth && scaleSrcHeight <= outHeight;

  // v259（codex）：縮圖路徑的第二段門檻 —— 用【實際】格子與輸出尺寸算出下面要配的緩衝，加上保留量，寫 BMP 檔頭之前檢查。
  //   不靠「縮放後格子一定小」的推論（奇怪長寬比、不縮放的小圖、之後別的呼叫端都可能不成立）。
  if (allowDctScale) {
    const size_t w = static_cast<size_t>(outWidth);
    const size_t need = static_cast<size_t>(MAX_MCU_HEIGHT) * static_cast<size_t>(scaleSrcWidth)  // mcuBuf
                        + (w + 3) / 4 * 4                                                         // bmpRow（取 8-bit 的最大者）
                        + (smoothUpscale ? w * 3 : (needsScaling ? w * 2 * sizeof(uint32_t) : 0))  // 縮放緩衝
                        + (w + 4) * sizeof(int16_t) * 3 + 64                                      // 抖色誤差列＋物件
                        + THUMB_RESERVE_BYTES;
    sJpgLastInfo.needBytes = static_cast<uint32_t>(need);
    if (ESP.getFreeHeap() < need) {
      sJpgLastInfo.memFail = true;
      snprintf(sJpgLastErr, sizeof(sJpgLastErr), "heap-post %u<%u", static_cast<unsigned>(ESP.getFreeHeap()),
               static_cast<unsigned>(need));
      return false;
    }
  }

  // v259：BMP 檔頭改在所有緩衝配完、縮圖路徑的保留量檢查過之後才寫（失敗時輸出檔是空的，不是半個檔頭）。
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = scaleSrcWidth;
  ctx.srcHeight = scaleSrcHeight;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = scaleX_fp;
  ctx.scaleY_fp = scaleY_fp;
  ctx.smoothUpscale = smoothUpscale;
  ctx.smoothScaleX_fp = interpolationStep(ctx.srcWidth, outWidth);
  ctx.smoothScaleY_fp = interpolationStep(ctx.srcHeight, outHeight);
  ctx.smoothNextOutY = 0;
  ctx.smoothPrevY = -1;
  ctx.rowsSinceYield = 0;
  ctx.blocksSinceYield = 0;
  ctx.error = false;

  // MCU row buffer: MAX_MCU_HEIGHT rows × decoded srcWidth columns of grayscale
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(MAX_MCU_HEIGHT * ctx.srcWidth);
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "OOM: MCU buffer (%d bytes)", MAX_MCU_HEIGHT * ctx.srcWidth);
    // v258（codex）：這個出口原本沒寫原因 —— 縮圖端靠 memFail 決定要不要退回 SD 路徑，漏標會被當成壞圖。
    sJpgLastInfo.memFail = true;
    snprintf(sJpgLastErr, sizeof(sJpgLastErr), "oom:%s", "MCU buffer");
    return false;
  }
  memset(ctx.mcuBuf.get(), 0, MAX_MCU_HEIGHT * ctx.srcWidth);

  ctx.bmpRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "OOM: BMP row buffer");
    sJpgLastInfo.memFail = true;
    snprintf(sJpgLastErr, sizeof(sJpgLastErr), "oom:%s", "BMP row buffer");
    return false;
  }

  if (smoothUpscale) {
    // One contiguous allocation avoids three heap blocks while keeping smoothing line-buffered.
    const size_t smoothRowsBytes = static_cast<size_t>(outWidth) * 3;
    ctx.smoothRows = makeUniqueNoThrow<uint8_t[]>(smoothRowsBytes);
    if (!ctx.smoothRows) {
      LOG_ERR("JPG", "OOM: progressive smoothing buffers");
    sJpgLastInfo.memFail = true;
    snprintf(sJpgLastErr, sizeof(sJpgLastErr), "oom:%s", "progressive smoothing buffers");
      return false;
    }
    ctx.smoothPrevRow = ctx.smoothRows.get();
    ctx.smoothCurrRow = ctx.smoothPrevRow + outWidth;
    ctx.smoothOutRow = ctx.smoothCurrRow + outWidth;
    LOG_DBG("JPG", "Progressive smoothing: %dx%d -> %dx%d, buffers=%u bytes", ctx.srcWidth, ctx.srcHeight, outWidth,
            outHeight, static_cast<unsigned>(smoothRowsBytes));
  } else if (needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(outWidth);
    ctx.rowCount = makeUniqueNoThrow<uint32_t[]>(outWidth);
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "OOM: scaling buffers");
    sJpgLastInfo.memFail = true;
    snprintf(sJpgLastErr, sizeof(sJpgLastErr), "oom:%s", "scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = scaleY_fp;
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    // v194：物件配到但 error row 沒配到 → 當失敗、退回無抖動量化，封面仍畫得出來。
    if (!ctx.atkinson1BitDitherer || !ctx.atkinson1BitDitherer->ok()) {
      const size_t bytes =
          sizeof(Atkinson1BitDitherer) + (static_cast<size_t>(outWidth) + 4) * sizeof(int16_t) * 3;
      noteDitherAllocFail("Atkinson1BitDitherer:JpegToBmp", bytes);
      ctx.atkinson1BitDitherer.reset();
    }
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth);
      if (!ctx.atkinsonDitherer || !ctx.atkinsonDitherer->ok()) {
        const size_t bytes =
            sizeof(AtkinsonDitherer) + (static_cast<size_t>(outWidth) + 4) * sizeof(int16_t) * 3;
        noteDitherAllocFail("AtkinsonDitherer:JpegToBmp", bytes);
        ctx.atkinsonDitherer.reset();
      }
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth);
      if (!ctx.fsDitherer || !ctx.fsDitherer->ok()) {
        const size_t bytes =
            sizeof(FloydSteinbergDitherer) + (static_cast<size_t>(outWidth) + 2) * sizeof(int16_t) * 2;
        noteDitherAllocFail("FloydSteinbergDitherer:JpegToBmp", bytes);
        ctx.fsDitherer.reset();
      }
    }
  }

  // v259（codex 第二輪）：上面那段是【算出來的】需求；這裡量【配完之後實際剩多少】—— 配置器的標頭與對齊、池不合併，
  //   算的不會剛好等於實際。縮圖路徑要求配完仍有 16KB 總量與 8KB 最大塊；不夠就放掉（unique_ptr 在 return 時釋放）、
  //   標 memFail 讓呼叫端退回。抖色誤差列配不到在這條路也當記憶體失敗：不要把「沒抖色的縮圖」寫成長期快取。
  if (allowDctScale) {
    const size_t freeAfter = ESP.getFreeHeap();
    const size_t largestAfter = ESP.getMaxAllocHeap();
    const bool ditherMissing = oneBit ? !ctx.atkinson1BitDitherer : false;
    if (freeAfter < THUMB_RESERVE_BYTES || largestAfter < 8 * 1024 || ditherMissing) {
      sJpgLastInfo.memFail = true;
      snprintf(sJpgLastErr, sizeof(sJpgLastErr), "heap-alloc free=%u max=%u dith=%u", static_cast<unsigned>(freeAfter),
               static_cast<unsigned>(largestAfter), ditherMissing ? 0u : 1u);
      return false;
    }
  }

  // Write BMP header with output dimensions
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
  }

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  const uint32_t decodeT0 = millis();
  rc = jpeg->decode(0, 0, jpegScaleOption);
  sJpgLastInfo.decodeMs = millis() - decodeT0;

  if (rc == 1 && ctx.smoothUpscale && !ctx.error) {
    finishSmoothUpscale(&ctx);
  }

  if (rc != 1 || ctx.error) {
    LOG_ERR("JPG", "JPEG decode failed (rc=%d, err=%d)", rc, jpeg->getLastError());
    snprintf(sJpgLastErr, sizeof(sJpgLastErr), "decode rc=%d err=%d %dx%d", rc, jpeg->getLastError(), srcWidth, srcHeight);
    return false;
  }

  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(halFileSource(jpegFile), bmpOut, targetWidth, targetHeight, false, crop, false);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(halFileSource(jpegFile), bmpOut, targetMaxWidth, targetMaxHeight, false, true, false);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(halFileSource(jpegFile), bmpOut, targetMaxWidth, targetMaxHeight, true, true, true);
}

// v258：同上，但來源是抽象的（主畫面縮圖直接從書裡的項目串流，不先抽到 SD）。
bool JpegToBmpConverter::jpegSourceTo1BitBmpStreamWithSize(const Source& source, Print& bmpOut, int targetMaxWidth,
                                                           int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(source, bmpOut, targetMaxWidth, targetMaxHeight, true, true, true);
}
