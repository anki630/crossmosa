// v343（帳本 B12）：狀態列時鐘的整合測試 —— 韌體的同一個 HalClock.cpp，配上 stubs/ 的替身（見 FakeClockHw.h）。
// 每一條對應 codex v343 兩輪複查點出的一個失敗情境；把對應的那一段改壞，這裡要變紅。

#include <gtest/gtest.h>

#include <ctime>

#include "HalClock.h"
#include "common/FsDateTime.h"

// HalClock.cpp 的 time(nullptr)：這個執行檔自己的定義優先（CMakeLists.txt），NTP 的時間由測試決定
extern "C" time_t time(time_t* t) noexcept {
  if (t) *t = static_cast<time_t>(fake::sysTime);
  return static_cast<time_t>(fake::sysTime);
}

namespace {

struct Hm {
  bool ok;
  int h, m;
};
Hm shown(const HalClock& c) {
  uint8_t h = 0, m = 0;
  const bool ok = c.getTime(h, m);
  return {ok, h, m};
}

// NTP 會在 delayMs 之後完成，系統時間是 y-mo-d h:mi:s（UTC）
void ntpWillAnswer(uint32_t delayMs, uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
  fake::sntpCompleteAtMs = fake::nowMs + delayMs;
  fake::sysTime = clockcache::epochOf(y, mo, d, h, mi, s);
}

struct Stamp {
  uint16_t date, time;
};
Stamp fatNow() {
  Stamp s{0xFFFF, 0xFFFF};
  if (FsDateTime::callback2) FsDateTime::callback2(&s.date, &s.time);
  return s;
}
bool isDefault(const Stamp& s) { return s.date == FS_DEFAULT_DATE && s.time == FS_DEFAULT_TIME; }
bool is(const Stamp& s, uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t sec) {
  return s.date == FS_DATE(y, mo, d) && s.time == FS_TIME(h, mi, sec);
}

class Clock : public ::testing::Test {
 protected:
  void SetUp() override {
    // HalClock.cpp 裡撐過重開機的「校時交易沒完成」標記是全域的（跟裝置一樣只有一顆 RTC）→
    // 每條測試開始前用一次成功的校時清掉
    fake::reset();
    {
      HalClock tmp;
      tmp.begin();
      fake::sntpCompleteAtMs = fake::nowMs;
      fake::sysTime = clockcache::epochOf(2026, 9, 26, 12, 0, 0);
      ASSERT_TRUE(tmp.syncFromNTP());
    }
    fake::reset();
  }
};

}  // namespace

// ---- 顯示 ----

TEST_F(Clock, ShowsTrustedTimeAndAdvances) {
  HalClock c;
  c.begin();
  auto t = shown(c);
  ASSERT_TRUE(t.ok);
  EXPECT_EQ(t.h, 12);
  EXPECT_EQ(t.m, 0);
  fake::nowMs += 70000;
  t = shown(c);
  ASSERT_TRUE(t.ok);
  EXPECT_EQ(t.m, 1);
}

TEST_F(Clock, RtcResetToYear2000IsNotShown) {  // 實機 2026-09-26：狀態列永遠 08:00
  HalClock c;
  c.begin();
  ASSERT_TRUE(shown(c).ok);
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  fake::nowMs += 10000;  // 下一次輪詢
  EXPECT_FALSE(shown(c).ok);
  fake::nowMs += 60000;
  EXPECT_FALSE(shown(c).ok);  // 不會「過一陣子又畫出來」
}

TEST_F(Clock, GarbageFieldsAreNotTrusted) {
  HalClock c;
  c.begin();
  fake::garbageMonth = true;
  EXPECT_EQ(c.probe(), HalClock::State::Invalid);
  EXPECT_FALSE(shown(c).ok);
}

TEST_F(Clock, ProbeSeeingABadTimeRevokesImmediately) {  // codex 第二輪 阻斷 1
  HalClock c;
  c.begin();
  ASSERT_TRUE(shown(c).ok);
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  fake::nowMs += 1000;  // 還在 10 秒輪詢窗口內 —— getTime 不會自己去讀
  uint16_t y = 0;
  EXPECT_EQ(c.probe(&y), HalClock::State::Invalid);
  EXPECT_EQ(y, 2000);
  EXPECT_FALSE(shown(c).ok);
}

