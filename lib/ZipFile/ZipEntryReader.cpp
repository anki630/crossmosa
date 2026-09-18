#include "ZipEntryReader.h"

#include <Arduino.h>
#include <Logging.h>

#include <cstring>
#include <new>

#include "ZipFile.h"

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
static uint32_t largestFreeBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT); }
#else
static uint32_t largestFreeBlock() { return 0; }  // 桌機工具
#endif

namespace {
constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflated = 8;
// 往回跳導致從頭重新解壓的上限（codex 複查：壞圖不能讓 render 在鎖內反覆重解）。JPEGDEC／PNGdec 只在檔頭解析時跳，
// 上面疊的預讀層還會吸收緩衝範圍內的回跳；超過就算錯誤，呼叫端退回抽到 SD。
constexpr uint32_t kMaxRestarts = 8;
// 單一圖片項目的合理上限（解碼器的像素上限遠低於此）；也擋掉 int32 溢位。
constexpr uint32_t kMaxEntryBytes = 64u * 1024u * 1024u;
// v250：壓縮讀取緩衝配不到時減半到這裡為止（它只決定一次從 SD 讀多少壓縮資料，不影響正確性）。
constexpr size_t kMinReadBuf = 1024;

// 開啟失敗的證人（IMGDEC 的 src=fb:open:sN@bytes）。⚠️ 必須在 close()／reset() 之前呼叫 —— 放掉之後量到的最大塊
// 會含剛還回去的狀態／視窗（codex 複查）。
void noteOpenFail(const uint8_t stage) {
  g_zipStreamStats.openFailStage = stage;
  g_zipStreamStats.openFailMax = largestFreeBlock();
}
}  // namespace

ZipEntryReader::ZipEntryReader() = default;

ZipEntryReader::~ZipEntryReader() { close(); }

bool ZipEntryReader::open(const std::string& zipPath, const char* entryName, const size_t readBufSize) {
  close();
  restarts_ = 0;
  error_ = false;
  g_zipStreamStats.inBufBytes = 0;
  g_zipStreamStats.openFailStage = 0;
  g_zipStreamStats.openFailMax = 0;
  zipPath_ = zipPath;
  zip_.reset(new (std::nothrow) ZipFile(zipPath_));
  if (!zip_) {
    noteOpenFail(1);
    return false;
  }
  const uint32_t t0 = static_cast<uint32_t>(micros());  // v247 儀器沿用：zset
  // 整個讀取期間 zip 檔柄都開著（ZipFile 內部的 ScopedOpenClose 看到已開就不會關）。
  if (!zip_->open()) {
    noteOpenFail(2);
    zip_.reset();
    return false;
  }
  ZipFile::FileStatSlim st = {};
  if (!zip_->loadFileStatSlim(entryName, &st)) {
    noteOpenFail(3);
    close();
    return false;
  }
  dataOffset_ = zip_->getDataOffset(st);
  if (dataOffset_ < 0) {
    noteOpenFail(4);
    close();
    return false;
  }
  method_ = st.method;
  size_ = st.uncompressedSize;
  compressed_ = st.compressedSize;
  if (st.uncompressedSize > kMaxEntryBytes || st.compressedSize > kMaxEntryBytes) {
    noteOpenFail(5);
    close();
    return false;
  }
  g_zipStreamStats.setupUs += static_cast<uint32_t>(micros()) - t0;
  g_zipStreamStats.method = method_;
  g_zipStreamStats.compressed = compressed_;
  g_zipStreamStats.uncompressed = st.uncompressedSize;

  if (method_ == kMethodStored) {
    if (compressed_ != st.uncompressedSize) {
      noteOpenFail(5);
      close();
      return false;
    }
    if (!zip_->file.seek(static_cast<size_t>(dataOffset_))) {
      noteOpenFail(6);
      close();
      return false;
    }
    phys_ = 0;
  } else if (method_ == kMethodDeflated) {
    // v250：缺一不可的先配（inflate 狀態 8,364B＋視窗 32,768B），壓縮讀取緩衝最後配、配不到就減半。
    // diag249：碎片化的 p2（空塊 52,092／24,576）上，解碼器物件進 24.5K 那塊，讀取緩衝 8KB＋狀態＋視窗三個全擠 52K 那塊，
    //   最後配的視窗要過 TLSF 取整（找 ≥34,816 的塊）—— v248 同佈局剩 52B 成功、v249 位移 200B 就全部退回抽圖。
    //   桌機用 ESP-IDF 真的 tlsf.c 重播（工作區 tools/tlsf-stream-open-sim）：這個順序在 v249 佈局上成功（讀取緩衝退到 4KB），
    //   原本的順序、只換順序不減半、縮小解碼器物件都失敗。
    if (!restartInflate()) {
      noteOpenFail(7);
      close();
      return false;
    }
    inBufSize_ = readBufSize > kMinReadBuf ? readBufSize : kMinReadBuf;
    for (;;) {
      inBuf_.reset(new (std::nothrow) uint8_t[inBufSize_]);
      if (inBuf_ || inBufSize_ == kMinReadBuf) break;
      inBufSize_ = inBufSize_ / 2 > kMinReadBuf ? inBufSize_ / 2 : kMinReadBuf;
    }
    if (!inBuf_) {
      noteOpenFail(8);
      close();
      return false;
    }
    g_zipStreamStats.inBufBytes = static_cast<uint16_t>(inBufSize_);
    restarts_ = 0;  // 開檔那一次不算
  } else {
    noteOpenFail(5);
    close();
    return false;
  }
  pos_ = 0;
  error_ = false;
  open_ = true;
  return true;
}

