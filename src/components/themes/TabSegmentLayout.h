#pragma once

// v336：Formosa Pro 分頁列（segmented control）的分段寬度 —— 純函式，主機測試在 test/tab_segment_layout/。
//
// 為什麼：原本外框只包到「字＋內距」那麼寬，靠左、右邊留空（X3 中文、X4 英文都沒填滿）。
//   維護者 2026-09-23：「將整個寬度填滿比較好看」。
//
// 規則（一律剛好填滿 areaW，除了 ③）：
//   ① 最寬的那段放得進平分寬 → 平分（iOS 分段選擇器的預設）。
//   ② 放不下平分（最長的字比平分寬還寬，例如 X3 英文 "Controls"、X4 英文 "Layout"）
//      → 各段先拿自己的寬，剩下的空間再平分 —— 仍然填滿，字不被擠。
//   ③ 自然寬總和本身就放不下（呼叫端該先換小一號字，目前沒有這種語言）→ 各段自然寬、不填滿，
//      字不被壓；回傳 false。
//   除不盡的像素從左邊開始每段 +1，所以總和逐像素等於 areaW。
namespace TabSegmentLayout {

constexpr int kMaxSegments = 8;

// natural[i]：第 i 段需要的寬（字寬＋左右內距）。outX[i]：相對 area 左緣的起點；outW[i]：段寬。
inline bool layout(const int* natural, const int n, const int areaW, const int gap, int* outX, int* outW) {
  if (n <= 0) return false;
  int sumNat = 0;
  int maxNat = 0;
  for (int i = 0; i < n; i++) {
    sumNat += natural[i];
    if (natural[i] > maxNat) maxNat = natural[i];
  }
  const int avail = areaW - (n - 1) * gap;  // 所有段寬加起來可以用多少
  bool filled = true;
  if (avail >= 0 && avail / n >= maxNat) {  // ① 平分
    const int base = avail / n;
    const int rem = avail % n;
    for (int i = 0; i < n; i++) outW[i] = base + (i < rem ? 1 : 0);
  } else if (avail >= sumNat) {  // ② 自然寬＋剩餘平分
    const int extra = avail - sumNat;
    const int share = extra / n;
    const int rem = extra % n;
    for (int i = 0; i < n; i++) outW[i] = natural[i] + share + (i < rem ? 1 : 0);
  } else {  // ③ 放不下：不填滿、不壓字
    for (int i = 0; i < n; i++) outW[i] = natural[i];
    filled = false;
  }
  int x = 0;
  for (int i = 0; i < n; i++) {
    outX[i] = x;
    x += outW[i] + gap;
  }
  return filled;
}

}  // namespace TabSegmentLayout
