#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "Epub/Epub/VerticalColumns.h"
#include "Epub/Epub/VerticalText.h"

namespace {

// X3/X4 的預設文字版心：viewable 邊（portrait 9/3/3/3，旋轉後跟著轉）、5px 使用者頁邊，
// 以及與 X3 實機 512x756 相同的狀態列保留量。純函式不依賴面板，這組資料專門防止某一方向漏測。
struct Viewport {
  int width;
  int height;
};

constexpr std::array<Viewport, 4> kViewports{{
    {512, 756},  // X3 528x792 portrait（實機）
    {770, 498},  // X3 792x528 landscape
    {464, 764},  // X4 480x800 portrait
    {778, 450},  // X4 800x480 landscape
}};

constexpr std::array<float, 7> kMeasuredEms{{45.8f, 46.0f, 54.0f, 58.3f, 67.0f, 41.7f, 29.0f}};

// 獨立的放欄模型（照 addColumnToPage／TXT renderPage）：第 k 欄（k=1 最右）左緣 ＝ vw − k·pitch ＋ dx，
// 字面貼著欄的左緣、寬 cell。量的是【墨水】兩側的空白，不是 cols×pitch 的格子。
// （v392 第一版用「零頭平分」的模型寫測試，測試跟實作共用同一個錯誤 → 全綠，實機截圖左 49／右 76。）
static void inkGaps(const int vw, const int pitch, const float cell, const int dx, float& left, float& right) {
  const int cols = vw / pitch;
  const float leftmost = static_cast<float>(vw - cols * pitch + dx);
  const float rightmostRight = static_cast<float>(vw - pitch + dx) + cell;
  left = leftmost;
  right = static_cast<float>(vw) - rightmostRight;
}

TEST(GridCenter, VerticalColumnsBalanceLeftAndRightInkForEveryDeviceSizeTierAndFontCell) {
  for (const auto vp : kViewports) {
    for (const float em : kMeasuredEms) {
      for (uint8_t tier = 0; tier < 3; ++tier) {
        for (const bool zhuyin : {false, true}) {
          const int pitch = vtext::columnPitchPx(em, tier, zhuyin);
          ASSERT_GT(pitch, 0);
          const float cell = vtext::columnCellPx(em, zhuyin);
          const int dx = vtext::centeredReverseGridOffset(vp.width, pitch, cell);
          float left = 0, right = 0;
          inkGaps(vp.width, pitch, cell, dx, left, right);
          EXPECT_LE(std::fabs(left - right), 1.0f)
              << "vw=" << vp.width << " em=" << em << " tier=" << int(tier) << " zhuyin=" << zhuyin;
          EXPECT_GE(left, 0.0f);
          EXPECT_GE(right, 0.0f);
        }
      }
    }
  }
}

TEST(GridCenter, KnownX3CasesMatchTheV392Screenshot) {
  // 實機 v392 截圖（GRID pitch=87 em16=928）：舊的 dx=-38 讓墨水左 41、右 68（版心內）。
  float left = 0, right = 0;
  inkGaps(512, 87, 58.0f, -38, left, right);
  EXPECT_FLOAT_EQ(left, 39.0f);
  EXPECT_FLOAT_EQ(right, 67.0f);
  const int dx = vtext::centeredReverseGridOffset(512, 87, 58.0f);
  EXPECT_EQ(dx, -24);
  inkGaps(512, 87, 58.0f, dx, left, right);
  EXPECT_LE(std::fabs(left - right), 1.0f);

  // 平常字級（pitch 69、em 46）：原本就幾乎平均（左 29、右 23），只該挪 3 px。
  EXPECT_EQ(vtext::centeredReverseGridOffset(512, 69, 46.0f), -3);

  // 變異檢查：短路成 0 或退回「零頭平分」（-38）都必紅。
  EXPECT_NE(vtext::centeredReverseGridOffset(512, 87, 58.0f), 0);
  EXPECT_NE(vtext::centeredReverseGridOffset(512, 87, 58.0f), -38);
}

TEST(GridCenter, ParagraphGapPageClampsLeftShiftAtTheActualLeftmostColumn) {
  constexpr int desiredDx = -14;  // 512 % 69 = 29，理想位移為 -14
  constexpr int minLeft = 5;      // 段落間隙讓最左欄不再落在 29

  // 變異證人：拿掉夾限、直接套理想位移時，這個實機型案例一定出版心。
  EXPECT_LT(minLeft + desiredDx, 0);
  const int pageDx = vtext::clampReverseGridOffset(desiredDx, minLeft);
  EXPECT_EQ(pageDx, -minLeft);
  EXPECT_GE(minLeft + pageDx, 0);
}

TEST(GridCenter, FixedPitchTxtColumnsUseTheSameClampWithoutChangingTheirOffset) {
  constexpr int viewportWidth = 512;
  constexpr int pitch = 69;
  const int desiredDx = vtext::centeredReverseGridOffset(viewportWidth, pitch, 46.0f);
  const int minLeft = viewportWidth - (viewportWidth / pitch) * pitch;

  EXPECT_EQ(vtext::clampReverseGridOffset(desiredDx, minLeft), desiredDx);
}

TEST(GridCenter, VerticalCellGridBalancesTopAndBottomUnlessHangingNeedsClamp) {
  constexpr float hangNeed = 16.0f;
  for (const auto vp : kViewports) {
    for (const float em : kMeasuredEms) {
      const auto centered = vtext::makeCenteredColumnGrid(static_cast<float>(vp.height), em, hangNeed);
      ASSERT_GT(centered.grid.charsPerColumn, 0);
      ASSERT_TRUE(centered.hangAllowed);
      const float top = static_cast<float>(centered.offsetPx);
      const float bottom = centered.grid.hangRoom - top;
      if (!centered.hangClamped) {
        EXPECT_LE(std::fabs(top - bottom), 1.0f)
            << "vh=" << vp.height << " em=" << em << " room=" << centered.grid.hangRoom;
      } else {
        EXPECT_LE(top + centered.grid.columnLength + hangNeed, static_cast<float>(vp.height) + 0.001f);
      }
    }
  }
}

TEST(GridCenter, X3Grid2UsesThirteenCellsAndCentersItsFiftyFourPixelRoom) {
  const auto centered = vtext::makeCenteredColumnGrid(756.0f, 54.0f, 16.0f);
  EXPECT_EQ(centered.grid.charsPerColumn, 13);
  EXPECT_FLOAT_EQ(centered.grid.columnLength, 702.0f);
  EXPECT_FLOAT_EQ(centered.grid.hangRoom, 54.0f);
  EXPECT_EQ(centered.offsetPx, 27);
  EXPECT_TRUE(centered.hangAllowed);
  EXPECT_FALSE(centered.hangClamped);
}

TEST(GridCenter, HangingClampKeepsInkInsideAndAttachedOutsideGrid) {
  // 3x30 格後剩 10px；8px 的句讀放得下，但普通置中的 5px 位移會只剩 5px，必須夾到 2px。
  const auto centered = vtext::makeCenteredColumnGrid(100.0f, 30.0f, 8.0f);
  ASSERT_EQ(centered.grid.charsPerColumn, 3);
  ASSERT_TRUE(centered.hangAllowed);
  EXPECT_TRUE(centered.hangClamped);
  EXPECT_EQ(centered.offsetPx, 2);
  EXPECT_FLOAT_EQ(static_cast<float>(centered.offsetPx) + centered.grid.columnLength + 8.0f, 100.0f);
}

}  // namespace
