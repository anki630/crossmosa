// v343（帳本 B12）：狀態列時鐘「可信時間」的判斷與快取（lib/hal/ClockCache.h）。
// 裝置上這段邏輯由 HalClock 包著 I2C 與鎖呼叫；這裡只測純邏輯 ——
// 任何一條改壞（年份界線、欄位檢查、過期、往前推、溢位）都要變紅。

#include <gtest/gtest.h>

#include <ctime>

#include "hal/ClockCache.h"

using clockcache::Cache;
using clockcache::trusted;
using clockcache::yearTrusted;

TEST(ClockTrust, YearBounds) {
  EXPECT_FALSE(yearTrusted(2000));  // 實機 2026-09-26：RTC 被歸零成 2000-01-01
  EXPECT_FALSE(yearTrusted(2024));
  EXPECT_TRUE(yearTrusted(2025));
  EXPECT_TRUE(yearTrusted(2026));
  EXPECT_TRUE(yearTrusted(2099));
  EXPECT_FALSE(yearTrusted(2100));
}

TEST(ClockTrust, EveryFieldIsChecked) {
  EXPECT_TRUE(trusted(2026, 9, 26, 16, 30, 59));
  EXPECT_FALSE(trusted(2000, 1, 1, 0, 0, 0));
  EXPECT_FALSE(trusted(2026, 0, 26, 16, 30, 0));   // 月 0
  EXPECT_FALSE(trusted(2026, 13, 26, 16, 30, 0));  // 月 13
  EXPECT_FALSE(trusted(2026, 9, 0, 16, 30, 0));    // 日 0
  EXPECT_FALSE(trusted(2026, 9, 32, 16, 30, 0));   // 日 32
  EXPECT_FALSE(trusted(2026, 9, 26, 24, 30, 0));   // 時 24
  EXPECT_FALSE(trusted(2026, 9, 26, 16, 60, 0));   // 分 60
  EXPECT_FALSE(trusted(2026, 9, 26, 16, 30, 60));  // 秒 60
  EXPECT_TRUE(trusted(2026, 12, 31, 23, 59, 59));
  EXPECT_TRUE(trusted(2026, 1, 1, 0, 0, 0));
  // 真實的公曆（codex 第三輪）
  EXPECT_FALSE(trusted(2026, 2, 31, 12, 0, 0));
  EXPECT_FALSE(trusted(2026, 4, 31, 12, 0, 0));
  EXPECT_FALSE(trusted(2026, 2, 29, 12, 0, 0));  // 平年
  EXPECT_TRUE(trusted(2028, 2, 29, 12, 0, 0));   // 閏年
  EXPECT_TRUE(trusted(2026, 2, 28, 12, 0, 0));
  EXPECT_TRUE(trusted(2026, 4, 30, 12, 0, 0));
}

TEST(ClockTrust, DaysInMonthMatchesLibc) {
  // 獨立實作對照：libc 的 timegm 把「下個月 1 日的前一天」正規化出來的日
  for (uint16_t y = 2025; y <= 2099; ++y) {
    for (uint8_t m = 1; m <= 12; ++m) {
      struct tm t{};
      t.tm_year = y - 1900 + (m == 12 ? 1 : 0);
      t.tm_mon = m == 12 ? 0 : m;
      t.tm_mday = 0;  // 0 ＝ 上個月的最後一天
      t.tm_hour = 12;
      timegm(&t);
      ASSERT_EQ(clockcache::daysInMonth(y, m), t.tm_mday) << y << "-" << int(m);
    }
  }
}

TEST(ClockCache, NoGoodReadNoTime) {
  Cache c;
  uint8_t h = 99, m = 99;
  EXPECT_FALSE(c.current(12345, h, m));
  EXPECT_EQ(h, 99);  // 沒有時間時不碰輸出
  EXPECT_EQ(m, 99);
}

TEST(ClockCache, AdvancesWithTheMonotonicClockInsteadOfFreezing) {
  Cache c;
  c.onGood(1000, 12, 58, 50);
  uint8_t h = 0, m = 0;
  ASSERT_TRUE(c.current(1000, h, m));
  EXPECT_EQ(h, 12);
  EXPECT_EQ(m, 58);
  ASSERT_TRUE(c.current(1000 + 9999, h, m));  // 12:58:59.999
  EXPECT_EQ(m, 58);
  ASSERT_TRUE(c.current(1000 + 10000, h, m));  // 12:59:00 —— 舊程式會一直畫 12:58
  EXPECT_EQ(h, 12);
  EXPECT_EQ(m, 59);
  ASSERT_TRUE(c.current(1000 + 59999, h, m));  // 12:59:49.999
  EXPECT_EQ(m, 59);
}

