// 單字卡排程核心 src/anki/AnkiSrs.h 的規則。

#include <gtest/gtest.h>

#include "src/anki/AnkiSrs.h"

using anki::answer;
using anki::CardRec;
using anki::CardState;
using anki::dayOf;
using anki::Rating;
using anki::SrsHeader;

namespace {
// 2026-11-14 10:00 本地時間左右（任意，只要過了凌晨 4 點）
constexpr uint32_t kNow = 20406u * 86400u + 10u * 3600u;
constexpr uint32_t kToday = 20406u;

CardRec reviewCard(uint16_t ivl, uint16_t ease = 2500) {
  CardRec c;
  c.state = CardState::Review;
  c.ivl = ivl;
  c.ease = ease;
  c.due = kToday;
  return c;
}
}  // namespace

TEST(AnkiSrs, DayRollsOverAtFourAm) {
  EXPECT_EQ(dayOf(kToday * 86400u + 4u * 3600u), kToday);
  EXPECT_EQ(dayOf(kToday * 86400u + 4u * 3600u - 1), kToday - 1);
  EXPECT_EQ(dayOf(0), 0u);
}

TEST(AnkiSrs, RecordRoundTrips) {
  CardRec c;
  c.state = CardState::Relearning;
  c.step = 1;
  c.ease = 2350;
  c.due = 0xA1B2C3D4;
  c.ivl = 400;
  c.lapses = 3;
  c.reps = 17;
  uint8_t buf[anki::kRecSize];
  anki::encodeRec(c, buf);
  EXPECT_EQ(anki::decodeRec(buf), c);
  uint8_t zeros[anki::kRecSize] = {};
  EXPECT_EQ(anki::decodeRec(zeros).state, CardState::New);
}

TEST(AnkiSrs, NewCardWalksLearningSteps) {
  CardRec c;
  c = answer(c, Rating::Good, kNow, 1);
  EXPECT_EQ(c.state, CardState::Learning);
  EXPECT_EQ(c.step, 1);
  EXPECT_EQ(c.due, kNow + 600);
  c = answer(c, Rating::Good, kNow + 600, 1);
  EXPECT_EQ(c.state, CardState::Review);
  EXPECT_EQ(c.ivl, 1);
  EXPECT_EQ(c.ease, 2500);
  EXPECT_EQ(c.due, kToday + 1);
}

TEST(AnkiSrs, NewCardAgainAndHard) {
  CardRec c = answer(CardRec{}, Rating::Again, kNow, 1);
  EXPECT_EQ(c.state, CardState::Learning);
  EXPECT_EQ(c.due, kNow + 60);
  c = answer(CardRec{}, Rating::Hard, kNow, 1);
  EXPECT_EQ(c.step, 0);
  EXPECT_EQ(c.due, kNow + 330);  // 1 分與 10 分的平均
}

TEST(AnkiSrs, EasyGraduatesImmediately) {
  const CardRec c = answer(CardRec{}, Rating::Easy, kNow, 1);
  EXPECT_EQ(c.state, CardState::Review);
  EXPECT_EQ(c.ivl, 4);
  EXPECT_EQ(c.due, kToday + 4);
}

TEST(AnkiSrs, ReviewIntervalsStrictlyIncrease) {
  for (uint16_t ivl : {1, 2, 5, 30, 200}) {
    const CardRec base = reviewCard(ivl);
    const CardRec hard = answer(base, Rating::Hard, kNow, 7);
    const CardRec good = answer(base, Rating::Good, kNow, 7);
    const CardRec easy = answer(base, Rating::Easy, kNow, 7);
    EXPECT_GT(hard.ivl, ivl) << ivl;
    // 擾動最多 ±5%：比較未擾動的下限
    EXPECT_GE(good.ivl * 21 / 20 + 1, hard.ivl) << ivl;
    EXPECT_GT(easy.ivl, ivl) << ivl;
    EXPECT_EQ(hard.ease, 2350);
    EXPECT_EQ(good.ease, 2500);
    EXPECT_EQ(easy.ease, 2650);
  }
  const CardRec good = answer(reviewCard(10), Rating::Good, kNow, 7);
  EXPECT_GE(good.ivl, 24);
  EXPECT_LE(good.ivl, 26);
}

TEST(AnkiSrs, LapseRelearnsThenReturns) {
  CardRec c = answer(reviewCard(40), Rating::Again, kNow, 3);
  EXPECT_EQ(c.state, CardState::Relearning);
  EXPECT_EQ(c.lapses, 1);
  EXPECT_EQ(c.ease, 2300);
  EXPECT_EQ(c.ivl, 1);
  EXPECT_EQ(c.due, kNow + 600);
  c = answer(c, Rating::Good, kNow + 600, 3);
  EXPECT_EQ(c.state, CardState::Review);
  EXPECT_EQ(c.due, kToday + 1);
}

TEST(AnkiSrs, EaseNeverBelowMinimum) {
  CardRec c = reviewCard(5, 1350);
  c = answer(c, Rating::Again, kNow, 1);
  EXPECT_EQ(c.ease, 1300);
  c = answer(reviewCard(5, 1350), Rating::Hard, kNow, 1);
  EXPECT_EQ(c.ease, 1300);
}

