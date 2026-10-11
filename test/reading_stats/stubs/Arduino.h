#pragma once
// 電腦端替身：ReadingStats.cpp 只用到 millis()。
#include <cstdint>
namespace fakers {
inline uint32_t nowMs = 0;
}
inline uint32_t millis() { return fakers::nowMs; }
