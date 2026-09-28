#pragma once
// 破音字解析器：給一段原字的碼位，算出每個位置要畫的字形（0 ＝ 基字，否則 PUA）。
// 與工作區 fonts-src/zhuyin/zy_block.py 的 Resolver 逐步對應；test/zhuyin_resolver 拿 Python 的答案逐字比對。
//
// 固定四步（設計 v3 §3.2）：
//   1. 辭典：左到右最長詞優先。只有「前兩字」在雙字索引裡才讀卡查詞組。
//   2. 沒被詞涵蓋的位置：引擎預設讀音表（沒有就是字型的預設讀音）。
//   3. 規則（只在沒被涵蓋的位置）：依表的順序，第一條符合的勝出。
//   3b.（v2）還是 0 的破音字 → 預設讀音的私用區字形（破音字的原碼位字形不帶注音，破音字一律輸出私用區）。
//   4. 一、不變調（只在沒被涵蓋的位置）：由右到左，看右鄰「最終」字形的聲調（右鄰不是漢字 → 本調）。
//
// 分批送出（feed）：
//   - 這一批必須以「上一批沒送出的部分」開頭（檢查長度與雜湊，不符就回 false —— 呼叫端的錯不能默默變成錯字）。
//   - 不是最後一批時，尾端 kCommitLag 個位置不送出；送出點前面緊鄰、沒被詞涵蓋的一／不也往回退（一串可以任意長）。
//   - 跨過送出點的詞，把後半的輸出帶到下一批的開頭；規則要看的前兩個字也帶過去。
//   → 任何切法的結果都與整段一次解析相同（測試逐切點驗證，含任意長的一不串）。
// 不配記憶體：out／covered 由呼叫端給，長度至少 n。
// 失敗（沒載入、參數錯、讀卡失敗、接續不符、輸出不屬於那個字）回
// false，分批狀態（前文、帶過去的詞、待接續的部分）不變； 診斷計數與讀卡快取可能已經變動。⭐
// 每一批交出去之前，逐位置再查一次「輸出屬於那個字」（v2 不變量的最後一道）。

#include <cstddef>
#include <cstdint>

#include "ZhuyinData.h"

namespace zhuyin {

class ZhuyinResolver {
 public:
  explicit ZhuyinResolver(ZhuyinData& data) : d_(data) {}
  ZhuyinResolver(const ZhuyinResolver&) = delete;
  ZhuyinResolver& operator=(const ZhuyinResolver&) = delete;

  void reset();

  // 整段一次解析（不帶、也不改分批狀態）。
  bool resolve(const uint32_t* cps, size_t n, uint16_t* out, uint8_t* covered);

  // 分批：cps ＝ 上一批沒送出的部分＋新進來的字。*commit ＝ 這次可以送出的前綴長度（out[0..commit) 已定案）。
  // 下一批要從 cps[*commit] 開始接。＝ resolveWindow ＋ commit(safe)。
  bool feed(const uint32_t* cps, size_t n, bool final, uint16_t* out, uint8_t* covered, size_t* commit);

  // 兩段式（排版只能在行的邊界送出，P2 設計 2）：
  //   resolveWindow：解析這個窗口（接續規則同 feed），*safe ＝ 可以安全送出的最長前綴。**不改分批狀態**。
  //   commit：送出前 k 個（0 ≤ k ≤ safe）；cps／out／covered 必須是上一次 resolveWindow 的同一段（長度＋雜湊核對）。
  //   任何 k ≤ safe 都正確：[0, k) 已定案；k 落在詞中間時詞尾帶過去；k−1 若是一、不，它的右鄰在 [0, safe) 內已定案，
  //   或 safe 本身已經退過一不串（測試：每一步隨機選 k，結果＝整段一次解析）。
  bool resolveWindow(const uint32_t* cps, size_t n, bool final, uint16_t* out, uint8_t* covered, size_t* safe);
  bool commit(const uint32_t* cps, size_t n, const uint16_t* out, const uint8_t* covered, size_t k);
  // 強制送出（只給 ZhuyinSession 處理「超長一不串」用）：k 可以超過 safe，但不能超過「分詞已定案」的界線
  // （非最後一批：n − kCommitLag；最後一批：n）。[safe, k) 那段的【變調】還沒定案 —— 呼叫端必須把它們當成不標；
  // 分詞狀態（跨界詞尾、前兩字）照樣正確，所以 k 之後的字讀音與整段解析相同。*limit ＝ 這個窗口可強制送出的上限。
  size_t forcedCommitLimit() const { return windowValid_ ? windowForcedLimit_ : 0; }
  bool commitForced(const uint32_t* cps, size_t n, const uint16_t* out, const uint8_t* covered, size_t k);

  uint32_t lookups() const { return lookups_; }

  // 跑資料區塊裡的自我測試節：每一句用新的解析器整段解析，逐位置比對參考模型的答案；
  // 比 kCommitLag 長的句子再用「兩批」在每一個切點餵一次（短句的分批不會提早送出，測不到分批）。
  // 全部相同 → 資料切成「可用」並回 true；否則資料狀態清空、回 false，*failedCase ＝ 第一句不同（或讀卡失敗）的序號。
  // ⚠️ 它是「開機健康檢查」：證明這台機器上編出來的解析器在這些句子上＝參考實作，不是全域等價的證明
  //    （全域等價在電腦端：zy_block_check 的逐字比對與端到端、C++ 測試）。
  //    堆疊峰值約 1.9 KB（-fstack-usage 實測框大小加總，見 ZhuyinData::forEachSelfTest）→ 要在堆疊夠大的任務裡跑。
  static bool selfTest(ZhuyinData& data, uint16_t* failedCase = nullptr);
  // 載入＋自我測試。只有兩者都過才可用；任何一步失敗，資料都是清空的（Arena 由呼叫端丟掉）。
  static LoadStatus open(ZhuyinData& data, BlockSource& src, Arena& arena, uint16_t* failedCase = nullptr);

 private:
  struct Carry {
    uint16_t out[kMaxWord];
    uint8_t len;
  };
  bool run(const uint32_t* cps, size_t n, const Carry& carry, const uint32_t* prev, uint8_t prevLen, uint16_t* out,
           uint8_t* covered);
  static uint64_t hashCps(const uint32_t* cps, size_t n);

  ZhuyinData& d_;
  uint32_t prev_[2] = {0, 0};  // 這一批之前的兩個字：prev_[1] 緊鄰、prev_[0] 更前面
  uint8_t prevLen_ = 0;
  Carry carry_ = {{}, 0};  // 上一批跨界詞的後半輸出
  size_t pendingLen_ = 0;  // 上一批沒送出的長度、前 kPendingExact 個碼位與 64 位元雜湊：這一批必須以它開頭
  uint64_t pendingHash_ = 0;
  uint32_t pendingHead_[kPendingExact] = {};
  // 上一次 resolveWindow（commit 要核對的就是它）
  bool windowValid_ = false;
  size_t windowN_ = 0, windowSafe_ = 0, windowForcedLimit_ = 0;
  bool commitImpl(const uint32_t* cps, size_t n, const uint16_t* out, const uint8_t* covered, size_t k, size_t limit);
  uint64_t windowHash_ = 0;
  uint32_t lookups_ = 0;
};

}  // namespace zhuyin
