#include "ZhuyinBufferedSource.h"

#include <cstring>

namespace zhuyin {

bool BufferedSource::rawRead(const uint32_t off, void* dst, const uint32_t len) {
  const bool ok = raw_.read(off, dst, len);
  if (counting_) {
    rawReads_++;
    if (ok) rawBytes_ += len;
    else rawFails_++;
  }
  return ok;
}

void BufferedSource::use(uint8_t* const buf, const uint32_t cap) {
  buf_ = nullptr;
  cap_ = 0;
  bufStart_ = 0;
  bufLen_ = 0;
  mode_ = Mode::Direct;
  counting_ = true;
  if (!buf || cap == 0 || degraded_) return;
  const uint32_t n = size();
  if (cap >= n) {
    if (n == 0) return;
    if (!rawRead(0, buf, n)) {  // 整塊讀不到 → 直接讀（不是用一個整塊那麼大的窗口再試大讀取），這一趟都不再借
      degraded_ = true;
      return;
    }
    buf_ = buf;
    cap_ = cap;
    bufLen_ = n;
    mode_ = Mode::Whole;
    return;
  }
  buf_ = buf;
  cap_ = cap;
  mode_ = Mode::Window;
}

void BufferedSource::release() {
  buf_ = nullptr;
  cap_ = 0;
  bufStart_ = 0;
  bufLen_ = 0;
  mode_ = Mode::Direct;
  counting_ = false;
  degraded_ = false;
}

bool BufferedSource::read(const uint32_t off, void* dst, const uint32_t len) {
  if (counting_) calls_++;
  const uint32_t n = size();
  if (off > n || len > n - off) return false;
  if (mode_ == Mode::Whole) {  // 整塊都在緩衝裡（上面已擋越界）
    std::memcpy(dst, buf_ + off, len);
    return true;
  }
  if (mode_ == Mode::Direct || len > cap_) return rawRead(off, dst, len);
  const bool inside = bufLen_ > 0 && off >= bufStart_ && off - bufStart_ <= bufLen_ && len <= bufLen_ - (off - bufStart_);
  if (!inside) {
    // 從 off 所在的 512 位元組邊界開始讀滿一個窗口（循序讀往後走）；對齊之後放不下 → 從 off 開始
    uint32_t start = off & ~static_cast<uint32_t>(511);
    if (off - start + len > cap_) start = off;
    const uint32_t want = (n - start) < cap_ ? (n - start) : cap_;
    if (!rawRead(start, buf_, want)) {
      // 窗口讀不到：這一次與這一趟剩下的都直接讀（v338 的讀法）—— 緩衝不能變成新的失敗點
      buf_ = nullptr;
      cap_ = 0;
      bufLen_ = 0;
      mode_ = Mode::Direct;
      degraded_ = true;
      return rawRead(off, dst, len);
    }
    bufStart_ = start;
    bufLen_ = want;
  }
  std::memcpy(dst, buf_ + (off - bufStart_), len);
  return true;
}

}  // namespace zhuyin
