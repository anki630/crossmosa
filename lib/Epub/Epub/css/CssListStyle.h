#pragma once

#include <cstddef>
#include <string_view>

#include "CssStyle.h"

// v362：list-style-type 與 list-style 縮寫的解析（CSS Lists 3）。純函式，主機測試在 test/list_marker。
//   原則：認得的照規格；任何一個值不認得（var(...)、lower-greek、字串記號…）→ 整條宣告不算數，
//   交給 <ul>／<ol> 的預設（ol 數字、ul 圓點）—— 寧可退回預設，也不要把看不懂的猜成「沒有記號」。
namespace CssListStyle {

namespace detail {

constexpr bool isWs(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

constexpr char lowerAscii(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

constexpr bool iequals(const std::string_view value, const std::string_view lowercaseKeyword) {
  if (value.size() != lowercaseKeyword.size()) return false;
  for (size_t i = 0; i < value.size(); i++) {
    if (lowerAscii(value[i]) != lowercaseKeyword[i]) return false;
  }
  return true;
}

constexpr std::string_view trim(std::string_view v) {
  while (!v.empty() && isWs(v.front())) v.remove_prefix(1);
  while (!v.empty() && isWs(v.back())) v.remove_suffix(1);
  return v;
}

inline bool isImageFunction(std::string_view name) {
  static constexpr std::string_view kPrefixes[] = {"-webkit-", "-moz-", "-o-", "-ms-"};
  for (const std::string_view prefix : kPrefixes) {
    if (name.size() > prefix.size() && iequals(name.substr(0, prefix.size()), prefix)) {
      name.remove_prefix(prefix.size());
      break;
    }
  }
  static constexpr std::string_view kImageFunctions[] = {
      "url",
      "image",
      "image-set",
      "cross-fade",
      "element",
      "linear-gradient",
      "radial-gradient",
      "conic-gradient",
      "repeating-linear-gradient",
      "repeating-radial-gradient",
      "repeating-conic-gradient",
  };
  for (const std::string_view f : kImageFunctions) {
    if (iequals(name, f)) return true;
  }
  return false;
}

// 記號樣式的關鍵字（不含 none 與 CSS 全域關鍵字）。
//   simp-chinese-informal
//   跟繁體共用：一到九千九百九十九的寫法相同，只有負號（负／負）不同，而清單編號實際上不會是負數。
inline bool typeKeyword(const std::string_view token, CssListStyleType& out) {
  struct Keyword {
    std::string_view name;
    CssListStyleType type;
  };
  static constexpr Keyword kKeywords[] = {
      {"disc", CssListStyleType::Disc},
      {"circle", CssListStyleType::Disc},
      {"square", CssListStyleType::Disc},
      {"disclosure-open", CssListStyleType::Disc},
      {"disclosure-closed", CssListStyleType::Disc},
      {"decimal", CssListStyleType::Decimal},
      {"decimal-leading-zero", CssListStyleType::DecimalLeadingZero},
      {"lower-alpha", CssListStyleType::LowerAlpha},
      {"lower-latin", CssListStyleType::LowerAlpha},
      {"upper-alpha", CssListStyleType::UpperAlpha},
      {"upper-latin", CssListStyleType::UpperAlpha},
      {"lower-roman", CssListStyleType::LowerRoman},
      {"upper-roman", CssListStyleType::UpperRoman},
      {"cjk-ideographic", CssListStyleType::CjkIdeographic},
      {"trad-chinese-informal", CssListStyleType::CjkIdeographic},
      {"simp-chinese-informal", CssListStyleType::SimpChineseInformal},
  };
  for (const Keyword& k : kKeywords) {
    if (iequals(token, k.name)) {
      out = k.type;
      return true;
    }
  }
  return false;
}

constexpr bool isCssWideKeyword(const std::string_view v) {
  return iequals(v, "initial") || iequals(v, "inherit") || iequals(v, "unset") || iequals(v, "revert") ||
         iequals(v, "revert-layer");
}

}  // namespace detail

// list-style-type 的值（呼叫端已去掉 !important）。認得回 true。
//   initial＝disc（初始值）；inherit／unset／revert 不支援 → 不算數。
inline bool parseType(std::string_view value, CssListStyleType& out) {
  value = detail::trim(value);
  if (detail::iequals(value, "none")) {
    out = CssListStyleType::None;
    return true;
  }
  if (detail::iequals(value, "initial")) {
    out = CssListStyleType::Disc;
    return true;
  }
  return detail::typeKeyword(value, out);
}

// list-style 縮寫（呼叫端已去掉 !important）。語法：位置 || 圖片 || 樣式，各最多一個，順序不拘。
//   合法時回 true 並給出 list-style-type —— 縮寫一定會設定它：沒寫樣式就是初始值 disc
//   （所以 `ol { list-style: inside }` 會讓編號變圓點，跟瀏覽器一樣）。
//   none 可以是圖片也可以是樣式：填進縮寫裡【沒被其他值設定】的那一個（規格原文），
//   所以「none decimal」是 decimal（none 是圖片）、「none url(x)」是 none（none 是樣式）、「none」是 none。
inline bool parseShorthand(std::string_view value, CssListStyleType& out) {
  value = detail::trim(value);
  if (value.empty()) return false;
  if (detail::isCssWideKeyword(value)) {
    if (!detail::iequals(value, "initial")) return false;
    out = CssListStyleType::Disc;
    return true;
  }

  int noneCount = 0;
  bool havePosition = false;
  bool haveImage = false;
  bool haveType = false;
  CssListStyleType type = CssListStyleType::Disc;

  size_t pos = 0;
  while (pos < value.size()) {
    while (pos < value.size() && detail::isWs(value[pos])) pos++;
    if (pos >= value.size()) break;

    // 字串（"→" 之類的記號）：不支援 → 整條不算數。
    if (value[pos] == '"' || value[pos] == '\'') return false;

    // 函式值：名字後面接「(」，整段（含括號裡的空白、引號、跳脫）一起吃掉。
    size_t nameEnd = pos;
    while (nameEnd < value.size() &&
           ((value[nameEnd] >= 'a' && value[nameEnd] <= 'z') || (value[nameEnd] >= 'A' && value[nameEnd] <= 'Z') ||
            (value[nameEnd] >= '0' && value[nameEnd] <= '9') || value[nameEnd] == '-' || value[nameEnd] == '_')) {
      nameEnd++;
    }
    if (nameEnd < value.size() && value[nameEnd] == '(') {
      const std::string_view fname = value.substr(pos, nameEnd - pos);
      int depthParen = 0;
      char quote = 0;
      size_t end = nameEnd;
      bool closed = false;
      for (; end < value.size(); end++) {
        const char c = value[end];
        if (c == '\\') {
          end++;  // 跳脫字元（引號裡外都算）：下一個字元是內容，包括引號與括號 —— url(foo\)bar.png)
        } else if (quote) {
          if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
          quote = c;
        } else if (c == '(') {
          depthParen++;
        } else if (c == ')' && --depthParen == 0) {
          end++;
          closed = true;
          break;
        }
      }
      if (!closed) return false;
      // 圖片：url() 與 CSS Images 的圖片函式（可帶 -webkit- 之類的前綴）。其他函式（symbols()、var()…）不支援。
      const bool isImage = detail::isImageFunction(fname);
      if (!isImage || haveImage) return false;
      haveImage = true;
      pos = end;
      continue;
    }

    size_t end = pos;
    while (end < value.size() && !detail::isWs(value[end])) end++;
    const std::string_view token = value.substr(pos, end - pos);
    pos = end;

    CssListStyleType tokenType;
    if (detail::iequals(token, "none")) {
      noneCount++;
    } else if (detail::iequals(token, "inside") || detail::iequals(token, "outside")) {
      if (havePosition) return false;
      havePosition = true;
    } else if (detail::typeKeyword(token, tokenType)) {
      if (haveType) return false;
      haveType = true;
      type = tokenType;
    } else {
      return false;  // 不認得的樣式名、CSS 全域關鍵字夾在別的值中間、其他垃圾
    }
  }

  // none 只能填沒被設定的圖片／樣式；填不下就是不合法。
  const int noneSlots = (haveImage ? 0 : 1) + (haveType ? 0 : 1);
  if (noneCount > noneSlots) return false;
  if (!haveType && noneCount > 0) {
    // 沒寫樣式：none 有一個會落到樣式上（只有一個 none 而圖片也沒寫時，兩者都是 none）。
    type = CssListStyleType::None;
  }
  out = type;
  return true;
}

}  // namespace CssListStyle
