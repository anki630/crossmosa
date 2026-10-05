// v362：list-style-type／list-style 縮寫的解析（lib/Epub/Epub/css/CssListStyle.h）。
//   期望值照 CSS Lists 3 手寫；縮寫裡 none 的歸屬、省略的樣式回到 disc、不認得就整條不算數，各自釘住。
#include <gtest/gtest.h>

#include <optional>
#include <string_view>

#include "Epub/Epub/css/CssListStyle.h"

namespace {

std::optional<CssListStyleType> type(const std::string_view v) {
  CssListStyleType t = CssListStyleType::Disc;
  if (!CssListStyle::parseType(v, t)) return std::nullopt;
  return t;
}

std::optional<CssListStyleType> shorthand(const std::string_view v) {
  CssListStyleType t = CssListStyleType::Disc;
  if (!CssListStyle::parseShorthand(v, t)) return std::nullopt;
  return t;
}

TEST(CssListStyleType, Keywords) {
  EXPECT_EQ(type("none"), CssListStyleType::None);
  EXPECT_EQ(type("disc"), CssListStyleType::Disc);
  EXPECT_EQ(type("square"), CssListStyleType::Disc);
  EXPECT_EQ(type("circle"), CssListStyleType::Disc);
  EXPECT_EQ(type("decimal"), CssListStyleType::Decimal);
  EXPECT_EQ(type("decimal-leading-zero"), CssListStyleType::DecimalLeadingZero);
  EXPECT_EQ(type("lower-alpha"), CssListStyleType::LowerAlpha);
  EXPECT_EQ(type("lower-latin"), CssListStyleType::LowerAlpha);
  EXPECT_EQ(type("UPPER-ALPHA"), CssListStyleType::UpperAlpha);  // 關鍵字不分大小寫
  EXPECT_EQ(type("upper-latin"), CssListStyleType::UpperAlpha);
  EXPECT_EQ(type("lower-roman"), CssListStyleType::LowerRoman);
  EXPECT_EQ(type("upper-roman"), CssListStyleType::UpperRoman);
  EXPECT_EQ(type("cjk-ideographic"), CssListStyleType::CjkIdeographic);
  EXPECT_EQ(type("trad-chinese-informal"), CssListStyleType::CjkIdeographic);
  EXPECT_EQ(type("simp-chinese-informal"), CssListStyleType::SimpChineseInformal);
  EXPECT_EQ(type("  decimal "), CssListStyleType::Decimal);
  EXPECT_EQ(type("initial"), CssListStyleType::Disc);  // 初始值
}

TEST(CssListStyleType, UnsupportedValuesDoNotCount) {
  EXPECT_EQ(type("var(--lst-custom)"), std::nullopt);  // 書庫的樣板 CSS 真的這樣寫
  EXPECT_EQ(type("lower-greek"), std::nullopt);
  EXPECT_EQ(type("inherit"), std::nullopt);
  EXPECT_EQ(type("unset"), std::nullopt);
  EXPECT_EQ(type("\"*\""), std::nullopt);
  EXPECT_EQ(type(""), std::nullopt);
  EXPECT_EQ(type("decimal inside"), std::nullopt);  // 長寫只收一個值
}

TEST(CssListStyleShorthand, NoneAlone) {
  EXPECT_EQ(shorthand("none"), CssListStyleType::None);
  EXPECT_EQ(shorthand("None"), CssListStyleType::None);
  EXPECT_EQ(shorthand("none inside"), CssListStyleType::None);
  EXPECT_EQ(shorthand("outside none"), CssListStyleType::None);
  EXPECT_EQ(shorthand("none none"), CssListStyleType::None);  // 圖片和樣式各一個
}

TEST(CssListStyleShorthand, NoneFillsWhicheverIsNotSet) {
  EXPECT_EQ(shorthand("none decimal"), CssListStyleType::Decimal);  // none 是圖片
  EXPECT_EQ(shorthand("decimal none"), CssListStyleType::Decimal);
  EXPECT_EQ(shorthand("none url(bullet.png)"), CssListStyleType::None);  // none 是樣式
  EXPECT_EQ(shorthand("url(bullet.png) none outside"), CssListStyleType::None);
}

TEST(CssListStyleShorthand, OmittedTypeIsDisc) {
  // 縮寫一定會設定樣式：沒寫就是初始值 disc —— `ol { list-style: inside }` 在瀏覽器裡也會變圓點。
  EXPECT_EQ(shorthand("inside"), CssListStyleType::Disc);
  EXPECT_EQ(shorthand("url(bullet.png)"), CssListStyleType::Disc);
  EXPECT_EQ(shorthand("url(\"bullet icon.svg\") outside"), CssListStyleType::Disc);  // 括號裡有空白和引號
  EXPECT_EQ(shorthand("Url('a\\'b.png')"), CssListStyleType::Disc);                  // 跳脫的引號
  EXPECT_EQ(shorthand("linear-gradient(red, blue)"), CssListStyleType::Disc);
  EXPECT_EQ(shorthand("-webkit-linear-gradient(red, blue)"), CssListStyleType::Disc);
  EXPECT_EQ(shorthand("repeating-radial-gradient(red, blue) none"), CssListStyleType::None);
  EXPECT_EQ(shorthand("url(foo\\)bar.png)"), CssListStyleType::Disc);  // 引號外跳脫的右括號
  EXPECT_EQ(shorthand("initial"), CssListStyleType::Disc);
}

TEST(CssListStyleShorthand, TypesAndPositions) {
  EXPECT_EQ(shorthand("decimal"), CssListStyleType::Decimal);
  EXPECT_EQ(shorthand("upper-roman inside"), CssListStyleType::UpperRoman);
  EXPECT_EQ(shorthand("outside lower-alpha url(x.png)"), CssListStyleType::LowerAlpha);
  EXPECT_EQ(shorthand("square"), CssListStyleType::Disc);
  EXPECT_EQ(shorthand("cjk-ideographic"), CssListStyleType::CjkIdeographic);
  EXPECT_EQ(shorthand("url(a.png)inside"), CssListStyleType::Disc);  // 函式後面沒空白
}

TEST(CssListStyleShorthand, AnythingUnrecognizedInvalidatesTheWholeDeclaration) {
  EXPECT_EQ(shorthand("none lower-greek"), std::nullopt);  // 不能只吃 none 而變成「沒有記號」
  EXPECT_EQ(shorthand("lower-greek"), std::nullopt);
  EXPECT_EQ(shorthand("var(--lst-custom)"), std::nullopt);
  EXPECT_EQ(shorthand("none var(--x)"), std::nullopt);
  EXPECT_EQ(shorthand("symbols(cyclic '*')"), std::nullopt);
  EXPECT_EQ(shorthand("bogus-gradient()"), std::nullopt);         // 不是圖片函式：名字結尾像也不算
  EXPECT_EQ(shorthand("\"\xe2\x86\x92\" inside"), std::nullopt);  // 字串記號
  EXPECT_EQ(shorthand("inherit"), std::nullopt);
  EXPECT_EQ(shorthand("decimal initial"), std::nullopt);
  EXPECT_EQ(shorthand(""), std::nullopt);
  EXPECT_EQ(shorthand("   "), std::nullopt);
  EXPECT_EQ(shorthand("url(unclosed.png"), std::nullopt);
}

TEST(CssListStyleShorthand, DuplicatesAreInvalid) {
  EXPECT_EQ(shorthand("inside outside"), std::nullopt);
  EXPECT_EQ(shorthand("decimal disc"), std::nullopt);
  EXPECT_EQ(shorthand("url(a.png) url(b.png)"), std::nullopt);
  EXPECT_EQ(shorthand("none none none"), std::nullopt);
  EXPECT_EQ(shorthand("none decimal url(x.png)"), std::nullopt);  // none 沒地方放
}

}  // namespace
