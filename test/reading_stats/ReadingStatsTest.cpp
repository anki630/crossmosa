// 閱讀統計・第一步（2026-10-07）：ReadingStatsCore.h 的規則，每一條對應三輪審查的一個點。
// 把核心對應的那一段改壞，這裡要變紅（交付前逐條注入過）。

#include <gtest/gtest.h>

#include "src/util/ReadingStatsCore.h"

using readingstats::BookRec;
using readingstats::DayInfo;
using readingstats::kBlobSize;
using readingstats::kCapMs;
using readingstats::LoadResult;
using readingstats::Recorder;
using readingstats::Snapshot;

namespace {

constexpr uint64_t A = 0x1111;
constexpr uint64_t B = 0x2222;
const DayInfo kNoClock{};
DayInfo day(uint32_t d) { return DayInfo{true, d}; }

constexpr uint32_t kDay0 = 20368;  // 2025-10-07 左右，任意的可信日

}  // namespace

// ---- 量測 ----

TEST(ReadingStats, TimeBetweenTwoBodyRendersIsCounted) {
  Recorder r;
  r.observe(A, true, 0, 0, 0, 1000, day(kDay0));   // 開書：只起算
  r.observe(A, true, 0, 1, 0, 61000, day(kDay0));  // 翻頁：結算 60 秒
  EXPECT_EQ(r.bookSeconds(A), 60u);
  EXPECT_EQ(r.snapshot().totalSec, 60u);
  EXPECT_EQ(r.daySeconds(day(kDay0)), 60u);
  EXPECT_EQ(r.snapshot().books[0].navs, 1u);
  EXPECT_TRUE(r.dirty());
}

TEST(ReadingStats, EachIntervalIsCappedAtFiveMinutes) {  // 書開著人走開不灌時數
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, kNoClock);
  r.observe(A, true, 0, 1, 0, 3 * 3600 * 1000, kNoClock);
  EXPECT_EQ(r.bookSeconds(A), kCapMs / 1000u);
  EXPECT_EQ(r.diag().capped, 1u);
  EXPECT_EQ(r.diag().gaps, 1u);  // 超過 30 分鐘：有一條路漏了 pause
}

TEST(ReadingStats, PauseIsIdempotentAndSleepIsNotCounted) {  // 第一輪：pause 結構性排除淺睡眠
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, kNoClock);
  r.pause(30000, kNoClock);
  r.pause(40000, kNoClock);  // 第二次：什麼都不做
  r.pause(900000, kNoClock);
  EXPECT_EQ(r.bookSeconds(A), 30u);
  EXPECT_FALSE(r.running());
}

TEST(ReadingStats, SamePageRedrawAfterWakeResumesTiming) {  // 第一輪阻斷：醒來只重畫同一頁
  Recorder r;
  r.observe(A, true, 0, 5, 0, 0, kNoClock);
  r.pause(10000, kNoClock);                       // 入睡
  r.observe(A, true, 0, 5, 0, 600000, kNoClock);  // 醒來重畫同一頁：從現在起算
  EXPECT_TRUE(r.running());
  r.observe(A, true, 0, 6, 0, 620000, kNoClock);  // 讀 20 秒後翻頁
  EXPECT_EQ(r.bookSeconds(A), 30u);               // 10 ＋ 20，睡著的 590 秒不算
  EXPECT_EQ(r.snapshot().books[0].navs, 1u);      // 醒來重畫不算翻頁
}

TEST(ReadingStats, SamePageRedrawWhileRunningDoesNotRestartTheClock) {  // 選單、截圖的同頁重畫
  Recorder r;
  r.observe(A, true, 0, 5, 0, 0, kNoClock);
  r.observe(A, true, 0, 5, 0, 20000, kNoClock);
  r.observe(A, true, 0, 6, 0, 50000, kNoClock);
  EXPECT_EQ(r.bookSeconds(A), 50u);
}

TEST(ReadingStats, ReflowIsNotAPageTurn) {  // 第一、二輪：改字級、轉向
  Recorder r;
  r.observe(A, true, 2, 10, 0, 0, kNoClock);
  r.observe(A, true, 2, 14, 1, 40000, kNoClock);  // 世代變了：位置變動是重排
  EXPECT_EQ(r.bookSeconds(A), 40u);               // 時間照算
  EXPECT_EQ(r.snapshot().books[0].navs, 0u);      // 不算翻頁
  EXPECT_EQ(r.diag().reflows, 1u);
  r.observe(A, true, 2, 15, 1, 50000, kNoClock);  // 之後同世代的翻頁照算
  EXPECT_EQ(r.snapshot().books[0].navs, 1u);
}

