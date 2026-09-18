#pragma once

#include <Arduino.h>

#include <cstdint>

// v246 儀器：第一次打開圖片頁的時間花在哪。
//
// v245 實機（diag245.log）第一次看到書首封面的補圖那一遍 `SEG tiled bw=6759`，但 bw 裡混著
// 「從 EPUB 抽圖到 SD」「解碼器讀檔」「解碼＋縮放＋抖色」「.pxc 快取逐列寫 SD」「讓出 CPU」「重建字型快取」，
// log 分不出來 —— 不先拆開就只能猜要改哪一段。
//
// 解碼器層（JPEG／PNG／GIF converter、PixelCache）累加到這一份；ImageBlock 每次解碼前歸零、之後讀走組成
// IMGDEC 麵包屑（lib 不能依賴 src 的 DiagLog，同 lastFailPath 的分層）。
// 只做加法與 micros()，不配置、不阻塞；只在「解碼圖片」這條冷路徑上跑。
struct DecodeStats {
  uint32_t readUs = 0;
  uint32_t readCalls = 0;
  uint32_t readBytes = 0;
  uint32_t writeUs = 0;  // .pxc 快取寫入（PixelCache::advanceTo／finalize 的 file.write）
  uint32_t writeCalls = 0;
  uint32_t writeBytes = 0;
  uint32_t yieldUs = 0;  // yieldDuringDecode 的 vTaskDelay
  uint32_t yields = 0;
  uint32_t readAheadCap = 0;  // v247：這次解碼用的預讀緩衝大小（0＝直讀）
  uint8_t ioError = 0;        // v247：解碼期間底層讀／跳出過錯（黏住）→ 不寫 .pxc、當暫時失敗
  uint8_t streamed = 0;          // v248：這次是直接從書裡讀（沒抽到 SD）
  uint8_t streamOpenFailed = 0;  // v248：想直接從書裡讀但開不起來（找不到項目／記憶體不夠）
  uint32_t streamRestarts = 0;   // v248：往回跳導致從頭重新解壓的次數
  uint32_t sourceBytes = 0;      // v248：解碼器看到的檔案大小（串流時＝解壓後大小）
  uint32_t setupUs = 0;     // 解碼器 open ＋ 檔頭 ＋ 快取 begin
  uint32_t decodeUs = 0;    // decoder->decode() 的牆鐘時間（含上面的讀／寫／讓出）
  uint32_t finalizeUs = 0;  // 快取 finalize（補零列＋關檔）
  uint16_t srcW = 0;
  uint16_t srcH = 0;
  uint16_t dstW = 0;
  uint16_t dstH = 0;
  uint8_t scaleDenom = 0;  // JPEG 內建縮小 1/N；PNG／GIF 為 1
  uint8_t progressive = 0;
  char fmt = '?';  // 'J' 'P' 'G'
};

inline DecodeStats g_decodeStats;

// RAII：量一段，加到指定欄位。
struct DecodeStatTimer {
  uint32_t& slot;
  const uint32_t startUs;
  explicit DecodeStatTimer(uint32_t& s) : slot(s), startUs(static_cast<uint32_t>(micros())) {}
  ~DecodeStatTimer() { slot += static_cast<uint32_t>(micros()) - startUs; }
  DecodeStatTimer(const DecodeStatTimer&) = delete;
  DecodeStatTimer& operator=(const DecodeStatTimer&) = delete;
};
