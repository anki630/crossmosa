#pragma once

// 單字卡的排程核心（純計算，不碰 SD／時鐘／畫面，主機上的 gtest 直接測：test/anki_srs）。
//
// 排程照 Anki 傳統的 SM-2（v2 排程器）簡化：
//   新卡／學習中：學習步驟 1 分、10 分；「良好」走下一步，走完畢業（1 天），「簡單」直接畢業（4 天）
//   複習：重來＝遺忘（易度 −20%、重學 10 分、間隔回 1 天）；困難 ×1.2、易度 −15%；
//         良好 ×易度；簡單 ×易度×1.3、易度 +15%；三者保證嚴格遞增
//   一天從凌晨 4 點開始算（同 Anki 預設）
//
// 時間一律用「本地 epoch 秒」（UTC 加上時區設定），所以 dayOf() 的 4 點是當地時間。
//
// 進度檔 .srs 的格式（little-endian）：
//   0..255   SrsHeader（kHeaderSize）
//   256..    每張卡 kRecSize（12）bytes，順序與 .xdk 的卡片順序相同；全零＝新卡

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace anki {

enum class CardState : uint8_t { New = 0, Learning = 1, Review = 2, Relearning = 3 };
enum class Rating : uint8_t { Again = 1, Hard = 2, Good = 3, Easy = 4 };

constexpr uint32_t kDayRolloverSec = 4u * 3600u;
constexpr uint32_t kLearnAheadSec = 20u * 60u;  // 沒有別的卡時，20 分鐘內到期的學習卡可以提早出（同 Anki）

struct Config {
  static constexpr uint8_t kMaxSteps = 4;
  uint32_t learnSteps[kMaxSteps] = {60, 600, 0, 0};  // 秒
  uint8_t learnStepCount = 2;
  uint32_t relearnStep = 600;
  uint16_t graduatingIvl = 1;
  uint16_t easyIvl = 4;
  uint16_t startEase = 2500;  // 千分比：2500 ＝ 2.5
  uint16_t minEase = 1300;
  uint16_t maxIvl = 36500;
  uint16_t hardFactor = 1200;  // 千分比
  uint16_t easyBonus = 1300;   // 千分比
};

// 磁碟上 12 bytes 的一張卡（欄位依序；手動編解碼，不靠 struct 排列）
struct CardRec {
  CardState state = CardState::New;
  uint8_t step = 0;    // 學習步驟（Learning／Relearning）
  uint16_t ease = 0;   // 千分比；新卡 0
  uint32_t due = 0;    // Review：日序號；Learning／Relearning：本地 epoch 秒；New：不用
  uint16_t ivl = 0;    // 天
  uint8_t lapses = 0;  // 遺忘次數（到 255 停）
  uint8_t reps = 0;    // 複習次數（到 255 停）

  bool operator==(const CardRec& o) const {
    return state == o.state && step == o.step && ease == o.ease && due == o.due && ivl == o.ivl &&
           lapses == o.lapses && reps == o.reps;
  }
  bool isLearning() const { return state == CardState::Learning || state == CardState::Relearning; }
};

constexpr size_t kRecSize = 12;
constexpr size_t kHeaderSize = 256;
constexpr int kHistDays = 64;

inline void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline void encodeRec(const CardRec& c, uint8_t out[kRecSize]) {
  out[0] = static_cast<uint8_t>(c.state);
  out[1] = c.step;
  put16(out + 2, c.ease);
  put32(out + 4, c.due);
  put16(out + 8, c.ivl);
  out[10] = c.lapses;
  out[11] = c.reps;
}

inline CardRec decodeRec(const uint8_t in[kRecSize]) {
  CardRec c;
  c.state = in[0] <= 3 ? static_cast<CardState>(in[0]) : CardState::New;
  c.step = in[1];
  c.ease = get16(in + 2);
  c.due = get32(in + 4);
  c.ivl = get16(in + 8);
  c.lapses = in[10];
  c.reps = in[11];
  return c;
}

// 本地 epoch 秒 → Anki 日序號（凌晨 4 點換日）
inline uint32_t dayOf(uint32_t localEpoch) {
  return localEpoch < kDayRolloverSec ? 0 : (localEpoch - kDayRolloverSec) / 86400u;
}
// 這個日序號的結束時刻（隔天凌晨 4 點，本地 epoch 秒）
inline uint32_t dayEnd(uint32_t day) { return (day + 1) * 86400u + kDayRolloverSec; }

// ---------------------------------------------------------------- 排程

namespace detail {
inline uint32_t mix(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
// 間隔 ≥3 天時加 ±5%（至少 ±1 天）的確定性擾動，避免同一天學的卡永遠擠在同一天
inline uint32_t fuzz(uint32_t ivl, uint32_t seed) {
  if (ivl < 3) return ivl;
  uint32_t span = ivl / 20;
  if (span < 1) span = 1;
  const uint32_t r = mix(seed) % (2 * span + 1);
  return ivl + r - span;
}
inline uint16_t clampIvl(uint32_t ivl, const Config& cfg) {
  if (ivl < 1) ivl = 1;
  if (ivl > cfg.maxIvl) ivl = cfg.maxIvl;
  return static_cast<uint16_t>(ivl);
}
inline uint8_t inc8(uint8_t v) { return v == 255 ? v : static_cast<uint8_t>(v + 1); }
}  // namespace detail

// 對一張卡按下評分後的新狀態。nowLocal＝本地 epoch 秒；seed 用來做擾動（卡片序號即可）。
inline CardRec answer(CardRec c, const Rating r, const uint32_t nowLocal, const uint32_t seed,
                      const Config& cfg = Config{}) {
  const uint32_t today = dayOf(nowLocal);
  const uint8_t steps = cfg.learnStepCount == 0 ? 1 : cfg.learnStepCount;
  c.reps = detail::inc8(c.reps);

  auto graduate = [&](uint16_t ivl) {
    c.state = CardState::Review;
    c.step = 0;
    if (c.ease == 0) c.ease = cfg.startEase;
    c.ivl = detail::clampIvl(ivl, cfg);
    c.due = today + c.ivl;
  };

  switch (c.state) {
    case CardState::New:
    case CardState::Learning: {
      if (c.state == CardState::New) c.step = 0;
      c.state = CardState::Learning;
      if (c.step >= steps) c.step = static_cast<uint8_t>(steps - 1);
      switch (r) {
        case Rating::Again:
          c.step = 0;
          c.due = nowLocal + cfg.learnSteps[0];
          break;
        case Rating::Hard: {
          uint32_t delay = cfg.learnSteps[c.step];
          if (c.step == 0 && steps > 1) delay = (cfg.learnSteps[0] + cfg.learnSteps[1]) / 2;
          c.due = nowLocal + delay;
          break;
        }
        case Rating::Good:
          if (c.step + 1 >= steps) {
            graduate(cfg.graduatingIvl);
          } else {
            c.step = static_cast<uint8_t>(c.step + 1);
            c.due = nowLocal + cfg.learnSteps[c.step];
          }
          break;
        case Rating::Easy:
          graduate(cfg.easyIvl);
          break;
      }
      return c;
    }

    case CardState::Relearning:
      switch (r) {
        case Rating::Again:
          c.step = 0;
          c.due = nowLocal + cfg.relearnStep;
          break;
        case Rating::Hard:
          c.due = nowLocal + cfg.relearnStep * 3 / 2;
          break;
        case Rating::Good:
        case Rating::Easy:
          c.state = CardState::Review;
          c.step = 0;
          c.ivl = detail::clampIvl(c.ivl, cfg);
          c.due = today + c.ivl;
          break;
      }
      return c;

    case CardState::Review: {
      if (c.ease == 0) c.ease = cfg.startEase;
      const uint32_t ivl = c.ivl == 0 ? 1 : c.ivl;
      const uint32_t late = today > c.due ? today - c.due : 0;
      if (r == Rating::Again) {
        c.lapses = detail::inc8(c.lapses);
        c.ease = c.ease >= cfg.minEase + 200 ? static_cast<uint16_t>(c.ease - 200) : cfg.minEase;
        c.ivl = 1;
        c.state = CardState::Relearning;
        c.step = 0;
        c.due = nowLocal + cfg.relearnStep;
        return c;
      }
      // 三個間隔先全部算出來，保證 困難 < 良好 < 簡單（同 Anki v2）
      uint32_t hardIvl = ivl * cfg.hardFactor / 1000;
      if (hardIvl < ivl + 1) hardIvl = ivl + 1;
      uint32_t goodIvl = static_cast<uint32_t>((static_cast<uint64_t>(ivl) + late / 2) * c.ease / 1000);
      if (goodIvl < hardIvl + 1) goodIvl = hardIvl + 1;
      uint32_t easyIvl =
          static_cast<uint32_t>((static_cast<uint64_t>(ivl) + late) * c.ease / 1000 * cfg.easyBonus / 1000);
      if (easyIvl < goodIvl + 1) easyIvl = goodIvl + 1;

      uint32_t next = goodIvl;
      if (r == Rating::Hard) {
        next = hardIvl;
        c.ease = c.ease >= cfg.minEase + 150 ? static_cast<uint16_t>(c.ease - 150) : cfg.minEase;
      } else if (r == Rating::Easy) {
        next = easyIvl;
        c.ease = static_cast<uint16_t>(c.ease + 150);
      }
      c.ivl = detail::clampIvl(detail::fuzz(next, seed * 2654435761u + c.reps), cfg);
      c.due = today + c.ivl;
      return c;
    }
  }
  return c;
}

// ---------------------------------------------------------------- 進度檔檔頭

struct SrsHeader {
  uint32_t cardCount = 0;
  uint32_t deckId = 0;
  uint32_t counterDay = 0;  // newToday／revToday 屬於哪一天
  uint16_t newToday = 0;
  uint16_t revToday = 0;
  uint16_t newPerDay = 20;
  uint16_t maxReviews = 200;
  uint32_t lastEpoch = 0;     // 上一次寫入時的本地 epoch 秒（時鐘不可信時當下限用）
  uint32_t newRemaining = 0;  // 還沒學過的卡
  uint32_t histBaseDay = 0;   // hist[0] 是哪一天（更早到期的也算在 hist[0]）
  uint16_t learnCount = 0;    // 學習中／重學中的卡
  uint16_t hist[kHistDays] = {};  // 複習卡依到期日的張數（histBaseDay 起算，超過 64 天不記）

  // 今天（day）的新卡額度與已到期的複習數 —— 牌組清單只讀檔頭就能顯示
  uint32_t newAvailable(uint32_t day) const {
    const uint32_t done = counterDay == day ? newToday : 0;
    const uint32_t quota = newPerDay > done ? newPerDay - done : 0;
    return quota < newRemaining ? quota : newRemaining;
  }
  // 回傳 -1 ＝ 統計已過期（超過 64 天沒打開），要掃描才知道
  int32_t reviewDue(uint32_t day) const {
    if (day < histBaseDay) return 0;
    if (day - histBaseDay >= static_cast<uint32_t>(kHistDays)) return -1;
    uint32_t n = 0;
    for (uint32_t d = 0; d <= day - histBaseDay; ++d) n += hist[d];
    const uint32_t done = counterDay == day ? revToday : 0;
    const uint32_t quota = maxReviews > done ? maxReviews - done : 0;
    return static_cast<int32_t>(n < quota ? n : quota);
  }

  // 一張卡從 before 變成 after：增量更新統計（每次評分都呼叫，不必重掃整個檔）
  void account(const CardRec& before, const CardRec& after) {
    remove(before);
    add(after);
  }
  void add(const CardRec& c) { adjust(c, +1); }
  void remove(const CardRec& c) { adjust(c, -1); }

  // 換到新的一天：計數歸零（呼叫端判斷 counterDay != today）
  void rollDay(uint32_t today) {
    if (counterDay == today) return;
    counterDay = today;
    newToday = 0;
    revToday = 0;
  }

  void encode(uint8_t out[kHeaderSize]) const {
    memset(out, 0, kHeaderSize);
    memcpy(out, "XSR1", 4);
    put32(out + 4, cardCount);
    put32(out + 8, deckId);
    put32(out + 12, counterDay);
    put16(out + 16, newToday);
    put16(out + 18, revToday);
    put16(out + 20, newPerDay);
    put16(out + 22, maxReviews);
    put32(out + 24, lastEpoch);
    put32(out + 28, newRemaining);
    put32(out + 32, histBaseDay);
    put16(out + 36, learnCount);
    for (int i = 0; i < kHistDays; ++i) put16(out + 40 + 2 * i, hist[i]);
  }
  bool decode(const uint8_t in[kHeaderSize]) {
    if (memcmp(in, "XSR1", 4) != 0) return false;
    cardCount = get32(in + 4);
    deckId = get32(in + 8);
    counterDay = get32(in + 12);
    newToday = get16(in + 16);
    revToday = get16(in + 18);
    newPerDay = get16(in + 20);
    maxReviews = get16(in + 22);
    lastEpoch = get32(in + 24);
    newRemaining = get32(in + 28);
    histBaseDay = get32(in + 32);
    learnCount = get16(in + 36);
    for (int i = 0; i < kHistDays; ++i) hist[i] = get16(in + 40 + 2 * i);
    return true;
  }

  // 全掃描之前呼叫：統計歸零、hist 從 today 起算
  void beginRecount(uint32_t today) {
    newRemaining = 0;
    learnCount = 0;
    histBaseDay = today;
    memset(hist, 0, sizeof(hist));
  }

 private:
  void adjust(const CardRec& c, int sign) {
    switch (c.state) {
      case CardState::New:
        newRemaining = static_cast<uint32_t>(static_cast<int64_t>(newRemaining) + sign);
        break;
      case CardState::Learning:
      case CardState::Relearning:
        learnCount = static_cast<uint16_t>(learnCount + sign);
        break;
      case CardState::Review: {
        const uint32_t idx = c.due <= histBaseDay ? 0 : c.due - histBaseDay;
        if (idx < static_cast<uint32_t>(kHistDays)) hist[idx] = static_cast<uint16_t>(hist[idx] + sign);
        break;
      }
    }
  }
};

// ---------------------------------------------------------------- 卡面文字

// .xdk 卡面的樣式碼（每行第一個字元；轉換程式 scripts/anki/apkg2x3.py 產生）
enum class LineStyle : char {
  Head = 'H',     // 大字、置中
  Sub = 'S',      // 置中
  Tag = 'T',      // 小字、置中
  Body = 'B',     // 內文
  Example = 'E',  // 例句（縮排）
  Trans = 'C',    // 例句翻譯（縮排、小字）
  Meta = 'M',     // 附註（小字、靠右）
  Rule = 'R',     // 分隔線
};

}  // namespace anki
