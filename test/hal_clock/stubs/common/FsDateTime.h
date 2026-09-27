#pragma once
// 電腦端替身：SdFat 的日期巨集與 callback 註冊（格式同 FAT：年-1980<<9 | 月<<5 | 日；時<<11 | 分<<5 | 秒/2）。
#include <cstdint>
#define FS_DATE(year, month, day) static_cast<uint16_t>(((year) - 1980) << 9 | (month) << 5 | (day))
#define FS_TIME(hour, minute, second) static_cast<uint16_t>((hour) << 11 | (minute) << 5 | (second) >> 1)
#define FS_DEFAULT_DATE FS_DATE(2026, 1, 1)
#define FS_DEFAULT_TIME FS_TIME(0, 0, 0)
namespace FsDateTime {
inline void (*callback2)(uint16_t* date, uint16_t* time) = nullptr;
inline void setCallback(void (*f)(uint16_t* date, uint16_t* time)) { callback2 = f; }
}  // namespace FsDateTime
