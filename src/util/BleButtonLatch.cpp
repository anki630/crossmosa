#include "util/BleButtonLatch.h"

void BleButtonLatch::pressEvent(uint8_t buttonIndex) {
  if (buttonIndex >= kButtonCount) {
    return;
  }
  Slot& s = slots_[buttonIndex];
  s.physicalDown = true;
  // Queue one click. If we're starting fresh in Idle with no pending press,
  // set pendingPress. Otherwise, queue into the one follow-up slot.
  if (s.phase == Phase::Idle && !s.pendingPress) {
    s.pendingPress = true;
  } else {
    s.pendingFollowUp = true;
  }
}

void BleButtonLatch::releaseEvent(uint8_t buttonIndex) {
  if (buttonIndex >= kButtonCount) {
    return;
  }
  slots_[buttonIndex].physicalDown = false;
}

uint8_t BleButtonLatch::poll(uint32_t nowMs) {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < kButtonCount; i++) {
    Slot& s = slots_[i];

    if (s.phase == Phase::Idle && (s.pendingPress || s.physicalDown)) {
      s.pendingPress = false;  // consume the current pending press
      s.phase = Phase::Asserted;
      s.polls = 0;
      s.phaseStartMs = nowMs;
    }

    if (s.phase == Phase::Asserted) {
      if (s.polls < 255) {
        s.polls++;
      }
      mask |= static_cast<uint8_t>(1u << i);
      const bool holdSatisfied = s.polls >= kMinAssertPolls && (nowMs - s.phaseStartMs) >= kMinAssertMs;
      if (!s.physicalDown && holdSatisfied) {
        s.phase = Phase::ReleaseGap;
        s.polls = 0;
        s.phaseStartMs = nowMs;
      }
    } else if (s.phase == Phase::ReleaseGap) {
      if (s.polls < 255) {
        s.polls++;
      }
      if (s.polls >= kMinReleasePolls && (nowMs - s.phaseStartMs) >= kMinReleaseMs) {
        s.phase = Phase::Idle;
        // Promote the queued follow-up to be the next pending press.
        if (s.pendingFollowUp) {
          s.pendingFollowUp = false;
          s.pendingPress = true;
        }
      }
    }
  }
  return mask;
}

void BleButtonLatch::reset() {
  for (auto& s : slots_) {
    s = Slot{};
  }
}
