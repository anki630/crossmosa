#pragma once

// v343（帳本 B12）：狀態列時鐘的「可信時間」判斷與快取 —— 純邏輯、不碰硬體，電腦端測試在 test/clock_cache。
//
// 事件（2026-09-26 實機）：RTC 裡是 2000-01-01 00:00（＋時區 +8 ＝ 狀態列永遠 08:00），SDK 的 now() 照樣回 true
// （它只看振盪器停止旗標）。另外舊的 getTime() 讀失敗時無限期沿用最後一次的時分 → 讀取一直失敗時畫面凍在同一個時間。
// 規則：
//   - 可信 ＝ 年份在 2025–2099 且每個欄位都在範圍內（暫存器壞掉可能讀出 13 月、25 時）。不可信 → 不畫（不畫錯的）。
//   - 快取記「上一次可信讀取的時刻（一天裡的第幾秒）」與當時的 millis()；顯示時用單調時鐘往前推 ——
//     讀取短暫失敗的那一分鐘內，畫的仍是正確的時間，而不是凍住的舊時間（codex v343 複查）。超過 kStaleMs 就不給時間。

#include <cstdint>

namespace clockcache {

constexpr uint16_t kMinTrustedYear = 2025;
constexpr uint16_t kMaxTrustedYear = 2099;
constexpr uint32_t kStaleMs = 60000;

inline bool yearTrusted(const uint16_t year) { return year >= kMinTrustedYear && year <= kMaxTrustedYear; }

inline bool isLeapYear(const uint16_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

inline uint8_t daysInMonth(const uint16_t y, const uint8_t m) {
  static constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  return m == 2 && isLeapYear(y) ? 29 : kDays[m - 1];
}

// codex v343 第三輪：日要照真實的公曆（2 月 31 日、4 月 31 日、平年的 2 月 29 日都不可信）——否則狀態列當它可信，
//   FAT 時戳的 epochOf 又會把它正規化成另一天。
inline bool trusted(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
                    const uint8_t minute, const uint8_t second) {
  return yearTrusted(year) && month >= 1 && month <= 12 && day >= 1 && day <= daysInMonth(year, month) && hour < 24 &&
         minute < 60 && second < 60;
}

// 1970-01-01 起的天數（公曆、UTC；Howard Hinnant 的 days_from_civil）。FAT 時戳往前推要用（callback 不能讀 I2C）。
inline int32_t daysFromCivil(int32_t y, const uint32_t m, const uint32_t d) {
  y -= m <= 2 ? 1 : 0;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153u * (m > 2 ? m - 3 : m + 9) + 2u) / 5u + d - 1u;
  const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

inline void civilFromDays(int32_t z, uint16_t& y, uint8_t& m, uint8_t& d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const uint32_t mp = (5u * doy + 2u) / 153u;
  d = static_cast<uint8_t>(doy - (153u * mp + 2u) / 5u + 1u);
  m = static_cast<uint8_t>(mp < 10u ? mp + 3u : mp - 9u);
  y = static_cast<uint16_t>(static_cast<int32_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0));
}

// UTC 秒數（1970 起；uint32 到 2106 年，可信範圍只到 2099）
inline uint32_t epochOf(const uint16_t y, const uint8_t mo, const uint8_t d, const uint8_t h, const uint8_t mi,
                        const uint8_t s) {
  return static_cast<uint32_t>(daysFromCivil(y, mo, d)) * 86400u + static_cast<uint32_t>(h) * 3600u +
         static_cast<uint32_t>(mi) * 60u + s;
}

inline void civilFromEpoch(const uint32_t t, uint16_t& y, uint8_t& mo, uint8_t& d, uint8_t& h, uint8_t& mi,
                           uint8_t& s) {
  civilFromDays(static_cast<int32_t>(t / 86400u), y, mo, d);
  const uint32_t r = t % 86400u;
  h = static_cast<uint8_t>(r / 3600u);
  mi = static_cast<uint8_t>((r / 60u) % 60u);
  s = static_cast<uint8_t>(r % 60u);
}

class Cache {
 public:
  // 讀得到而且可信的一次讀取（呼叫端先用 trusted() 判過）
  void onGood(const uint32_t nowMs, const uint8_t hour, const uint8_t minute, const uint8_t second) {
    valid_ = true;
    secOfDay_ = static_cast<uint32_t>(hour) * 3600u + static_cast<uint32_t>(minute) * 60u + second;
    atMs_ = nowMs;
  }
  // 讀得到但不可信（RTC 被歸零）→ 立刻不給時間
  void invalidate() { valid_ = false; }
  bool valid() const { return valid_; }

  // 上一次可信讀取往前推到 nowMs。沒有可信讀取、或已經超過 kStaleMs → false。
  // nowMs − atMs 用無號相減：跨一次 millis() 溢位也對（49.7 天一輪；淺睡眠 30 分鐘後就真關機，一次開機活不到一輪）。
  bool current(const uint32_t nowMs, uint8_t& hour, uint8_t& minute) const {
    if (!valid_) return false;
    const uint32_t elapsed = nowMs - atMs_;
    if (elapsed >= kStaleMs) return false;
    const uint32_t s = (secOfDay_ + elapsed / 1000u) % 86400u;
    hour = static_cast<uint8_t>(s / 3600u);
    minute = static_cast<uint8_t>((s / 60u) % 60u);
    return true;
  }

 private:
  bool valid_ = false;
  uint32_t secOfDay_ = 0;
  uint32_t atMs_ = 0;
};

}  // namespace clockcache
