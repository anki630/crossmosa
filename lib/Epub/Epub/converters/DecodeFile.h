#pragma once

#include <HalStorage.h>
#include <Memory.h>
#include <ZipEntryReader.h>
#include <esp_heap_caps.h>

#include <cstdint>
#include <memory>
#include <new>

#include "DecodeStats.h"
#include "ReadAheadCore.h"

// v247：JPEG／PNG 解碼器的檔案把手 ＝ 來源 ＋ 預讀緩衝（ReadAheadCore，語意與桌機比對見那個檔）。
// v248：來源可以是 SD 上的檔案（HalFile），或【書裡的項目】（ZipEntryReader，不先抽到 SD）。
//
// 緩衝只在解碼期間存在（close 回呼釋放），不留長壽配置。大小依當下最大連續塊決定，且至少留 32KB 給
// 之後才配的快取帶狀緩衝（PixelCache::MAX_BAND_BYTES＝24KB，漸進式 JPEG 放大時會用到）與繪製（codex 複查：
// 原本的 8KB 不夠）；配不到就 cap=0 退回直讀直跳。
struct DecodeFile {
  HalFile file;
  ZipEntryReader zip;
  bool streamed = false;
  std::unique_ptr<uint8_t[]> buf;
  ReadAheadCore<HalFile> ra;
  ReadAheadCore<ZipEntryReader> raZip;

  int read(uint8_t* dst, size_t len) { return streamed ? raZip.read(dst, len) : ra.read(dst, len); }
  bool seek(size_t pos) { return streamed ? raZip.seek(pos) : ra.seek(pos); }
  // v248：串流時讀取器自己的錯誤也算（解壓出錯的那一次 read 可能仍回正數，預讀層看不到）。
  bool hadError() const { return streamed ? (raZip.hadError() || zip.hadError()) : ra.hadError(); }
};

// v248：目前這一次解碼的把手（同一時間只有一個解碼在跑：render task、持鎖）。open 設、close 清。
// 轉換器在「解碼器說成功、寫出快取之前」用它做串流來源的完整性檢查 —— close 回呼要等轉換器 return 才跑，太晚。
inline DecodeFile* g_activeDecodeFile = nullptr;

// 串流來源：把沒讀到的部分解完、確認正好在宣告大小結束且全程沒出錯。非串流一律 true。
inline bool verifyActiveDecodeSourceComplete() {
  DecodeFile* d = g_activeDecodeFile;
  if (!d || !d->streamed) return true;
  return !d->raZip.hadError() && d->zip.verifyComplete();
}

// v248：ImageBlock 在呼叫 decodeToFramebuffer 之前設、之後清。設著的時候，解碼器的開檔回呼不開 SD 上的檔名，
// 改開這本書裡的 src 項目。只有 JPEG／PNG 的轉換器走 openDecodeFile；其他格式看不到這個請求（照舊開檔 → 失敗 → 退回抽圖）。
using DecodeStreamOpenFn = bool (*)(void* ctx, const char* src, ZipEntryReader& reader, size_t readBufSize);
struct DecodeStreamRequest {
  DecodeStreamOpenFn open = nullptr;
  void* ctx = nullptr;
  const char* src = nullptr;
};
inline DecodeStreamRequest g_decodeStreamRequest;

inline DecodeFile* openDecodeFile(const char* tag, const char* filename, int32_t* size) {
  // v194：throwing new 在 -fno-exceptions 下 OOM 會 abort。失敗不呼叫 close()。
  DecodeFile* d = new (std::nothrow) DecodeFile();
  if (!d) {
    HalStorage::noteAllocFail("DecodeFile:open", sizeof(DecodeFile));
    return nullptr;
  }
  size_t fileSize = 0;
  if (g_decodeStreamRequest.open) {
    // 壓縮讀取緩衝最多 8KB（與抽圖時相同；v250 起配不到會減半）。inflate 狀態＋視窗約 41KB 在這裡配 —— 解碼器自己的大塊已經先配好了
    // （JPEGDEC 物件、PNG 的三塊都在 open() 之前），配不到就讓這次 open 失敗、呼叫端退回抽到 SD。
    if (!g_decodeStreamRequest.open(g_decodeStreamRequest.ctx, g_decodeStreamRequest.src, d->zip, 8 * 1024)) {
      g_decodeStats.streamOpenFailed = 1;
      delete d;
      return nullptr;
    }
    d->streamed = true;
    fileSize = d->zip.size();
    g_decodeStats.streamed = 1;
  } else {
    if (!Storage.openFileForRead(tag, filename, d->file)) {
      delete d;
      return nullptr;
    }
    fileSize = d->file.size();
  }
  *size = static_cast<int32_t>(fileSize);
  g_decodeStats.sourceBytes = static_cast<uint32_t>(fileSize);

  constexpr size_t kMaxReadAhead = 16 * 1024;
  constexpr size_t kMinReadAhead = 4 * 1024;
  constexpr size_t kHeadroom = 32 * 1024;
  size_t cap = kMaxReadAhead;
  if (fileSize < cap) cap = fileSize;  // 小檔不必配滿（整個檔一次讀進來）
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  while (cap > kMinReadAhead && cap + kHeadroom > largest) cap /= 2;
  if (cap + kHeadroom > largest) cap = 0;
  if (cap > 0) {
    d->buf = makeUniqueNoThrow<uint8_t[]>(cap);
    if (!d->buf) cap = 0;
  }
  if (d->streamed) {
    d->raZip.attach(&d->zip, fileSize, d->buf.get(), cap);
  } else {
    d->ra.attach(&d->file, fileSize, d->buf.get(), cap);
  }
  g_decodeStats.readAheadCap = static_cast<uint32_t>(cap);
  g_activeDecodeFile = d;
  return d;
}

inline void closeDecodeFile(void* handle) {
  auto* d = static_cast<DecodeFile*>(handle);
  if (!d) return;
  if (g_activeDecodeFile == d) g_activeDecodeFile = nullptr;
  if (d->streamed) {
    g_decodeStats.streamRestarts = d->zip.restarts();
    d->zip.close();
  } else {
    d->file.close();
  }
  delete d;
}
