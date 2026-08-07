#pragma once

#include <cstdint>

// Latches remote-button press/release events so they survive (a) the main
// loop's render stalls — a page render blocks input polls for ~1.3 s — and
// (b) InputManager's composite-state debounce, which only commits a bit seen
// by >=2 polls spanning >5 ms (freeink-sdk InputManager.cpp update()).
//
// Pure logic, no Arduino: the poll timestamp is a parameter, and locking is
// the caller's job (poll() runs on the main task via the button hook while
// pressEvent()/releaseEvent() run on the NimBLE host task).
//
// One follow-up click per button is queued while a previous one is still
// draining; further clicks collapse into it (turning three impatient clicks
// during a render into two page turns, never a surprise backlog).
class BleButtonLatch {
 public:
  static constexpr uint8_t kButtonCount = 7;  // HalGPIO::BTN_* index space
  static constexpr uint8_t kMinAssertPolls = 3;
  static constexpr uint32_t kMinAssertMs = 12;  // 2x InputManager DEBOUNCE_DELAY + margin
  static constexpr uint8_t kMinReleasePolls = 3;
  static constexpr uint32_t kMinReleaseMs = 12;

  void pressEvent(uint8_t buttonIndex);
  void releaseEvent(uint8_t buttonIndex);
  // Called once per InputManager poll (from the button hook). Returns the
  // bitmask to OR into the raw button state.
  uint8_t poll(uint32_t nowMs);
  void reset();

 private:
  enum class Phase : uint8_t { Idle, Asserted, ReleaseGap };
  struct Slot {
    Phase phase = Phase::Idle;
    bool physicalDown = false;  // remote currently reports the key held
    bool pendingPress = false;  // a click waiting for its assertion window
    bool pendingFollowUp = false;  // a second click queued while first is draining
    uint8_t polls = 0;
    uint32_t phaseStartMs = 0;
  };
  Slot slots_[kButtonCount];
};
