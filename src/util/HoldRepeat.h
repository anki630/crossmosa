#pragma once

// 一組導覽鍵（NavNext 或 NavPrevious）的「短按放開／長按連發」判斷（2026-10-08，v367 Formosa
// Cover）。純邏輯，電腦端測試在 test/formosa_cover。
//   為什麼不用 ButtonNavigator：它的長按看 getHeldTime()，那是【所有鍵共用】的計時 —— 先按住 ⑥ 再按 ④，④
//   會立刻被當成長按； 它也不記「這個畫面看過這顆鍵按下」，帶著按住的鍵進來或從彈窗回來，放開時會多走一格（codex 複查
//   v367）。 規則：只認在這裡看過按下的那一次；按住 kStartMs 起每 kIntervalMs 發一次 Long；放開時沒發過 Long 才算
//   Short。

#include <cstdint>

class HoldRepeat {
 public:
  enum class Action : uint8_t { None, Short, Long };
  static constexpr uint32_t kStartMs = 500;  // 同 ButtonNavigator（設定頁的長按換分類）
  static constexpr uint32_t kIntervalMs = 500;
  // 分頁列的長按換分頁（v380，diag379：長按換分頁 1.58 秒裡有 0.5 秒是在等門檻；⑥ 自然放開只要 0.11–0.15 秒）
  static constexpr uint32_t kTabStartMs = 300;

  // pressed／released：這一輪的邊緣；down：目前電平；now：毫秒；startMs：按住多久算長按
  Action update(const bool pressed, const bool released, const bool down, const uint32_t now,
                const uint32_t startMs = kStartMs) {
    if (pressed) {
      seen_ = true;
      downAt_ = now;
      lastLong_ = 0;
      longFired_ = false;
    }
    if (!seen_) return Action::None;
    if (released) {
      seen_ = false;
      return longFired_ ? Action::None : Action::Short;
    }
    if (down && now - downAt_ >= startMs && (!longFired_ || now - lastLong_ >= kIntervalMs)) {
      longFired_ = true;
      lastLong_ = now;
      return Action::Long;
    }
    if (!down) seen_ = false;  // 漏掉放開邊緣（例如彈窗吃掉了）：當作沒按過
    return Action::None;
  }

  // 彈窗打開／關掉、換畫面時：忘掉正在按的那一次（之後的放開不算）
  void reset() { seen_ = false; }

  // 這一次按下的時間（v379 換分頁量測用）
  uint32_t downAt() const { return downAt_; }

 private:
  bool seen_ = false;
  bool longFired_ = false;
  uint32_t downAt_ = 0;
  uint32_t lastLong_ = 0;
};
