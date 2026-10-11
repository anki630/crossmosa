#pragma once

// Formosa Cover：一格封面（首頁正在閱讀 160×240、最近閱讀 128×192、書架 144×216），2026-10-07（v367 最近閱讀
// 112→128）。
//   有縮圖（thumb_<高度>.bmp）就畫縮圖；沒有就畫歐風底框＋書名（FallbackCover）；底框尺寸不支援時退回 v175
//   書名幾何封面。 最後補圓角遮罩與 1 px 外框（同 Formosa Pro 首頁封面）。

#include <cstdint>
#include <string>

class GfxRenderer;

namespace CoverTile {

constexpr int kRadius = 12;
constexpr int kSmoothing = 60;  // Formosa Pro 的 cornerSmoothing（iOS 連續曲率）

// thumbPath：縮圖完整路徑（空＝沒有）。title／author：沒縮圖時畫在底框上（author 可空）。
// favorite：右上角畫我的最愛星號（v367）。回傳 true＝畫的是真封面縮圖。
bool draw(GfxRenderer& renderer, int x, int y, int w, int h, const std::string& thumbPath, const std::string& title,
          const std::string& author, bool favorite = false);

// 我的最愛星號：封面右上角，白圓底＋黑星（直徑 23 px），真封面上也看得清楚。
void drawFavoriteBadge(const GfxRenderer& renderer, int coverX, int coverY, int coverW);

// 選取框：封面格外圍 3 px 圓角框（v370 由 2 px
// 加粗：維護者「很難一眼看到選到哪一項」）。erase＝用白色擦掉（移動選取時不重畫封面）。
void drawRing(const GfxRenderer& renderer, int x, int y, int w, int h, bool erase);

// v380 量測（書架「全部」第一頁＝六張真封面，畫 0.63 秒；一般頁 0.29 秒）：一個畫面裡所有封面格各段累計多久。
//   呼叫端整頁重畫前 beginStats()，送面板前 logStats() 寫一行 COVERDRAW。只計時，不改畫法。
//   lookupUs：呼叫端自己量的「找這本的縮圖路徑／書名」時間（書架的 thumbPathFor 對不在最近閱讀的書會 exists() 一次）。
void beginStats(const GfxRenderer& renderer);
void logStats(const GfxRenderer& renderer, const char* who, int page, uint32_t lookupUs);

}  // namespace CoverTile
