#pragma once

// Formosa Cover：沒有封面縮圖的書（txt、還沒打開過、取不到封面）畫歐風底框＋書名（2026-10-07）。
//   底框十七種（歐風三款＋北歐針織三款／北歐民俗花／原住民織紋／貓／柴犬／勇者魔龍／公主／纏繞畫＋v373
//   鳥獸戲畫／愛麗絲／山海經／福爾摩斯），依書名雜湊輪流（FNV-1a % 17）； 點陣在
//   CoverFrames.h（scripts/gen_cover_frames.py 產生），只支援三種尺寸：160×240、144×216、128×192（v367 起，原
//   112×168）。 圓角遮罩與 1 px 外框由呼叫端畫（跟真封面同一套）。

#include <string>

class GfxRenderer;

namespace FallbackCover {

// 有這個尺寸的底框嗎（沒有就由呼叫端退回 v175 的書名幾何封面）
bool supports(int w, int h);

// 畫底框＋書名（＋作者，可空）。回傳 false＝尺寸不支援或解壓／配置失敗（什麼都沒畫）。
// 暫用堆積：解壓緩衝 ((w+7)/8)*h bytes（最大 4,800 B），畫完就釋放。
bool draw(const GfxRenderer& renderer, int x, int y, int w, int h, const std::string& title, const std::string& author);

}  // namespace FallbackCover
