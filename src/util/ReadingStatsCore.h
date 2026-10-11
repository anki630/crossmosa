#pragma once

// 閱讀統計・第一步（只記錄、不顯示）的純邏輯核心 —— 不碰硬體、不鎖、不 I/O，電腦端測試在 test/reading_stats。
// 裝置端的鎖、NVS、時鐘、卡片序號在 ReadingStats.{h,cpp}。設計與三輪審查：工作區
// docs/specs/2026-10-07-reading-stats-recorder.md（第三版＋§9）。
//
// 規則（全部來自審查，不是偏好）：
//   - 「在讀」＝兩次【正文成功繪製】之間的時間；位置用 render 開頭捕捉的快照（不回讀可變成員）。
//   - 每段上限 5 分鐘（書開著人走開不灌時數）；超過 30 分鐘另計一次 gap（＝有一條路漏了 pause）。
//   - 同頁重繪：暫停中就從現在起算（淺睡醒來、從子畫面回來）；計時中不動。
//   - 位置變了：結算上一段；版面世代（gen）沒變才算一次導覽，變了是重排（改字級、轉向），不算。
//   - 結書畫面：只在「正文 → 結書」那一次結算＋算一次導覽，之後重畫都不再算。
//   - pause() 冪等。
//   - 持久化存【整份快照】，不存增量：寫失敗就保持 dirty，下次整塊重寫，沒有東西要「加回去」。
//   - 每日只在時鐘可信時記（X4 沒有 RTC → 永遠不記）；時鐘往回跳（新日 < lastDay）就不歸日。
//   - 每本只記最近 10 本；滿了淘汰 lastSeq 最小的（存取序號，不依賴時鐘 → X4 也有定義）。

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace readingstats {

constexpr uint32_t kCapMs = 5u * 60u * 1000u;
constexpr uint32_t kGapMs = 30u * 60u * 1000u;
constexpr int kDays = 32;
constexpr int kBooks = 10;

// FNV-1a 64 位元（書的身分＝卡片序號雜湊 ‖ 路徑；codex 第三輪：兩張卡同路徑的書不能合併）
constexpr uint64_t kFnv64Basis = 1469598103934665603ull;
inline uint64_t fnv64(const void* data, const size_t len, uint64_t h = kFnv64Basis) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

struct BookRec {
  uint64_t key = 0;  // 0＝空格
  uint32_t seconds = 0;
  uint32_t navs = 0;
  uint32_t lastSeq = 0;
};

// 存進 NVS 的整份狀態
struct Snapshot {
  uint32_t seq = 0;        // 存取序號（換書時 +1，寫進那本的 lastSeq）
  uint32_t lastDay = 0;    // 最後一次歸日的本地日（1970 起的日數）；daysValid==0 時無意義
  uint32_t daysValid = 0;  // bit (day % 32)：那一格屬於最近 32 天內、有被記過
  uint32_t totalSec = 0;
  uint32_t days[kDays] = {};
  BookRec books[kBooks] = {};
};

struct DayInfo {
  bool valid = false;
  uint32_t day = 0;
};

// ---------- 序列化：逐欄 little-endian，不 raw-write struct ----------
constexpr uint8_t kMagic = 'R';
constexpr uint8_t kVersion = 1;
constexpr size_t kBlobSize = 4 + 4 * 4 + 4 * kDays + kBooks * 20;  // 348
static_assert(kBlobSize == 348, "rstat v1 layout");

namespace detail {
inline void put32(uint8_t*& p, const uint32_t v) {
  for (int i = 0; i < 4; ++i) *p++ = static_cast<uint8_t>(v >> (8 * i));
}
inline void put64(uint8_t*& p, const uint64_t v) {
  for (int i = 0; i < 8; ++i) *p++ = static_cast<uint8_t>(v >> (8 * i));
}
inline uint32_t get32(const uint8_t*& p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(*p++) << (8 * i);
  return v;
}
inline uint64_t get64(const uint8_t*& p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(*p++) << (8 * i);
  return v;
}
}  // namespace detail

inline void encode(const Snapshot& s, uint8_t (&out)[kBlobSize]) {
  uint8_t* p = out;
  *p++ = kMagic;
  *p++ = kVersion;
  *p++ = 0;  // flags（保留，寫 0）
  *p++ = 0;
  detail::put32(p, s.seq);
  detail::put32(p, s.lastDay);
  detail::put32(p, s.daysValid);
  detail::put32(p, s.totalSec);
  for (const uint32_t d : s.days) detail::put32(p, d);
  for (const BookRec& b : s.books) {
    detail::put64(p, b.key);
    detail::put32(p, b.seconds);
    detail::put32(p, b.navs);
    detail::put32(p, b.lastSeq);
  }
}

// codex 第三輪阻斷 3：讀取要分三態 —— 「比我新」的資料這一次開機絕對不能覆寫。
enum class LoadResult : uint8_t { Absent = 0, Corrupt = 1, Compatible = 2, Newer = 3 };

// len＝NVS 裡那個 blob 的實際長度（呼叫端先查長度；超過緩衝區上限的長度由呼叫端直接判 Newer）。
inline LoadResult decode(const uint8_t* data, const size_t len, Snapshot& out) {
  if (len < 2 || data[0] != kMagic) return LoadResult::Corrupt;
  if (data[1] > kVersion) return LoadResult::Newer;
  if (data[1] != kVersion || len != kBlobSize || data[2] != 0 || data[3] != 0) return LoadResult::Corrupt;
  Snapshot s;
  const uint8_t* p = data + 4;
  s.seq = detail::get32(p);
  s.lastDay = detail::get32(p);
  s.daysValid = detail::get32(p);
  s.totalSec = detail::get32(p);
  for (uint32_t& d : s.days) d = detail::get32(p);
  for (BookRec& b : s.books) {
    b.key = detail::get64(p);
    b.seconds = detail::get32(p);
    b.navs = detail::get32(p);
    b.lastSeq = detail::get32(p);
  }
  // 結構驗證：同一個 key 不可出現兩次
  for (int i = 0; i < kBooks; ++i) {
    if (s.books[i].key == 0) continue;
    for (int j = i + 1; j < kBooks; ++j) {
      if (s.books[j].key == s.books[i].key) return LoadResult::Corrupt;
    }
  }
  out = s;
  return LoadResult::Compatible;
}

inline uint32_t satAdd(const uint32_t a, const uint32_t b) { return a > UINT32_MAX - b ? UINT32_MAX : a + b; }

// ---------- 計時器 ----------
class Recorder {
 public:
  // 診斷計數（自上次成功存檔以來；存檔成功時呼叫 clearDiag）
  struct Diag {
    uint32_t addMs = 0;
    uint32_t navs = 0;
    uint32_t capped = 0;
    uint32_t reflows = 0;
    uint32_t gaps = 0;
    uint32_t backDays = 0;  // 時鐘往回跳而不歸日的次數
  };

  void load(const Snapshot& s) {
    snap_ = s;
    dirty_ = false;
    for (uint32_t& r : bookRem_) r = 0;
  }
  const Snapshot& snapshot() const { return snap_; }
  bool dirty() const { return dirty_; }
  uint32_t mutationGen() const { return mutGen_; }
  // 存檔成功：只有在取快照之後沒有新的變動時才清 dirty（codex 第三輪：mutation generation）
  void markSaved(const uint32_t genAtSnapshot) {
    if (genAtSnapshot == mutGen_) dirty_ = false;
  }
  const Diag& diag() const { return diag_; }
  void clearDiag() { diag_ = Diag{}; }
  bool running() const { return running_; }

  // 正文成功繪製。book＝身分（永不為 0）；perBook＝要不要記進每本（讀不到卡片序號時 false）；
  //   a、b＝位置快照（EPUB：spine、頁；TXT：offset、0；XTC：頁、0）；gen＝版面世代。
  void observe(const uint64_t book, const bool perBook, const uint32_t a, const uint32_t b, const uint32_t gen,
               const uint32_t nowMs, const DayInfo& day) {
    if (!hasBook_ || book != curBook_) {
      settle(nowMs, day);
      curBook_ = book;
      curPerBook_ = perBook;
      hasBook_ = true;
      posA_ = a;
      posB_ = b;
      gen_ = gen;
      atEob_ = false;
      if (perBook) {
        snap_.seq = satAdd(snap_.seq, 1);
        touchBook(book)->lastSeq = snap_.seq;
        changed();
      }
      running_ = true;
      startMs_ = nowMs;
      return;
    }
    const bool moved = a != posA_ || b != posB_ || atEob_;
    if (moved) {
      settle(nowMs, day);
      if (gen != gen_) {
        diag_.reflows++;
      } else if (!atEob_) {  // 從結書畫面回到正文不算一次翻頁（進結書那一次已經算過）
        countNav();
      }
      posA_ = a;
      posB_ = b;
      gen_ = gen;
      atEob_ = false;
      running_ = true;
      startMs_ = nowMs;
      return;
    }
    gen_ = gen;
    if (!running_) {  // 同頁重繪：暫停中就從現在起算
      running_ = true;
      startMs_ = nowMs;
    }
  }

  // 結書畫面：只在正文 → 結書那一次結算＋算一次導覽
  void endOfBook(const uint32_t nowMs, const DayInfo& day) {
    if (!hasBook_ || atEob_) {
      pause(nowMs, day);
      return;
    }
    settle(nowMs, day);
    countNav();
    atEob_ = true;
    running_ = false;
  }

  // 結算並暫停；冪等
  void pause(const uint32_t nowMs, const DayInfo& day) {
    settle(nowMs, day);
    running_ = false;
  }

  // 離開閱讀器：暫停＋結束這一段閱讀。之後再開同一本（A → 首頁 → A）也算一次重新開啟，
  //   會刷新它的存取序號（codex 程式碼複查：否則剛重讀的書可能被最近 10 本淘汰）。睡眠入口用 pause()，不用這個。
  void endSession(const uint32_t nowMs, const DayInfo& day) {
    pause(nowMs, day);
    hasBook_ = false;
  }

  // 自動搬到 /Read（路徑改了 → 身分改了）：把舊 key 的累計搬到新 key；新 key 已存在就相加。
  void rename(const uint64_t oldKey, const uint64_t newKey) {
    if (oldKey == newKey || oldKey == 0 || newKey == 0) return;
    BookRec* o = findBook(oldKey);
    if (hasBook_ && curBook_ == oldKey) curBook_ = newKey;
    if (!o) return;
    BookRec* n = findBook(newKey);
    if (n) {
      uint32_t& oRem = bookRem_[o - snap_.books];
      uint32_t& nRem = bookRem_[n - snap_.books];
      nRem += oRem;
      n->seconds = satAdd(satAdd(n->seconds, o->seconds), nRem / 1000u);
      nRem %= 1000u;
      oRem = 0;
      n->navs = satAdd(n->navs, o->navs);
      if (o->lastSeq > n->lastSeq) n->lastSeq = o->lastSeq;
      *o = BookRec{};
    } else {
      o->key = newKey;
    }
    changed();
  }

  // 查詢（之後的卡片與統計頁用；這一版只給證人）
  uint32_t bookSeconds(const uint64_t key) const {
    for (const BookRec& r : snap_.books)
      if (r.key == key && key != 0) return r.seconds;
    return 0;
  }
  uint32_t daySeconds(const DayInfo& day) const {
    if (!day.valid || !(snap_.daysValid & dayBit(day.day)) || day.day > snap_.lastDay ||
        snap_.lastDay - day.day >= static_cast<uint32_t>(kDays))
      return 0;
    return snap_.days[day.day % kDays];
  }

 private:
  static uint32_t dayBit(const uint32_t day) { return 1u << (day % kDays); }

  void changed() {
    dirty_ = true;
    ++mutGen_;
  }

  void countNav() {
    diag_.navs++;
    if (curPerBook_) {
      if (BookRec* r = findBook(curBook_)) {
        r->navs = satAdd(r->navs, 1);
        changed();
      }
    }
  }

  BookRec* findBook(const uint64_t key) {
    for (BookRec& r : snap_.books)
      if (r.key == key) return &r;
    return nullptr;
  }

  // 找不到就佔一格：先找空格，沒有就淘汰 lastSeq 最小的
  BookRec* touchBook(const uint64_t key) {
    if (BookRec* r = findBook(key)) return r;
    BookRec* victim = nullptr;
    for (BookRec& r : snap_.books) {
      if (r.key == 0) {
        victim = &r;
        break;
      }
      if (!victim || r.lastSeq < victim->lastSeq) victim = &r;
    }
    *victim = BookRec{};
    victim->key = key;
    bookRem_[victim - snap_.books] = 0;
    return victim;
  }

  // 歸日：回傳這一段能不能記進每日。新日 > lastDay → 跳過的格子歸零；新日 < lastDay（時鐘往回）→ 不記。
  bool rollDay(const uint32_t day) {
    if (snap_.daysValid == 0) {
      snap_.lastDay = day;
      return true;
    }
    if (day < snap_.lastDay) return false;
    if (day > snap_.lastDay) {
      const uint32_t span = day - snap_.lastDay;
      if (span >= static_cast<uint32_t>(kDays)) {
        for (uint32_t& d : snap_.days) d = 0;
        snap_.daysValid = 0;
      } else {
        for (uint32_t d = snap_.lastDay + 1; d <= day; ++d) {
          snap_.days[d % kDays] = 0;
          snap_.daysValid &= ~dayBit(d);
        }
      }
      snap_.lastDay = day;
      dayRemMs_ = 0;
    }
    return true;
  }

  void settle(const uint32_t nowMs, const DayInfo& day) {
    if (!running_) return;
    uint32_t ms = nowMs - startMs_;  // 無號相減：跨 millis() 溢位也對
    startMs_ = nowMs;
    if (ms >= kGapMs) diag_.gaps++;
    if (ms > kCapMs) {
      ms = kCapMs;
      diag_.capped++;
    }
    if (ms == 0) return;
    diag_.addMs = satAdd(diag_.addMs, ms);
    // 總數
    totalRemMs_ += ms;
    snap_.totalSec = satAdd(snap_.totalSec, totalRemMs_ / 1000u);
    totalRemMs_ %= 1000u;
    // 每日
    if (day.valid) {
      if (rollDay(day.day)) {
        dayRemMs_ += ms;
        uint32_t& slot = snap_.days[day.day % kDays];
        slot = satAdd(slot, dayRemMs_ / 1000u);
        dayRemMs_ %= 1000u;
        snap_.daysValid |= dayBit(day.day);
      } else {
        diag_.backDays++;
      }
    }
    // 每本
    if (curPerBook_ && hasBook_) {
      BookRec* r = touchBook(curBook_);
      uint32_t& rem = bookRem_[r - snap_.books];  // 每本自己的零頭（codex 程式碼複查：換書不能清掉）
      rem += ms;
      r->seconds = satAdd(r->seconds, rem / 1000u);
      rem %= 1000u;
    }
    changed();
  }

  Snapshot snap_;
  bool dirty_ = false;
  uint32_t mutGen_ = 0;
  Diag diag_;

  bool hasBook_ = false;
  bool curPerBook_ = false;
  uint64_t curBook_ = 0;
  uint32_t posA_ = 0;
  uint32_t posB_ = 0;
  uint32_t gen_ = 0;
  bool atEob_ = false;
  bool running_ = false;
  uint32_t startMs_ = 0;
  uint32_t bookRem_[kBooks] = {};  // 每本的毫秒零頭，跟 snap_.books 同一個索引；只在 RAM（斷電最多丟 < 1 秒／本）
  uint32_t dayRemMs_ = 0;
  uint32_t totalRemMs_ = 0;
};

}  // namespace readingstats