TEST(ClockCache, StaleAfterSixtySeconds) {
  Cache c;
  c.onGood(5000, 8, 0, 0);
  uint8_t h = 0, m = 0;
  EXPECT_TRUE(c.current(5000 + clockcache::kStaleMs - 1, h, m));
  EXPECT_FALSE(c.current(5000 + clockcache::kStaleMs, h, m));
  EXPECT_FALSE(c.current(5000 + 3600000, h, m));  // 一小時讀不到：絕不再畫 08:00
}

TEST(ClockCache, InvalidateIsImmediate) {
  Cache c;
  c.onGood(0, 10, 0, 0);
  c.invalidate();  // 讀得到但年份不可信
  uint8_t h = 0, m = 0;
  EXPECT_FALSE(c.valid());
  EXPECT_FALSE(c.current(1, h, m));
  c.onGood(2, 10, 0, 1);  // 校時之後又可信
  EXPECT_TRUE(c.current(3, h, m));
}

TEST(ClockCache, WrapsAtMidnight) {
  Cache c;
  c.onGood(0, 23, 59, 50);
  uint8_t h = 0, m = 0;
  ASSERT_TRUE(c.current(20000, h, m));  // 23:59:50 + 20 s
  EXPECT_EQ(h, 0);
  EXPECT_EQ(m, 0);
}

TEST(ClockCache, SurvivesMillisOverflow) {
  Cache c;
  const uint32_t nearWrap = 0xFFFFFFFFu - 4999u;  // 溢位前 5 秒
  c.onGood(nearWrap, 7, 30, 0);
  uint8_t h = 0, m = 0;
  ASSERT_TRUE(c.current(nearWrap + 30000u, h, m));  // 溢位後 25 秒（無號相加自然繞回）
  EXPECT_EQ(h, 7);
  EXPECT_EQ(m, 30);
  EXPECT_FALSE(c.current(nearWrap + clockcache::kStaleMs, h, m));
}

// ---- 日期換算：拿電腦 libc 的 timegm／gmtime_r 當獨立實作對照（不是拿同一份程式碼算答案） ----
#include <ctime>

TEST(ClockDates, EpochMatchesLibcTimegmEveryDayFrom1970To2106) {
  // 1970-01-01 起每一天（到 uint32 秒數的上限附近）：epochOf ＝ timegm，civilFromEpoch ＝ gmtime_r
  for (uint32_t day = 0; day < 49710; ++day) {
    const time_t t = static_cast<time_t>(day) * 86400 + 12 * 3600 + 34 * 60 + 56;
    struct tm g{};
    gmtime_r(&t, &g);
    const uint32_t e = clockcache::epochOf(static_cast<uint16_t>(g.tm_year + 1900), static_cast<uint8_t>(g.tm_mon + 1),
                                           static_cast<uint8_t>(g.tm_mday), 12, 34, 56);
    ASSERT_EQ(static_cast<time_t>(e), timegm(&g)) << "day " << day;
    uint16_t y;
    uint8_t mo, d, h, mi, s;
    clockcache::civilFromEpoch(e, y, mo, d, h, mi, s);
    ASSERT_EQ(y, g.tm_year + 1900) << "day " << day;
    ASSERT_EQ(mo, g.tm_mon + 1) << "day " << day;
    ASSERT_EQ(d, g.tm_mday) << "day " << day;
    ASSERT_EQ(h, 12);
    ASSERT_EQ(mi, 34);
    ASSERT_EQ(s, 56);
  }
}

TEST(ClockDates, KnownValues) {
  EXPECT_EQ(clockcache::epochOf(1970, 1, 1, 0, 0, 0), 0u);
  EXPECT_EQ(clockcache::epochOf(2000, 1, 1, 0, 0, 0), 946684800u);       // 實機歸零的那個時刻
  EXPECT_EQ(clockcache::epochOf(2024, 2, 29, 23, 59, 59), 1709251199u);  // 閏日
  EXPECT_EQ(clockcache::epochOf(2099, 12, 31, 23, 59, 59), 4102444799u);
}
