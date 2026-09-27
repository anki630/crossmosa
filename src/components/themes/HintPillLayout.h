#pragma once

// v335：底部按鍵提示「膠囊」的寬度分配 —— 純函式（不碰 renderer／gpio），Formosa 與 Formosa Pro 共用，
//   主機測試在 test/hint_pill_layout/。
//
// 為什麼需要它：膠囊原本一律 80px、字置中、不檢查寬度 → 放不下的字直接畫到膠囊外面
//   （2026-09-23 實機：X4 看圖的「設為待機畫面」；量過全部 23 個按鍵字串，4 個超出、3 個貼邊）。
//   中文一個字約 21px，80px 最多放三個字 —— 靠改字救不完，要讓膠囊能往旁邊的空位長。
//
// 規則：
//   ① 放得下（need ≤ nominalW）→ 原位原寬，一個像素都不動。
//   ② 放不下 → 以按鍵中心往兩側長，夾在「左右鄰居的邊緣 ∓ gap」與螢幕邊緣之間；
//      一側撞到就往另一側移 —— 膠囊永遠還罩著自己那顆鍵的中心（使用者靠位置認鍵）。
//   ③ 兩個相鄰的都要長 → 中間的空隙對半分，誰也不會壓到誰。
//   ④ 長到上限還放不下 → 回傳上限寬，文字由呼叫端截斷（truncatedText 加「…」）。
//   鄰居 ＝ 會畫出來的膠囊：有字的，或 Lyra 那種沒字也畫的小膠囊（occupied）。
namespace HintPillLayout {

constexpr int kCount = 4;
constexpr int kGap = 4;       // 兩個膠囊之間至少留的空
constexpr int kEdge = 2;      // 離螢幕左右邊緣至少留的空
constexpr int kTextPad = 6;   // 字到膠囊左右邊的最小內距（呼叫端算 need 時用）

struct Pill {
  int x;
  int w;  // 0 ＝ 這顆不畫
};

inline void layout(const int (&nominalX)[kCount], const int nominalW, const int screenW, const int (&need)[kCount],
                   const bool (&occupied)[kCount], Pill (&out)[kCount]) {
  bool present[kCount];
  bool grows[kCount];
  for (int i = 0; i < kCount; i++) {
    present[i] = need[i] > 0 || occupied[i];
    grows[i] = need[i] > nominalW;
  }
  for (int i = 0; i < kCount; i++) {
    if (!present[i]) {
      out[i] = {nominalX[i], 0};
      continue;
    }
    if (!grows[i]) {
      out[i] = {nominalX[i], nominalW};
      continue;
    }
    int left = kEdge;
    for (int j = i - 1; j >= 0; j--) {
      if (!present[j]) continue;
      const int jRight = nominalX[j] + nominalW;
      left = grows[j] ? (jRight + nominalX[i]) / 2 + kGap / 2 : jRight + kGap;
      break;
    }
    int right = screenW - kEdge;
    for (int k = i + 1; k < kCount; k++) {
      if (!present[k]) continue;
      const int iRight = nominalX[i] + nominalW;
      right = grows[k] ? (iRight + nominalX[k]) / 2 - kGap / 2 : nominalX[k] - kGap;
      break;
    }
    const int room = right - left;
    if (room <= nominalW) {  // 沒有比原本更多的空間（不會發生在 X3／X4 的位置上）→ 原位原寬，只長不縮
      out[i] = {nominalX[i], nominalW};
      continue;
    }
    const int w = need[i] < room ? need[i] : room;
    int x = nominalX[i] + nominalW / 2 - w / 2;
    if (x + w > right) x = right - w;
    if (x < left) x = left;
    out[i] = {x, w};
  }
}

}  // namespace HintPillLayout
