#include "components/covers/FallbackCover.h"

#include <GfxRenderer.h>
#include <InflateReader.h>

#include <cstdlib>
#include <numeric>

#include "components/covers/CoverFrames.h"
#include "components/covers/CoverTitleLayout.h"
#include "fontIds.h"

namespace FallbackCover {
namespace {

const coverframes::Frame* findFrame(const int style, const int w, const int h) {
  for (int s = 0; s < coverframes::kSizes; ++s) {
    const coverframes::Frame& f = coverframes::kFrames[style * coverframes::kSizes + s];
    if (f.width == w && f.height == h) return &f;
  }
  return nullptr;
}

// 同 LyraTheme::drawTitleCoverPlaceholder 的變體選擇（v175）：FNV-1a 32 % 3
int styleFor(const std::string& title) {
  const uint32_t h = std::accumulate(title.begin(), title.end(), uint32_t{2166136261u},
                                     [](const uint32_t acc, const unsigned char c) { return (acc ^ c) * 16777619u; });
  return static_cast<int>(h % static_cast<uint32_t>(coverframes::kStyles));
}

}  // namespace

bool supports(const int w, const int h) { return findFrame(0, w, h) != nullptr; }

bool draw(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const std::string& title,
          const std::string& author) {
  const coverframes::Frame* f = findFrame(styleFor(title), w, h);
  if (!f) return false;
  const size_t stride = (static_cast<size_t>(w) + 7) / 8;
  const size_t bytes = stride * static_cast<size_t>(h);
  auto* buf = static_cast<uint8_t*>(malloc(bytes));
  if (!buf) return false;
  InflateReader inflater;
  inflater.init(false);  // 一次解完：輸出緩衝區就是字典，不配 32 KB 窗口
  inflater.setSource(f->deflated, f->deflatedLen);
  const bool ok = inflater.read(buf, bytes);
  inflater.deinit();
  if (!ok) {
    free(buf);
    return false;
  }
  // 逐點畫黑點（renderer 的邏輯座標，直向／橫向都對；drawImage 是面板原生方向，不能用）
  for (int yy = 0; yy < h; ++yy) {
    const uint8_t* row = buf + static_cast<size_t>(yy) * stride;
    for (int xx = 0; xx < w; ++xx) {
      if (row[xx >> 3] & (0x80 >> (xx & 7))) renderer.drawPixel(x + xx, y + yy, true);
    }
  }
  free(buf);

  // 書名區：左右各留 4 px、上下各留 3 px
  const int padX = 4;
  const int padY = 3;
  const int boxX = x + f->textX + padX;
  const int boxW = f->textW - 2 * padX;
  const int boxY = y + f->textY + padY;
  const int boxH = f->textH - 2 * padY;
  const int authorLineH = renderer.getLineHeight(UI_10_FONT_ID) - 2;
  const int gap = 6;
  int authorH = author.empty() ? 0 : gap + authorLineH;
  const auto width = [&](const std::string& s, const bool big) {
    return renderer.getTextWidth(big ? UI_12_FONT_ID : UI_10_FONT_ID, s.c_str(), EpdFontFamily::BOLD);
  };
  const auto lineH = [&](const bool big) { return renderer.getLineHeight(big ? UI_12_FONT_ID : UI_10_FONT_ID); };
  // 書名優先：帶作者會讓書名截斷或斷字（v370 裝飾藝術款寬而矮）就只放書名 —— 字不畫到花紋上
  bool showAuthor = false;
  const covertitle::Layout lay = covertitle::fitWithAuthor(title, boxW, boxH, authorH, width, lineH, showAuthor);
  if (!showAuthor) authorH = 0;
  const int font = lay.big ? UI_12_FONT_ID : UI_10_FONT_ID;
  const int lh = renderer.getLineHeight(font) - 2;
  const int blockH = static_cast<int>(lay.lines.size()) * lh + authorH;
  int ty = boxY + (boxH - blockH) / 2;
  for (const std::string& line : lay.lines) {
    const int tw = renderer.getTextWidth(font, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(font, boxX + (boxW - tw) / 2, ty - 2, line.c_str(), true, EpdFontFamily::BOLD);
    ty += lh;
  }
  if (showAuthor) {
    ty += gap;
    const std::string a = renderer.truncatedText(UI_10_FONT_ID, author.c_str(), boxW);
    const int aw = renderer.getTextWidth(UI_10_FONT_ID, a.c_str());
    renderer.drawText(UI_10_FONT_ID, boxX + (boxW - aw) / 2, ty - 2, a.c_str(), true);
  }
  return true;
}

}  // namespace FallbackCover
