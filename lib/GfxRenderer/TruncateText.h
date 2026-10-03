#pragma once

// v361（上游 #3573 的想法）：加省略號的截斷改二分搜尋。原本從尾巴一個字一個字刪、每刪一次就量一次整串寬度，
//   標題越長越貴（字數平方次的字形量測）；二分只要量 log2(字數) 次。
//   跟 GfxRenderer 分開放是為了主機測試（test/truncate_text）—— 寬度怎麼量由呼叫端給。
#include <cstddef>
#include <string>

namespace TruncateText {

// width(const std::string&) -> int。
// 整串寬度 ≤ maxWidth（含等於）→ 原樣回傳。
// 否則回傳「最長的前綴＋ellipsis」，寬度要【嚴格小於】maxWidth；一個字都放不下就只回 ellipsis。
// 兩個邊界都跟原本的線性版相同。前綴以 UTF-8 碼位為單位，不會切在半個字上。
// 假設前綴越長越寬；字距讓它不單調時，回傳的仍然放得下，只是不一定是最長的那個。
template <typename WidthFn>
std::string fit(const std::string& text, const int maxWidth, const char* ellipsis, WidthFn&& width) {
  if (width(text) <= maxWidth) return text;

  size_t codepoints = 0;
  for (const unsigned char c : text) {
    if ((c & 0xC0) != 0x80) codepoints++;
  }

  std::string candidate;
  const auto setCandidate = [&](const size_t keep) {
    size_t end = 0;
    for (size_t n = 0; n < keep && end < text.size(); n++) {
      end++;
      while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) end++;
    }
    candidate.assign(text, 0, end);
    candidate += ellipsis;
  };

  // 不變量：low 個字＋省略號放得下（low == 0 時不必放得下 —— 那就是只回省略號的情況）。
  size_t low = 0;
  size_t high = codepoints;
  while (low < high) {
    const size_t mid = low + (high - low + 1) / 2;
    setCandidate(mid);
    if (width(candidate) < maxWidth) {
      low = mid;
    } else {
      high = mid - 1;
    }
  }
  setCandidate(low);
  return candidate;
}

}  // namespace TruncateText
