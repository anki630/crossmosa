#include "ZhuyinEngine.h"

#include <new>

#include "ZhuyinResolver.h"

namespace zhuyin {

void* ZhuyinEngine::BlockArena::alloc(const size_t n, const size_t align) {
  if (n == 0 || n > kMaxBlockBytes || count >= kMaxArenaBlocks || align == 0 || align > 8) return nullptr;
  auto* p = new (std::nothrow) uint8_t[n];
  if (!p) return nullptr;
  if (reinterpret_cast<uintptr_t>(p) % align != 0) {  // 配置器的對齊不夠（不該發生）→ 當成配不到
    delete[] p;
    return nullptr;
  }
  blocks[count++] = p;
  bytes += n;
  return p;
}

void ZhuyinEngine::BlockArena::release() {
  for (size_t i = 0; i < count; i++) delete[] static_cast<uint8_t*>(blocks[i]);
  count = 0;
  bytes = 0;
}

bool ZhuyinEngine::allocScratch() {
  cps_ = new (std::nothrow) uint32_t[kWindowCap];
  out_ = new (std::nothrow) uint16_t[kWindowCap];
  covered_ = new (std::nothrow) uint8_t[kWindowCap];
  flags_ = new (std::nothrow) uint8_t[kWindowCap];
  queue_ = new (std::nothrow) uint32_t[kQueueCap];
  lineSwaps_ = new (std::nothrow) Swap[kLineSwapsCap];
  if (!cps_ || !out_ || !covered_ || !flags_ || !queue_ || !lineSwaps_) {
    freeScratch();
    return false;
  }
  scratch_ = SessionScratch{};
  scratch_.cps = cps_;
  scratch_.out = out_;
  scratch_.covered = covered_;
  scratch_.flags = flags_;
  scratch_.windowCap = kWindowCap;
  scratch_.queue = queue_;
  scratch_.queueCap = kQueueCap;
  scratchBytes_ = kWindowCap * (sizeof(uint32_t) + sizeof(uint16_t) + 2) + kQueueCap * sizeof(uint32_t) +
                  kLineSwapsCap * sizeof(Swap);
  return true;
}

void ZhuyinEngine::freeScratch() {
  delete[] cps_;
  delete[] out_;
  delete[] covered_;
  delete[] flags_;
  delete[] queue_;
  delete[] lineSwaps_;
  lineSwaps_ = nullptr;
  cps_ = nullptr;
  out_ = nullptr;
  covered_ = nullptr;
  flags_ = nullptr;
  queue_ = nullptr;
  scratch_ = SessionScratch{};
  scratchBytes_ = 0;
}

LoadStatus ZhuyinEngine::prepare(BlockSource& src, uint16_t* failedCase, const OpenHooks* hooks) {
  registration_.release();
  prepared_ = false;
  data_.unload();  // 資料的指標都指進 arena：放掉 arena
                   // 之前先清空（否則下面配暫存失敗就提早回去，留下「已載入」的懸空資料）
  freeScratch();
  arena_.release();
  if (!allocScratch()) return LoadStatus::NoMemory;
  // ＝ ZhuyinResolver::open，拆成兩段只為了在中間給呼叫端一個時機。任一段失敗，資料狀態都已清空。
  LoadStatus st = data_.load(src, arena_);
  if (st == LoadStatus::Ok) {
    if (hooks && hooks->afterStructural) hooks->afterStructural(hooks->ctx);
    if (!ZhuyinResolver::selfTest(data_, failedCase)) st = LoadStatus::BadSelfTest;
  }
  if (st != LoadStatus::Ok) {
    data_.unload();  // load／selfTest 失敗時已經清過；這裡再清一次，不依賴它們的實作
    freeScratch();
    arena_.release();
    return st;
  }
  preparedSerial_ = data_.stateSerial();
  prepared_ = true;
  return st;
}

bool ZhuyinEngine::publish(const int fontId) {
  // 沒有完整成功的 prepare：不公開。prepared_ 另外擋「有人經過 data() 從外面把資料載好」—— 那時引擎沒有暫存，公開出去
  // session 會用到空指標
  if (!prepared_ || !data_.loaded() || data_.stateSerial() != preparedSerial_) return false;
  registration_.attach(&data_, fontId, this);
  return true;
}

LoadStatus ZhuyinEngine::open(BlockSource& src, const int fontId, uint16_t* failedCase, const OpenHooks* hooks) {
  const LoadStatus st = prepare(src, failedCase, hooks);
  if (st == LoadStatus::Ok) publish(fontId);
  return st;
}

bool ZhuyinEngine::ready() const {
  const ActiveEngine e = activeEngine();
  return e.data == &data_ && engineUsable(e);
}

ZhuyinEngine::~ZhuyinEngine() {
  registration_.release();  // 先從登記處拿掉，才放暫存與資料
  freeScratch();
}

}  // namespace zhuyin
