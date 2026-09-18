#pragma once

#include <HalStorage.h>
#include <InflateStream.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class ZipFile;

// v248：把 EPUB（zip）裡的一個項目當成「可以讀、可以跳位」的檔案 —— 圖片解碼器直接從書裡讀，不先抽到 SD。
//
// 為什麼（v247 實機 IMGDEC，封面 672KB）：抽圖＝讀 669KB 壓縮（1,506ms）＋解壓（152ms）＋**寫 672KB 到 SD（1,076ms）**，
// 解碼器再**把這 672KB 從 SD 讀回來（1,127ms）**。寫出去再讀回來那兩段約 2.2 秒是純浪費。
//
// 語意與 ReadAheadCore 期待的 File 相同：int read(void*, size_t)；bool seek(size_t)。
//   - stored（method 0）：隨機存取，位置＝資料起點＋pos。
//   - deflated（method 8）：往後跳＝解壓後丟掉；往回跳＝從頭重新解壓再丟到目標（JPEGDEC 只在檔頭解析時跳，
//     上面疊的預讀層會把「跳回緩衝範圍內」吸收掉，實際重來的機會很少）。
//   - 解壓出錯、讀檔出錯、解出來比宣告大小多 → 之後 read 回 -1（黏住），呼叫端的預讀層會標 hadError。
// 生命週期：open 之後 zip 檔柄一直開著，close／解構時關。記憶體：deflated 時 InflateStream 狀態 8,364B＋視窗 32,768B
//   （先配，缺一不可）＋壓縮讀取緩衝 readBufSize（v250：最後配，配不到減半到 1KB）；配不到 open 回 false（呼叫端退回抽到 SD），
//   失敗的步驟與當時最大塊記在 g_zipStreamStats.openFailStage／openFailMax。
class ZipEntryReader {
 public:
  ZipEntryReader();
  ~ZipEntryReader();
  ZipEntryReader(const ZipEntryReader&) = delete;
  ZipEntryReader& operator=(const ZipEntryReader&) = delete;

  bool open(const std::string& zipPath, const char* entryName, size_t readBufSize);
  void close();
  bool isOpen() const { return open_; }

  size_t size() const { return size_; }
  uint16_t method() const { return method_; }
  uint32_t compressedSize() const { return compressed_; }

  int read(void* buf, size_t n);
  bool seek(size_t pos);
  bool hadError() const { return error_; }

  // v248（codex 複查）：解碼器說成功之後、寫出快取之前呼叫。把還沒被讀到的部分解完，確認
  //   (1) 全程沒出過錯、(2) 正好在宣告大小結束（deflated：到 size 時再解一次必須是 Done 且沒有多的位元組）。
  //   解碼器常在 EOI／IEND 就停，不會自己讀到尾 —— 不做這一步，被截斷或多出資料的項目會被當成成功
  //   （舊的抽圖路徑在這些情況會回報失敗）。
  bool verifyComplete();

  // 桌機測試與儀器用：往回跳導致從頭重新解壓的次數。
  uint32_t restarts() const { return restarts_; }

 private:
  static size_t fillThunk(void* ctx, const uint8_t** data);
  size_t fill(const uint8_t** data);
  bool restartInflate();
  bool skipForward(size_t n);

  std::string zipPath_;  // ZipFile 只存參考 —— 必須先於 zip_ 存在、活得比它久
  std::unique_ptr<ZipFile> zip_;
  long dataOffset_ = -1;
  size_t size_ = 0;
  uint32_t compressed_ = 0;
  uint16_t method_ = 0xFFFF;
  size_t pos_ = 0;   // 解壓後（邏輯）位置
  size_t phys_ = 0;  // stored：zip 檔柄目前在資料區裡的位置（避免多餘 seek）
  bool open_ = false;
  bool error_ = false;
  bool inflateDone_ = false;
  uint32_t restarts_ = 0;

  InflateStream inflate_;
  std::unique_ptr<uint8_t[]> inBuf_;
  size_t inBufSize_ = 0;
  size_t compRemaining_ = 0;
};