TEST_F(Clock, ShortReadFailuresAdvanceThenHideAfterSixtySeconds) {  // codex 第一輪 B
  HalClock c;
  c.begin();
  ASSERT_TRUE(shown(c).ok);  // 12:00:00 @ t0
  fake::i2cBroken = true;
  fake::nowMs += 30000;
  auto t = shown(c);
  ASSERT_TRUE(t.ok);  // 寬限期內：往前推著畫
  EXPECT_EQ(t.m, 0);
  fake::nowMs += 29000;  // t0 + 59 s
  EXPECT_TRUE(shown(c).ok);
  fake::nowMs += 2000;  // t0 + 61 s
  EXPECT_FALSE(shown(c).ok);
}

TEST_F(Clock, PollWindowLimitsI2cReads) {
  HalClock c;
  c.begin();
  const int before = fake::reads;
  for (int i = 0; i < 5; ++i) {
    shown(c);
    fake::nowMs += 1000;
  }
  EXPECT_EQ(fake::reads - before, 1);
}

TEST_F(Clock, ProbeRetriesOneTransientFailure) {
  HalClock c;
  c.begin();
  fake::i2cFailNext = 1;
  EXPECT_EQ(c.probe(), HalClock::State::Ok);
  fake::i2cFailNext = 2;
  EXPECT_EQ(c.probe(), HalClock::State::ReadFailed);
}

TEST_F(Clock, OscillatorStoppedIsReadFailedNotInvalid) {
  HalClock c;
  c.begin();
  fake::osf = true;
  EXPECT_EQ(c.probe(), HalClock::State::ReadFailed);
}

TEST_F(Clock, AbsentRtc) {
  fake::rtcPresent = false;
  HalClock c;
  c.begin();
  EXPECT_FALSE(c.isAvailable());
  EXPECT_EQ(c.probe(), HalClock::State::Absent);
  EXPECT_FALSE(shown(c).ok);
  EXPECT_FALSE(c.syncFromNTP());
}

// ---- 校時 ----

TEST_F(Clock, SyncFromResetRtcWritesVerifiesAndShows) {
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  HalClock c;
  c.begin();
  EXPECT_FALSE(shown(c).ok);
  ntpWillAnswer(1000, 2026, 9, 26, 12, 34, 56);
  ASSERT_TRUE(c.syncFromNTP());
  EXPECT_EQ(fake::sets, 1);
  EXPECT_EQ(c.probe(), HalClock::State::Ok);
  const auto t = shown(c);
  ASSERT_TRUE(t.ok);
  EXPECT_EQ(t.h, 12);
  EXPECT_EQ(t.m, 34);
  EXPECT_TRUE(c.syncRetryDue());
}

TEST_F(Clock, SyncThatTheChipDoesNotKeepIsAFailure) {  // codex 第一輪 1：讀回
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  fake::setSticks = false;  // 寫入「成功」但晶片沒存（還是 2000）
  HalClock c;
  c.begin();
  ntpWillAnswer(0, 2026, 9, 26, 12, 34, 56);
  EXPECT_FALSE(c.syncFromNTP());
  EXPECT_FALSE(shown(c).ok);
  EXPECT_FALSE(c.syncRetryDue());
}

TEST_F(Clock, SyncReadBackMustMatchWhatWasWritten) {  // codex 第二輪 4：讀回合理但不是剛寫的
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  fake::setOffsetSec = -86400;  // 晶片存成前一天（仍在可信範圍）
  HalClock c;
  c.begin();
  ntpWillAnswer(0, 2026, 9, 26, 12, 34, 56);
  EXPECT_FALSE(c.syncFromNTP());
  EXPECT_FALSE(shown(c).ok);
  fake::setOffsetSec = 3;  // 在容許範圍內（寫入到讀回之間本來會過一點時間）
  fake::nowMs += HalClock::kSyncBackoffMs;
  ntpWillAnswer(0, 2026, 9, 26, 13, 0, 0);
  EXPECT_TRUE(c.syncFromNTP());
}

