#include "GifToFramebufferConverter.h"

#include <AnimatedGIF.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// 與 PNG 轉換器同法：context 走 pDraw->pUser（playFrame 的 pUser 參數），
// 檔案 I/O callback 走 pFile->fHandle，不留全域可變狀態。
struct GifContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  uint32_t lastYieldMs{0};  // yieldDuringDecode() 的節流狀態
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};

  // 交錯 GIF 的行序非循序：關快取（.pxc 的單列帶只能往下捲）、關 lastDstY 去重
  //（去重假設行序遞增，交錯下會把後到的行整批丟掉）。framebuffer 本身可隨機寫，畫面正確。
  bool interlaced{false};

  PixelCache cache;
  bool caching{false};

  uint8_t* grayLineBuffer{nullptr};
};

void* gifOpenWithHandle(const char* filename, int32_t* size) {
  HalFile* f = new (std::nothrow) HalFile();
  if (!f) {
    // v194：與 PNG／JPEG 同一證人；失敗不呼叫 close()。
    HalStorage::noteAllocFail("HalFile:gifOpen", sizeof(HalFile));
    return nullptr;
  }
  // v194：HalStorage 已有 const char* 多載。std::string(filename) 在 nothrow 守衛之後仍會 abort。
  if (!Storage.openFileForRead("GIF", filename, *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void gifCloseWithHandle(void* handle) {
  HalFile* f = reinterpret_cast<HalFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

int32_t gifReadWithHandle(GIFFILE* pFile, uint8_t* pBuf, int32_t len) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  const int32_t n = f->read(pBuf, len);
  if (n > 0) pFile->iPos += n;  // AnimatedGIF 依 iPos 追蹤位置（PNGdec 是庫內自理，這裡要自己記）
  return n;
}

int32_t gifSeekWithHandle(GIFFILE* pFile, int32_t pos) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return -1;
  if (!f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

// AnimatedGIF 物件（含 GIFIMAGE）約 28.5KB（MAX_WIDTH=3072 之後），單次解碼期間的暫態配置。
// 與 PNGdec（約 44KB）同一種待遇：用時才配、用完即還。
constexpr size_t GIF_DECODER_APPROX_SIZE = 30 * 1024;
constexpr size_t MIN_FREE_HEAP_FOR_GIF = GIF_DECODER_APPROX_SIZE + 16 * 1024;

// 交錯旗標在【第一個 image descriptor】的 flags bit6 —— AnimatedGIF 的 GIFDRAW 不吐這個，
// 自己掃檔頭。走法：跳過 GCT 與 extension blocks，到 0x2C 讀第 9 位元組。
bool probeInterlaced(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("GIF", path, f)) return false;
  uint8_t hdr[13];
  if (f.read(hdr, 13) != 13) return false;
  int32_t pos = 13;
  if (hdr[10] & 0x80) pos += 3 * (1 << ((hdr[10] & 7) + 1));  // Global Color Table
  // 上限 64 個區塊：正常檔第一個 descriptor 前只有寥寥數個 extension
  for (int guard = 0; guard < 64; guard++) {
    if (!f.seek(pos)) return false;
    uint8_t b;
    if (f.read(&b, 1) != 1) return false;
    pos++;
    if (b == 0x2C) {  // Image Descriptor：跳 8 bytes（位置+尺寸）讀 flags
      uint8_t desc[9];
      if (f.read(desc, 9) != 9) return false;
      return (desc[8] & 0x40) != 0;
    }
    if (b == 0x21) {  // Extension：label + sub-blocks
      if (f.read(&b, 1) != 1) return false;
      pos++;
      for (;;) {
        if (!f.seek(pos)) return false;
        uint8_t len;
        if (f.read(&len, 1) != 1) return false;
        pos++;
        if (len == 0) break;
        pos += len;
      }
      continue;
    }
    return false;  // 其他位元組 = 結構異常，保守當非交錯（解碼端自會報錯）
  }
  return false;
}

void gifDrawCallback(GIFDRAW* pDraw) {
  GifContext* ctx = reinterpret_cast<GifContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return;

  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);

  // 幀相對畫布的偏移（首幀通常 0,0；防禦性帶上）
  const int srcY = pDraw->iY + pDraw->y;
  if (srcY < 0 || srcY >= ctx->srcHeight) return;

  // 與 PNG 相同的列映射：縮小時多列選一、放大時一列鋪多列
  int firstDstY = (srcY * ctx->dstHeight) / ctx->srcHeight;
  int endDstY = firstDstY + 1;
  if (ctx->dstHeight > ctx->srcHeight) {
    endDstY = ((srcY + 1) * ctx->dstHeight) / ctx->srcHeight;
  }
  if (!ctx->interlaced) {
    if (firstDstY <= ctx->lastDstY) firstDstY = ctx->lastDstY + 1;
  }
  if (firstDstY >= endDstY || firstDstY >= ctx->dstHeight) return;
  if (endDstY > ctx->dstHeight) endDstY = ctx->dstHeight;

  // 整行先轉灰階：調色盤 RGB888 → luma；透明像素 = 白（e-ink 頁面背景，
  // 與首幀「畫在白底上」的語意一致——host 驗證與 PIL 參考 0.00% 偏差）
  {
    const uint8_t* pal = pDraw->pPalette24;
    const int frameX = pDraw->iX;
    // 幀寬可能小於畫布寬：幀外區域維持白
    memset(ctx->grayLineBuffer, 255, ctx->srcWidth);
    if (pal) {
      for (int x = 0; x < pDraw->iWidth; x++) {
        const int cx = frameX + x;
        if (cx < 0 || cx >= ctx->srcWidth) continue;
        const uint8_t idx = pDraw->pPixels[x];
        if (pDraw->ucHasTransparency && idx == pDraw->ucTransparent) continue;
        const uint8_t* rgb = &pal[idx * 3];
        ctx->grayLineBuffer[cx] = static_cast<uint8_t>((rgb[0] * 299 + rgb[1] * 587 + rgb[2] * 114) / 1000);
      }
    }
  }

  const int dstWidth = ctx->dstWidth;
  const int outXBase = ctx->config->x;
  const int screenWidth = ctx->screenWidth;
  const bool useDithering = ctx->config->useDithering;
  const int srcWidth = ctx->srcWidth;

  DirectPixelWriter pw;
  pw.init(*ctx->renderer);

  for (int dstY = firstDstY; dstY < endDstY; dstY++) {
    if (!ctx->interlaced) ctx->lastDstY = dstY;
    const int outY = ctx->config->y + dstY;
    if (outY >= ctx->screenHeight) continue;

    pw.beginRow(outY);

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
      const int outX = outXBase + dstX;
      if (outX < screenWidth) {
        const uint8_t gray = ctx->grayLineBuffer[srcX];
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
      error += srcWidth;
      while (error >= dstWidth) {
        error -= dstWidth;
        srcX++;
      }
    }
  }
}

}  // namespace

bool GifToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  // 尺寸在檔頭第 6-9 位元組（LE），與 PNG 的 getDimensionsStatic 同理由：
  // 排版階段絕不為了讀個尺寸配 28KB 的解碼器（教訓 A-10 的自癒前提）。
  HalFile file;
  if (!Storage.openFileForRead("GIF", imagePath, file)) {
    LOG_ERR("GIF", "Failed to open GIF for dimensions: %s", imagePath.c_str());
    return false;
  }
  uint8_t hdr[10];
  const int n = file.read(hdr, 10);
  file.close();
  if (n != 10 || memcmp(hdr, "GIF8", 4) != 0) {
    return false;
  }
  const uint32_t width = static_cast<uint32_t>(hdr[6] | (hdr[7] << 8));
  const uint32_t height = static_cast<uint32_t>(hdr[8] | (hdr[9] << 8));
  return validateAndStoreDimensions(width, height, out, "GIF", /*applyPixelCap=*/false);
}

