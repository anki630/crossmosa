#include "ScreenshotUtil.h"

#include "DiagLog.h"

#include <Arduino.h>
#include <BitmapHelpers.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>

#include "Bitmap.h"  // Required for BmpHeader struct definition
#include "activities/Activity.h"

void ScreenshotUtil::buildFilename(const ScreenshotInfo& info, char* buf, size_t bufSize) {
  const unsigned long ts = millis();

  if (info.readerType == ScreenshotInfo::ReaderType::None || info.title[0] == '\0') {
    snprintf(buf, bufSize, "/screenshots/screenshot-%lu.bmp", ts);
    return;
  }

  char sanitizedTitle[64];
  FsHelpers::sanitizePathComponentForFat32(info.title, sanitizedTitle, sizeof(sanitizedTitle));
  if (sanitizedTitle[0] == '\0') {
    snprintf(buf, bufSize, "/screenshots/screenshot-%lu.bmp", ts);
    return;
  }

  int pct = info.progressPercent;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  // Display spine index as 1-based for user-facing filenames
  const int chapterNum = info.spineIndex + 1;

  if (info.readerType == ScreenshotInfo::ReaderType::Epub && info.spineIndex >= 0) {
    snprintf(buf, bufSize, "/screenshots/%s/%s_ch%d_p%d_%dpct_%lu.bmp", sanitizedTitle, sanitizedTitle, chapterNum,
             info.currentPage, pct, ts);
  } else {
    snprintf(buf, bufSize, "/screenshots/%s/%s_p%d_%dpct_%lu.bmp", sanitizedTitle, sanitizedTitle, info.currentPage,
             pct, ts);
  }

  // Truncate title if total path exceeds FAT32 limit
  if (strlen(buf) > 255) {
    size_t titleLen = strlen(sanitizedTitle);
    size_t overhead = strlen(buf) - 2 * titleLen;
    if (overhead < 255) {
      size_t maxTitleLen = (255 - overhead) / 2;
      // Walk back to a valid UTF-8 boundary to avoid corrupting multibyte characters
      while (maxTitleLen > 0 && (sanitizedTitle[maxTitleLen] & 0xC0) == 0x80) {
        maxTitleLen--;
      }
      sanitizedTitle[maxTitleLen] = '\0';
      if (info.readerType == ScreenshotInfo::ReaderType::Epub && info.spineIndex >= 0) {
        snprintf(buf, bufSize, "/screenshots/%s/%s_ch%d_p%d_%dpct_%lu.bmp", sanitizedTitle, sanitizedTitle, chapterNum,
                 info.currentPage, pct, ts);
      } else {
        snprintf(buf, bufSize, "/screenshots/%s/%s_p%d_%dpct_%lu.bmp", sanitizedTitle, sanitizedTitle, info.currentPage,
                 pct, ts);
      }
    } else {
      snprintf(buf, bufSize, "/screenshots/screenshot-%lu.bmp", ts);
    }
  }
}

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) {
    LOG_ERR("SCR", "Framebuffer not available");
    return;
  }

  ScreenshotInfo info = activityManager.getScreenshotInfo();
  char filename[256];
  buildFilename(info, filename, sizeof(filename));

  bool saved = saveFramebufferAsBmp(filename, fb, renderer.getDisplayWidth(), renderer.getDisplayHeight());
  if (saved) {
    LOG_DBG("SCR", "Screenshot saved to %s", filename);
  } else {
    LOG_ERR("SCR", "Failed to save screenshot");
    return;
  }

  // Display a border around the screen to indicate a screenshot was taken
  if (renderer.storeBwBuffer()) {
    int marginTop, marginRight, marginBottom, marginLeft;
    renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
    int width = renderer.getScreenWidth() - marginLeft - marginRight - 1;
    int height = renderer.getScreenHeight() - marginTop - marginBottom - 1;
    // Add extra margin to the border to make it more visible
    renderer.drawRect(marginLeft + 1, marginTop + 1, width - 2, height - 2, 2, true);
    renderer.displayBuffer();
    delay(1000);
    renderer.restoreBwBuffer();
    renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
  }
}

// 建不出書名資料夾時的保底路徑：平鋪在 /screenshots/ 底下。
// ⚠️ 為什麼要有：截圖失敗對使用者是**完全無聲的**（只有 LOG_ERR，這台等於丟掉）。
//    書名資料夾只是整理上的方便，不值得讓整個功能失效。
// ⚠️⚠️ **備援路徑必須在【根目錄】**，不能在 `/screenshots` 底下。
//    原本寫的是 `/screenshots/screenshot-%lu.bmp` —— 那正是剛剛建【失敗】的那個目錄，
//    於是備援會再試一次同一個 mkdir、再失敗、再備援…**無限遞迴**。
//    而備援存在的理由就是「那個目錄建不出來」，所以它絕對不能依賴那個目錄。
static void flatFallbackPath(char* buf, size_t bufSize) {
  snprintf(buf, bufSize, "/screenshot-%lu.bmp", static_cast<unsigned long>(millis()));
}