TEST_F(Clock, SetFailingHalfwayKeepsTheChipDistrusted) {  // codex 第三輪 阻斷：寫到一半失敗
  fake::setRtc(2026, 9, 20, 10, 0, 0);                    // 晶片裡是一個「合理」的時間
  HalClock c;
  c.begin();
  ASSERT_TRUE(shown(c).ok);
  fake::setFailsHalfway = true;
  ntpWillAnswer(0, 2026, 9, 26, 12, 34, 56);
  EXPECT_FALSE(c.syncFromNTP());
  EXPECT_FALSE(shown(c).ok);  // 晶片裡現在是「新時分配舊日期」—— 看起來合理，但不能畫
  fake::nowMs += 60000;
  EXPECT_FALSE(shown(c).ok);
  EXPECT_EQ(c.probe(), HalClock::State::Invalid);
  fake::setFailsHalfway = false;  // 下一次校時成功才解除
  fake::nowMs += HalClock::kSyncBackoffMs;
  ntpWillAnswer(0, 2026, 9, 26, 13, 34, 56);
  ASSERT_TRUE(c.syncFromNTP());
  EXPECT_TRUE(shown(c).ok);
}

TEST_F(Clock, ReadBackFailingAfterAnUnkeptSetKeepsTheChipDistrusted) {  // codex 第三輪 阻斷：寫入「成功」但讀回讀不到
  fake::setRtc(2026, 9, 20, 10, 0, 0);
  HalClock c;
  c.begin();
  ASSERT_TRUE(shown(c).ok);
  fake::setSticks = false;      // 晶片沒存，還是 9/20 10:00（合理但錯）
  fake::failReadsAfterSet = 2;  // 讀回兩次都讀不到（重讀也救不了）
  ntpWillAnswer(0, 2026, 9, 26, 12, 34, 56);
  EXPECT_FALSE(c.syncFromNTP());
  fake::nowMs += 11000;
  EXPECT_FALSE(shown(c).ok);  // 讀得到 9/20 10:00，但不能畫
}

TEST_F(Clock, OneReadBackGlitchIsRetried) {
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  HalClock c;
  c.begin();
  fake::failReadsAfterSet = 1;
  ntpWillAnswer(0, 2026, 9, 26, 12, 34, 56);
  EXPECT_TRUE(c.syncFromNTP());
  EXPECT_TRUE(shown(c).ok);
}

TEST_F(Clock, ImpossibleCalendarDatesAreNotTrusted) {  // codex 第三輪 重要 2
  HalClock c;
  c.begin();
  fake::rawActive = true;
  fake::rawYear = 2026;
  fake::rawHour = 12;
  const struct {
    uint16_t y;
    uint8_t m, d;
    bool ok;
  } cases[] = {{2026, 2, 31, false}, {2026, 4, 31, false}, {2026, 2, 29, false}, {2028, 2, 29, true},
               {2100, 2, 29, false}, {2026, 12, 31, true}, {2026, 1, 1, true}};
  for (const auto& k : cases) {
    fake::rawYear = k.y;
    fake::rawMonth = k.m;
    fake::rawDay = k.d;
    EXPECT_EQ(c.probe(), k.ok ? HalClock::State::Ok : HalClock::State::Invalid)
        << k.y << "-" << int(k.m) << "-" << int(k.d);
  }
}

