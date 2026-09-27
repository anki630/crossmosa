#pragma once
// 引擎的全部記憶體與狀態（P2 設計第 4 節；codex 修訂 3、12、13）：資料（ZhuyinData）與它的 arena（每塊 ≤ 2 KB）、
// session 的暫存（窗口＋佇列）、登記處的登記。一次配好，跟著閱讀字型走（SdCardFont 擁有）。
//
// 解構順序：登記先拿掉 → 暫存 → 資料 → arena（解構子本體先拿掉登記、放暫存；成員照宣告的反序）。
// 執行緒：open／解構只在沒有任何排版、繪製、序列化進行時（韌體：RenderLock 底下），跟字型的載入與卸載同一個時機。

#include <cstddef>
#include <cstdint>

#include "ZhuyinActive.h"
#include "ZhuyinData.h"
#include "ZhuyinSession.h"

namespace zhuyin {

class ZhuyinEngine {
 public:
  static constexpr size_t kWindowCap = 128;    // session 的解析窗口（碼位）
  static constexpr size_t kQueueCap = 512;     // 佇列（漢字）：× 4 B ＝ 2 KB
  static constexpr size_t kMaxArenaBlocks = 48;
  static constexpr size_t kMaxBlockBytes = 2048;  // 不變量 4：每塊配置 ≤ 2 KB

  ZhuyinEngine() = default;
  ~ZhuyinEngine();
  ZhuyinEngine(const ZhuyinEngine&) = delete;
  ZhuyinEngine& operator=(const ZhuyinEngine&) = delete;

  // 載入分兩段（＝ ZhuyinResolver::open：先結構載入、再自我測試）；兩段之間給呼叫端一個時機：
  //   afterStructural：結構載入成功之後、自我測試之前 —— 韌體在這裡記時間、換讀取緩衝（常駐資料到這時已經配完，
  //   見 ZhuyinBufferedSource.h）。結構載入失敗就不呼叫。
  struct OpenHooks {
    void* ctx = nullptr;
    void (*afterStructural)(void* ctx) = nullptr;
  };
  // 韌體分兩步（codex v339 複查）：
  //   prepare ＝ 配暫存 → 載入＋自我測試（在呼叫端的堆疊上跑，約 1.9 KB）。成功也【不】登記：別的 task 還看不到它。
  //   publish ＝ 登記（fontId ＝ 擁有它的閱讀字型）＝ 公開。一公開，別的 task 就可能經過讀取來源查詢 ——
  //   所以呼叫端在兩步之間做完所有會反悔的事（配對檢查、記憶體檢查）與收拾（還掉借來的讀取緩衝），公開之後不再反悔。
  // 失敗 → 回傳原因，引擎是空的、沒有登記（可以直接丟掉）。src 要活得比引擎久。
  LoadStatus prepare(BlockSource& src, uint16_t* failedCase = nullptr, const OpenHooks* hooks = nullptr);
  // 只有「最近一次 prepare 完整成功（自我測試也過了）」才登記，回傳 true；否則什麼都不做、回傳 false
  // （prepare 失敗、還在 prepare 裡面〔例如 hook 裡〕、或資料之後被清掉，都不登記）。
  bool publish(int fontId);
  // prepare＋publish（電腦端測試與不需要中間檢查的呼叫端）
  LoadStatus open(BlockSource& src, int fontId, uint16_t* failedCase = nullptr, const OpenHooks* hooks = nullptr);
  // 已登記、而且登記處上的就是它、而且資料可用
  bool ready() const;

  ZhuyinData& data() { return data_; }
  SessionScratch& scratch() { return scratch_; }
  // 排版取出一行時組替換清單用（一行最多 kMaxLineSwaps 筆；ParsedText 在 RenderLock 底下同步用完）
  Swap* lineSwaps() { return lineSwaps_; }
  static constexpr size_t kLineSwapsCap = kMaxLineSwaps;
  size_t residentBytes() const { return arena_.bytes + scratchBytes_; }
  size_t arenaBlocks() const { return arena_.count; }

 private:
  struct BlockArena final : Arena {
    void* blocks[kMaxArenaBlocks] = {};
    size_t count = 0;
    size_t bytes = 0;
    void* alloc(size_t n, size_t align) override;
    void release();
    ~BlockArena() override { release(); }
  };
  bool allocScratch();
  void freeScratch();

  BlockArena arena_;
  ZhuyinData data_;
  bool prepared_ = false;         // 最近一次 prepare 完整成功（publish 的前提）
  uint32_t preparedSerial_ = 0;   // 那時資料的狀態序號：之後資料被就地換過（例如經過 data() 重新載入）就不算（codex v339 第四輪）
  uint32_t* cps_ = nullptr;
  uint16_t* out_ = nullptr;
  uint8_t* covered_ = nullptr;
  uint8_t* flags_ = nullptr;
  uint32_t* queue_ = nullptr;
  Swap* lineSwaps_ = nullptr;
  size_t scratchBytes_ = 0;
  SessionScratch scratch_;
  EngineRegistration registration_;  // 最後宣告 → 成員裡最先解構（解構子本體也先拿掉它）
};

}  // namespace zhuyin
