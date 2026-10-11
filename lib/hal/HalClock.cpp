#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <common/FsDateTime.h>
#include <esp_sntp.h>
#include <time.h>

HalClock halClock;  // Singleton instance

// v343（codex 第四輪）：「校時交易進行中」也要撐過重開機 —— 晶片寫到一半、接著重開機（淺睡 30 分鐘後就真關機），
//   RAM 裡的 _chipDistrusted 就不見了，那個看起來合理的錯時間會重新被信任；設定裡「已校時」是 1 → 自動校時也不會再跑。
//   所以同一個標記也寫在 RTC 不初始化的記憶體（軟重開、panic、看門狗、深睡都保留；斷電不保留 —— 斷電時 DS3231
//   通常也一起沒電， OSF 會亮）。寫入前設、讀回驗證通過才清；開機讀到就維持不信，直到下一次校時成功。
//   兩個值互為反碼（冷開機的隨機內容幾乎不可能湊成）；volatile 同 EpubReaderActivity
//   的預排保險絲（別讓編譯器當成死寫入）。
volatile RTC_NOINIT_ATTR uint32_t g_clockSyncDirtyMagic;
volatile RTC_NOINIT_ATTR uint32_t g_clockSyncDirtyInv;
constexpr uint32_t CLOCK_SYNC_DIRTY_MAGIC = 0x434C4B44u;  // "CLKD"

namespace {
bool clockSyncDirtyPersisted() {
  return g_clockSyncDirtyMagic == CLOCK_SYNC_DIRTY_MAGIC && g_clockSyncDirtyInv == ~CLOCK_SYNC_DIRTY_MAGIC;
}
void setClockSyncDirty(const bool on) {
  g_clockSyncDirtyMagic = on ? CLOCK_SYNC_DIRTY_MAGIC : 0u;
  g_clockSyncDirtyInv = on ? ~CLOCK_SYNC_DIRTY_MAGIC : 0u;
}
}  // namespace

void HalClock::fatDateTimeCb(uint16_t* date, uint16_t* time) {
  // v194：SdFat 在持鎖改目錄時同步呼叫。這裡不准 I2C／LOG／寫檔，只讀發布的值立刻回。
  // v343（codex 第二輪）：發布的是「上一次可信讀取的 UTC 秒數＋當時的 millis()」→ 這裡用單調時鐘往前推，
  //   不再停在最後一次發布的時分。seqlock：讀到一半被寫入就重讀（單核：寫的一方被這裡搶先時重讀也沒用，
  //   四次都不成就回佔位值）。秒數 0 ＝ 撤銷；往前推超過 kFatMaxAgeMs 也回佔位值。
  uint32_t epoch = 0;
  uint32_t atMs = 0;
  const bool ok = halClock.readPublished(epoch, atMs);
  const uint32_t age = static_cast<uint32_t>(millis()) - atMs;
  if (!ok || age >= kFatMaxAgeMs) {
    // 跟沒有 callback 時同一個佔位時戳（編譯年 1 月 1 日 00:00）
    *date = FS_DEFAULT_DATE;
    *time = FS_DEFAULT_TIME;
    return;
  }
  uint16_t y;
  uint8_t mo, d, h, mi, s;
  clockcache::civilFromEpoch(epoch + age / 1000u, y, mo, d, h, mi, s);
  *date = FS_DATE(y, mo, d);
  *time = FS_TIME(h, mi, s);
}

bool HalClock::readPublished(uint32_t& epoch, uint32_t& atMs) const {
  bool ok = false;
  for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
    const uint32_t s1 = _fatSeq.load(std::memory_order_acquire);
    if (s1 & 1u) continue;  // 寫入中
    epoch = _fatEpoch.load(std::memory_order_relaxed);
    atMs = _fatAtMs.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    ok = _fatSeq.load(std::memory_order_relaxed) == s1;
  }
  return ok && epoch != 0;  // 秒數 0 ＝ 撤銷
}

bool HalClock::trustedUtcNow(uint32_t& epochOut, const uint32_t maxAgeMs) const {
  if (!_available) return false;
  uint32_t epoch = 0;
  uint32_t atMs = 0;
  if (!readPublished(epoch, atMs)) return false;
  const uint32_t age = static_cast<uint32_t>(millis()) - atMs;
  if (age >= maxAgeMs) return false;
  epochOut = epoch + age / 1000u;
  return true;
}