TEST_F(Clock, AnUnfinishedSyncSurvivesAReboot) {  // codex 第四輪 阻斷
  fake::setRtc(2026, 9, 20, 10, 0, 0);
  {
    HalClock c;
    c.begin();
    ASSERT_TRUE(shown(c).ok);
    fake::setFailsHalfway = true;  // 寫到一半：晶片裡變成「新時分配舊日期」，看起來合理
    ntpWillAnswer(0, 2026, 9, 26, 12, 34, 56);
    EXPECT_FALSE(c.syncFromNTP());
  }
  fake::setFailsHalfway = false;
  // 重開機：新的 HalClock、RAM 全新 —— 標記要從 RTC 記憶體讀回來
  HalClock again;
  again.begin();
  EXPECT_TRUE(again.chipDistrusted());  // 開機證人 CLK boot dirty=1
  EXPECT_FALSE(shown(again).ok);
  EXPECT_EQ(again.probe(), HalClock::State::Invalid);  // → 自動校時會把它當「不可信」而去校
  ntpWillAnswer(0, 2026, 9, 26, 12, 40, 0);
  ASSERT_TRUE(again.syncFromNTP());
  HalClock third;  // 校時成功之後再重開機：標記已清
  third.begin();
  EXPECT_FALSE(third.chipDistrusted());
  EXPECT_TRUE(shown(third).ok);
}

TEST_F(Clock, BootWithAnUnfinishedSyncPublishesNoFatStamp) {  // 開機時不能把半寫入的時間發布成 FAT 時戳
  halClock.begin();
  ntpWillAnswer(0, 2026, 9, 26, 12, 0, 0);
  ASSERT_TRUE(halClock.syncFromNTP());
  fake::setFailsHalfway = true;
  ntpWillAnswer(0, 2026, 9, 26, 12, 30, 0);
  EXPECT_FALSE(halClock.syncFromNTP());
  fake::setFailsHalfway = false;
  halClock.begin();  // 重開機
  EXPECT_TRUE(isDefault(fatNow()));
  uint8_t h, m;
  EXPECT_FALSE(halClock.getTime(h, m));
}

TEST_F(Clock, NtpWithAnUntrustedTimeIsNeverWritten) {
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  HalClock c;
  c.begin();
  fake::sntpCompleteAtMs = fake::nowMs;
  fake::sysTime = 0;  // 1970：SNTP 說完成但系統時間沒設好
  EXPECT_FALSE(c.syncFromNTP());
  EXPECT_EQ(fake::sets, 0);
}

TEST_F(Clock, NtpTimeoutFailsAfterAboutFiveSeconds) {
  HalClock c;
  c.begin();
  const uint32_t t0 = fake::nowMs;
  EXPECT_FALSE(c.syncFromNTP());  // SNTP 永遠不完成
  EXPECT_GE(fake::nowMs - t0, 5000u);
  EXPECT_LE(fake::nowMs - t0, 5100u);
  EXPECT_FALSE(c.syncRetryDue());
}

TEST_F(Clock, NoWifiIsAFailure) {
  fake::wifiConnected = false;
  HalClock c;
  c.begin();
  EXPECT_FALSE(c.syncFromNTP());
  EXPECT_EQ(fake::sets, 0);
}

// ---- 退避 ----

TEST_F(Clock, FailureBacksOffForAnHour) {
  HalClock c;
  c.begin();
  EXPECT_TRUE(c.syncRetryDue());
  EXPECT_FALSE(c.syncFromNTP());  // timeout
  EXPECT_FALSE(c.syncRetryDue());
  fake::nowMs += HalClock::kSyncBackoffMs - 10000;
  EXPECT_FALSE(c.syncRetryDue());
  fake::nowMs += 20000;
  EXPECT_TRUE(c.syncRetryDue());
}

TEST_F(Clock, AnySuccessfulSyncClearsTheBackoff) {  // codex 第二輪 10：手動成功要清掉自動的退避
  HalClock c;
  c.begin();
  EXPECT_FALSE(c.syncFromNTP());  // 自動的那次失敗 → 退避
  ASSERT_FALSE(c.syncRetryDue());
  ntpWillAnswer(0, 2026, 9, 26, 12, 10, 0);
  ASSERT_TRUE(c.syncFromNTP());  // 手動校時成功
  EXPECT_TRUE(c.syncRetryDue());
}

