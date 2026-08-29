#include "Lyra3CoversTheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
}  // namespace

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / 3;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0;
           i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount); i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath =
              UITheme::getCoverThumbPath(coverPath, Lyra3CoversMetrics::values.homeCoverHeight);

          // First time: load cover from SD and render
          HalFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              float coverHeight = static_cast<float>(bitmap.getHeight());
              float coverWidth = static_cast<float>(bitmap.getWidth());
              float ratio = coverWidth / coverHeight;
              const float tileRatio = static_cast<float>(tileWidth - 2 * hPaddingInSelection) /
                                      static_cast<float>(Lyra3CoversMetrics::values.homeCoverHeight);
              float cropX = 1.0f - (tileRatio / ratio);

              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection, Lyra3CoversMetrics::values.homeCoverHeight,
                                  cropX);
            } else {
              hasCover = false;
            }
            file.close();
          }
        }
        // Draw either way
        renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                          Lyra3CoversMetrics::values.homeCoverHeight, true);

        if (!hasCover) {
          // v175（使用者的設計稿）：沒有封面（txt、封面缺失、尚未產生）一律畫「書名幾何封面」。
          LyraTheme::drawTitleCoverPlaceholder(
              renderer, tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
              Lyra3CoversMetrics::values.homeCoverHeight,
              LyraTheme::displayTitleFor(recentBooks[i].title, recentBooks[i].path));
        }

        // v168（使用者拍板）：進度徽章畫在封面右下角 —— 白底黑字＋1px 框（蓋在封面上
        // 要可讀；小面積收編例外，同值欄類）。畫在快照之前，跟著 cover buffer 一起保存。
        if (recentBooks[i].progressPercent > 0) {
          const std::string pct = std::to_string(recentBooks[i].progressPercent) + "%";
          const int tw = renderer.getTextWidth(UI_10_FONT_ID, pct.c_str());
          const int bh = renderer.getLineHeight(UI_10_FONT_ID);
          const int bw = tw + 10;
          const int bx = tileX + hPaddingInSelection + (tileWidth - 2 * hPaddingInSelection) - bw - 4;
          const int by = tileY + hPaddingInSelection + Lyra3CoversMetrics::values.homeCoverHeight - bh - 4;
          renderer.fillRoundedRect(bx, by, bw, bh, 4, Color::White);
          renderer.drawRoundedRect(bx, by, bw, bh, 1, 4, true);
          renderer.drawText(UI_10_FONT_ID, bx + 5, by, pct.c_str(), true);
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), Lyra3CoversMetrics::values.homeRecentBooksCount);
         i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;

      const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;

      // v168（使用者拍板）：百分比改畫在封面右下角小徽章 —— 書名保有完整 3 行，
      // 徽章白底黑字帶 1px 框（蓋在封面上要可讀，屬 v50 原則的小面積收編例外，同值欄類）。
      const std::string displayTitle = LyraTheme::displayTitleFor(recentBooks[i].title, recentBooks[i].path);
      auto titleLines = renderer.wrappedText(UI_10_FONT_ID, displayTitle.c_str(), maxLineWidth, 3);

      const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int dynamicBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight;
      // Add a little padding below the text inside the selection box just like the top padding (5 + hPaddingSelection)
      (void)dynamicBlockHeight;
      // v182（維護者回饋，同 Pro v180）：選取框固定「封面＋3 行書名」的高度，不隨書名行數變 —— 三張卡一致。
      const int dynamicTitleBoxHeight = 3 * titleLineHeight + hPaddingInSelection + 5;

      if (bookSelected) {
        // Draw selection box（v44：整卡 2px 圓角外框；三卡相鄰無溝槽，全機語言的左豎條在此省略）
        renderer.drawRoundedRect(
            tileX, tileY, tileWidth,
            hPaddingInSelection + Lyra3CoversMetrics::values.homeCoverHeight + dynamicTitleBoxHeight, 2, cornerRadius,
            true);
      }

      int currentY = tileY + Lyra3CoversMetrics::values.homeCoverHeight + hPaddingInSelection + 5;
      for (const auto& line : titleLines) {
        renderer.drawText(UI_10_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
        currentY += titleLineHeight;
      }

    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}