// v194（複查）：順帶在第一次有可信時間時補註冊 ——原本只在開機試一次，那一次 I2C 失敗就整次開機都不會有 FAT 時戳。
void HalClock::publishFatStamp(const Rtc::DateTime& dt, const uint32_t nowMs) const {
  if (!isTrusted(dt)) return;  // v343：跟狀態列同一個「可信」定義（原本只看年份 2020–2107）
  const uint32_t epoch = epochOf(dt);
  if (epoch == 0) return;  // 0 是撤銷哨兵（1970 年早就被 isTrusted 擋掉，理論上不會）
  const uint32_t s = _fatSeq.load(std::memory_order_relaxed);
  _fatSeq.store(s + 1, std::memory_order_relaxed);  // 奇數：寫入中
  std::atomic_thread_fence(std::memory_order_release);
  _fatEpoch.store(epoch, std::memory_order_relaxed);
  _fatAtMs.store(nowMs, std::memory_order_relaxed);
  _fatSeq.store(s + 2, std::memory_order_release);  // 偶數：完成
  if (!_fatCbInstalled) {
    FsDateTime::setCallback(&HalClock::fatDateTimeCb);
    _fatCbInstalled = true;
    LOG_INF("CLK", "FsDateTime callback registered year=%u", dt.year);
  }
}

void HalClock::revokeFatStamp() const {
  const uint32_t s = _fatSeq.load(std::memory_order_relaxed);
  _fatSeq.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  _fatEpoch.store(0, std::memory_order_relaxed);
  _fatSeq.store(s + 2, std::memory_order_release);
}

void HalClock::noteTrusted(const Rtc::DateTime& dt, const uint32_t nowMs) const {
  _cache.onGood(nowMs, dt.hour, dt.minute, dt.second);
  publishFatStamp(dt, nowMs);
}

void HalClock::noteUntrusted() const {
  _cache.invalidate();
  revokeFatStamp();
}

void HalClock::noteReadFailed(const uint32_t nowMs) const {
  // codex v343 第三輪：每一個「讀不到」的出口都走這裡 —— 上一次可信讀取已經超過 clockcache::kStaleMs → 撤銷 FAT
  //   （跟狀態列同一個判斷；只剩一個出口做這件事的話，狀態列關著時 FAT 會用舊基準往前推到 24 小時）。
  uint8_t h, m;
  if (!_cache.current(nowMs, h, m)) revokeFatStamp();
}

void HalClock::noteSyncFailed() {
  uint32_t until = static_cast<uint32_t>(millis()) + kSyncBackoffMs;
  if (until == 0) until = 1;  // 0 是「沒有退避」的哨兵
  _syncRetryAfterMs.store(until, std::memory_order_relaxed);
}

bool HalClock::syncRetryDue() const {
  const uint32_t until = _syncRetryAfterMs.load(std::memory_order_relaxed);
  return until == 0 || static_cast<int32_t>(static_cast<uint32_t>(millis()) - until) >= 0;
}

void HalClock::installFatDateTimeCallbackIfKnown() {
  if (!_available) return;
  std::lock_guard<std::mutex> lk(_mu);
  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) return;
  // v194：時鐘已知才註冊（不可信就維持 SdFat 的佔位時戳）。v343：上一次校時交易沒完成（重開機前）也不註冊。
  if (!_chipDistrusted && isTrusted(dt)) noteTrusted(dt, millis());
}

void HalClock::begin() {
  // v343：開機＝全新狀態；上一次開機留下的「校時交易沒完成」從 RTC 記憶體讀回來（見檔頭）
  {
    std::lock_guard<std::mutex> lk(_mu);
    _polled = false;
    _cache.invalidate();
    _chipDistrusted = clockSyncDirtyPersisted();
  }
  _syncRetryAfterMs.store(0, std::memory_order_relaxed);
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
  // v194：開機時 RTC 已有可信時間才掛 FAT 時戳；沒對過就維持編譯年 1 月 1 日。
  installFatDateTimeCallbackIfKnown();
}

const char* HalClock::stateName(const State s) {
  switch (s) {
    case State::Absent:
      return "absent";
    case State::ReadFailed:
      return "readfail";
    case State::Invalid:
      return "invalid";
    case State::Ok:
      return "ok";
  }
  return "?";
}

