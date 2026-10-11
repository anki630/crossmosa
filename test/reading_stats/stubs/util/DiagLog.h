#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
namespace DiagLog {
inline std::string last;
inline int lines = 0;
inline bool line(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  last = buf;
  ++lines;
  return true;
}
}  // namespace DiagLog