TEST(ReadingStats, EndOfBookSettlesOnceAndStops) {  // 第三輪：結書 nav 只在「正文 → 結書」加一次
  Recorder r;
  r.observe(A, true, 0, 99, 0, 0, kNoClock);
  r.endOfBook(30000, kNoClock);
  r.endOfBook(90000, kNoClock);  // 結書畫面重畫（子畫面返回、建議載入）
  r.endOfBook(400000, kNoClock);
  EXPECT_EQ(r.bookSeconds(A), 30u);
  EXPECT_EQ(r.snapshot().books[0].navs, 1u);
  EXPECT_FALSE(r.running());
  r.observe(A, true, 0, 99, 0, 500000, kNoClock);  // 從結書回到最後一頁：不再算一次翻頁，從現在起算
  EXPECT_EQ(r.snapshot().books[0].navs, 1u);
  EXPECT_TRUE(r.running());
}

TEST(ReadingStats, SwitchingBooksSettlesTheOldOne) {
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, kNoClock);
  r.observe(B, true, 0, 0, 0, 25000, kNoClock);
  r.observe(B, true, 0, 1, 0, 35000, kNoClock);
  EXPECT_EQ(r.bookSeconds(A), 25u);
  EXPECT_EQ(r.bookSeconds(B), 10u);
}

TEST(ReadingStats, MillisecondRemaindersAreKeptPerAccumulator) {  // 第二、三輪：短區間不能反覆捨去
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, day(kDay0));
  uint32_t t = 0;
  for (uint32_t p = 1; p <= 10; ++p) {
    t += 1500;
    r.observe(A, true, 0, p, 0, t, day(kDay0));
  }
  EXPECT_EQ(r.bookSeconds(A), 15u);
  EXPECT_EQ(r.snapshot().totalSec, 15u);
  EXPECT_EQ(r.daySeconds(day(kDay0)), 15u);
}

TEST(ReadingStats, PerBookRemaindersSurviveSwitchingBooks) {  // 程式碼複查阻斷 2：零頭不能因換書被清掉
  Recorder r;
  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) {  // A、B 輪流各讀 500 ms
    r.observe(i % 2 ? B : A, true, 0, 0, 0, t, kNoClock);
    t += 500;
  }
  r.pause(t, kNoClock);
  EXPECT_EQ(r.bookSeconds(A), 5u);
  EXPECT_EQ(r.bookSeconds(B), 5u);
  EXPECT_EQ(r.snapshot().totalSec, 10u);
}

TEST(ReadingStats, LeavingAndReopeningTheSameBookIsANewSession) {  // 程式碼複查：A → 首頁 → A
  Recorder r;
  r.observe(A, true, 0, 7, 0, 0, kNoClock);
  const uint32_t seqBefore = r.snapshot().seq;
  r.endSession(10000, kNoClock);
  r.observe(A, true, 0, 7, 0, 50000, kNoClock);
  EXPECT_EQ(r.snapshot().seq, seqBefore + 1);
  EXPECT_EQ(r.snapshot().books[0].lastSeq, r.snapshot().seq);
  EXPECT_EQ(r.snapshot().books[0].navs, 0u);  // 重開不算翻頁
  r.observe(A, true, 0, 8, 0, 60000, kNoClock);
  EXPECT_EQ(r.bookSeconds(A), 20u);  // 首頁那 40 秒不算
}

TEST(ReadingStats, WithoutACardIdOnlyTotalsAndDaysAreKept) {  // 讀不到 CID：不記每本
  Recorder r;
  r.observe(A, false, 0, 0, 0, 0, day(kDay0));
  r.observe(A, false, 0, 1, 0, 20000, day(kDay0));
  EXPECT_EQ(r.bookSeconds(A), 0u);
  for (const BookRec& b : r.snapshot().books) EXPECT_EQ(b.key, 0u);
  EXPECT_EQ(r.snapshot().totalSec, 20u);
  EXPECT_EQ(r.daySeconds(day(kDay0)), 20u);
}

// ---- 每日 ----

TEST(ReadingStats, NoClockMeansNoDailyRecord) {  // X4 沒有 RTC
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, kNoClock);
  r.observe(A, true, 0, 1, 0, 20000, kNoClock);
  EXPECT_EQ(r.snapshot().daysValid, 0u);
  EXPECT_EQ(r.snapshot().totalSec, 20u);
}

