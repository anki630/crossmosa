// v335：按鍵提示膠囊寬度分配（src/components/themes/HintPillLayout.h）的主機測試。
//   位置表與寬度照抄兩個主題（FormosaProTheme／LyraTheme::drawButtonHints），螢幕寬＝直向寬（X3 528、X4 480）。
//   窮舉 × 不變量，再加上實機回報的那幾個字的具體數字（2026-09-23 從 ubuntu_10 glyph 表量的）。
#include <gtest/gtest.h>

#include <string>

#include "src/components/themes/HintPillLayout.h"

namespace {

using HintPillLayout::kCount;
using HintPillLayout::kEdge;
using HintPillLayout::kGap;
using HintPillLayout::Pill;

constexpr int kNominalW = 80;

struct Device {
  const char* name;
  int pos[kCount];
  int screenW;
};

constexpr Device kX3{"X3", {65, 157, 291, 383}, 528};
constexpr Device kX4{"X4", {58, 146, 254, 342}, 480};

void run(const Device& d, const int (&need)[kCount], const bool (&occupied)[kCount], Pill (&out)[kCount]) {
  HintPillLayout::layout(d.pos, kNominalW, d.screenW, need, occupied, out);
}

std::string describe(const Device& d, const int (&need)[kCount], const bool (&occ)[kCount]) {
  std::string s = std::string(d.name) + " need=";
  for (int i = 0; i < kCount; i++) s += std::to_string(need[i]) + (occ[i] ? "*" : "") + (i + 1 < kCount ? "," : "");
  return s;
}

void checkInvariants(const Device& d, const int (&need)[kCount], const bool (&occ)[kCount], const Pill (&out)[kCount]) {
  const std::string ctx = describe(d, need, occ);
  int prevRight = -1;
  for (int i = 0; i < kCount; i++) {
    const bool present = need[i] > 0 || occ[i];
    if (!present) {
      EXPECT_EQ(out[i].w, 0) << ctx << " pill " << i << " should not be drawn";
      continue;
    }
    ASSERT_GT(out[i].w, 0) << ctx << " pill " << i;
    if (need[i] <= kNominalW) {
      EXPECT_EQ(out[i].x, d.pos[i]) << ctx << " pill " << i << " fits but moved";
      EXPECT_EQ(out[i].w, kNominalW) << ctx << " pill " << i << " fits but resized";
    } else {
      EXPECT_GE(out[i].w, kNominalW) << ctx << " pill " << i << " shrank";
      EXPECT_LE(out[i].w, need[i]) << ctx << " pill " << i << " wider than needed";
    }
    EXPECT_GE(out[i].x, kEdge) << ctx << " pill " << i << " past left edge";
    EXPECT_LE(out[i].x + out[i].w, d.screenW - kEdge) << ctx << " pill " << i << " past right edge";
    const int center = d.pos[i] + kNominalW / 2;
    EXPECT_LE(out[i].x, center) << ctx << " pill " << i << " no longer covers its button";
    // 膠囊涵蓋 [x, x+w)：右界要嚴格大於（codex：>= 會放過差一像素的反例）
    EXPECT_GT(out[i].x + out[i].w, center) << ctx << " pill " << i << " no longer covers its button";
    if (prevRight >= 0) {
      EXPECT_GE(out[i].x - prevRight, kGap) << ctx << " pill " << i << " overlaps / crowds its left neighbour";
    }
    prevRight = out[i].x + out[i].w;
  }
}

TEST(HintPillLayout, ExhaustiveInvariantsBothDevices) {
  constexpr int kNeeds[] = {0, 40, 80, 81, 95, 108, 120, 140, 300};
  for (const Device* d : {&kX3, &kX4}) {
    int need[kCount];
    bool occ[kCount];
    for (int a : kNeeds)
      for (int b : kNeeds)
        for (int c : kNeeds)
          for (int e : kNeeds)
            for (int mask = 0; mask < 16; mask++) {
              need[0] = a;
              need[1] = b;
              need[2] = c;
              need[3] = e;
              for (int i = 0; i < kCount; i++) occ[i] = (mask >> i) & 1;
              Pill out[kCount];
              run(*d, need, occ, out);
              checkInvariants(*d, need, occ, out);
              if (HasFailure()) return;  // 一個反例就夠讀，不要洗版
            }
  }
}

// 實機那幾個字（寬度＝字寬＋左右各 kTextPad，四捨五入）。
TEST(HintPillLayout, ImageViewerX4AllFourLabelsFit) {
  // 「« 返回」57、「設為待機」83、「上一張」62、「下一張」62
  const int need[kCount] = {57 + 12, 83 + 12, 62 + 12, 62 + 12};
  const bool occ[kCount] = {};
  Pill out[kCount];
  run(kX4, need, occ, out);
  EXPECT_EQ(out[1].w, need[1]);  // 設為待機：整個放得下，不截斷
  EXPECT_EQ(out[0].x, kX4.pos[0]);
  EXPECT_EQ(out[2].x, kX4.pos[2]);
  EXPECT_EQ(out[3].x, kX4.pos[3]);
}

TEST(HintPillLayout, SixCharLabelStillTooWideOnX4) {
  // 「設為待機畫面」125 → X4 中間兩顆之間最多長到 108 → 呼叫端要截斷（所以字串改成四個字）
  const int need[kCount] = {69, 125 + 12, 74, 74};
  const bool occ[kCount] = {};
  Pill out[kCount];
  run(kX4, need, occ, out);
  EXPECT_LT(out[1].w, need[1]);
  EXPECT_EQ(out[1].w, (kX4.pos[2] - kGap) - (kX4.pos[0] + kNominalW + kGap));
}

TEST(HintPillLayout, DownloadFitsOnX4WithArrowNeighbours) {
  // 字型下載：btn2 "Download" 96（英文），btn3／btn4 是短的上下鍵
  const int need[kCount] = {72, 96 + 12, 40, 40};
  const bool occ[kCount] = {};
  Pill out[kCount];
  run(kX4, need, occ, out);
  EXPECT_EQ(out[1].w, need[1]);
}

TEST(HintPillLayout, BothMiddlePillsGrowSplitTheGap) {
  const int need[kCount] = {0, 120, 120, 0};
  const bool occ[kCount] = {};
  for (const Device* d : {&kX3, &kX4}) {
    Pill out[kCount];
    run(*d, need, occ, out);
    EXPECT_GE(out[2].x - (out[1].x + out[1].w), kGap) << d->name;
    EXPECT_GT(out[1].w, kNominalW) << d->name;
    EXPECT_GT(out[2].w, kNominalW) << d->name;
  }
}

TEST(HintPillLayout, NothingChangesWhenEverythingFits) {
  const int need[kCount] = {60, 70, 0, 80};
  const bool occ[kCount] = {false, false, true, false};
  for (const Device* d : {&kX3, &kX4}) {
    Pill out[kCount];
    run(*d, need, occ, out);
    for (int i = 0; i < kCount; i++) {
      EXPECT_EQ(out[i].x, d->pos[i]) << d->name << " " << i;
      EXPECT_EQ(out[i].w, kNominalW) << d->name << " " << i;
    }
  }
}

}  // namespace
