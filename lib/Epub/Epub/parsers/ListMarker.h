#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "../css/CssStyle.h"

// v362：<li> 前面的記號（含後綴「.」或「、」）。純函式，主機測試在 test/list_marker。
//   範圍照 CSS Counter Styles 3：字母從 1 起、羅馬數字 1–3999、中文 -9999–9999，超出就退回十進位（規格的 fallback）。
//   不配記憶體（重排視窗是記憶體最緊的時候），寫進呼叫端給的緩衝區。
namespace ListMarker {

// 最長的是「負九千九百九十九、」（27 bytes）與「MMMDCCCLXXXVIII.」（16）；十進位最多「-99999.」。
constexpr size_t MAX_BYTES = 32;

namespace detail {

// 整段寫得下才寫（不切斷 UTF-8）；寫不下就停在那裡，之後的也不寫（結果一定是完整記號的前綴）。
struct Writer {
  char* out;
  size_t cap;
  size_t pos = 0;
  bool full = false;
  void append(const char* s) {
    const size_t n = strlen(s);
    if (full || pos + n >= cap) {
      full = true;
      return;
    }
    memcpy(out + pos, s, n);
    pos += n;
    out[pos] = '\0';
  }
};

inline size_t decimal(const int n, char* out, const size_t cap, const bool leadingZero) {
  // decimal-leading-zero 補到兩位；負號算在兩位裡（CSS 的 pad），所以負數不補。
  const int len = snprintf(out, cap, (leadingZero && n >= 0 && n < 10) ? "0%d." : "%d.", n);
  if (len < 0) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(len) < cap ? static_cast<size_t>(len) : cap - 1;
}

// 字母：a … z、aa、ab …（雙射 26 進位）。n ≥ 1。
inline size_t alpha(const int n, char* out, const size_t cap, const char base) {
  char reversed[8];
  size_t len = 0;
  unsigned v = static_cast<unsigned>(n);
  while (v > 0 && len < sizeof(reversed)) {
    v -= 1;
    reversed[len++] = static_cast<char>(base + v % 26);
    v /= 26;
  }
  Writer w{out, cap};
  char one[2] = {0, 0};
  while (len > 0) {
    one[0] = reversed[--len];
    w.append(one);
  }
  w.append(".");
  return w.pos;
}

// 羅馬數字（加法制）。1 ≤ n ≤ 3999。
inline size_t roman(int n, char* out, const size_t cap, const bool upper) {
  struct Step {
    int value;
    const char* lower;
    const char* upper;
  };
  static constexpr Step kSteps[] = {{1000, "m", "M"}, {900, "cm", "CM"}, {500, "d", "D"}, {400, "cd", "CD"},
                                    {100, "c", "C"},  {90, "xc", "XC"},  {50, "l", "L"},  {40, "xl", "XL"},
                                    {10, "x", "X"},   {9, "ix", "IX"},   {5, "v", "V"},   {4, "iv", "IV"},
                                    {1, "i", "I"}};
  Writer w{out, cap};
  for (const Step& s : kSteps) {
    while (n >= s.value) {
      w.append(upper ? s.upper : s.lower);
      n -= s.value;
    }
  }
  w.append(".");
  return w.pos;
}

// 中文數字（cjk-ideographic＝trad-chinese-informal；simp-chinese-informal
// 只差負號「负」）：一、十、十一、二十、一百零一、一千零一十。
//   規則照 CSS Counter Styles 3 的中文長寫：中間連續的零併成一個「零」、結尾的零不寫、十到十九省略十位的「一」。
//   -9999 ≤ n ≤ 9999。
inline size_t cjk(int n, char* out, const size_t cap, const bool simplified) {
  static constexpr const char* kDigits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  static constexpr const char* kUnits[] = {"", "十", "百", "千"};
  Writer w{out, cap};
  if (n < 0) {
    w.append(simplified ? "负" : "負");
    n = -n;
  }
  if (n == 0) {
    w.append(kDigits[0]);
  } else {
    const int digits[4] = {n / 1000 % 10, n / 100 % 10, n / 10 % 10, n % 10};
    bool started = false;
    bool pendingZero = false;
    for (int i = 0; i < 4; i++) {
      const int d = digits[i];
      const int unit = 3 - i;
      if (d == 0) {
        if (started) pendingZero = true;
        continue;
      }
      if (pendingZero) {
        w.append(kDigits[0]);
        pendingZero = false;
      }
      if (!(unit == 1 && d == 1 && n <= 19)) w.append(kDigits[d]);
      w.append(kUnits[unit]);
      started = true;
    }
  }
  w.append("、");
  return w.pos;
}

}  // namespace detail

// 寫出第 n 項的記號，回傳位元組數；0＝不放記號（None）。cap 要 ≥ MAX_BYTES 才保證不截斷。
inline size_t format(const CssListStyleType type, const int n, char* out, const size_t cap) {
  if (cap == 0) return 0;
  out[0] = '\0';
  switch (type) {
    case CssListStyleType::None:
      return 0;
    case CssListStyleType::Disc: {
      detail::Writer w{out, cap};
      w.append("\xe2\x80\xa2");
      return w.pos;
    }
    case CssListStyleType::Decimal:
      return detail::decimal(n, out, cap, false);
    case CssListStyleType::DecimalLeadingZero:
      return detail::decimal(n, out, cap, true);
    case CssListStyleType::LowerAlpha:
    case CssListStyleType::UpperAlpha:
      if (n >= 1) return detail::alpha(n, out, cap, type == CssListStyleType::LowerAlpha ? 'a' : 'A');
      break;
    case CssListStyleType::LowerRoman:
    case CssListStyleType::UpperRoman:
      if (n >= 1 && n <= 3999) return detail::roman(n, out, cap, type == CssListStyleType::UpperRoman);
      break;
    case CssListStyleType::CjkIdeographic:
    case CssListStyleType::SimpChineseInformal:
      if (n >= -9999 && n <= 9999) {
        return detail::cjk(n, out, cap, type == CssListStyleType::SimpChineseInformal);
      }
      break;
  }
  return detail::decimal(n, out, cap, false);  // 超出範圍（或壞值）：十進位
}

}  // namespace ListMarker