TEST(ReadingStats, NewDayClearsSkippedSlots) {
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, day(kDay0));
  r.observe(A, true, 0, 1, 0, 60000, day(kDay0));
  r.observe(A, true, 0, 2, 0, 120000, day(kDay0 + 3));  // 隔三天
  EXPECT_EQ(r.daySeconds(day(kDay0)), 60u);
  EXPECT_EQ(r.daySeconds(day(kDay0 + 1)), 0u);
  EXPECT_EQ(r.daySeconds(day(kDay0 + 3)), 60u);
  // 32 天後同一格不能把舊資料當成今天的
  r.observe(A, true, 0, 3, 0, 180000, day(kDay0 + 32));
  EXPECT_EQ(r.daySeconds(day(kDay0 + 32)), 60u);
  EXPECT_EQ(r.daySeconds(day(kDay0)), 0u);
}

TEST(ReadingStats, ClockGoingBackwardsIsNotAttributed) {  // 第二輪：RTC 重設、改時區
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, day(kDay0 + 5));
  r.observe(A, true, 0, 1, 0, 10000, day(kDay0 + 5));
  r.observe(A, true, 0, 2, 0, 20000, day(kDay0 + 2));  // 往回跳
  EXPECT_EQ(r.daySeconds(day(kDay0 + 5)), 10u);
  EXPECT_EQ(r.daySeconds(day(kDay0 + 2)), 0u);
  EXPECT_EQ(r.diag().backDays, 1u);
  EXPECT_EQ(r.snapshot().totalSec, 20u);  // 總數照算
}

// ---- 最近 10 本 ----

TEST(ReadingStats, EleventhBookEvictsTheLeastRecentlyOpened) {
  Recorder r;
  uint32_t t = 0;
  for (uint64_t k = 1; k <= 11; ++k) {
    r.observe(k, true, 0, 0, 0, t, kNoClock);
    t += 10000;
  }
  r.pause(t, kNoClock);
  EXPECT_EQ(r.bookSeconds(1), 0u);  // 最早開的那本被淘汰
  EXPECT_EQ(r.bookSeconds(2), 10u);
  EXPECT_EQ(r.bookSeconds(11), 10u);
}

TEST(ReadingStats, ReopeningABookRefreshesItsRecency) {
  Recorder r;
  uint32_t t = 0;
  for (uint64_t k = 1; k <= 10; ++k) {
    r.observe(k, true, 0, 0, 0, t, kNoClock);
    t += 10000;
  }
  r.observe(1, true, 0, 0, 0, t, kNoClock);  // 重開第 1 本
  t += 10000;
  r.observe(11, true, 0, 0, 0, t, kNoClock);
  r.pause(t + 10000, kNoClock);
  EXPECT_EQ(r.bookSeconds(2), 0u);  // 換成第 2 本被淘汰
  EXPECT_GT(r.bookSeconds(1), 0u);
}

TEST(ReadingStats, EvictedBookDoesNotPassItsRemainderToTheNewcomer) {
  Recorder r;
  uint32_t t = 0;
  r.observe(1, true, 0, 0, 0, t, kNoClock);
  t += 900;                             // 第 1 本（存取序號最小）留下 900 ms 零頭
  for (uint64_t k = 2; k <= 10; ++k) {  // 再開 9 本，10 格滿
    r.observe(k, true, 0, 0, 0, t, kNoClock);
    t += 10000;
  }
  r.observe(11, true, 0, 0, 0, t, kNoClock);  // 第 11 本進來：淘汰第 1 本那一格
  t += 200;
  r.pause(t, kNoClock);
  EXPECT_EQ(r.bookSeconds(1), 0u);
  EXPECT_EQ(r.bookSeconds(11), 0u);  // 200 ms：不能繼承被淘汰那格的 900 ms 變成 1 秒
}

TEST(ReadingStats, RenameMergeCarriesBothRemainders) {
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, kNoClock);
  r.pause(10600, kNoClock);  // A：10 秒＋600 ms
  r.observe(B, true, 0, 0, 0, 20000, kNoClock);
  r.pause(25500, kNoClock);  // B：5 秒＋500 ms
  r.rename(A, B);
  EXPECT_EQ(r.bookSeconds(B), 16u);  // 10＋5＋（600＋500 → 1 秒）
}

TEST(ReadingStats, RenameMovesOrMergesTheTotals) {  // 第三輪阻斷 2：自動搬到 /Read
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, kNoClock);
  r.observe(A, true, 0, 1, 0, 30000, kNoClock);
  r.pause(30000, kNoClock);
  r.rename(A, B);
  EXPECT_EQ(r.bookSeconds(A), 0u);
  EXPECT_EQ(r.bookSeconds(B), 30u);
  // 新 key 已存在 → 相加
  constexpr uint64_t C = 0x3333;
  r.observe(C, true, 0, 0, 0, 40000, kNoClock);
  r.observe(C, true, 0, 1, 0, 50000, kNoClock);
  r.pause(50000, kNoClock);
  r.rename(C, B);
  EXPECT_EQ(r.bookSeconds(B), 40u);
  EXPECT_EQ(r.bookSeconds(C), 0u);
}

