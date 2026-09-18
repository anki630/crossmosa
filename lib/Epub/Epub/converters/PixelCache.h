#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <stdint.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "DecodeStats.h"

// Streaming cache writer for 2-bit pixels (4 levels). Packs 4 pixels per byte,
// MSB first.
//
// The .pxc file is written incrementally in small row bands rather than holding
// the whole decoded image in one heap buffer. A full-page image (e.g. 482x728)
// needs ~88KB packed, which will not fit alongside the ~20KB JPEG decoder on a
// fragmented 380KB heap (free heap is routinely ~55KB on an image page). When
// the cache cannot be written, every render pass re-decodes the JPEG from
// scratch; an anti-aliased image page renders ~14 times (BW + AA restore + two
// grayscale planes x ~6 strips), so a 2s decode becomes a ~30s freeze / watchdog
// reset. Streaming keeps the working set to a single MCU-row band, so caching
// succeeds and the image is decoded exactly once.
//
// Correctness relies on JPEGDEC delivering blocks in raster MCU order (outer
// loop over y, inner over x: see jpeg.inl DecodeJPEG). Consecutive MCU rows map
// to contiguous, non-overlapping destination row ranges, so once a block whose
// top row is Y arrives, every output row < Y is final and is flushed to disk.
struct PixelCache {
  uint8_t* buffer;   // band buffer: (bandRows + 1) rows; last row kept zeroed
  uint8_t* zeroRow;  // points at the spare zeroed row, for gap/clip fill
  int width;
  int height;
  int bytesPerRow;
  int originX;      // config.x - to convert screen coords to cache coords
  int originY;      // config.y
  int bandRows;     // rows held in the band buffer
  int bandStart;    // image-local row index of band buffer row 0
  int flushedRows;  // image-local rows already written to file
  HalFile file;
  std::string cachePathStr;
  bool ok;
  // v248：寫入累積緩衝。v247 實機批次後每次約 726B（一個 MCU 列）仍小於 sector，封面 128 次寫 441ms；
  // 湊滿 4KB 才寫，讓 SdFat 走多 sector。配不到（nullptr）就照 v247 直接寫。
  uint8_t* wbuf = nullptr;
  size_t wlen = 0;
  static constexpr size_t WBUF_BYTES = 4096;

  PixelCache()
      : buffer(nullptr),
        zeroRow(nullptr),
        width(0),
        height(0),
        bytesPerRow(0),
        originX(0),
        originY(0),
        bandRows(0),
        bandStart(0),
        flushedRows(0),
        ok(false) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr int MIN_BAND_ROWS = 16;
  static constexpr size_t MAX_BAND_BYTES = 24 * 1024;  // band working-set ceiling

  // Open the cache file, write the header, and allocate a band buffer big enough
  // to hold the tallest single decode block (maxBlockDstRows output rows).
  bool begin(const std::string& cachePath, int w, int h, int ox, int oy, int maxBlockDstRows) {
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    bandStart = 0;
    flushedRows = 0;
    ok = false;

    int wantRows = maxBlockDstRows + 2;
    if (wantRows < MIN_BAND_ROWS) wantRows = MIN_BAND_ROWS;
    if (wantRows > h) wantRows = h;

    size_t maxRowsByMem = MAX_BAND_BYTES / (size_t)bytesPerRow;
    if (maxRowsByMem < 1) maxRowsByMem = 1;
    if ((size_t)wantRows > maxRowsByMem) wantRows = (int)maxRowsByMem;

    // A single decode block must fit inside the band, otherwise streaming would
    // drop rows. This only fails for pathological upscales that could not be
    // cached at all; fall back to the no-cache path.
    if (wantRows < maxBlockDstRows) {
      LOG_ERR("IMG", "Cache band too small (%d < %d rows) for %dx%d", wantRows, maxBlockDstRows, w, h);
      return false;
    }
    bandRows = wantRows;

    const size_t bufSize = (size_t)(bandRows + 1) * bytesPerRow;  // +1 spare zero row
    buffer = (uint8_t*)malloc(bufSize);
    if (!buffer) {
      LOG_ERR("IMG", "OOM cache band: %u bytes", (unsigned)bufSize);
      return false;
    }
    memset(buffer, 0, bufSize);
    zeroRow = buffer + (size_t)bandRows * bytesPerRow;
    wlen = 0;
    if (!wbuf) wbuf = (uint8_t*)malloc(WBUF_BYTES);  // 失敗＝直接寫，不是錯誤

    if (!Storage.openFileForWrite("IMG", cachePath, file)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath.c_str());
      free(buffer);
      buffer = nullptr;
      return false;
    }
    cachePathStr = cachePath;

    uint16_t w16 = (uint16_t)w;
    uint16_t h16 = (uint16_t)h;
    if (file.write(&w16, 2) != 2 || file.write(&h16, 2) != 2) {
      LOG_ERR("IMG", "Failed to write cache header: %s", cachePath.c_str());
      abort();
      return false;
    }