void ZipEntryReader::close() {
  inflate_.deinit();
  inBuf_.reset();
  inBufSize_ = 0;
  if (zip_) {
    zip_->close();
    zip_.reset();
  }
  open_ = false;
  pos_ = 0;
  phys_ = 0;
  size_ = 0;
  dataOffset_ = -1;
  method_ = 0xFFFF;
  inflateDone_ = false;
}

size_t ZipEntryReader::fillThunk(void* ctx, const uint8_t** data) { return static_cast<ZipEntryReader*>(ctx)->fill(data); }

size_t ZipEntryReader::fill(const uint8_t** data) {
  if (compRemaining_ == 0) return 0;
  const size_t toRead = compRemaining_ < inBufSize_ ? compRemaining_ : inBufSize_;
  const uint32_t t0 = static_cast<uint32_t>(micros());
  const int r = zip_->file.read(inBuf_.get(), toRead);
  g_zipStreamStats.readUs += static_cast<uint32_t>(micros()) - t0;
  if (r <= 0) {
    error_ = true;  // 壓縮資料還沒讀完卻讀不到
    return 0;
  }
  g_zipStreamStats.readBytes += static_cast<uint32_t>(r);
  compRemaining_ -= static_cast<size_t>(r);
  *data = inBuf_.get();
  return static_cast<size_t>(r);
}

bool ZipEntryReader::restartInflate() {
  if (restarts_ >= kMaxRestarts) return false;  // v250（codex）：原本 > 會允許第 9 次
  if (!zip_->file.seek(static_cast<size_t>(dataOffset_))) return false;
  compRemaining_ = compressed_;
  // v251：環形視窗依解壓後大小取 2 的次方（小項目不需要 32KB；總輸出不超過環就不會繞回，見 InflateStream::init）。
  if (!inflate_.init(true, size_)) return false;
  inflate_.setFill(&ZipEntryReader::fillThunk, this);
  pos_ = 0;
  inflateDone_ = false;
  restarts_++;
  return true;
}

int ZipEntryReader::read(void* buf, const size_t n) {
  if (!open_ || error_) return -1;
  if (n == 0 || pos_ >= size_) return 0;
  const size_t want = (size_ - pos_) < n ? (size_ - pos_) : n;
  if (method_ == kMethodStored) {
    if (phys_ != pos_) {
      if (!zip_->file.seek(static_cast<size_t>(dataOffset_) + pos_)) {
        error_ = true;
        return -1;
      }
      phys_ = pos_;
    }
    const uint32_t t0 = static_cast<uint32_t>(micros());
    const int r = zip_->file.read(buf, want);
    g_zipStreamStats.readUs += static_cast<uint32_t>(micros()) - t0;
    if (r <= 0 || static_cast<size_t>(r) > want) {
      error_ = true;
      return -1;
    }
    g_zipStreamStats.readBytes += static_cast<uint32_t>(r);
    pos_ += static_cast<size_t>(r);
    phys_ = pos_;
    return r;
  }
  // deflated
  auto* dst = static_cast<uint8_t*>(buf);
  size_t total = 0;
  while (total < want && !inflateDone_) {
    size_t produced = 0;
    const uint32_t readBefore = g_zipStreamStats.readUs;
    const uint32_t t0 = static_cast<uint32_t>(micros());
    const InflateStream::Status status = inflate_.readAtMost(dst + total, want - total, &produced);
    g_zipStreamStats.inflateUs += (static_cast<uint32_t>(micros()) - t0) - (g_zipStreamStats.readUs - readBefore);
    total += produced;
    if (status == InflateStream::Status::Error || error_) {  // 讀檔錯誤優先於 Done（fill 回 0 時 tinfl 可能報 Done）
      error_ = true;
      break;
    } else if (status == InflateStream::Status::Done) {
      inflateDone_ = true;
    } else if (produced == 0) {
      error_ = true;  // Ok 卻沒有產出：不該發生，當成壞資料
      break;
    }
  }
  pos_ += total;
  if (total == 0 && error_) return -1;
  if (inflateDone_ && pos_ < size_) error_ = true;  // 解壓結束卻比宣告的少：之後的讀取回 -1
  return static_cast<int>(total);
}

bool ZipEntryReader::verifyComplete() {
  if (!open_ || error_) return false;
  // 還沒讀到的部分解完（通常只剩 EOI／IEND 之後的零頭；預讀層多半已經讀過了）。
  if (pos_ < size_ && !skipForward(size_ - pos_)) return false;
  if (error_ || pos_ != size_) return false;
  if (method_ == kMethodDeflated && !inflateDone_) {
    uint8_t extra;
    size_t produced = 0;
    const InflateStream::Status status = inflate_.readAtMost(&extra, 1, &produced);
    if (produced != 0 || status != InflateStream::Status::Done || error_) {
      error_ = true;  // 解出來比宣告的多，或串流沒有正常結束
      return false;
    }
    inflateDone_ = true;
  }
  return true;
}

bool ZipEntryReader::skipForward(size_t n) {
  uint8_t scratch[512];
  while (n > 0) {
    const size_t chunk = n < sizeof(scratch) ? n : sizeof(scratch);
    const int r = read(scratch, chunk);
    if (r <= 0) return false;
    n -= static_cast<size_t>(r);
  }
  return true;
}

bool ZipEntryReader::seek(const size_t pos) {
  if (!open_ || pos > size_) return false;
  if (method_ == kMethodStored) {
    pos_ = pos;  // 真的讀的時候才去 SD 跳
    return true;
  }
  if (error_) return false;
  if (pos == pos_) return true;
  if (pos < pos_) {
    if (!restartInflate()) {
      error_ = true;
      return false;
    }
  }
  if (!skipForward(pos - pos_)) {
    error_ = true;
    return false;
  }
  return true;
}
