#include "ZhuyinTxtCursor.h"

#include <algorithm>

#include "ZhuyinFormat.h"
#include "ZhuyinUtf8.h"

namespace zhuyin {

namespace {
bool isCont(uint8_t b) { return (b & 0xC0) == 0x80; }
}  // namespace

bool ZhuyinTxtCursor::byteAt(const size_t i, uint8_t* b) {
  if (i >= src_.size()) return false;
  if (i < bufStart_ || i >= bufStart_ + bufLen_) {
    // 只有往回找邊界時用：讀一個結束在 i 之後 4 個位元組的窗口。decodeAt 從 i 往後最多要看 4 個位元組 ——
    // 窗口剛好結束在 i 的話，往回每走一個碼位都要重讀兩次（往後一次、再往回一次；實測一頁 18 次讀卡）。
    const size_t end = std::min(src_.size(), i + 4);
    const size_t from = end > kTxtReadBuffer ? end - kTxtReadBuffer : 0;
    const size_t n = end - from;
    if (!src_.read(from, buf_, n)) return false;
    bufStart_ = from;
    bufLen_ = n;
  }
  *b = buf_[i - bufStart_];
  return true;
}

bool ZhuyinTxtCursor::decodeAt(const size_t pos, uint32_t* cp, size_t* len, const uint8_t** bytes) {
  const size_t need = std::min<size_t>(4, src_.size() - pos);  // 解一個碼位最多要 4 個位元組（或到檔尾）
  if (pos < bufStart_ || pos + need > bufStart_ + bufLen_) {
    const size_t n = std::min(kTxtReadBuffer, src_.size() - pos);
    if (!src_.read(pos, buf_, n)) return false;
    bufStart_ = pos;
    bufLen_ = n;
  }
  const uint8_t* p = buf_ + (pos - bufStart_);
  const uint8_t* q = p;
  *cp = decodeUtf8(q, buf_ + bufLen_);
  *len = static_cast<size_t>(q - p);
  *bytes = p;
  return true;
}

bool ZhuyinTxtCursor::findRestart(const size_t start, size_t* restart) {
  const size_t limit = start > kTxtMaxLookBehind ? start - kTxtMaxLookBehind : 0;
  size_t p = start;  // p 永遠是碼位邊界
  for (;;) {
    if (p == 0) {
      *restart = 0;
      return true;
    }
    uint8_t b = 0;
    if (!byteAt(p - 1, &b)) return fail(TxtCursorFail::Io);
    if (b == '\n') {  // 段首
      *restart = p;
      return true;
    }
    // 前一個碼位：往回跨過最多 3 個接續位元組找首位元組，從那裡解碼必須合法、而且剛好結束在 p
    size_t q = p - 1;
    for (int k = 0; k < 3 && q > 0 && isCont(b); k++) {
      if (!byteAt(q - 1, &b)) return fail(TxtCursorFail::Io);
      q--;
    }
    uint32_t c = 0;
    size_t len = 0;
    const uint8_t* bytes = nullptr;
    if (!decodeAt(q, &c, &len, &bytes)) return fail(TxtCursorFail::Io);
    if (c != 0xFFFD && q + len == p) {
      // 重新開始的位置離起點不超過 kTxtMaxLookBehind（包括剛好等於）；超過 → 這一頁不標
      if (q < limit) return fail(TxtCursorFail::LookBehind);
      if (!isIdeograph(c) && !isTransparent(c)) {  // 硬邊界：連同它一起送
        *restart = q;
        return true;
      }
      p = q;
      continue;
    }
    // 前面拼不出「剛好結束在 p 的合法碼位」→ 從段首往後的任何解碼，p 前一個碼位一定是 U+FFFD
    // （合法的碼位最多 4 個位元組，上面往回找過了；p 是邊界，所以沒有序列跨過它）。U+FFFD 就是硬邊界 → 從 p 開始。
    *restart = p;
    return true;
  }
}

bool ZhuyinTxtCursor::begin(const size_t start) {
  failed_ = false;
  reason_ = TxtCursorFail::None;
  paragraphEnded_ = false;
  skip_ = 0;
  start_ = start;
  lookBehind_ = 0;
  if (start > src_.size()) return fail(TxtCursorFail::PastEof);
  size_t restart = 0;
  if (!findRestart(start, &restart)) return false;  // findRestart 自己記了原因
  lookBehind_ = start - restart;
  pos_ = restart;
  session_.begin();
  return true;
}

bool ZhuyinTxtCursor::feed() {
  if (pos_ >= src_.size()) {  // 檔尾 ＝ 段落結尾
    paragraphEnded_ = true;
    return session_.finish() || fail(TxtCursorFail::Session);
  }
  uint32_t c = 0;
  size_t len = 0;
  const uint8_t* bytes = nullptr;
  if (!decodeAt(pos_, &c, &len, &bytes)) return fail(TxtCursorFail::Io);
  if (c == '\n') {
    pos_ += len;
    paragraphEnded_ = true;
    return session_.finish() || fail(TxtCursorFail::Session);
  }
  if (!session_.addWord(reinterpret_cast<const char*>(bytes), len)) return fail(TxtCursorFail::Session);
  if (isIdeograph(c) && pos_ < start_) skip_++;  // session 佇列裝每一個漢字：起點之前的那幾個要丟掉
  pos_ += len;
  return true;
}

bool ZhuyinTxtCursor::next(const uint32_t cp, uint16_t* out) {
  if (failed_) return false;
  for (;;) {
    while (skip_ > 0 && session_.available() > 0) {
      uint32_t c = 0;
      uint16_t o = 0;
      if (!session_.takeAny(&c, &o)) return fail(TxtCursorFail::Session);
      skip_--;
    }
    if (skip_ == 0 && session_.available() > 0) {
      if (!session_.take(cp, out)) return fail(session_.degraded() ? TxtCursorFail::Session : TxtCursorFail::Mismatch);
      return true;
    }
    if (paragraphEnded_) {
      if (!session_.done()) {  // 佇列滿了分次送：取出之後再送一次
        if (!session_.finish()) return fail(TxtCursorFail::Session);
        continue;
      }
      if (pos_ >= src_.size()) return fail(TxtCursorFail::PastEof);  // 檔尾了還要字 → 排版交來的跟檔案對不上
      session_.begin();                                              // 下一段
      paragraphEnded_ = false;
    }
    if (!feed()) return false;  // feed 自己記了原因
  }
}

}  // namespace zhuyin
