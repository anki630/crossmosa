#pragma once

// 一次複習的出卡順序（純計算，主機 gtest：test/anki_srs）。
//
// 開始複習時掃一遍進度檔，把卡分到三個佇列：
//   學習中：今天結束前到期的學習／重學卡（含開始後才答錯、進入學習的卡）
//   複習：  今天以前到期的複習卡，最多到「今日複習上限」
//   新卡：  還沒學過的卡，照牌組順序取到「今日新卡數」
// 出卡：到期的學習卡優先 → 複習與新卡交錯（每 N 張複習夾一張新卡）→
//       20 分鐘內就到期的學習卡提早出 → 只剩還沒到期的學習卡就請使用者等 → 全部做完。

#include <cstdint>
#include <vector>

#include "AnkiSrs.h"

namespace anki {

enum class Source : uint8_t { None, Learn, Review, New };

class StudyQueue {
 public:
  struct LearnItem {
    uint32_t index;
    uint32_t due;  // 本地 epoch 秒
  };

  enum class Pick : uint8_t { Card, Wait, Done };

  // 掃描前呼叫。quota＝今天還能出多少張（已扣掉今天做過的）。
  void begin(const uint32_t today, const uint32_t newQuota, const uint32_t reviewQuota) {
    today_ = today;
    newQuota_ = newQuota;
    reviewQuota_ = reviewQuota;
    learn_.clear();
    review_.clear();
    new_.clear();
    learn_.reserve(32);
    review_.reserve(reviewQuota < 256 ? reviewQuota : 256);
    new_.reserve(newQuota < 64 ? newQuota : 64);
    reviewHead_ = 0;
    newHead_ = 0;
    sinceNew_ = 0;
  }

  // 掃描時每張卡呼叫一次（照卡片順序）
  void offer(const uint32_t index, const CardRec& c) {
    switch (c.state) {
      case CardState::New:
        if (new_.size() < newQuota_) new_.push_back(index);
        break;
      case CardState::Learning:
      case CardState::Relearning:
        if (c.due < dayEnd(today_)) learn_.push_back({index, c.due});
        break;
      case CardState::Review:
        if (c.due <= today_ && review_.size() < reviewQuota_) review_.push_back(index);
        break;
    }
  }

  // 掃描完呼叫：決定複習與新卡怎麼交錯
  void finish() {
    const size_t n = new_.size();
    gap_ = n == 0 ? 0 : static_cast<uint32_t>(review_.size() / n);
    if (n != 0 && gap_ < 1) gap_ = 1;
  }

  // 取下一張。Wait 時 waitUntil＝最早到期的學習卡時刻。
  Pick next(const uint32_t now, uint32_t& index, Source& src, uint32_t& waitUntil) {
    int li = earliestLearn();
    if (li >= 0 && learn_[li].due <= now) return takeLearn(li, index, src);

    const bool hasReview = reviewHead_ < review_.size();
    const bool hasNew = newHead_ < new_.size();
    if (hasReview || hasNew) {
      const bool pickNew = hasNew && (!hasReview || sinceNew_ >= gap_);
      if (pickNew) {
        sinceNew_ = 0;
        index = new_[newHead_++];
        src = Source::New;
      } else {
        ++sinceNew_;
        index = review_[reviewHead_++];
        src = Source::Review;
      }
      return Pick::Card;
    }

    if (li >= 0 && learn_[li].due <= now + kLearnAheadSec) return takeLearn(li, index, src);
    if (li >= 0) {
      waitUntil = learn_[li].due;
      return Pick::Wait;
    }
    return Pick::Done;
  }

  // 答完一張：還在學習、而且今天會再到期 → 放回學習佇列
  void answered(const uint32_t index, const CardRec& after) {
    if (after.isLearning() && after.due < dayEnd(today_)) learn_.push_back({index, after.due});
  }

  // 撤銷用（兩步）：
  //   ① unanswer：撤掉 answered() 為那張卡放回學習佇列的那一筆（那張卡接著直接出、不經過佇列）
  //   ② putBack：撤銷時畫面上那張還沒答的卡，放回它原本佇列的最前面（學習卡用它自己的到期時間）
  void unanswer(const uint32_t index) {
    for (size_t i = 0; i < learn_.size(); ++i) {
      if (learn_[i].index == index) {
        learn_.erase(learn_.begin() + static_cast<long>(i));
        return;
      }
    }
  }
  void putBack(const uint32_t index, const Source src, const uint32_t learnDue) {
    switch (src) {
      case Source::New:
        if (newHead_ > 0 && new_[newHead_ - 1] == index) {
          --newHead_;
        } else {
          new_.insert(new_.begin() + static_cast<long>(newHead_), index);
        }
        break;
      case Source::Review:
        if (reviewHead_ > 0 && review_[reviewHead_ - 1] == index) {
          --reviewHead_;
        } else {
          review_.insert(review_.begin() + static_cast<long>(reviewHead_), index);
        }
        break;
      case Source::Learn:
        learn_.push_back({index, learnDue});
        break;
      case Source::None:
        break;
    }
  }

  uint32_t newLeft() const { return static_cast<uint32_t>(new_.size() - newHead_); }
  uint32_t reviewLeft() const { return static_cast<uint32_t>(review_.size() - reviewHead_); }
  uint32_t learnLeft() const { return static_cast<uint32_t>(learn_.size()); }
  uint32_t today() const { return today_; }

 private:
  int earliestLearn() const {
    int best = -1;
    for (size_t i = 0; i < learn_.size(); ++i) {
      if (best < 0 || learn_[i].due < learn_[static_cast<size_t>(best)].due) best = static_cast<int>(i);
    }
    return best;
  }
  Pick takeLearn(const int li, uint32_t& index, Source& src) {
    index = learn_[static_cast<size_t>(li)].index;
    src = Source::Learn;
    learn_.erase(learn_.begin() + li);
    return Pick::Card;
  }

  uint32_t today_ = 0;
  uint32_t newQuota_ = 0;
  uint32_t reviewQuota_ = 0;
  std::vector<LearnItem> learn_;
  std::vector<uint32_t> review_;
  std::vector<uint32_t> new_;
  size_t reviewHead_ = 0;
  size_t newHead_ = 0;
  uint32_t gap_ = 0;
  uint32_t sinceNew_ = 0;
};

}  // namespace anki