TEST_F(Clock, BackoffSurvivesMillisOverflow) {
  // 失敗時（等 SNTP 5 秒之後）離溢位還有 30 分鐘 → 一小時的截止時間一定繞過 0：
  //   失敗當下 now 很大、截止時間很小 ——「now >= until」這種比法會馬上判成已到期
  fake::nowMs = 0xFFFFFFFFu - 5000u - 30u * 60u * 1000u;
  HalClock c;
  c.begin();
  EXPECT_FALSE(c.syncFromNTP());  // timeout
  // 前提：截止時間（now ＋ 一小時）真的繞過 0 —— 不成立這條測試就測不到東西
  ASSERT_LT(static_cast<uint32_t>(fake::nowMs + HalClock::kSyncBackoffMs), fake::nowMs);
  EXPECT_FALSE(c.syncRetryDue());
  fake::nowMs += 59u * 60u * 1000u;  // 繞過 0 之後、截止之前
  EXPECT_FALSE(c.syncRetryDue());
  fake::nowMs += 2u * 60u * 1000u;
  EXPECT_TRUE(c.syncRetryDue());
}

// ---- FAT 時戳（callback 讀的是全域 halClock，所以用全域那一個，照順序走一個情境） ----

TEST_F(Clock, FatStampFollowsTheMonotonicClockAndIsRevokedOnEvidence) {  // codex 第二輪 阻斷 2
  fake::setRtc(2026, 9, 26, 23, 0, 0);
  halClock.begin();
  ASSERT_NE(FsDateTime::callback2, nullptr);  // 開機時可信 → 註冊
  EXPECT_TRUE(is(fatNow(), 2026, 9, 26, 23, 0, 0));
  // 三小時沒有人呼叫 getTime（狀態列不畫時鐘）：時戳自己往前推，跨過午夜也對
  fake::nowMs += 3u * 3600u * 1000u;
  EXPECT_TRUE(is(fatNow(), 2026, 9, 27, 2, 0, 0));
  // 超過 24 小時沒有新的可信讀取 → 佔位時戳
  fake::nowMs += 22u * 3600u * 1000u;
  EXPECT_TRUE(isDefault(fatNow()));
  // 新的可信讀取 → 重新發布
  fake::setRtc(2026, 9, 28, 1, 2, 4);
  uint8_t h, m;
  ASSERT_TRUE(halClock.getTime(h, m));
  EXPECT_TRUE(is(fatNow(), 2026, 9, 28, 1, 2, 4));
  // 讀到確定壞的時間 → 立刻撤銷
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  EXPECT_EQ(halClock.probe(), HalClock::State::Invalid);
  EXPECT_TRUE(isDefault(fatNow()));
  // 校時成功 → 回來
  ntpWillAnswer(0, 2026, 9, 28, 5, 6, 8);
  ASSERT_TRUE(halClock.syncFromNTP());
  EXPECT_TRUE(is(fatNow(), 2026, 9, 28, 5, 6, 8));
  // 讀回失敗的校時 → 撤銷
  fake::setSticks = false;
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  ntpWillAnswer(0, 2026, 9, 28, 6, 0, 0);
  EXPECT_FALSE(halClock.syncFromNTP());
  EXPECT_TRUE(isDefault(fatNow()));
  // 讀回失敗之後，晶片就算讀出合理的時間也不信（第三輪：交易沒完成就維持不信），直到下一次校時成功
  fake::setSticks = true;
  fake::setRtc(2026, 9, 28, 7, 0, 0);
  fake::nowMs += 11000;
  EXPECT_FALSE(halClock.getTime(h, m));
  EXPECT_TRUE(isDefault(fatNow()));
  fake::nowMs += HalClock::kSyncBackoffMs;
  ntpWillAnswer(0, 2026, 9, 28, 8, 0, 0);
  ASSERT_TRUE(halClock.syncFromNTP());
  // 一直讀不到超過 60 秒 → 撤銷（跟狀態列同一個判斷）
  fake::nowMs += 11000;
  ASSERT_TRUE(halClock.getTime(h, m));
  fake::i2cBroken = true;
  fake::nowMs += 61000;
  EXPECT_FALSE(halClock.getTime(h, m));
  EXPECT_TRUE(isDefault(fatNow()));
}

