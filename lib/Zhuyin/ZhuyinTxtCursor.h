#pragma once
// TXT 的注音讀音來源（P2 設計第 2c 節；codex 修訂 7）。
//
// TXT 沒有章節快取：每一頁都是從那一頁的起點位元組單獨排的（往後翻、往前翻、跳頁都一樣），
// 而讀音必須跟「整段一次解析」完全相同（不變量 3）→ 解析不能從頁的起點開始。這個游標是
// 【檔案位元組＋起點】的純函式：不論這一頁是怎麼到的，給的讀音都一樣。
//
// 用法：begin(這一頁的起點) → 排版每取出一行，照順序對那一行的每個漢字呼叫 next(碼位, &讀音)。
//   - 往前：從起點往前找最近的硬邊界 —— 不是漢字、也不是透明字元的碼位（連同它一起送），或段首（'\n' 之後、檔頭）——
//     從那裡送進 session；起點之前的漢字的讀音丟掉。
//   - 往後：需要時自己往後讀檔（送出的延遲、一不串），直到這個漢字定案；段落結尾（'\n'、檔尾）一次送完、下一段重新開始。
//   - 每個漢字都核對碼位：排版交來的字跟檔案裡的字對不上 → 之後都不標。
// 失敗（讀卡、往前 kTxtMaxLookBehind 位元組內找不到硬邊界、核對不符、session 失敗、頁要的漢字比檔案多）→
// 這一頁之後都不標（v2 字型下＝破音字不標）。
// ⚠️ 確定的是【畫出來的讀音】：每一個字的讀音只由檔案位元組決定（等於整段一次解析），跟這一頁怎麼到的無關。
//    【畫不畫得出來】則還看資源 —— 記憶體、讀卡、堆疊、引擎在不在（codex 整合複查 B3）。資源不夠只會少標，不會標錯；
//    只有「往前 4 KB 找不到硬邊界」這一種失敗是純由檔案內容決定的。
//
// 為什麼從「那個硬邊界的位置」開始解碼，結果跟從段首解碼一樣：嚴格解碼（ZhuyinUtf8.h）遇到不合法的位元組只吃一個，
// 而一個合法序列的首位元組不可能被前面的序列當成接續位元組吃掉 → 從更前面任何一個邊界往後解碼，都會在這個位置切開。
// 往回遇到拼不成合法碼位的位元組 → 那裡一定解成 U+FFFD（也是硬邊界）→ 就從目前的邊界開始。

#include <cstddef>
#include <cstdint>

#include "ZhuyinSession.h"

namespace zhuyin {

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual size_t size() const = 0;
  virtual bool read(size_t offset, uint8_t* dst, size_t len) = 0;
};

constexpr size_t kTxtMaxLookBehind = 4096;  // 往前找硬邊界最遠幾個位元組（中文書的標點間隔遠小於此）
constexpr size_t kTxtReadBuffer = 256;

// 游標為什麼停了（證人；先到先記，之後不覆寫）。
enum class TxtCursorFail : uint8_t {
  None = 0,
  NotBegun,    // begin() 還沒呼叫
  Io,          // 讀卡失敗
  LookBehind,  // 往前 kTxtMaxLookBehind 位元組內找不到硬邊界
  Mismatch,    // 排版交來的漢字跟檔案對不上 —— 應該永遠不發生；發生了＝「排版交出每一個漢字」的推理有洞
  Session,     // session 失敗（暫存被別人拿走、資料讀不到、佇列滿）
  PastEof,     // 頁要的漢字比檔案多
  Abandoned,   // 排版端放棄（有一段停止標注、或餵進了不安全的位元組）：之後的漢字不會照順序交來
};

class ZhuyinTxtCursor {
 public:
  ZhuyinTxtCursor(ZhuyinData& data, SessionScratch& scratch, ByteSource& src) : session_(data, scratch), src_(src) {}
  ZhuyinTxtCursor(const ZhuyinTxtCursor&) = delete;
  ZhuyinTxtCursor& operator=(const ZhuyinTxtCursor&) = delete;

  // start 必須是碼位邊界（頁的起點都是）。失敗 → false，next() 之後都回 false。
  bool begin(size_t start);
  // 起點之後、下一個漢字的讀音（*out ＝ 0 ＝ 不換）。cp 必須跟檔案裡那個字相同。
  bool next(uint32_t cp, uint16_t* out);
  // ⚠️ 核對只比碼位，所以「每一個漢字都照順序交來」是呼叫端的責任：漏交一個字，後面剛好是同一個字時核對照樣通過，
  //    讀音就錯位了。排版端只要有一個字不會交來（某段停止標注、某塊位元組不安全），就必須先呼叫這個 → 之後都不標。
  //    不碰 session 與暫存（引擎可能已經不在了）。
  void abandon() { fail(TxtCursorFail::Abandoned); }
  bool failed() const { return failed_; }
  TxtCursorFail failReason() const { return reason_; }
  size_t lookBehind() const { return lookBehind_; }  // 證人：這一頁往前讀了幾個位元組的上下文

 private:
  bool findRestart(size_t start, size_t* restart);
  bool byteAt(size_t i, uint8_t* b);
  bool decodeAt(size_t pos, uint32_t* cp, size_t* len, const uint8_t** bytes);
  bool feed();
  bool fail(TxtCursorFail why) {
    if (!failed_ || reason_ == TxtCursorFail::NotBegun) reason_ = why;
    failed_ = true;
    return false;
  }

  ZhuyinSession session_;
  ByteSource& src_;
  uint8_t buf_[kTxtReadBuffer] = {};
  size_t bufStart_ = 0, bufLen_ = 0;
  size_t start_ = 0, pos_ = 0, lookBehind_ = 0;
  uint32_t skip_ = 0;  // 起點之前送進去、讀音要丟掉的漢字數
  bool paragraphEnded_ = false;
  bool failed_ = true;  // begin() 之前不能用
  TxtCursorFail reason_ = TxtCursorFail::NotBegun;
};

}  // namespace zhuyin
