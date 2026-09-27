#pragma once
// 引擎載入期間的讀取緩衝（v339）。
//
// 結構載入＋自我測試對區塊做約 520 次小讀取（CRC 掃一遍、各節各掃一遍，都是 256 B 一次；自我測試的查詢約 97 次，
// 在字詞群組之間隨機跳）。區塊在字型檔的尾端，卡上每一次讀都要付一次 seek＋讀取 → 實機 diag338 `ZY load ms=2035`，
// 進書、醒來回書每次都付。這一層包在底下的讀取來源外面，只在載入期間借一塊緩衝：
//   - 緩衝 ≥ 區塊：整個區塊一次讀進來，之後全部從緩衝拿；
//   - 緩衝 < 區塊：當窗口用，同一個窗口裡的讀取從緩衝拿（循序讀的時候有用；隨機跳讀接不住）；
//   - 沒有緩衝：直接轉給底下的來源（v338 的行為：慢，但一樣對）。
// 緩衝由呼叫端給、呼叫端擁有 —— 這一層自己不配記憶體，因為【什麼時候、從哪裡借】才是重點：
//   結構載入會陸續配出常駐資料。這時候若借一塊堆積、載入完才放，放掉的地方卡在常駐資料【前面】，
//   整段閱讀的最大連續塊都被切小（硬限制第 6 條「成長搬遷在池中間挖永久空洞」，同一個形狀）。
//   → 韌體的用法：結構載入用堆疊上的窗口（不碰堆積）；常駐資料配完之後，自我測試才借堆積讀整塊。
//     在 v338 量到的拓樸下（p2 整池空著、常駐小塊落在 p2 開頭），那一塊配在常駐資料後面、放掉就接回去 ——
//     這是那個拓樸下的預期，不是 TLSF 的保證（codex v339 複查 4）；實機以 `MEM zy-load` 的 p2 最大塊對照 v338 驗證。
// ⭐ 緩衝只是加速，不能變成新的失敗點：任何一次經過緩衝的讀卡失敗（整塊或窗口）→ 這一次改直接讀、這一趟載入剩下的
//   都直接讀（之後的 use() 不再借緩衝，到 release() 為止；v338 的行為）。例：卡對大塊讀取不穩、小讀取正常 → 照樣載得起來
//   （codex v339 複查 3，第二輪：退回要「黏住」，不能下一次 use() 又去試大讀取）。
// release() 之後（以及第一次 use 之前）直接轉給底下的來源：排版時的查詢不經過緩衝，也不再記數 ——
//   引擎公開之後可能有別的 task 經過這裡讀，計數只在載入期間（單一 task）動（codex v339 複查 1）。
// ⚠️ 資料（ZhuyinData）會留著這個物件的指標做執行期的查詢，所以它要跟引擎活得一樣久（不是載入用完就丟的暫時物件）。
// ⚠️ 引擎在 release() 之後才能公開（ZhuyinEngine::prepare → release → publish）：公開之後別的 task 就可能經過這裡讀，
//   那時緩衝必須已經還給呼叫端 —— 否則呼叫端 delete 緩衝的同時，別的 task 還在從它 memcpy。

#include <cstdint>

#include "ZhuyinData.h"

namespace zhuyin {

class BufferedSource final : public BlockSource {
 public:
  enum class Mode : uint8_t { Direct = 0, Window = 1, Whole = 2 };

  explicit BufferedSource(BlockSource& raw) : raw_(raw) {}
  BufferedSource(const BufferedSource&) = delete;
  BufferedSource& operator=(const BufferedSource&) = delete;

  // 載入期間：借 buf[0, cap) 當緩衝，直到下一次 use()／release() 為止（這段期間呼叫端不能動它），並開始記數。
  // cap ≥ 區塊大小 → 馬上整塊讀進來（讀不到 → 直接讀）。buf == nullptr 或 cap == 0 → 直接讀（照樣記數）。
  // 這一趟載入已經有過緩衝讀卡失敗（degraded）→ 不借，直接讀。
  void use(uint8_t* buf, uint32_t cap);
  // 載入結束：還掉緩衝、停止記數（計數保留原值供讀取）、清掉 degraded（下一趟重新來）。之後直接讀。
  void release();
  bool degraded() const { return degraded_; }  // 這一趟載入有過經過緩衝的讀卡失敗（之後都直接讀）

  uint32_t size() const override { return raw_.size(); }
  bool read(uint32_t off, void* dst, uint32_t len) override;

  Mode mode() const { return mode_; }
  // 載入期間的累計（use 到 release；不歸零 —— 呼叫端自己在兩段之間取差）
  uint32_t calls() const { return calls_; }        // 收到幾次讀取
  uint32_t rawReads() const { return rawReads_; }  // 其中真的轉給底下來源（讀卡）幾次（含失敗的）
  uint32_t rawBytes() const { return rawBytes_; }  // 讀卡【成功】讀到的位元組
  uint32_t rawFails() const { return rawFails_; }  // 底層讀取失敗的【嘗試】次數（窗口失敗＋直接重試 ＝ 最多兩次）

 private:
  bool rawRead(uint32_t off, void* dst, uint32_t len);

  BlockSource& raw_;
  uint8_t* buf_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t bufStart_ = 0;
  uint32_t bufLen_ = 0;  // 緩衝裡有效的位元組（0 ＝ 空的）
  Mode mode_ = Mode::Direct;
  bool counting_ = false;
  bool degraded_ = false;
  uint32_t calls_ = 0;
  uint32_t rawReads_ = 0;
  uint32_t rawBytes_ = 0;
  uint32_t rawFails_ = 0;
};

}  // namespace zhuyin