bool ScreenshotUtil::saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width,
                                          int height, const bool allowFallback) {
  if (!framebuffer) {
    return false;
  }

  // Note: the width and height, we rotate the image 90d counter-clockwise to match the default display orientation
  int phyWidth = height;
  int phyHeight = width;

  // ⚠️ **遞迴建目錄。** 原本只建最後一層 —— 路徑是 `/screenshots/<書名>/x.bmp`，
  //    第一次替某本書截圖時 `/screenshots` 若不存在，非遞迴的 mkdir 會失敗。
  const std::string path(filename);
  const size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    for (size_t pos = path.find('/', 1); pos != std::string::npos && pos <= last_slash;
         pos = path.find('/', pos + 1)) {
      const std::string dir = path.substr(0, pos);
      if (!dir.empty() && !Storage.exists(dir.c_str()) && !Storage.mkdir(dir.c_str())) {
        // ⚠️ 失敗必須看得見：這台沒有序列埠，LOG_ERR 等於丟掉，
        //    使用者只會看到「按了截圖但什麼都沒發生」。
        //    len= 是關鍵：v210 就是靠它才確定「路徑真的以半截位元組結尾」，
        //    而不是 log 被截斷（DiagLog 的緩衝是 384，容得下）。
        DiagLog::line("SCRFAIL mkdir len=%u %s", static_cast<unsigned>(dir.size()), dir.c_str());
        char flat[64];
        flatFallbackPath(flat, sizeof(flat));
        if (!allowFallback) return false;  // 遞迴閘：只退一次
        DiagLog::line("SCRFALLBACK %s", flat);
        return saveFramebufferAsBmp(flat, framebuffer, width, height, false);
      }
    }
    const std::string leaf = path.substr(0, last_slash);
    if (!leaf.empty() && !Storage.exists(leaf.c_str()) && !Storage.mkdir(leaf.c_str())) {
      DiagLog::line("SCRFAIL mkdir len=%u %s", static_cast<unsigned>(leaf.size()), leaf.c_str());
      char flat[64];
      flatFallbackPath(flat, sizeof(flat));
      if (!allowFallback) return false;  // 遞迴閘：只退一次
      DiagLog::line("SCRFALLBACK %s", flat);
      return saveFramebufferAsBmp(flat, framebuffer, width, height, false);
    }
  }

  HalFile file;
  if (!Storage.openFileForWrite("SCR", filename, file)) {
    DiagLog::line("SCRFAIL open len=%u %s", static_cast<unsigned>(strlen(filename)), filename);
    LOG_ERR("SCR", "Failed to save screenshot");
    return false;
  }

  BmpHeader header;

  createBmpHeader(&header, phyWidth, phyHeight, BmpRowOrder::BottomUp);

  bool write_error = false;
  if (file.write(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    write_error = true;
  }

  if (write_error) {
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filename);
    return false;
  }

  const uint32_t rowSizePadded = (phyWidth + 31) / 32 * 4;
  // Max row size for 528px height (X3) after rotation = 68 bytes; use fixed buffer to avoid VLA
  constexpr size_t kMaxRowSize = 68;
  if (rowSizePadded > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %u exceeds buffer capacity", rowSizePadded);
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(filename);
    return false;
  }

  // rotate the image 90d counter-clockwise on-the-fly while writing to save memory
  uint8_t rowBuffer[kMaxRowSize];
  memset(rowBuffer, 0, rowSizePadded);

  for (int outY = 0; outY < phyHeight; outY++) {
    for (int outX = 0; outX < phyWidth; outX++) {
      // 90d counter-clockwise: source (srcX, srcY)
      // BMP rows are bottom-to-top, so outY=0 is the bottom of the displayed image
      int srcX = width - 1 - outY;     // phyHeight == width
      int srcY = phyWidth - 1 - outX;  // phyWidth == height
      int fbIndex = srcY * (width / 8) + (srcX / 8);
      uint8_t pixel = (framebuffer[fbIndex] >> (7 - (srcX % 8))) & 0x01;
      rowBuffer[outX / 8] |= pixel << (7 - (outX % 8));
    }
    if (file.write(rowBuffer, rowSizePadded) != rowSizePadded) {
      write_error = true;
      break;
    }
    memset(rowBuffer, 0, rowSizePadded);  // Clear the buffer for the next row
  }

  // Explicitly close() file before calling Storage.remove()
  file.close();

  if (write_error) {
    Storage.remove(filename);
    return false;
  }

  return true;
}