    LOG_DBG("IMG", "Cache stream started: %s (%dx%d, band %d rows)", cachePath.c_str(), w, h, bandRows);
    ok = true;
    return true;
  }

  // Flush every output row below newTopRow (they are final in raster order) and
  // reposition the band to start at newTopRow. Returns false if a write failed,
  // in which case the caller must stop caching for the rest of the decode.
  bool advanceTo(int newTopRow) {
    if (!ok) return false;
    if (newTopRow <= bandStart) return true;
    if (newTopRow > height) newTopRow = height;

    // v247：帶狀緩衝裡的列在記憶體中本來就連續 → 一次寫出（v246 實機：每列一次 128B 寫入，0.6ms／次）。
    //   帶子之外（idx ≥ bandRows）的列才逐列寫零列，那只在影像被裁切時出現。
    const int total = newTopRow - bandStart;
    const int fromBand = total < bandRows ? total : bandRows;
    if (fromBand > 0 && !writeRows(buffer, fromBand)) {
      LOG_ERR("IMG", "Cache write error at rows %d..%d", bandStart, bandStart + fromBand - 1);
      ok = false;
      return false;
    }
    for (int i = fromBand; i < total; ++i) {
      if (!writeRows(zeroRow, 1)) {
        LOG_ERR("IMG", "Cache write error at row %d", bandStart + i);
        ok = false;
        return false;
      }
    }
    flushedRows = newTopRow;
    bandStart = newTopRow;
    memset(buffer, 0, (size_t)bandRows * bytesPerRow);  // fresh band (gaps stay black)
    return true;
  }

  // Flush the final band and zero-fill any rows never covered (image clipped by
  // the screen), then close the file.
  bool finalize() {
    if (!ok) {
      abort();
      return false;
    }
    // v247：同 advanceTo —— flushedRows == bandStart 恆成立（兩者一起更新），所以帶子裡的列從 buffer[0] 起連續。
    const int total = height - flushedRows;
    const int fromBand = total < bandRows ? total : bandRows;
    if (fromBand > 0 && !writeRows(buffer, fromBand)) {
      LOG_ERR("IMG", "Cache write error at rows %d..%d", flushedRows, flushedRows + fromBand - 1);
      abort();
      return false;
    }
    for (int i = fromBand; i < total; ++i) {
      if (!writeRows(zeroRow, 1)) {
        LOG_ERR("IMG", "Cache write error at row %d", flushedRows + i);
        abort();
        return false;
      }
    }
    if (!flushPending()) {
      LOG_ERR("IMG", "Cache write error at final flush");
      abort();
      return false;
    }
    file.close();
    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", cachePathStr.c_str(), width, height,
            4 + bytesPerRow * height);
    ok = false;  // file handed off; nothing left to clean up
    return true;
  }

  // v247：一次寫 n 列（連續記憶體）。v248：先進累積緩衝，滿 4KB 才真的寫。v246 的儀器只算真的寫。
  bool writeRows(const uint8_t* rows, const int n) {
    const size_t bytes = static_cast<size_t>(n) * static_cast<size_t>(bytesPerRow);
    if (!wbuf) return writeRaw(rows, bytes);
    if (wlen + bytes <= WBUF_BYTES) {
      memcpy(wbuf + wlen, rows, bytes);
      wlen += bytes;
      return wlen < WBUF_BYTES || flushPending();
    }
    if (!flushPending()) return false;
    if (bytes >= WBUF_BYTES) return writeRaw(rows, bytes);
    memcpy(wbuf, rows, bytes);
    wlen = bytes;
    return true;
  }

  bool flushPending() {
    if (!wbuf || wlen == 0) return true;
    const size_t n = wlen;
    wlen = 0;
    return writeRaw(wbuf, n);
  }

  bool writeRaw(const uint8_t* data, const size_t bytes) {
    size_t wrote;
    {
      DecodeStatTimer t(g_decodeStats.writeUs);
      wrote = file.write(data, bytes);
    }
    g_decodeStats.writeCalls++;
    g_decodeStats.writeBytes += static_cast<uint32_t>(bytes);
    return wrote == bytes;
  }

  // Drop a partial/failed cache so a later decode re-creates it cleanly.
  void abort() {
    wlen = 0;  // v248：累積中的列一起丟掉
    if (file.isOpen()) file.close();
    if (!cachePathStr.empty()) {
      Storage.remove(cachePathStr.c_str());
    }
    ok = false;
  }

  ~PixelCache() {
    if (file.isOpen()) {
      // The file is still open, so neither finalize() nor abort() ran, or a
      // mid-stream write failed (advanceTo() cleared ok but left the file open).
      // Drop the partial cache so we leave no corrupt file behind.
      abort();
    }
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
    if (wbuf) {
      free(wbuf);
      wbuf = nullptr;
    }
  }
};
