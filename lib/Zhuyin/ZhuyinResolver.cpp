#include "ZhuyinResolver.h"

namespace zhuyin {

namespace {

constexpr uint32_t kYi = 0x4E00;  // 一
constexpr uint32_t kBu = 0x4E0D;  // 不

// covered[] 的標記：哪些位置被詞涵蓋、是不是詞的開頭，分批送出時用來找跨界詞
constexpr uint8_t kFree = 0;
constexpr uint8_t kWordStart = 1;
constexpr uint8_t kWordRest = 2;
constexpr uint8_t kCarried = 3;

}  // namespace

void ZhuyinResolver::reset() {
  windowForcedLimit_ = 0;
  prevLen_ = 0;
  carry_.len = 0;
  pendingLen_ = 0;
  pendingHash_ = 0;
  for (auto& c : pendingHead_) c = 0;
  windowValid_ = false;
}

uint64_t ZhuyinResolver::hashCps(const uint32_t* cps, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;  // FNV-1a 64，逐碼位 4 位元組。呼叫端是自己的韌體、不是攻擊者：
  for (size_t i = 0; i < n; i++) {      // 這裡擋的是整合錯誤（接錯批次），前 kPendingExact 個碼位另外逐字比
    for (int b = 0; b < 4; b++) {
      h ^= (cps[i] >> (8 * b)) & 0xFFu;
      h *= 0x100000001B3ull;
    }
  }
  return h;
}

bool ZhuyinResolver::selfTest(ZhuyinData& data, uint16_t* failedCase) {
  if (failedCase) *failedCase = 0;
  if (!data.structurallyLoaded()) return false;
  struct Ctx {
    ZhuyinData* d;
    uint16_t index;
  } ctx{&data, 0};
  data.selfTesting_ = true;  // 自我測試期間准許查詢（結構已驗、還沒驗證）
  const bool ok = data.forEachSelfTest(
      [](void* p, const uint32_t* cps, const uint16_t* expected, size_t n) -> bool {
        auto* c = static_cast<Ctx*>(p);
        ZhuyinResolver r(*c->d);
        uint16_t out[kMaxSelfTestLen];
        uint8_t covered[kMaxSelfTestLen];
        if (!r.resolve(cps, n, out, covered)) return false;
        for (size_t i = 0; i < n; i++) {
          if (out[i] != expected[i]) return false;
        }
        if (n > kCommitLag) {
          for (size_t cut = 1; cut < n; cut++) {
            ZhuyinResolver s(*c->d);
            size_t c1 = 0, c2 = 0;
            if (!s.feed(cps, cut, false, out, covered, &c1)) return false;
            for (size_t i = 0; i < c1; i++) {
              if (out[i] != expected[i]) return false;
            }
            if (!s.feed(cps + c1, n - c1, true, out, covered, &c2) || c1 + c2 != n) return false;
            for (size_t i = 0; i < c2; i++) {
              if (out[i] != expected[c1 + i]) return false;
            }
          }
        }
        c->index++;
        return true;
      },
      &ctx);
  data.selfTesting_ = false;
  if (failedCase) *failedCase = ctx.index;
  if (!ok || data.selfTestCount() < kMinSelfTestCases) {
    data.clearState();  // 沒過：整份資料關掉，要重新載入
    return false;
  }
  data.setState(ZhuyinData::State::Verified);
  return true;
}

LoadStatus ZhuyinResolver::open(ZhuyinData& data, BlockSource& src, Arena& arena, uint16_t* failedCase) {
  const LoadStatus s = data.load(src, arena);
  if (s != LoadStatus::Ok) return s;
  return selfTest(data, failedCase) ? LoadStatus::Ok : LoadStatus::BadSelfTest;
}

bool ZhuyinResolver::resolve(const uint32_t* cps, size_t n, uint16_t* out, uint8_t* covered) {
  const Carry none = {{}, 0};
  return run(cps, n, none, nullptr, 0, out, covered);
}

bool ZhuyinResolver::feed(const uint32_t* cps, size_t n, bool final, uint16_t* out, uint8_t* covered,
                          size_t* commitLen) {
  if (!commitLen) return false;
  size_t safe = 0;
  if (!resolveWindow(cps, n, final, out, covered, &safe)) return false;
  if (!commit(cps, n, out, covered, safe)) return false;
  *commitLen = safe;
  return true;
}

bool ZhuyinResolver::resolveWindow(const uint32_t* cps, size_t n, bool final, uint16_t* out, uint8_t* covered,
                                   size_t* safe) {
  windowValid_ = false;
  if (!safe) return false;
  if (n > 0 && !cps) return false;
  // 呼叫端必須把上一批沒送出的部分原封不動放在開頭；對不上就是呼叫端的錯，不能默默變成錯字
  if (n < pendingLen_) return false;
  if (pendingLen_) {
    const size_t exact = pendingLen_ < kPendingExact ? pendingLen_ : kPendingExact;
    for (size_t i = 0; i < exact; i++) {
      if (cps[i] != pendingHead_[i]) return false;
    }
    if (hashCps(cps, pendingLen_) != pendingHash_) return false;
  }
  if (!run(cps, n, carry_, prev_ + (2 - prevLen_), prevLen_, out, covered)) return false;

  size_t c = final ? n : (n > kCommitLag ? n - kCommitLag : 0);
  if (!final) {
    // 送出點前面緊鄰、沒被詞涵蓋的一／不：它的聲調看右鄰，而右鄰還沒定案 → 往回退（一串可以任意長）
    while (c > 0 && covered[c - 1] == kFree && (cps[c - 1] == kYi || cps[c - 1] == kBu)) c--;
  }
  *safe = c;
  windowValid_ = true;
  windowN_ = n;
  windowSafe_ = c;
  windowForcedLimit_ = final ? n : (n > kCommitLag ? n - kCommitLag : 0);
  windowHash_ = n ? hashCps(cps, n) : 0;
  return true;
}

bool ZhuyinResolver::commit(const uint32_t* cps, size_t n, const uint16_t* out, const uint8_t* covered, size_t k) {
  return commitImpl(cps, n, out, covered, k, windowSafe_);
}

bool ZhuyinResolver::commitForced(const uint32_t* cps, size_t n, const uint16_t* out, const uint8_t* covered,
                                  size_t k) {
  return commitImpl(cps, n, out, covered, k, windowForcedLimit_ > windowSafe_ ? windowForcedLimit_ : windowSafe_);
}

bool ZhuyinResolver::commitImpl(const uint32_t* cps, size_t n, const uint16_t* out, const uint8_t* covered, size_t k,
                                size_t limit) {
  if (!windowValid_ || n != windowN_ || k > limit) return false;
  if (n > 0 && (!cps || !out || !covered || hashCps(cps, n) != windowHash_)) return false;
  windowValid_ = false;  // 一次 resolveWindow 只能 commit 一次

  // 帶到下一批的詞尾：送出點落在詞中間（或帶進來的尾巴中間）時，剩下的輸出原封不動帶過去
  Carry next = {{}, 0};
  if (k < n && covered[k] == kWordRest) {
    size_t e = k;
    while (e < n && covered[e] == kWordRest) e++;
    for (size_t j = k; j < e; j++) next.out[next.len++] = out[j];
  } else if (k < carry_.len) {
    for (size_t j = k; j < carry_.len; j++) next.out[next.len++] = carry_.out[j];
  }

  // 下一批要看的「前兩個字」：這一批送出的最後兩個字（不夠就接上一批留下的）
  uint32_t p[2] = {prev_[0], prev_[1]};
  uint8_t pl = prevLen_;
  for (size_t i = (k >= 2 ? k - 2 : 0); i < k; i++) {
    p[0] = p[1];
    p[1] = cps[i];
    if (pl < 2) pl++;
  }

  carry_ = next;
  prev_[0] = p[0];
  prev_[1] = p[1];
  prevLen_ = pl;
  pendingLen_ = n - k;
  pendingHash_ = n > k ? hashCps(cps + k, n - k) : 0;
  for (size_t i = 0; i < kPendingExact; i++) pendingHead_[i] = (k + i < n) ? cps[k + i] : 0;
  return true;
}

bool ZhuyinResolver::run(const uint32_t* cps, size_t n, const Carry& carry, const uint32_t* prev, uint8_t prevLen,
                         uint16_t* out, uint8_t* covered) {
  if (!d_.usable()) return false;
  if (n == 0) return true;
  if (!cps || !out || !covered) return false;

  // 上一批跨界詞的後半：直接定案，辭典從它後面接著切
  const size_t m = carry.len < n ? carry.len : n;
  for (size_t j = 0; j < n; j++) {
    out[j] = j < m ? carry.out[j] : 0;
    covered[j] = j < m ? kCarried : kFree;
  }

  // 1. 辭典：最長詞優先
  size_t k = m;
  while (k < n) {
    if (k + 1 < n && cps[k] <= 0xFFFF && cps[k + 1] <= 0xFFFF) {
      uint32_t index;
      if (d_.findKey((cps[k] << 16) | cps[k + 1], &index)) {
        lookups_++;
        uint8_t len = 0;
        uint16_t outs[kMaxWord];
        if (!d_.matchLongest(index, cps + k, n - k, &len, outs)) return false;
        if (len) {
          for (uint8_t j = 0; j < len; j++) {
            out[k + j] = outs[j];
            covered[k + j] = j == 0 ? kWordStart : kWordRest;
          }
          k += len;
          continue;
        }
      }
    }
    k++;
  }

  // 位置 i 前面第 back 個字（back ＝ 1 或 2）：先看這一批，不夠就看上一批帶過來的
  auto before = [&](size_t i, size_t back, uint32_t* v) -> bool {
    if (i >= back) {
      *v = cps[i - back];
      return true;
    }
    const size_t need = back - i;  // 還要往上一批找幾個
    if (need > prevLen) return false;
    *v = prev[prevLen - need];
    return true;
  };

  // 2＋3. 預設讀音，再套規則
  for (size_t i = 0; i < n; i++) {
    if (covered[i] != kFree) continue;
    uint16_t o = d_.defaultOut(cps[i]);
    uint32_t p1 = 0, p2 = 0;
    const bool hasPrev = before(i, 1, &p1);
    const bool hasPrev2 = before(i, 2, &p2);
    const bool hasNext = i + 1 < n;
    uint16_t r;
    if (d_.applyRules(cps[i], hasPrev, p1, hasPrev2, p2, hasNext, hasNext ? cps[i + 1] : 0, &r)) o = r;
    out[i] = o;
  }

  // 3b.（v2）還是 0 的破音字 → 預設讀音的私用區字形（它的原碼位字形不帶注音）。
  //     放在變調之前：一、不看的右鄰聲調，要是最後畫出來的那個字形。
  for (size_t i = 0; i < n; i++) {
    if (out[i] != 0) continue;
    const uint16_t pd = d_.polyDefault(cps[i]);
    if (pd) out[i] = pd;
  }

  // 4. 一、不變調：由右到左，先定右鄰
  for (size_t i = n; i-- > 0;) {
    if (covered[i] != kFree) continue;
    const uint32_t c = cps[i];
    if (c != kYi && c != kBu) continue;
    if (i + 1 >= n || !isIdeograph(cps[i + 1])) {
      out[i] = (c == kYi) ? d_.yi1() : d_.bu4();
      continue;
    }
    const bool t4 = d_.tone4(cps[i + 1], out[i + 1]);
    if (c == kBu) {
      out[i] = t4 ? d_.bu2() : d_.bu4();
      continue;
    }
    uint32_t p1 = 0;
    const bool hasPrev = before(i, 1, &p1);
    if ((hasPrev && d_.yiPrevPlain(p1)) || d_.yiNextPlain(cps[i + 1])) {
      out[i] = d_.yi1();
    } else {
      out[i] = t4 ? d_.yi2() : d_.yi4();
    }
  }

  // 5. 最後一道：每個輸出都要屬於它那個字（辭典、帶過去的詞、預設、規則、變調、3b 全部經過這裡）。
  //    不屬於 → 整批失敗，不交出任何部分結果（codex P1.5 複查 2、3）
  for (size_t i = 0; i < n; i++) {
    if (!d_.ownedOut(cps[i], out[i])) return false;
  }
  return true;
}

}  // namespace zhuyin
