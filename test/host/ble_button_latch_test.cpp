// Host-side unit test for BleButtonLatch (pure logic, no HAL).
#include "util/BleButtonLatch.h"

#include <cstdio>

namespace {
int gFailures = 0;

void expectEq(const char* what, long long got, long long want) {
  if (got == want) {
    std::printf("PASS (%s): %lld\n", what, got);
    return;
  }
  std::printf("FAIL (%s): got %lld, want %lld\n", what, got, want);
  gFailures++;
}

constexpr uint8_t kUp = 4;           // HalGPIO::BTN_UP mirror
constexpr uint8_t kUpBit = 1u << 4;

// Counts assertion phases over a scripted poll sequence.
int countAssertions(BleButtonLatch& l, uint32_t startMs, uint32_t stepMs, int pollCount) {
  int assertions = 0;
  bool wasSet = false;
  for (int i = 0; i < pollCount; i++) {
    const bool set = (l.poll(startMs + static_cast<uint32_t>(i) * stepMs) & kUpBit) != 0;
    if (set && !wasSet) assertions++;
    wasSet = set;
  }
  return assertions;
}
}  // namespace

int main() {
  {  // Live click at normal 10 ms cadence: assert >=3 polls, then clean gap.
    BleButtonLatch l;
    l.pressEvent(kUp);
    expectEq("live p1", l.poll(0) & kUpBit, kUpBit);
    expectEq("live p2", l.poll(10) & kUpBit, kUpBit);
    l.releaseEvent(kUp);
    // polls=3 and 20ms >= 12ms at the third poll: bit still returned there,
    // release gap starts after it.
    expectEq("live p3", l.poll(20) & kUpBit, kUpBit);
    expectEq("gap p1", l.poll(30) & kUpBit, 0);
    expectEq("gap p2", l.poll(40) & kUpBit, 0);
    expectEq("gap p3", l.poll(50) & kUpBit, 0);
  }

  {  // Click landing entirely inside a 1.3 s render stall is not lost.
    BleButtonLatch l;
    l.pressEvent(kUp);
    l.releaseEvent(kUp);  // both events before any poll ran
    expectEq("stall p1", l.poll(1300) & kUpBit, kUpBit);
    expectEq("stall p2", l.poll(1310) & kUpBit, kUpBit);
    expectEq("stall p3", l.poll(1320) & kUpBit, kUpBit);
    expectEq("stall done", l.poll(1330) & kUpBit, 0);
  }

  {  // Fast polls (skipLoopDelay: ~1 ms apart) must hold until >=12 ms elapse.
    BleButtonLatch l;
    l.pressEvent(kUp);
    l.releaseEvent(kUp);
    int held = 0;
    for (int t = 0; t <= 20; t++) {
      if (l.poll(static_cast<uint32_t>(t)) & kUpBit) held++;
    }
    // Asserted from t=0 through at least t=12 (13 polls) before the gap.
    expectEq("fast hold >=12ms", held >= 13 ? 1 : 0, 1);
    expectEq("fast eventually released", (l.poll(40) & kUpBit) | (l.poll(50) & kUpBit), 0);
  }

  {  // Double click during a stall replays as exactly two assertions.
    BleButtonLatch l;
    l.pressEvent(kUp);
    l.releaseEvent(kUp);
    l.pressEvent(kUp);
    l.releaseEvent(kUp);
    expectEq("double click", countAssertions(l, 0, 10, 20), 2);
  }

  {  // Triple+ clicks collapse into two (one live + one queued).
    BleButtonLatch l;
    for (int i = 0; i < 5; i++) {
      l.pressEvent(kUp);
      l.releaseEvent(kUp);
    }
    expectEq("clicks collapse", countAssertions(l, 0, 10, 30), 2);
  }

  {  // Long press: bit held while physicalDown, released after.
    BleButtonLatch l;
    l.pressEvent(kUp);
    for (int i = 0; i < 100; i++) {
      expectEq("hold", (l.poll(static_cast<uint32_t>(i) * 10) & kUpBit) != 0 ? 1 : 0, 1);
    }
    l.releaseEvent(kUp);
    expectEq("hold last", l.poll(1000) & kUpBit, kUpBit);  // conditions met, gap next
    expectEq("hold released", l.poll(1010) & kUpBit, 0);
  }

  {  // Independent buttons don't interfere; out-of-range index is ignored.
    BleButtonLatch l;
    l.pressEvent(4);
    l.pressEvent(5);
    l.pressEvent(200);  // ignored
    const uint8_t m = l.poll(0);
    expectEq("two buttons", m, (1u << 4) | (1u << 5));
  }

  {  // reset() drops everything.
    BleButtonLatch l;
    l.pressEvent(kUp);
    l.reset();
    expectEq("reset", l.poll(0), 0);
  }

  {  // Regression: 2 clicks pre-poll + 1 click mid-drain → exactly 2 assertions.
    // This tests the one-follow-up-queued design. The third click must collapse
    // into the queued follow-up, not create a third assertion.
    BleButtonLatch l;
    l.pressEvent(kUp);
    l.releaseEvent(kUp);
    l.pressEvent(kUp);
    l.releaseEvent(kUp);
    int assertions = 0;
    bool wasSet = false;
    for (int i = 0; i < 10; i++) {
      const bool set = (l.poll(static_cast<uint32_t>(i) * 10) & kUpBit) != 0;
      if (set && !wasSet) assertions++;
      wasSet = set;
      // After first assertion drains (poll 5), inject a third press+release.
      if (i == 5) {
        l.pressEvent(kUp);
        l.releaseEvent(kUp);
      }
    }
    expectEq("regression: 2+1 click", assertions, 2);
  }

  std::printf(gFailures ? "ble_button_latch_test: %d FAILURE(S)\n" : "ble_button_latch_test: all passed\n",
              gFailures);
  return gFailures ? 1 : 0;
}