HalClock::State HalClock::probe(uint16_t* yearOut) const {
  if (yearOut) *yearOut = 0;
  if (!_available) return State::Absent;
  std::lock_guard<std::mutex> lk(_mu);
  Rtc::DateTime dt;
  bool ok = _sdkRtc.now(dt);
  if (!ok) {  // v343（codex 複查）：一次短暫的 I2C 失誤不算 → 隔 2 ms 重讀一次
    delay(2);
    ok = _sdkRtc.now(dt);
  }
  if (!ok) {
    noteReadFailed(millis());
    return State::ReadFailed;
  }
  if (yearOut) *yearOut = dt.year;
  if (_chipDistrusted || !isTrusted(dt)) {
    noteUntrusted();  // v343（codex 第二輪）：已經親眼看到壞時間 → 顯示與 FAT 立刻撤銷，不等下一次輪詢
    return State::Invalid;
  }
  noteTrusted(dt, millis());
  return State::Ok;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;
  std::lock_guard<std::mutex> lk(_mu);

  const uint32_t now = millis();
  if (_polled && (now - _lastPollMs) < CLOCK_POLL_MS) {
    // v343：兩次讀取之間用單調時鐘往前推（分鐘不會落後 10 秒）。窗口內剛好過期 → 也撤銷 FAT（codex 第三輪）
    if (_cache.current(now, hour, minute)) return true;
    noteReadFailed(now);
    return false;
  }
  _polled = true;
  _lastPollMs = now;  // v343：讀失敗也算一次輪詢（RTC 壞掉時不要每畫一次就打一次 I2C）

  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) {
    // 讀不到：上一次可信讀取在 clockcache::kStaleMs 內 → 往前推著畫；否則不畫
    //   （v343 以前無限期沿用最後一次的時分 → 讀取一直失敗時畫面凍在同一個時間）
    if (_cache.current(now, hour, minute)) return true;
    noteReadFailed(now);
    return false;
  }
  if (_chipDistrusted || !isTrusted(dt)) {
    // v343（帳本 B12）：讀得到但不可信（RTC 被歸零成 2000 年，或上一次校時讀回不一致）→ 不畫，不畫錯的。
    //   下一次連上 WiFi 會自動校時。
    noteUntrusted();
    return false;
  }
  // v194：FAT callback 只讀發布的值。真正的 I2C 取時放這裡——狀態列低頻輪詢，不持 SdFat 鎖。
  noteTrusted(dt, now);
  return _cache.current(now, hour, minute);
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    noteSyncFailed();
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      Rtc::DateTime dt;
      dt.year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      dt.month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      dt.day = static_cast<uint8_t>(timeinfo.tm_mday);
      dt.hour = static_cast<uint8_t>(timeinfo.tm_hour);
      dt.minute = static_cast<uint8_t>(timeinfo.tm_min);
      dt.second = static_cast<uint8_t>(timeinfo.tm_sec);
      dt.weekday = static_cast<uint8_t>(timeinfo.tm_wday);
      if (!isTrusted(dt)) {  // v343：不把不可信的時間寫進晶片（SNTP 說完成但系統時間還是 1970 之類）
        LOG_ERR("CLK", "NTP returned year %u, not written", dt.year);
        noteSyncFailed();
        return false;
      }
      std::lock_guard<std::mutex> lk(_mu);  // 只包寫入與讀回（等 SNTP 的迴圈不持鎖）
      _chipDistrusted = true;               // 交易開始：從現在起到讀回驗證通過之前，晶片給的時間都不信（見 HalClock.h）
      setClockSyncDirty(true);              // 同一個標記也撐過重開機（codex 第四輪）
      if (!_sdkRtc.set(dt)) {               // 寫到一半失敗 → 晶片可能被改了一半
        LOG_ERR("CLK", "RTC set failed");
        noteUntrusted();
        noteSyncFailed();
        return false;
      }
      // v343（codex 兩輪）：讀回驗證 —— 寫入成功不代表晶片存得住。讀回要可信、而且跟寫進去的一致（±幾秒，
      //   寫入到讀回之間本來就會過一點時間）；否則算失敗：不設「已校時」、不顯示「校時完成」，狀態列也不畫。
      //   ⚠️ 驗得到「寫進去的就是讀出來的」，驗不到「振盪器之後會不會走」（要隔一段時間再讀，之後的項目）。
      Rtc::DateTime back;
      bool readBack = _sdkRtc.now(back);
      if (!readBack) {  // 一次短暫的 I2C 失誤不算（同 probe）—— 否則一次小失誤就讓時鐘藏到下一次校時成功
        delay(2);
        readBack = _sdkRtc.now(back);
      }
      const uint32_t wrote = epochOf(dt);
      const uint32_t got = readBack ? epochOf(back) : 0;
      const uint32_t diff = got > wrote ? got - wrote : wrote - got;
      if (!readBack || !isTrusted(back) || diff > kSyncReadbackToleranceSec) {
        LOG_ERR("CLK", "RTC read-back after NTP set failed (read=%d diff=%lu)", readBack ? 1 : 0,
                static_cast<unsigned long>(diff));
        noteUntrusted();  // _chipDistrusted 維持 true
        noteSyncFailed();
        return false;
      }
      const uint32_t nowMs = millis();
      _polled = true;
      _lastPollMs = nowMs;
      _chipDistrusted = false;
      setClockSyncDirty(false);
      noteTrusted(back, nowMs);  // v194（複查）：對過時間之後立刻發布並補註冊
      _syncRetryAfterMs.store(0, std::memory_order_relaxed);
      LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC", back.year, back.month, back.day, back.hour,
              back.minute, back.second);
      return true;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  noteSyncFailed();
  return false;
}
