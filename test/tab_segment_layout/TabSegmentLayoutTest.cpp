// v336：Formosa Pro 分頁列分段寬度（src/components/themes/TabSegmentLayout.h）的主機測試。
//   窮舉 × 不變量，再加上 2026-09-23 從 ubuntu_10／ubuntu_14 glyph 表量出來的四種實機情況。
#include "src/components/themes/TabSegmentLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace {

using TabSegmentLayout::kMaxSegments;

struct Result {
  int x[kMaxSegments];
  int w[kMaxSegments];
  bool filled;
};

Result run(const int* natural, int n, int areaW, int gap) {
  Result r{};
  r.filled = TabSegmentLayout::layout(natural, n, areaW, gap, r.x, r.w);
  return r;
}

TEST(TabSegmentLayout, ExhaustiveInvariants) {
  constexpr int kNat[] = {40, 60, 74, 88, 104, 112, 129, 200};
  constexpr int kAreas[] = {300, 440, 488, 1000};
  constexpr int kGaps[] = {0, 8};
  int natural[kMaxSegments];
  for (int n = 1; n <= 5; n++) {
    int idx[5] = {};
    while (true) {
      for (int i = 0; i < n; i++) natural[i] = kNat[idx[i]];
      for (int areaW : kAreas) {
        for (int gap : kGaps) {
          const Result r = run(natural, n, areaW, gap);
          std::string ctx = "n=" + std::to_string(n) + " area=" + std::to_string(areaW) + " gap=" + std::to_string(gap) + " nat=";
          for (int i = 0; i < n; i++) ctx += std::to_string(natural[i]) + ",";
          int sumNat = 0, maxNat = 0;
          for (int i = 0; i < n; i++) {
            sumNat += natural[i];
            maxNat = std::max(maxNat, natural[i]);
          }
          const bool fits = sumNat + (n - 1) * gap <= areaW;
          EXPECT_EQ(r.filled, fits) << ctx;
          EXPECT_EQ(r.x[0], 0) << ctx;
          for (int i = 0; i < n; i++) {
            EXPECT_GE(r.w[i], natural[i]) << ctx << " seg " << i << " squeezes its text";
            if (i > 0) {
              EXPECT_EQ(r.x[i], r.x[i - 1] + r.w[i - 1] + gap) << ctx << " seg " << i << " gap wrong";
            }
          }
          if (fits) {
            EXPECT_EQ(r.x[n - 1] + r.w[n - 1], areaW) << ctx << " not exactly filled";
          } else {
            for (int i = 0; i < n; i++) EXPECT_EQ(r.w[i], natural[i]) << ctx << " seg " << i;
          }
          // 平分的條件成立 → 各段寬度最多差 1px（除不盡的餘數）
          const int avail = areaW - (n - 1) * gap;
          if (avail >= 0 && avail / n >= maxNat) {
            const int lo = *std::min_element(r.w, r.w + n);
            const int hi = *std::max_element(r.w, r.w + n);
            EXPECT_LE(hi - lo, 1) << ctx << " should be equal";
          }
          if (HasFailure()) return;
        }
      }
      int k = 0;
      while (k < n && ++idx[k] == static_cast<int>(sizeof(kNat) / sizeof(kNat[0]))) idx[k++] = 0;
      if (k == n) break;
    }
  }
}

// 四種實機情況（字寬＋左右各 8 內距；可用寬＝螢幕寬 − 2×20；段間 8）。
TEST(TabSegmentLayout, X3ChineseSettingsIsEqual) {
  const int nat[] = {74, 104, 74, 74};
  const Result r = run(nat, 4, 528 - 40, 8);
  for (int i = 0; i < 4; i++) EXPECT_EQ(r.w[i], 116) << i;
}

TEST(TabSegmentLayout, X3EnglishSettingsKeepsItsNaturalWidths) {
  // 自然寬總和＋間距剛好 488 → 本來就滿，外觀一個像素都不變
  const int nat[] = {112, 111, 129, 112};
  const Result r = run(nat, 4, 528 - 40, 8);
  for (int i = 0; i < 4; i++) EXPECT_EQ(r.w[i], nat[i]) << i;
  EXPECT_TRUE(r.filled);
}

TEST(TabSegmentLayout, X4EnglishSettingsSmallFontIsEqual) {
  const int nat[] = {85, 84, 97, 85};
  const Result r = run(nat, 4, 480 - 40, 8);
  for (int i = 0; i < 4; i++) EXPECT_EQ(r.w[i], 104) << i;
}

TEST(TabSegmentLayout, X4EnglishTextSettingsSharesTheRest) {
  // "Layout" 107 > 平分寬 104 → 自然寬＋剩餘平分：剩 416 − 335 ＝ 81 → 每段 +20，餘 1 給第一段
  const int nat[] = {77, 69, 107, 82};
  const Result r = run(nat, 4, 480 - 40, 8);
  EXPECT_EQ(r.w[0], 98);
  EXPECT_EQ(r.w[1], 89);
  EXPECT_EQ(r.w[2], 127);
  EXPECT_EQ(r.w[3], 102);
  EXPECT_EQ(r.x[3] + r.w[3], 440);
}

}  // namespace