TEST(AnkiSrs, HeaderRoundTripsAndCounts) {
  SrsHeader h;
  h.cardCount = 100;
  h.deckId = 0xDEADBEEF;
  h.beginRecount(kToday);
  for (int i = 0; i < 100; ++i) h.add(CardRec{});
  EXPECT_EQ(h.newRemaining, 100u);
  h.rollDay(kToday);
  EXPECT_EQ(h.newAvailable(kToday), 20u);

  const CardRec before{};
  const CardRec after = answer(before, Rating::Easy, kNow, 1);  // 4 天後到期
  h.account(before, after);
  h.newToday++;
  EXPECT_EQ(h.newRemaining, 99u);
  EXPECT_EQ(h.newAvailable(kToday), 19u);
  EXPECT_EQ(h.reviewDue(kToday), 0);
  EXPECT_EQ(h.reviewDue(kToday + 4), 1);
  EXPECT_EQ(h.newAvailable(kToday + 1), 20u);  // 新的一天額度回來
  EXPECT_EQ(h.reviewDue(kToday + 64), -1);     // 統計過期

  uint8_t buf[anki::kHeaderSize];
  h.encode(buf);
  SrsHeader g;
  ASSERT_TRUE(g.decode(buf));
  EXPECT_EQ(g.deckId, h.deckId);
  EXPECT_EQ(g.newRemaining, 99u);
  EXPECT_EQ(g.reviewDue(kToday + 4), 1);
  buf[0] = 'Y';
  EXPECT_FALSE(g.decode(buf));
}

TEST(AnkiSrs, ReviewQuotaCapsDueCount) {
  SrsHeader h;
  h.beginRecount(kToday);
  h.maxReviews = 3;
  for (int i = 0; i < 10; ++i) h.add(reviewCard(1));
  h.rollDay(kToday);
  EXPECT_EQ(h.reviewDue(kToday), 3);
  h.revToday = 2;
  EXPECT_EQ(h.reviewDue(kToday), 1);
}

// ---- 出卡順序（src/anki/AnkiQueue.h）

#include "src/anki/AnkiQueue.h"

using anki::Source;
using anki::StudyQueue;

namespace {
CardRec learning(uint32_t due) {
  CardRec c;
  c.state = CardState::Learning;
  c.due = due;
  return c;
}
}  // namespace

TEST(AnkiQueue, QuotasAndInterleave) {
  StudyQueue q;
  q.begin(kToday, 2, 4);
  for (uint32_t i = 0; i < 10; ++i) q.offer(i, CardRec{});       // 新卡 0..9，只取 2 張
  for (uint32_t i = 10; i < 20; ++i) q.offer(i, reviewCard(3));  // 到期的複習 10..19，只取 4 張
  CardRec future = reviewCard(3);
  future.due = kToday + 1;
  q.offer(20, future);  // 明天才到期：不出
  q.finish();
  EXPECT_EQ(q.newLeft(), 2u);
  EXPECT_EQ(q.reviewLeft(), 4u);

  // 4 張複習、2 張新卡 → 每 2 張複習夾 1 張新卡
  std::vector<Source> order;
  uint32_t idx = 0, wait = 0;
  Source src;
  while (q.next(kNow, idx, src, wait) == StudyQueue::Pick::Card) order.push_back(src);
  ASSERT_EQ(order.size(), 6u);
  EXPECT_EQ(order[0], Source::Review);
  EXPECT_EQ(order[1], Source::Review);
  EXPECT_EQ(order[2], Source::New);
  EXPECT_EQ(order[5], Source::New);
}

TEST(AnkiQueue, LearningDueFirstThenWait) {
  StudyQueue q;
  q.begin(kToday, 0, 10);
  q.offer(1, reviewCard(3));
  q.offer(2, learning(kNow - 5));                   // 已到期
  q.offer(3, learning(kNow + 3600));                // 一小時後
  q.offer(4, learning(anki::dayEnd(kToday) + 10));  // 明天：不算今天
  q.finish();
  EXPECT_EQ(q.learnLeft(), 2u);

  uint32_t idx = 0, wait = 0;
  Source src;
  ASSERT_EQ(q.next(kNow, idx, src, wait), StudyQueue::Pick::Card);
  EXPECT_EQ(idx, 2u);
  EXPECT_EQ(src, Source::Learn);
  ASSERT_EQ(q.next(kNow, idx, src, wait), StudyQueue::Pick::Card);
  EXPECT_EQ(idx, 1u);
  ASSERT_EQ(q.next(kNow, idx, src, wait), StudyQueue::Pick::Wait);
  EXPECT_EQ(wait, kNow + 3600);
  // 20 分鐘內到期就提早出
  ASSERT_EQ(q.next(kNow + 3600 - 60, idx, src, wait), StudyQueue::Pick::Card);
  EXPECT_EQ(idx, 3u);
  EXPECT_EQ(q.next(kNow, idx, src, wait), StudyQueue::Pick::Done);
}

TEST(AnkiQueue, AnsweredAgainComesBackAndUndoRestores) {
  StudyQueue q;
  q.begin(kToday, 1, 0);
  q.offer(7, CardRec{});
  q.finish();
  uint32_t idx = 0, wait = 0;
  Source src;
  ASSERT_EQ(q.next(kNow, idx, src, wait), StudyQueue::Pick::Card);
  EXPECT_EQ(src, Source::New);
  const CardRec after = answer(CardRec{}, Rating::Again, kNow, idx);
  q.answered(idx, after);
  EXPECT_EQ(q.learnLeft(), 1u);
  EXPECT_EQ(q.newLeft(), 0u);

  q.unanswer(7);
  q.putBack(7, Source::New, 0);
  EXPECT_EQ(q.learnLeft(), 0u);
  EXPECT_EQ(q.newLeft(), 1u);
  ASSERT_EQ(q.next(kNow, idx, src, wait), StudyQueue::Pick::Card);
  EXPECT_EQ(idx, 7u);
  EXPECT_EQ(src, Source::New);
}