bool GifToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("GIF", "Decoding GIF: %s", imagePath.c_str());

  const size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_GIF) {
    LOG_ERR("GIF", "Not enough heap for GIF decoder (%u free, need %u)", freeHeap, MIN_FREE_HEAP_FOR_GIF);
    setLastError(true, "gif-heap %u<%u", static_cast<unsigned>(freeHeap), static_cast<unsigned>(MIN_FREE_HEAP_FOR_GIF));
    return false;
  }

  std::unique_ptr<AnimatedGIF> gif(new (std::nothrow) AnimatedGIF());
  if (!gif) {
    LOG_ERR("GIF", "Failed to allocate GIF decoder");
    setLastError(true, "gif-alloc-decoder");
    return false;
  }
  gif->begin(GIF_PALETTE_RGB888);

  GifContext ctx;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();
  ctx.interlaced = probeInterlaced(imagePath);

  if (!gif->open(imagePath.c_str(), gifOpenWithHandle, gifCloseWithHandle, gifReadWithHandle, gifSeekWithHandle,
                 gifDrawCallback)) {
    LOG_ERR("GIF", "Failed to open GIF: %d", gif->getLastError());
    setLastError(false, "gif-open %d", gif->getLastError());
    return false;
  }
  const ScopedCleanup cleanup{[&gif]() { gif->close(); }};

  ImageDimensions sourceDimensions;
  if (!validateAndStoreDimensions(gif->getCanvasWidth(), gif->getCanvasHeight(), sourceDimensions, "GIF")) return false;
  ctx.srcWidth = sourceDimensions.width;
  ctx.srcHeight = sourceDimensions.height;

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
  } else {
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    float scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (scale > 1.0f) scale = 1.0f;
    ctx.dstWidth = (int)(ctx.srcWidth * scale);
    ctx.dstHeight = (int)(ctx.srcHeight * scale);
  }
  if (ctx.dstWidth <= 0 || ctx.dstHeight <= 0) {
    LOG_ERR("GIF", "Degenerate output size %dx%d", ctx.dstWidth, ctx.dstHeight);
    return false;
  }
  ctx.lastDstY = -1;

  ctx.grayLineBuffer = static_cast<uint8_t*>(malloc(ctx.srcWidth));
  if (!ctx.grayLineBuffer) {
    LOG_ERR("GIF", "Failed to allocate gray line buffer (%d bytes)", ctx.srcWidth);
    return false;
  }
  const ScopedCleanup lineCleanup{[&ctx]() {
    free(ctx.grayLineBuffer);
    ctx.grayLineBuffer = nullptr;
  }};

  // 與 PNG 同：有 cachePath 且非交錯才寫 .pxc（交錯行序會亂、單列帶不能倒捲）
  if (!config.cachePath.empty() && !ctx.interlaced) {
    ctx.caching = ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1);
  }

  LOG_DBG("GIF", "GIF %dx%d -> %dx%d interlaced=%d", ctx.srcWidth, ctx.srcHeight, ctx.dstWidth, ctx.dstHeight,
          ctx.interlaced ? 1 : 0);

  // 只解第一幀：e-ink 不放動畫。playFrame 回 0=最後一幀、1=還有下一幀、<0=錯誤；
  // 首幀解完即收工，後續幀（若有）完全不讀。
  ctx.lastYieldMs = millis();
  const int rc = gif->playFrame(false, nullptr, &ctx);
  const int err = gif->getLastError();
  if (rc < 0 || (err != GIF_SUCCESS && err != GIF_EMPTY_FRAME)) {
    LOG_ERR("GIF", "GIF decode failed: rc=%d err=%d", rc, err);
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  if (ctx.caching) {
    ctx.cache.finalize();
  }
  return true;
}

bool GifToFramebufferConverter::supportsFormat(const std::string& extension) { return extension == ".gif"; }
