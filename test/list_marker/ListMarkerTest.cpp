// v362：清單記號（lib/Epub/Epub/parsers/ListMarker.h）。期望值是手寫的（CSS Counter Styles 3 的定義），
//   不是拿同一份程式產生的；範圍兩端與退回十進位的邊界都釘住，最長的兩種記號確認寫得下。
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Epub/Epub/parsers/ListMarker.h"

namespace {

std::string marker(const CssListStyleType type, const int n) {
  char buf[ListMarker::MAX_BYTES];
  const size_t len = ListMarker::format(type, n, buf, sizeof(buf));
  EXPECT_EQ(len, strlen(buf)) << "回傳的長度要跟寫進去的一樣";
  return std::string(buf, len);
}

TEST(ListMarker, NoneWritesNothing) {
  char buf[ListMarker::MAX_BYTES] = "xyz";
  EXPECT_EQ(ListMarker::format(CssListStyleType::None, 3, buf, sizeof(buf)), 0u);
  EXPECT_STREQ(buf, "");
}

TEST(ListMarker, DiscIsBulletRegardlessOfNumber) {
  EXPECT_EQ(marker(CssListStyleType::Disc, 1), "\xe2\x80\xa2");
  EXPECT_EQ(marker(CssListStyleType::Disc, 1234), "\xe2\x80\xa2");
}

TEST(ListMarker, Decimal) {
  EXPECT_EQ(marker(CssListStyleType::Decimal, 1), "1.");
  EXPECT_EQ(marker(CssListStyleType::Decimal, 12), "12.");
  EXPECT_EQ(marker(CssListStyleType::Decimal, 0), "0.");
  EXPECT_EQ(marker(CssListStyleType::Decimal, -3), "-3.");
  EXPECT_EQ(marker(CssListStyleType::Decimal, 99999), "99999.");
  EXPECT_EQ(marker(CssListStyleType::Decimal, -99999), "-99999.");
}

TEST(ListMarker, DecimalLeadingZero) {
  EXPECT_EQ(marker(CssListStyleType::DecimalLeadingZero, 0), "00.");
  EXPECT_EQ(marker(CssListStyleType::DecimalLeadingZero, 1), "01.");
  EXPECT_EQ(marker(CssListStyleType::DecimalLeadingZero, 9), "09.");
  EXPECT_EQ(marker(CssListStyleType::DecimalLeadingZero, 10), "10.");
  EXPECT_EQ(marker(CssListStyleType::DecimalLeadingZero, 123), "123.");
  // 負號算在兩位裡：CSS Counter Styles 3 的 pad —— 「counter value 是負的，補位數再扣掉負號的長度」→ -1 不補零。
  EXPECT_EQ(marker(CssListStyleType::DecimalLeadingZero, -1), "-1.");
}

TEST(ListMarker, Alpha) {
  EXPECT_EQ(marker(CssListStyleType::LowerAlpha, 1), "a.");
  EXPECT_EQ(marker(CssListStyleType::LowerAlpha, 26), "z.");
  EXPECT_EQ(marker(CssListStyleType::LowerAlpha, 27), "aa.");
  EXPECT_EQ(marker(CssListStyleType::LowerAlpha, 52), "az.");
  EXPECT_EQ(marker(CssListStyleType::LowerAlpha, 53), "ba.");
  EXPECT_EQ(marker(CssListStyleType::LowerAlpha, 702), "zz.");
  EXPECT_EQ(marker(CssListStyleType::LowerAlpha, 703), "aaa.");
  EXPECT_EQ(marker(CssListStyleType::UpperAlpha, 1), "A.");
  EXPECT_EQ(marker(CssListStyleType::UpperAlpha, 8), "H.");
  EXPECT_EQ(marker(CssListStyleType::UpperAlpha, 28), "AB.");
  EXPECT_EQ(marker(CssListStyleType::UpperAlpha, 99999), "EQXC.");
}

TEST(ListMarker, AlphaOutOfRangeFallsBackToDecimal) {
  EXPECT_EQ(marker(CssListStyleType::LowerAlpha, 0), "0.");
  EXPECT_EQ(marker(CssListStyleType::UpperAlpha, -2), "-2.");
}

TEST(ListMarker, Roman) {
  EXPECT_EQ(marker(CssListStyleType::LowerRoman, 1), "i.");
  EXPECT_EQ(marker(CssListStyleType::LowerRoman, 4), "iv.");
  EXPECT_EQ(marker(CssListStyleType::LowerRoman, 9), "ix.");
  EXPECT_EQ(marker(CssListStyleType::LowerRoman, 14), "xiv.");
  EXPECT_EQ(marker(CssListStyleType::LowerRoman, 40), "xl.");
  EXPECT_EQ(marker(CssListStyleType::LowerRoman, 90), "xc.");
  EXPECT_EQ(marker(CssListStyleType::LowerRoman, 400), "cd.");
  EXPECT_EQ(marker(CssListStyleType::UpperRoman, 1994), "MCMXCIV.");
  EXPECT_EQ(marker(CssListStyleType::UpperRoman, 3999), "MMMCMXCIX.");
  EXPECT_EQ(marker(CssListStyleType::UpperRoman, 3888), "MMMDCCCLXXXVIII.");  // 最長的羅馬數字
}

TEST(ListMarker, RomanOutOfRangeFallsBackToDecimal) {
  EXPECT_EQ(marker(CssListStyleType::UpperRoman, 4000), "4000.");
  EXPECT_EQ(marker(CssListStyleType::LowerRoman, 0), "0.");
  EXPECT_EQ(marker(CssListStyleType::LowerRoman, -1), "-1.");
}

TEST(ListMarker, CjkIdeographic) {
  const char* const expected[] = {"零、", "一、", "二、", "三、", "四、",   "五、",  "六、",
                                  "七、", "八、", "九、", "十、", "十一、", "十二、"};
  for (int n = 0; n <= 12; n++) {
    EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, n), expected[n]) << "n=" << n;
  }
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 19), "十九、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 20), "二十、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 21), "二十一、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 100), "一百、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 101), "一百零一、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 110), "一百一十、");  // 十位的「一」只有十到十九才省
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 111), "一百一十一、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 1000), "一千、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 1001), "一千零一、");  // 連續的零只寫一個
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 1010), "一千零一十、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 1100), "一千一百、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 2305), "二千三百零五、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 9999), "九千九百九十九、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, -3), "負三、");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, -9999), "負九千九百九十九、");  // 最長的記號（27 bytes）
}

