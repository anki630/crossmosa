#pragma once
// 注音 session：一個段落一個（P2 設計第 2 節＋第 6 節 codex 修訂 4–7）。
// 把排版收到的字詞（顯示字串的 UTF-8）變成解析器的碼位串流，送出已定案的讀音，
// 並照「漢字的順序」交給取出行的那一端（每個漢字都核對碼位）。
//
// 規則（都只由文字本身決定 → 任何切法、從哪個硬邊界開始，結果都一樣）：
//   - 透明的格式字元（軟連字號、零寬、BOM、變體選擇器…）解析器看不到，也不是硬邊界。
//   - 出版社標注：帶 ruby 的字詞（addWord 的 annotated）、後面跟變體選擇器的漢字 → 不猜（輸出 0 ＝ 不換）。
//   - v342（B 路線）：後面跟 bpmfvs 選擇符號（U+E01E0–E01EF）的漢字 → 照書：VS17 ＝ 不標注音（0）、
//     VS18 起 ＝ 第 2、3…個讀音（字型的讀音表順序；字型沒有那個讀音 → 0）。ruby 優先（出版社自己標了就不再加注音）。
//   - v342：預先標注的章節（setDocumentAnnotated，章節 <head> 的 zhuyin-ivs 標記）→ 沒有選擇符號的破音字用第一個讀音，
//     不採用解析器的判斷（連超長一不串也照書，因為變調已經寫在選擇符號裡）。
//   - 超長一不串：連續超過 kMaxYiBuRun 個「一／不」→ 整串不標；串照樣送進解析器（強制送出），
//     所以串後面的字讀音與整段解析完全相同。
//   - 其他漢字：與 ZhuyinResolver::resolve 整段解析逐字相同。
// 不配記憶體：暫存區（解析窗口與佇列）由呼叫端給，跟著引擎一起配、每段重用。

#include <cstddef>
#include <cstdint>

#include "ZhuyinResolver.h"

namespace zhuyin {

constexpr uint32_t kMaxYiBuRun = 64;

// 透明的格式字元：不是硬邊界，解析器看不到它們（P2 設計 2b）
inline bool isTransparent(uint32_t cp) {
  return cp == 0x00AD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF ||
         (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}
inline bool isVariationSelector(uint32_t cp) {
  return (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

// 引擎載入時配好（每塊 ≤ 2 KB）、每段重用。
struct SessionScratch {
  uint32_t* cps = nullptr;  // 解析窗口：還沒送出的＋新進來的碼位
  uint16_t* out = nullptr;
  uint8_t* covered = nullptr;
  uint8_t* flags = nullptr;   // bit0 出版社標注、bit1 超長一不串
  size_t windowCap = 0;       // ≥ kMaxYiBuRun + kCommitLag + 每次最多送進來的量
  uint32_t* queue = nullptr;  // (碼位 << 16) | 輸出，先進先出環形
  size_t queueCap = 0;
  // 暫存只有一份，而一個段落的排版可能跨好幾個建置 tick：中途別的排版（設定頁預覽、另一章）拿去用了，
  // 被暫停的那一段回來時窗口與佇列都已經不是它的 → 每次 begin() 領一個新號碼，之後每一步都核對（不符 → 降級失敗）。
  uint32_t owner = 0;
  uint32_t nextOwner = 0;
};

class ZhuyinSession {
 public:
  ZhuyinSession(ZhuyinData& data, SessionScratch& scratch) : d_(data), s_(scratch), r_(data) {}
  ZhuyinSession(const ZhuyinSession&) = delete;
  ZhuyinSession& operator=(const ZhuyinSession&) = delete;

  void begin();  // 新段落（狀態歸零；預先標注的旗標不歸零 —— 它屬於章節，不屬於段落）
  // v342：這一段屬於預先標注的章節（bpmfvs）→ 沒有選擇符號的破音字 ＝ 第一個讀音。begin() 之前或之後呼叫都可以。
  void setDocumentAnnotated(bool on) { docAnnotated_ = on; }
  // 收一個字詞。annotated ＝ 出版社已標注（ruby）→ 裡面的漢字不猜。失敗 → 之後都不標（failed()）。
  bool addWord(const char* utf8, size_t len, bool annotated = false);
  // 段落結束：送出佇列放得下的部分（背壓）；還沒送完（done() == false）→ 呼叫端取出行之後再呼叫一次。
  bool finish();
  bool done() const { return n_ == 0; }

  size_t available() const { return queueLen_; }  // 佇列裡可以拿的漢字數
  // 下一個漢字的輸出；cp 必須跟佇列裡的一樣（核對）。*out ＝ 0 ＝ 不換。不符或沒得拿 → false，之後都不標。
  bool take(uint32_t cp, uint16_t* out);
  // 取出下一個漢字、不核對（TXT 丟掉頁起點之前的上下文用）：*cp ＝ 佇列裡的碼位。沒得拿 → false，之後都不標。
  bool takeAny(uint32_t* cp, uint16_t* out);

  bool failed() const { return failed_; }
  bool degraded() const { return degraded_; }  // 失敗的原因是讀卡或資源（章節身分要寫「沒注音」）

 private:
  bool push(uint32_t cp, uint8_t flags);
  bool process(bool final);
  uint16_t outFor(uint32_t cp, uint8_t flags, uint16_t resolved) const;  // 送出時這個漢字的字形（見檔頭的規則）
  void fail(bool degraded) {
    failed_ = true;
    degraded_ = degraded_ || degraded;
  }

  ZhuyinData& d_;
  SessionScratch& s_;
  ZhuyinResolver r_;
  size_t n_ = 0;         // 窗口裡的碼位數
  size_t runStart_ = 0;  // 窗口尾端那段連續「一／不」的起點（沒有 → runLen_ == 0）
  size_t runLen_ = 0;
  bool runLong_ = false;  // 目前這段「一／不」已經超過上限
  size_t queueHead_ = 0, queueLen_ = 0;
  bool failed_ = false, degraded_ = false;
  bool docAnnotated_ = false;  // v342：預先標注的章節
  uint32_t claim_ = 0;         // begin() 領到的暫存號碼
  bool owns() {
    if (s_.owner == claim_ && claim_ != 0) return true;
    fail(true);  // 暫存被別的段落拿走了（資源問題，不是內容）→ 章節身分要寫「沒注音」
    return false;
  }
};

}  // namespace zhuyin
