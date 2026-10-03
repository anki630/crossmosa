#pragma once
// v350：固定版面 spine 清單（遞增的 uint16 索引）的容器。
//   -fno-exceptions 下 std::vector 擴容配不到記憶體＝abort；「先看最大連續塊再 reserve」也不是保證
//   （檢查與配置之間別的 task 可能先配走，codex 第二輪）。所以這裡自己管一塊 new (std::nothrow) 的陣列：
//   配不到就回 false，呼叫端把這本書當作「清單不知道」（照一般版面顯示），不會讓機器重開。
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

class SpineIndexList {
 public:
  SpineIndexList() = default;
  ~SpineIndexList() { delete[] data_; }
  SpineIndexList(const SpineIndexList&) = delete;
  SpineIndexList& operator=(const SpineIndexList&) = delete;
  SpineIndexList(SpineIndexList&& o) noexcept : data_(o.data_), size_(o.size_), cap_(o.cap_) {
    o.data_ = nullptr;
    o.size_ = o.cap_ = 0;
  }
  SpineIndexList& operator=(SpineIndexList&& o) noexcept {
    if (this != &o) {
      delete[] data_;
      data_ = o.data_;
      size_ = o.size_;
      cap_ = o.cap_;
      o.data_ = nullptr;
      o.size_ = o.cap_ = 0;
    }
    return *this;
  }

  // 容量不夠就擴成兩倍（第一次 16）；配不到回 false，內容不變。
  bool push(const uint16_t v) {
    if (size_ == cap_ && !grow(cap_ ? cap_ * 2 : 16)) return false;
    data_[size_++] = v;
    return true;
  }
  // 一次配好 n 個的空間（讀檔時知道筆數）。配不到回 false。
  bool reserve(const size_t n) { return n <= cap_ || grow(n); }
  void clear() {
    delete[] data_;
    data_ = nullptr;
    size_ = cap_ = 0;
  }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  uint16_t operator[](const size_t i) const { return data_[i]; }
  uint16_t back() const { return data_[size_ - 1]; }
  const uint16_t* data() const { return data_; }
  // 遞增清單的二分搜尋。
  bool contains(const uint16_t v) const {
    size_t lo = 0, hi = size_;
    while (lo < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (data_[mid] < v) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo < size_ && data_[lo] == v;
  }
  // 拿掉一個值（只搬移、不重新配置）。回傳有沒有拿掉。
  bool remove(const uint16_t v) {
    for (size_t i = 0; i < size_; i++) {
      if (data_[i] != v) continue;
      memmove(data_ + i, data_ + i + 1, (size_ - i - 1) * sizeof(uint16_t));
      size_--;
      return true;
    }
    return false;
  }

 private:
  bool grow(const size_t n) {
    auto* nd = new (std::nothrow) uint16_t[n];
    if (!nd) return false;
    if (size_) memcpy(nd, data_, size_ * sizeof(uint16_t));
    delete[] data_;
    data_ = nd;
    cap_ = n;
    return true;
  }
  uint16_t* data_ = nullptr;
  size_t size_ = 0;
  size_t cap_ = 0;
};
