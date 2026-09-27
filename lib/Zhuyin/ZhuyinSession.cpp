#include "ZhuyinSession.h"

#include <cstring>

#include "ZhuyinActive.h"
#include "ZhuyinUtf8.h"

namespace zhuyin {

namespace {

constexpr uint32_t kYi = 0x4E00;
constexpr uint32_t kBu = 0x4E0D;
constexpr uint8_t kFlagAnnotated = 1;  // 出版社已標注（ruby、bpmfvs 以外的變體選擇器）→ 不猜
constexpr uint8_t kFlagLongRun = 2;    // 超長一不串的一員 → 不標
// v342：bpmfvs 選擇符號（VS17 ＝ U+E01E0 … VS32 ＝ U+E01EF）。flags 的 bit2–6 存「第幾個」＋1（0 ＝ 沒有）。
constexpr uint32_t kVs17 = 0xE01E0;
constexpr uint32_t kVs32 = 0xE01EF;
constexpr uint8_t kSelShift = 2;
constexpr uint8_t kSelMask = 0x1F << kSelShift;

}  // namespace

void ZhuyinSession::begin() {
  claim_ = ++s_.nextOwner;
  if (claim_ == 0) claim_ = ++s_.nextOwner;  // 0 保留給「沒有人」
  s_.owner = claim_;
  r_.reset();
  n_ = 0;
  runStart_ = runLen_ = 0;
  runLong_ = false;
  queueHead_ = queueLen_ = 0;
  failed_ = degraded_ = false;
}

bool ZhuyinSession::addWord(const char* utf8, size_t len, bool annotated) {
  if (failed_ || !owns()) return false;
  if (!resolverStackOk()) {  // 進解析器之前：這個任務的堆疊還夠嗎（不夠 → 不標，不是爆掉）
    fail(true);
    return false;
  }
  if (!s_.cps || !s_.out || !s_.covered || !s_.flags || !s_.queue || s_.windowCap < kMaxYiBuRun + kCommitLag + 2 ||
      s_.queueCap == 0 || (len && !utf8)) {
    fail(true);
    return false;
  }
  const auto* p = reinterpret_cast<const uint8_t*>(utf8);
  const auto* end = p + len;
  while (p < end) {
    const uint32_t cp = decodeUtf8(p, end);
    if (isVariationSelector(cp)) {
      // 跟在漢字後面 ＝ 出版社指定了字形／讀音 → 那個字不猜（它還在窗口裡：中間沒有 process）
      if (n_ > 0 && isIdeograph(s_.cps[n_ - 1])) {
        uint8_t& fl = s_.flags[n_ - 1];
        if (cp >= kVs17 && cp <= kVs32 && !(fl & kSelMask)) {
          fl |= static_cast<uint8_t>((cp - kVs17 + 1) << kSelShift);  // v342：bpmfvs → 照書（outFor）
        } else {
          fl |= kFlagAnnotated;  // 其他選擇符號、或同一個字的第二個 bpmfvs 選擇符號（不合規格）→ 不猜
        }
      }
      continue;
    }
    if (isTransparent(cp)) continue;
    if (!push(cp, annotated ? kFlagAnnotated : 0)) return false;
  }
  return true;
}

bool ZhuyinSession::push(uint32_t cp, uint8_t fl) {
  if (n_ == s_.windowCap && !process(false)) return false;
  if (n_ == s_.windowCap) {  // 一個都送不出去：佇列滿了、呼叫端一直沒取出行（待送出最多 kMaxYiBuRun + kCommitLag）
    fail(true);
    return false;
  }
  if (cp == kYi || cp == kBu) {
    if (runLen_ == 0) {
      runStart_ = n_;
      runLong_ = false;
    }
    runLen_++;
    if (!runLong_ && runLen_ > kMaxYiBuRun) {
      runLong_ = true;  // 這一串一個都還沒送出（還沒判定時整串扣住）→ 全部標成「不標」
      for (size_t i = runStart_; i < n_; i++) s_.flags[i] |= kFlagLongRun;
    }
    if (runLong_) fl |= kFlagLongRun;
  } else {
    runLen_ = 0;
    runLong_ = false;
  }
  s_.cps[n_] = cp;
  s_.flags[n_] = fl;
  n_++;
  return true;
}

bool ZhuyinSession::process(bool final) {
  if (failed_) return false;
  if (n_ == 0) return true;
  size_t safe = 0;
  if (!r_.resolveWindow(s_.cps, n_, final, s_.out, s_.covered, &safe)) {
    fail(true);  // 讀卡失敗、引擎被撤銷
    return false;
  }
  size_t k = safe;
  if (!final) {
    // 超長一不串的成員反正不標 → 強制送出穿過它們（分詞狀態照樣正確）。串還開著、或已經結束但成員還在窗口裡都一樣：
    // 解析器只會往回退過「連續、沒被詞涵蓋的一／不」，所以 [safe, …) 是同一串的成員 —— 要嘛全都標了、要嘛全都沒標。
    const size_t limit = r_.forcedCommitLimit();
    while (k < limit && (s_.flags[k] & kFlagLongRun)) k++;
    // 窗口尾端那一串還沒判定長短：從串頭起整串扣住
    if (runLen_ > 0 && !runLong_ && runStart_ < k) k = runStart_;
  }
  // 背壓：只送出佇列放得下的漢字數（任何 k ≤ 上限都正確，剩下的留在窗口裡）
  {
    const size_t room = s_.queueCap - queueLen_;
    size_t han = 0, lim = 0;
    for (; lim < k; lim++) {
      if (!isIdeograph(s_.cps[lim])) continue;
      if (han == room) break;
      han++;
    }
    k = lim;
  }
  const bool forced = k > safe;
  if (forced ? !r_.commitForced(s_.cps, n_, s_.out, s_.covered, k) : !r_.commit(s_.cps, n_, s_.out, s_.covered, k)) {
    fail(true);
    return false;
  }
  for (size_t i = 0; i < k; i++) {
    const uint32_t cp = s_.cps[i];
    if (!isIdeograph(cp)) continue;  // 只有漢字進佇列（取出行的那一端用同一個分類）
    if (queueLen_ >= s_.queueCap) {  // 背壓已經算過，不該發生
      fail(true);
      return false;
    }
    const uint16_t o = outFor(cp, s_.flags[i], s_.out[i]);
    s_.queue[(queueHead_ + queueLen_) % s_.queueCap] = (cp << 16) | o;
    queueLen_++;
  }
  // 窗口往前挪
  const size_t rest = n_ - k;
  if (k && rest) {
    std::memmove(s_.cps, s_.cps + k, rest * sizeof(uint32_t));
    std::memmove(s_.flags, s_.flags + k, rest);
  }
  n_ = rest;
  if (runLen_ > 0) {
    const size_t start = runStart_ > k ? runStart_ : k;  // 串裡已送出的部分（只有超長串會被送出）
    runLen_ = runStart_ + runLen_ - start;
    runStart_ = start - k;
  }
  return true;
}

uint16_t ZhuyinSession::outFor(uint32_t cp, uint8_t fl, uint16_t resolved) const {
  if (fl & kFlagAnnotated) return 0;  // ruby 或其他選擇符號：出版社自己處理了
  if (fl & kSelMask) {                // v342：bpmfvs 選擇符號 → 照書
    const uint32_t k = ((fl & kSelMask) >> kSelShift) - 1;  // 0 ＝ VS17 ＝ 不標注音
    return k == 0 ? 0 : d_.variantOut(cp, k);            // 字型沒有這個讀音 → 0（畫原字，不畫錯的注音）
  }
  if (docAnnotated_) return d_.variantOut(cp, 0);  // v342：預先標注的章節、沒有選擇符號 ＝ 第一個讀音（單音字 0）
  if (fl & kFlagLongRun) return 0;
  return resolved;
}

bool ZhuyinSession::finish() {
  if (failed_ || !owns()) return false;
  if (!resolverStackOk()) {
    fail(true);
    return false;
  }
  return process(true);
}

bool ZhuyinSession::take(uint32_t cp, uint16_t* out) {
  if (!failed_ && !owns()) return false;
  if (failed_ || !out || queueLen_ == 0) {
    failed_ = true;  // 呼叫端的錯（先看 available()）：之後都不標；不算降級（不是資源問題）
    return false;
  }
  const uint32_t e = s_.queue[queueHead_];
  if ((e >> 16) != cp) {
    failed_ = true;  // 顯示的漢字跟解析的對不上（雙向重排或沒預料到的轉換）→ 之後都不標
    return false;
  }
  *out = static_cast<uint16_t>(e & 0xFFFFu);
  queueHead_ = (queueHead_ + 1) % s_.queueCap;
  queueLen_--;
  return true;
}

bool ZhuyinSession::takeAny(uint32_t* cp, uint16_t* out) {
  if (!failed_ && !owns()) return false;
  if (failed_ || !cp || !out || queueLen_ == 0) {
    failed_ = true;
    return false;
  }
  const uint32_t e = s_.queue[queueHead_];
  *cp = e >> 16;
  *out = static_cast<uint16_t>(e & 0xFFFFu);
  queueHead_ = (queueHead_ + 1) % s_.queueCap;
  queueLen_--;
  return true;
}

}  // namespace zhuyin
