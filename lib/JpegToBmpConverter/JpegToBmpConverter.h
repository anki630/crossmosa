#pragma once

#include <HalStorage.h>

#include <cstdint>

class Print;
class ZipFile;

class JpegToBmpConverter {
 public:
  // v258：解碼來源。HalFile 版本內部包成這個；主畫面縮圖用它直接從書裡的項目串流（不先抽到 SD）。
  //   read：回傳讀到的位元組數（<0 視同 0）。seek：絕對位置，成功回 true。size：整個項目的位元組數。
  //   ctx 的生命週期由呼叫端負責，必須活過整次轉檔。
  struct Source {
    void* ctx = nullptr;
    int32_t (*read)(void* ctx, uint8_t* buf, int32_t len) = nullptr;
    bool (*seek)(void* ctx, int32_t pos) = nullptr;
    int32_t size = 0;
  };
  // v258：最近一次轉檔的幾何（src＝原圖、dec＝JPEGDEC 解出的格子、out＝寫進 BMP 的大小）、縮放分母與解碼時間。
  struct Info {
    uint16_t srcW = 0, srcH = 0, decW = 0, decH = 0, outW = 0, outH = 0;
    uint8_t scale = 0;
    bool progressive = false;
    bool memFail = false;  // 失敗原因是配置／堆積不夠（呼叫端可以釋放別的東西後再試）；成功或其他失敗為 false
    uint32_t needBytes = 0;  // v259：縮圖路徑第二段門檻算出的需求（緩衝＋16KB 保留）
    uint32_t decodeMs = 0;
  };

 private:
  static bool jpegFileToBmpStreamInternal(const Source& source, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop, bool allowDctScale);

 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop = true);
  // Convert with custom target size (for thumbnails)
  static bool jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight);
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  // v258：這兩個 1-bit 入口允許 JPEGDEC 的 DCT 縮放（解出來仍 ≥ 輸出的最大縮放）；其他入口照舊全解析度。
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                              int targetMaxHeight);
  static bool jpegSourceTo1BitBmpStreamWithSize(const Source& source, Print& bmpOut, int targetMaxWidth,
                                                int targetMaxHeight);
  // v174：最近一次轉檔失敗的原因（靜態緩衝；成功時為空字串）。X3 沒有序列埠，LOG_ERR 等於丟掉。
  static const char* lastError();
  static const Info& lastInfo();
};
