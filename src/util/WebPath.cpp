#include "WebPath.h"

#include <utility>

namespace WebPath {

namespace {
// SdFat 解析一段路徑時的去頭去尾（FatFile::parsePathName）：開頭空白、結尾的點與空白都不算名字。
// 去完是空的 ＝ SdFat 開不起來的段。
bool namesSomething(std::string_view seg) {
  size_t begin = 0;
  while (begin < seg.size() && seg[begin] == ' ') begin++;
  size_t end = seg.size();
  while (end > begin && (seg[end - 1] == '.' || seg[end - 1] == ' ')) end--;
  return end > begin;
}

bool hasForbiddenByte(std::string_view s) { return s.find_first_of(std::string_view("\\\0", 2)) != s.npos; }
}  // namespace

bool canonicalize(const std::string_view in, std::string& out) {
  if (hasForbiddenByte(in)) return false;
  std::string result;
  result.reserve(in.size() + 1);
  size_t pos = 0;
  while (pos < in.size()) {
    const size_t slash = in.find('/', pos);
    const size_t end = slash == in.npos ? in.size() : slash;
    const std::string_view seg = in.substr(pos, end - pos);
    if (!seg.empty()) {
      if (!namesSomething(seg)) return false;
      result += '/';
      result.append(seg.data(), seg.size());
    }
    pos = end + 1;
  }
  if (result.empty()) result = "/";
  out = std::move(result);
  return true;
}

bool isSafeComponent(const std::string_view name) {
  return name.find('/') == name.npos && !hasForbiddenByte(name) && namesSomething(name);
}

}  // namespace WebPath
