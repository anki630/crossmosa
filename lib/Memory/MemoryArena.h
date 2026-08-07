#pragma once

#include <cstddef>
#include <cstdint>

// 一次配滿、就地重用的記憶體競技場。
//
// 存在理由(2026-08-05,實測 diag89):BLE 連結進來之後,備援池裡有 57,060 空閒
// 但最大連續塊只有 32,768,而字型快取一頁要 38,076 → 每頁都在賭,實測 40 頁掉 826 個字。
// 那是**外部碎片化**,不是總量不足;而近代 OS 對付它的三個標準解法
// (分頁重映射 / 搬移式 GC / 依大小分池)在這顆晶片上分別被硬體、語言、
// ESP-IDF 的 caps 模型封死(理由見 docs/superpowers/specs/2026-08-05-memory-arena-design.md)。
// 剩下的唯一解法就是本類別:**在堆積還乾淨的開機當下把區塊要走,之後永不歸還。**
//
// 配置順序決定終身佈局——先要到的東西,後來者只能繞著它排。
// 本專案已經用過同一招兩次:v55 的 advance table、v85 的 framebuffer。
//
// ⚠️ 這是最佳化,不是相依。`reserve()` 失敗或 `alloc()` 放不下時一律回 nullptr,
//    呼叫端必須退回原本的 `new (std::nothrow)` 路徑。最壞情況 = 改動前的行為。
class MemoryArena {
 public:
  MemoryArena() = default;
  ~MemoryArena();

  // 擁有裸緩衝區,複製會造成雙重釋放 → 編譯期擋掉。
  MemoryArena(const MemoryArena&) = delete;
  MemoryArena& operator=(const MemoryArena&) = delete;
  MemoryArena(MemoryArena&&) = delete;
  MemoryArena& operator=(MemoryArena&&) = delete;

  // 取得底層區塊。開機時呼叫一次(見 main.cpp)。已經 active 時直接回 true
  // (冪等,讓 ensureLoaded() 可以無條件呼叫)。失敗回 false —— 呼叫端照常用一般 heap。
  bool reserve(size_t bytes);

  // 歸還底層區塊(WiFi/OPDS 前,那些路徑需要同一個池的 40-55KB 連續塊)。
  //
  // ⚠️ **只有在 used() == 0 時才會真的釋放**,否則回 false 且不做任何事。
  // 那道守衛是刻意的:競技場配出去的指標分不出自己的出身,若在還有人持有時
  // 釋放,那些指標會變成懸空,而持有者之後會拿它去 `delete[]` → 堆積損毀。
  // 正確順序是「先讓所有使用者放手 → reset() → release()」。
  bool release();

  bool active() const { return base_ != nullptr; }

  // 「釋放全部」= 把游標歸零,O(1)。不歸還底層區塊。
  void reset() { used_ = 0; }

  // 對齊配置。放不下回 nullptr(呼叫端退回一般 heap)。
  // align 必須是 2 的冪;傳 alignof(T) 即可。
  void* alloc(size_t bytes, size_t align = 4);

  bool owns(const void* p) const {
    const auto* q = static_cast<const uint8_t*>(p);
    return base_ != nullptr && q >= base_ && q < base_ + capacity_;
  }

  size_t capacity() const { return capacity_; }
  size_t used() const { return used_; }
  // 診斷:歷來用過的峰值。用來校準 reserve() 的大小——
  // 貼著 capacity 代表設小了,遠低於則代表可以還一些給系統。
  size_t highWater() const { return highWater_; }
  // 診斷:放不下而被迫退回一般 heap 的次數。>0 代表容量不足。
  uint32_t missCount() const { return missCount_; }

 private:
  uint8_t* base_ = nullptr;
  size_t capacity_ = 0;
  size_t used_ = 0;
  size_t highWater_ = 0;
  uint32_t missCount_ = 0;
};

// --- 全域實例 ---
//
// 兩塊都在 main.cpp 的 setup() 裡、framebuffer 之後、BLE 之前取得。
// 順序是設計的一部分:那時候堆積還是乾淨的一大塊,取得之後其他配置只能繞著它排。
//
// ⚠️ 兩塊必須【一起】保留。只保留字型那塊會把記憶體從公共池抽走,
//    而抗鋸齒的平面緩衝正是從同一個池要的 —— 結果是修好掉字、弄壞抗鋸齒。

// 字型點陣(SdCardFont 的 miniBitmap)。**只服務這一個配置**——
// 同一個函式裡的 glyph 表 / interval 表 / kern 表都是幾 KB 的小配置,
// 在碎片化的池裡本來就要得到(alloc_fail 計的只有 bitmap),
// 把它們也放進來只會吃掉邊際。
extern MemoryArena FONT_BITMAP_ARENA;

// 抗鋸齒的兩個灰階平面(純文字頁 strip=80:2 × 80 × 99 = 15,840)。
// 圖片頁的 strip=264(2 × 26,136)刻意不保留——那要 52,272,等於第二張 framebuffer。
extern MemoryArena GRAY_PLANE_ARENA;

// 實測導出的容量(diag89.log,2026-08-05):
//   bitmap_cum 峰值 = 38,076(一頁全部字面合計)→ 40,960 留 2,884 邊際
//   peak_single    = 44,005(單一字面單次嘗試)→ **刻意不覆蓋**,那種頁走既有降級階梯
constexpr size_t kFontBitmapArenaBytes = 40960;
constexpr size_t kGrayPlaneArenaBytes = 15840;
