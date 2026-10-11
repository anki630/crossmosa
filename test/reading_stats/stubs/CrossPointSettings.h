#pragma once
#include <cstdint>
struct FakeSettings {
  uint8_t clockUtcOffsetQ = 48;  // 48 ＝ UTC+0
};
inline FakeSettings SETTINGS;