// ---- 持久化 ----

TEST(ReadingStats, MarkSavedOnlyClearsDirtyIfNothingChangedSince) {  // 第三輪：mutation generation
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, kNoClock);
  r.observe(A, true, 0, 1, 0, 10000, kNoClock);
  const uint32_t gen = r.mutationGen();
  r.observe(A, true, 0, 2, 0, 20000, kNoClock);  // 寫 NVS 的期間又有變動
  r.markSaved(gen);
  EXPECT_TRUE(r.dirty());
  r.markSaved(r.mutationGen());
  EXPECT_FALSE(r.dirty());
}

TEST(ReadingStats, FailedSaveRetriesTheWholeSnapshotWithoutDoubleCounting) {  // 存快照不存增量
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, kNoClock);
  r.observe(A, true, 0, 1, 0, 10000, kNoClock);
  r.pause(10000, kNoClock);
  // 第一次存失敗：什麼都不做（不 markSaved）；第二次存的是同一份完整狀態
  const Snapshot first = r.snapshot();
  r.pause(20000, kNoClock);  // 冪等的 pause 不能讓下一次快照多算
  EXPECT_EQ(r.snapshot().totalSec, first.totalSec);
  EXPECT_TRUE(r.dirty());
}

TEST(ReadingStats, EncodeDecodeRoundTrip) {
  Recorder r;
  r.observe(A, true, 0, 0, 0, 0, day(kDay0));
  r.observe(A, true, 0, 1, 0, 70000, day(kDay0));
  r.observe(B, true, 0, 0, 0, 80000, day(kDay0 + 1));
  r.pause(95000, day(kDay0 + 1));
  uint8_t buf[kBlobSize];
  readingstats::encode(r.snapshot(), buf);
  Snapshot back;
  ASSERT_EQ(readingstats::decode(buf, sizeof(buf), back), LoadResult::Compatible);
  Recorder r2;
  r2.load(back);
  EXPECT_FALSE(r2.dirty());
  EXPECT_EQ(r2.bookSeconds(A), r.bookSeconds(A));
  EXPECT_EQ(r2.bookSeconds(B), r.bookSeconds(B));
  EXPECT_EQ(r2.snapshot().totalSec, r.snapshot().totalSec);
  EXPECT_EQ(r2.daySeconds(day(kDay0)), 70u);
  EXPECT_EQ(r2.daySeconds(day(kDay0 + 1)), 25u);
  EXPECT_EQ(buf[4], static_cast<uint8_t>(r.snapshot().seq));  // little-endian
}

TEST(ReadingStats, DecodeIsThreeState) {  // 第三輪阻斷 3：比我新的不能當成壞掉
  Snapshot s;
  uint8_t buf[kBlobSize];
  readingstats::encode(Snapshot{}, buf);

  uint8_t newer[600] = {};
  newer[0] = 'R';
  newer[1] = 2;
  EXPECT_EQ(readingstats::decode(newer, sizeof(newer), s), LoadResult::Newer);
  EXPECT_EQ(readingstats::decode(newer, 2, s), LoadResult::Newer);

  EXPECT_EQ(readingstats::decode(buf, sizeof(buf) - 1, s), LoadResult::Corrupt);  // 我們的版本、長度錯
  uint8_t bad[kBlobSize];
  std::memcpy(bad, buf, sizeof(bad));
  bad[0] = 'X';
  EXPECT_EQ(readingstats::decode(bad, sizeof(bad), s), LoadResult::Corrupt);
  std::memcpy(bad, buf, sizeof(bad));
  bad[2] = 1;  // 保留旗標不是 0
  EXPECT_EQ(readingstats::decode(bad, sizeof(bad), s), LoadResult::Corrupt);
  EXPECT_EQ(readingstats::decode(buf, 1, s), LoadResult::Corrupt);
}

TEST(ReadingStats, DecodeRejectsDuplicateBookKeys) {
  Snapshot dup;
  dup.books[0].key = A;
  dup.books[3].key = A;
  uint8_t buf[kBlobSize];
  readingstats::encode(dup, buf);
  Snapshot s;
  EXPECT_EQ(readingstats::decode(buf, sizeof(buf), s), LoadResult::Corrupt);
}

TEST(ReadingStats, BlobLayoutIsStable) {  // 格式一旦進機器就不能悄悄變
  EXPECT_EQ(kBlobSize, 348u);
  uint8_t buf[kBlobSize];
  readingstats::encode(Snapshot{}, buf);
  EXPECT_EQ(buf[0], 'R');
  EXPECT_EQ(buf[1], 1);
}
