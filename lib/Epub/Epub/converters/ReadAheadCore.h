#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// v247：解碼器讀檔的預讀層（純邏輯，不依賴裝置 —— 桌機測試直接拿真的 JPEGDEC／PNGdec 來比對輸出）。
//
// 為什麼（v246 實機 IMGDEC）：JPEGDEC 的內部緩衝只有 JPEG_FILE_BUF_SIZE=2048，用掉一些就補讀，
// 平均一次 1.5KB；672KB 的封面讀 432 次、每次 4–6.5ms，光讀檔 1.7 秒（佔第一次開圖片頁的 26%）。
// 小讀取在 SdFat 走單 sector＋快取複製＋儲存層互斥；一次讀一大段才走多 sector 直讀。
//
// 語意（必須與「直接呼叫 File::read／seek」完全一致，差一個位元組就是一張花掉的圖被寫進永久 .pxc 快取）：
//   - read(dst, n)：從目前位置讀最多 n 位元組，回傳實際讀到的數量（EOF 時變少）；第一次底層讀取就出錯時
//     回傳底層的值（可能是負數），已經讀到一些才出錯則回傳已讀的量。位置前進實際讀到的量。
//   - seek(pos)：pos > 檔案大小時失敗（不移動）；否則只記下位置，真的要讀時才去 SD 跳。
//   - 緩衝裡的內容一旦讀進來就永遠有效（唯讀檔）；跳回緩衝範圍內不碰 SD。
//   - 要讀的量 ≥ 緩衝容量時直接讀進呼叫端（不經過緩衝，不多一次複製）。
//   - 沒有緩衝（cap == 0）時完全退回直讀直跳。
//   - 前提：檔案在開著的期間不會被改（解碼用的圖檔唯讀）；size 是開檔當下的大小。
//   - 任何一次底層讀取出錯（回傳負數、或還沒到檔尾卻回 0、或回傳比要求還多）、或底層 seek 失敗 →
//     hadError() 從此為真（黏住），而且把「實體位置」標成未知，下一次讀之前一定重新 seek（codex 複查）。
//     呼叫端據此不把這次解碼的結果寫進 .pxc 快取 —— 讀錯時解碼器可能把它當成檔尾、畫出殘缺的圖。
// File 需要：int read(void* buf, size_t n)；bool seek(size_t pos)。
template <typename File>
class ReadAheadCore {
 public:
  void attach(File* file, size_t fileSize, uint8_t* buf, size_t cap) {
    file_ = file;
    size_ = fileSize;
    buf_ = cap > 0 ? buf : nullptr;
    cap_ = buf_ ? cap : 0;
    bufStart_ = 0;
    bufLen_ = 0;
    pos_ = 0;
    phys_ = 0;
    error_ = false;
  }

  size_t capacity() const { return cap_; }
  bool hadError() const { return error_; }
  size_t position() const { return pos_; }

  bool seek(size_t pos) {
    if (!file_) return false;
    if (cap_ == 0) {
      // 直讀模式：原樣轉給底層。
      if (!file_->seek(pos)) {
        if (pos <= size_) error_ = true;  // 範圍內卻跳不過去才是 I/O 錯；跳過檔尾是呼叫端的問題（同緩衝模式）
        return false;
      }
      pos_ = pos;
      phys_ = pos;
      return true;
    }
    if (pos > size_) return false;
    pos_ = pos;
    return true;
  }

  int read(uint8_t* dst, size_t len) {
    if (!file_) return 0;
    if (cap_ == 0) {
      const int r = file_->read(dst, len);
      if (r < 0 || static_cast<size_t>(r) > len || (r == 0 && len > 0 && pos_ < size_)) error_ = true;
      if (r > 0 && static_cast<size_t>(r) <= len) {
        pos_ += static_cast<size_t>(r);
        phys_ = pos_;
      }
      return r;
    }
    size_t total = 0;
    while (len > 0) {
      if (pos_ >= bufStart_ && pos_ < bufStart_ + bufLen_) {
        const size_t off = pos_ - bufStart_;
        size_t n = bufLen_ - off;
        if (n > len) n = len;
        memcpy(dst, buf_ + off, n);
        dst += n;
        len -= n;
        pos_ += n;
        total += n;
        continue;
      }
      if (pos_ >= size_) break;  // EOF
      if (phys_ != pos_) {
        if (!file_->seek(pos_)) {
          error_ = true;
          phys_ = kUnknown;
          return total > 0 ? static_cast<int>(total) : -1;
        }
        phys_ = pos_;
      }
      if (len >= cap_) {
        const int r = file_->read(dst, len);
        if (r <= 0 || static_cast<size_t>(r) > len) {
          error_ = true;  // 還沒到檔尾（上面檢查過 pos_ < size_）卻讀不到，或底層回報超量
          phys_ = kUnknown;
          return total > 0 ? static_cast<int>(total) : (r < 0 ? r : 0);
        }
        phys_ += static_cast<size_t>(r);
        pos_ += static_cast<size_t>(r);
        total += static_cast<size_t>(r);
        dst += r;
        // 短讀：交回給呼叫端（不在這裡重試）；不一定是檔尾，下一次呼叫會照常再讀。
        if (static_cast<size_t>(r) < len) break;
        len -= static_cast<size_t>(r);
        continue;
      }
      const int r = file_->read(buf_, cap_);
      if (r <= 0 || static_cast<size_t>(r) > cap_) {
        error_ = true;
        phys_ = kUnknown;
        bufLen_ = 0;
        return total > 0 ? static_cast<int>(total) : (r < 0 ? r : 0);
      }
      bufStart_ = pos_;
      bufLen_ = static_cast<size_t>(r);
      phys_ += bufLen_;
    }
    return static_cast<int>(total);
  }

 private:
  File* file_ = nullptr;
  size_t size_ = 0;
  uint8_t* buf_ = nullptr;
  size_t cap_ = 0;
  size_t bufStart_ = 0;  // 緩衝 buf_[0] 在檔案裡的位移
  size_t bufLen_ = 0;
  static constexpr size_t kUnknown = static_cast<size_t>(-1);
  size_t pos_ = 0;   // 解碼器眼中的目前位置
  size_t phys_ = 0;  // 底層檔案實際的位置（避免多餘的 seek）；kUnknown＝出錯後不可信，下次必 seek
  bool error_ = false;
};
