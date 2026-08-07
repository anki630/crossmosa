#include "MemoryArena.h"

#include <new>

// 全域實例(宣告與容量常數見標頭)。這裡只是定義,不在此取得記憶體——
// 取得的時機是設計的一部分,由 main.cpp 的 setup() 明確控制。
MemoryArena FONT_BITMAP_ARENA;
MemoryArena GRAY_PLANE_ARENA;

MemoryArena::~MemoryArena() {
  // 解構時無條件釋放:全域實例的生命週期與程式相同,走到這裡代表程式正在結束
  // (或桌面測試在收尾),此時 used() 守衛沒有保護對象。
  delete[] base_;
  base_ = nullptr;
}

bool MemoryArena::reserve(const size_t bytes) {
  if (base_ != nullptr) return true;  // 冪等:已經有了就算成功
  if (bytes == 0) return false;

  // new (std::nothrow):`-fno-exceptions` 下裸 new 失敗會 abort() 而不是回 nullptr
  // (見 src/CLAUDE.md「`new` is not nothrow on ESP32」)。
  base_ = new (std::nothrow) uint8_t[bytes];
  if (base_ == nullptr) return false;

  capacity_ = bytes;
  used_ = 0;
  highWater_ = 0;
  missCount_ = 0;
  return true;
}

bool MemoryArena::release() {
  if (base_ == nullptr) return true;  // 已經是釋放狀態
  // 見標頭:還有人持有競技場內的指標時釋放 = 製造懸空指標,而持有者稍後會
  // 拿它去 delete[]。寧可不釋放(後果只是 WiFi 少一塊記憶體)也不能損毀堆積。
  if (used_ != 0) return false;

  delete[] base_;
  base_ = nullptr;
  capacity_ = 0;
  highWater_ = 0;
  missCount_ = 0;
  return true;
}

void* MemoryArena::alloc(const size_t bytes, const size_t align) {
  if (base_ == nullptr || bytes == 0) return nullptr;

  // align 必須是 2 的冪(0 與非冪次都當作無效,退回一般 heap)。
  if (align == 0 || (align & (align - 1)) != 0) return nullptr;

  // base_ 來自 new[],已滿足最大基本對齊,所以只需要對齊偏移量本身。
  const size_t misalign = used_ & (align - 1);
  const size_t pad = misalign != 0 ? align - misalign : 0;

  // 減法形式的容量檢查:先確認 pad 放得下,再確認 bytes 放得下。
  // 寫成 used_ + pad + bytes > capacity_ 會在極大的 bytes 上溢位並誤判為放得下。
  if (pad > capacity_ - used_) {
    missCount_++;
    return nullptr;
  }
  const size_t offset = used_ + pad;
  if (bytes > capacity_ - offset) {
    missCount_++;
    return nullptr;
  }

  used_ = offset + bytes;
  if (used_ > highWater_) highWater_ = used_;
  return base_ + offset;
}