TEST(ListMarker, SimpChineseInformalDiffersOnlyInNegativeSign) {
  EXPECT_EQ(marker(CssListStyleType::SimpChineseInformal, 12), "十二、");
  EXPECT_EQ(marker(CssListStyleType::SimpChineseInformal, 1010), "一千零一十、");
  EXPECT_EQ(marker(CssListStyleType::SimpChineseInformal, -3), "负三、");
  EXPECT_EQ(marker(CssListStyleType::SimpChineseInformal, 10000), "10000.");
}

TEST(ListMarker, CjkOutOfRangeFallsBackToDecimal) {
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, 10000), "10000.");
  EXPECT_EQ(marker(CssListStyleType::CjkIdeographic, -10000), "-10000.");
}

TEST(ListMarker, LongestMarkersFitInMaxBytes) {
  EXPECT_LT(strlen("負九千九百九十九、"), ListMarker::MAX_BYTES);
  EXPECT_LT(strlen("MMMDCCCLXXXVIII."), ListMarker::MAX_BYTES);
}

// 緩衝區太小時不寫半個 UTF-8 字，回傳的長度跟寫進去的一致、字串一定有結尾。
TEST(ListMarker, SmallBufferNeverSplitsUtf8) {
  char buf[8];
  const size_t len = ListMarker::format(CssListStyleType::CjkIdeographic, 1001, buf, sizeof(buf));
  EXPECT_EQ(len, strlen(buf));
  EXPECT_EQ(len % 3, 0u) << "中文字一個 3 bytes，不能切在中間";
  EXPECT_EQ(std::string(buf, len), "一千");
  char two[2];  // 只放得下一個字：寫出記號的前綴「M」，之後的（再一個 M）就停了，不會跳著寫
  EXPECT_EQ(ListMarker::format(CssListStyleType::UpperRoman, 3888, two, sizeof(two)), 1u);
  EXPECT_STREQ(two, "M");
}

}  // namespace
