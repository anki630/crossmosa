#pragma once

#include <Arduino.h>
#include <Rtc.h>

#include <atomic>
#include <mutex>

#include "ClockCache.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  // v343（codex 複查）：RTC 的 I2C 操作與快取狀態跨任務共用（render 任務的狀態列、主任務的校時／探測）→ 一把鎖。
  //   只包 I2C 與狀態更新；等 SNTP 的那幾秒不持鎖（否則狀態列會卡住）。FAT callback 不拿這把鎖（它只讀下面的原子值）。
  mutable std::mutex _mu;
  mutable clockcache::Cache _cache;  // v343：上一次可信讀取＋單調時鐘往前推（ClockCache.h）
  mutable bool _polled = false;
  mutable uint32_t _lastPollMs = 0;
  // v343（codex 第二輪）：校時失敗後的退避（只在 RAM）。任何一次校時成功（自動或手動）就清掉。0 ＝ 沒有退避。
  std::atomic<uint32_t> _syncRetryAfterMs{0};
  // v343（codex 第二、三輪）：校時寫入是一個交易 ——
  // 開始寫就記「這顆晶片現在給的時間不能信」，只有「讀回可信、而且跟寫進去的
  //   差 ≤
  //   kSyncReadbackToleranceSec」才解除。寫到一半失敗（可能被改了一半）、讀回讀不到、讀回不合理、讀回不一致，都維持不信：
  //   之後讀到的合理時間也當不可信，直到下一次校時成功（持有 _mu 時讀寫；只在 RAM，重開機就重來）。
  mutable bool _chipDistrusted = false;

  // v194：FAT 時戳 callback 只讀這裡，不准 I2C／LOG／寫檔（SdFat 在持鎖改目錄時同步呼叫）。
  // v343（codex 第二輪）：發布的是「上一次可信讀取的 UTC 秒數＋當時的 millis()」，callback 自己用單調時鐘往前推
  //   （以前停在最後一次發布的時分：狀態列不畫時鐘時可以停好幾個小時）。兩個 32 位元值用 seqlock 一起發布，
  //   讀的一方不會看到「新秒數配舊 millis」。秒數 0 ＝ 撤銷（時間確定不可信）→ SdFat 的佔位時戳；
  //   往前推超過 kFatMaxAgeMs 也退回佔位值。
  mutable std::atomic<uint32_t> _fatSeq{0};
  mutable std::atomic<uint32_t> _fatEpoch{0};
  mutable std::atomic<uint32_t> _fatAtMs{0};
  mutable bool _fatCbInstalled = false;
  static constexpr uint32_t kFatMaxAgeMs = 24u * 3600u * 1000u;
  void publishFatStamp(const Rtc::DateTime& dt, uint32_t nowMs) const;
  void revokeFatStamp() const;
  // seqlock 讀發布值（FAT callback 與 trustedUtcNow 共用；不鎖、不 I2C）。讀不到一致的一份、或已撤銷 → false。
  bool readPublished(uint32_t& epoch, uint32_t& atMs) const;
  // 以下三個在持有 _mu 時呼叫
  void noteTrusted(const Rtc::DateTime& dt, uint32_t nowMs) const;  // 可信讀取 → 快取＋FAT
  void noteUntrusted() const;                                       // 確定不可信 → 快取失效＋撤銷 FAT
  void noteReadFailed(uint32_t nowMs) const;                        // 讀不到：上一次可信讀取超過 60 秒 → 撤銷 FAT
  void noteSyncFailed();

  static bool isTrusted(const Rtc::DateTime& dt) {
    return clockcache::trusted(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  }
  static uint32_t epochOf(const Rtc::DateTime& dt) {
    return clockcache::epochOf(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  }

  void installFatDateTimeCallbackIfKnown();
  static void fatDateTimeCb(uint16_t* date, uint16_t* time);

  static constexpr uint32_t CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // v343（帳本 B12）：「時間可不可信」。RTC 被歸零（實機 2026-09-26：2000-01-01 00:00 ＋ 時區 +8 ＝ 狀態列永遠
  // 08:00）時
  //   SDK 的 now() 照樣回 true（它只看振盪器停止旗標），所以要自己看：年份早於 2025 或晚於 2099、或任一欄位超出範圍
  //   ＝ 沒有時間（clockcache::trusted）。Invalid ＝ 讀得到但不可信。
  enum class State : uint8_t { Absent = 0, ReadFailed = 1, Invalid = 2, Ok = 3 };
  static const char* stateName(State s);
  // 現讀一次並分類（讀不到會隔 2 ms 重讀一次，一次短暫的 I2C 失誤不算）；yearOut 帶回讀到的年份（讀不到＝0）。
  // 讀到可信的時間 → 順便更新顯示快取與 FAT 時戳；讀到確定不可信的時間 → 立刻撤銷兩者（codex 第二輪）。
  // 會打 I2C —— 不要在 render 裡呼叫（設定頁在進頁面時探測一次）。
  State probe(uint16_t* yearOut = nullptr) const;

  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available, or the time is not trustworthy (v343: no trusted read within the last
  // clockcache::kStaleMs, or the chip reads a reset/garbage time) — callers then draw no time. Between reads the
  // last trusted time is advanced with millis(), so a short run of failed reads never freezes the display.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available or the time is not trustworthy (see getTime).
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // 閱讀統計（2026-10-07）：上一次可信讀取的 UTC 秒數往前推到現在 —— 讀的是 FAT 時戳那份 seqlock 發布值，
  //   【不鎖 _mu、不打 I2C】，render 任務裡可以呼叫。沒有 RTC、時間不可信（已撤銷）、或發布值比 maxAgeMs 舊 → false。
  //   發布值由 getTime()（每 CLOCK_POLL_MS 一次 I2C）與 probe() 更新：要新鮮度的人在主任務定期呼叫 getTime()。
  bool trustedUtcNow(uint32_t& epochOut, uint32_t maxAgeMs) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated — v343: the NTP time must be trusted, and the chip is read back
  // after the write and must match what was written (±kSyncReadbackToleranceSec); anything else is a failure.
  // Every failure starts the kSyncBackoffMs backoff that syncRetryDue() reports; every success clears it.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();
  static constexpr uint32_t kSyncBackoffMs = 3600000u;
  static constexpr uint32_t kSyncReadbackToleranceSec = 5;
  // v343：自動校時（時間不可信的那一條）現在可不可以再試 —— 上一次失敗後 kSyncBackoffMs 內不再試。
  bool syncRetryDue() const;
  // v343：現在是不是「校時交易沒完成、晶片不信」（開機證人 CLK boot dirty= 用）
  bool chipDistrusted() const {
    std::lock_guard<std::mutex> lk(_mu);
    return _chipDistrusted;
  }
};