TEST_F(Clock, EveryReadFailurePathRevokesTheFatStampAfterSixtySeconds) {  // codex 第三輪 重要 1
  // 全域 halClock（callback 讀它）：自己 begin（不靠前一條測試），再校一次
  halClock.begin();
  ntpWillAnswer(0, 2026, 9, 26, 12, 0, 0);
  ASSERT_TRUE(halClock.syncFromNTP());
  uint8_t h, m;
  ASSERT_TRUE(halClock.getTime(h, m));
  EXPECT_TRUE(is(fatNow(), 2026, 9, 26, 12, 0, 0));
  // (a) 只有 probe() 在讀（狀態列關著）：讀不到超過 60 秒 → 撤銷
  fake::i2cBroken = true;
  fake::nowMs += 61000;
  EXPECT_EQ(halClock.probe(), HalClock::State::ReadFailed);
  EXPECT_TRUE(isDefault(fatNow()));
  // (b) 輪詢窗口內剛好過期：t+55 讀失敗（寬限內 → 照畫），t+61 在窗口內 → 不讀，但要撤銷
  fake::i2cBroken = false;
  fake::nowMs += HalClock::kSyncBackoffMs;
  ntpWillAnswer(0, 2026, 9, 26, 14, 0, 0);
  ASSERT_TRUE(halClock.syncFromNTP());  // 快取與 FAT @ t
  fake::i2cBroken = true;
  fake::nowMs += 55000;
  ASSERT_TRUE(halClock.getTime(h, m));  // t+55：讀失敗、寬限內
  EXPECT_FALSE(isDefault(fatNow()));
  fake::nowMs += 6000;  // t+61：離上一次輪詢 6 秒 → 窗口內
  EXPECT_FALSE(halClock.getTime(h, m));
  EXPECT_TRUE(isDefault(fatNow()));
}

// ---- 閱讀統計（2026-10-07）：trustedUtcNow 讀的是 FAT 時戳那份發布值，不鎖、不 I2C ----

TEST_F(Clock, TrustedUtcNowAdvancesFromThePublishedStampAndExpires) {
  HalClock c;
  c.begin();
  ASSERT_TRUE(shown(c).ok);  // 一次可信讀取 → 發布
  const uint32_t base = clockcache::epochOf(2026, 9, 26, 12, 0, 0);
  uint32_t e = 0;
  ASSERT_TRUE(c.trustedUtcNow(e, 600000));
  EXPECT_EQ(e, base);
  const int readsBefore = fake::reads;
  fake::nowMs += 90000;  // 不再讀 RTC：靠單調時鐘往前推
  ASSERT_TRUE(c.trustedUtcNow(e, 600000));
  EXPECT_EQ(e, base + 90);
  EXPECT_EQ(fake::reads, readsBefore) << "trustedUtcNow 不可以打 I2C";
  fake::nowMs += 600000;  // 發布值超過 maxAge → 不給
  EXPECT_FALSE(c.trustedUtcNow(e, 600000));
}

TEST_F(Clock, TrustedUtcNowIsRevokedWhenTheChipReadsAResetTime) {
  HalClock c;
  c.begin();
  ASSERT_TRUE(shown(c).ok);
  uint32_t e = 0;
  ASSERT_TRUE(c.trustedUtcNow(e, 600000));
  fake::setRtc(2000, 1, 1, 0, 0, 0);
  fake::nowMs += 11000;  // 過了輪詢窗，下一次 getTime 會真的讀
  EXPECT_FALSE(shown(c).ok);
  EXPECT_FALSE(c.trustedUtcNow(e, 600000)) << "不可信的時間要撤銷，不能再靠舊值往前推";
}

TEST_F(Clock, TrustedUtcNowWithoutAnRtcIsAlwaysFalse) {
  fake::rtcPresent = false;
  HalClock c;
  c.begin();
  uint32_t e = 0;
  EXPECT_FALSE(c.trustedUtcNow(e, 600000));
}
