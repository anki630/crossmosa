// v361：省略號截斷的二分搜尋（lib/GfxRenderer/TruncateText.h）。
//   兩個邊界釘死（整串「等於」寬度 → 原樣；截斷後「等於」寬度 → 不收），再拿原本的線性版當參考逐一比對。
#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>

#include "GfxRenderer/TruncateText.h"

namespace {

const char* const kEllipsis = "\xe2\x80\xa6";

// 寬度＝每個碼位 10。省略號也是一個碼位。
int fixedWidth(const std::string& s) {
  int w = 0;
  for (const unsigned char c : s) {
    if ((c & 0xC0) != 0x80) w += 10;
  }
  return w;
}

// 原本 GfxRenderer::truncatedText 的線性版（v360 以前），當參考答案。
template <typename WidthFn>
std::string linearReference(std::string item, const int maxWidth, WidthFn&& width) {
  if (width(item) <= maxWidth) return item;
  while (!item.empty() && width(item + kEllipsis) >= maxWidth) {
    size_t pos = item.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(item[pos]) & 0xC0) == 0x80) --pos;
    item.resize(pos);
  }
  return item.empty() ? kEllipsis : item + kEllipsis;
}

TEST(TruncateText, FullTextExactlyAtWidthIsKept) {
  EXPECT_EQ(TruncateText::fit("abcde", 50, kEllipsis, fixedWidth), "abcde");
}

TEST(TruncateText, TruncatedCandidateExactlyAtWidthIsRejected) {
  // "abcd…" 正好 50：不收（嚴格小於），所以是 "abc…"（40）。
  EXPECT_EQ(TruncateText::fit("abcdef", 50, kEllipsis, fixedWidth), std::string("abc") + kEllipsis);
  EXPECT_EQ(TruncateText::fit("abcdef", 51, kEllipsis, fixedWidth), std::string("abcd") + kEllipsis);
}

TEST(TruncateText, NothingFitsReturnsEllipsisOnly) {
  EXPECT_EQ(TruncateText::fit("abc", 5, kEllipsis, fixedWidth), kEllipsis);
  EXPECT_EQ(TruncateText::fit("abc", 10, kEllipsis, fixedWidth), kEllipsis);
}

TEST(TruncateText, NeverSplitsAMultiByteCodepoint) {
  EXPECT_EQ(TruncateText::fit("範例書坊名", 35, kEllipsis, fixedWidth), std::string("範例") + kEllipsis);
  EXPECT_EQ(TruncateText::fit("a範b例c", 45, kEllipsis, fixedWidth), std::string("a範b") + kEllipsis);
}

TEST(TruncateText, LongTitleNeedsLogarithmicMeasurements) {
  std::string title;
  for (int i = 0; i < 1500; i++) title += "字";
  int calls = 0;
  const auto counted = [&](const std::string& s) {
    calls++;
    return fixedWidth(s);
  };
  const std::string out = TruncateText::fit(title, 400, kEllipsis, counted);
  EXPECT_EQ(fixedWidth(out), 390);
  EXPECT_LE(calls, 1 + 12 + 1);  // 整串一次＋二分 ⌈log2 1501⌉ 次＋（不再量）最後組字串
}

// 單調但不均勻的寬度（依碼位值給 3–17），隨機字串與寬度，跟線性版逐一相同。
TEST(TruncateText, MatchesLinearReferenceOnRandomInputs) {
  const auto varWidth = [](const std::string& s) {
    int w = 0;
    uint32_t cp = 0;
    for (size_t i = 0; i < s.size(); i++) {
      const unsigned char c = s[i];
      if ((c & 0xC0) != 0x80) {
        if (i) w += 3 + static_cast<int>(cp % 15);
        cp = c;
      } else {
        cp = (cp << 6) | (c & 0x3F);
      }
    }
    if (!s.empty()) w += 3 + static_cast<int>(cp % 15);
    return w;
  };
  const char* const pieces[] = {"a", "W", "i", " ", "範", "例", "é", "\xf0\x9f\x93\x96", "-"};
  std::mt19937 rng(361);
  for (int round = 0; round < 3000; round++) {
    std::string text;
    const int len = static_cast<int>(rng() % 40);
    for (int i = 0; i < len; i++) text += pieces[rng() % (sizeof(pieces) / sizeof(pieces[0]))];
    const int maxWidth = 1 + static_cast<int>(rng() % 300);
    ASSERT_EQ(TruncateText::fit(text, maxWidth, kEllipsis, varWidth), linearReference(text, maxWidth, varWidth))
        << "text=" << text << " max=" << maxWidth;
  }
}

}  // namespace
