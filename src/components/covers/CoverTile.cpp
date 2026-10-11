#include "components/covers/CoverTile.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstdlib>

#include "components/covers/FallbackCover.h"
#include "components/themes/lyra/LyraTheme.h"
#include "util/DiagLog.h"

namespace CoverTile {

namespace {
struct Stats {
  uint32_t t0 = 0;  // beginStats 的 micros()
  uint16_t tiles = 0, real = 0, fallback = 0;
  uint32_t openUs = 0, parseUs = 0, bmpUs = 0, fbUs = 0, edgeUs = 0;
  uint32_t smb0 = 0;  // beginStats 時的圓角表建表次數
};
Stats sStats;
}  // namespace

void beginStats(const GfxRenderer& renderer) {
  sStats = Stats{};
  sStats.t0 = micros();
  sStats.smb0 = GfxRenderer::smoothTableBuilds();
  renderer.resetBitmapStats();
  renderer.setBitmapSampling(true);
  Bitmap::beginIoStats();
}

void logStats(const GfxRenderer& renderer, const char* who, const int page, const uint32_t lookupUs) {
  const uint32_t frameUs = micros() - sStats.t0;
  renderer.setBitmapSampling(false);
  Bitmap::endIoStats();
  const auto& b = renderer.bitmapStats();
  DiagLog::line(
      "COVERDRAW who=%s page=%d tiles=%u real=%u fb=%u open=%lu parse=%lu bmp=%lu read=%lu pix=%lu rows=%lu "
      "onebit=%u fbdraw=%lu edge=%lu lookup=%lu frame=%lu scaled=%u fast=%lu smb=%lu io=%lu row1=%lu",
      who, page, static_cast<unsigned>(sStats.tiles), static_cast<unsigned>(sStats.real),
      static_cast<unsigned>(sStats.fallback), static_cast<unsigned long>(sStats.openUs / 1000),
      static_cast<unsigned long>(sStats.parseUs / 1000), static_cast<unsigned long>(sStats.bmpUs / 1000),
      static_cast<unsigned long>(b.readUs / 1000), static_cast<unsigned long>(b.pixUs / 1000),
      static_cast<unsigned long>(b.rows), static_cast<unsigned>(b.oneBit),
      static_cast<unsigned long>(sStats.fbUs / 1000), static_cast<unsigned long>(sStats.edgeUs / 1000),
      static_cast<unsigned long>(lookupUs / 1000), static_cast<unsigned long>(frameUs / 1000),
      static_cast<unsigned>(b.scaled), static_cast<unsigned long>(b.fastRows),
      static_cast<unsigned long>(GfxRenderer::smoothTableBuilds() - sStats.smb0),
      static_cast<unsigned long>(Bitmap::ioUs() / 1000), static_cast<unsigned long>(Bitmap::fastRows1Bit()));
  // v383：每張圖取樣一列，比「平常」與「關中斷」的每點 CPU 週期（×10）。
  //   n ≈ fast → 迴圈本身就這麼慢；n ≫ fast → 被打斷（或第二次跑時快取已暖，codex 提醒）。slow＝同一列逐點 drawPixel。
  //   都是「每個來源像素」的平均（白點也算）。
  if (b.sampRows > 0 && b.sampPx > 0) {
    DiagLog::line("COVERSAMP who=%s page=%d mhz=%u rows=%u px=%lu us=%lu n10=%lu fast10=%lu slow10=%lu", who, page,
                  static_cast<unsigned>(getCpuFrequencyMhz()), static_cast<unsigned>(b.sampRows),
                  static_cast<unsigned long>(b.sampPx), static_cast<unsigned long>(b.sampUs),
                  static_cast<unsigned long>(static_cast<uint64_t>(b.sampCycN) * 10 / b.sampPx),
                  static_cast<unsigned long>(static_cast<uint64_t>(b.sampCycFast) * 10 / b.sampPx),
                  static_cast<unsigned long>(static_cast<uint64_t>(b.sampCycSlow) * 10 / b.sampPx));
  }
}

bool draw(GfxRenderer& renderer, const int x, const int y, const int w, const int h, const std::string& thumbPath,
          const std::string& title, const std::string& author, const bool favorite) {
  bool real = false;
  sStats.tiles++;
  if (!thumbPath.empty()) {
    HalFile file;
    uint32_t t = micros();
    const bool opened = Storage.openFileForRead("COVER", thumbPath, file);
    sStats.openUs += micros() - t;
    if (opened) {
      Bitmap bitmap(file);
      t = micros();
      const bool parsed = bitmap.parseHeaders() == BmpReaderError::Ok;
      sStats.parseUs += micros() - t;
      if (parsed && bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
        // 覆蓋整框（教訓 A-9）。v386（diag385：每頁剩一張走縮放）：縮圖只保證蓋滿框、溢出的那一維不裁 →
        //   比框瘦長的封面縮圖比框高，以前照長寬比只裁左右、高的那張整張等比縮小（逐點浮點、右邊留約 9 點白）。
        //   縮圖本來就是照這個框產的（至少一邊跟框差不到 2 點）→ 寬、高各自把多出的置中裁掉，不縮放。
        //   +0.5：floor(W×crop／2) 正好是 (W−w)／2 取整，裁後比框小半點 → drawBitmap 不判成縮放（多 1 點由 v382
        //   夾框裁）。 上下裁只給由上往下存的圖：由下往上＋cropY 的 drawBitmap 第一列就
        //   break（舊問題；縮圖轉檔器都寫由上往下）。
        const int bw = bitmap.getWidth();
        const int bh = bitmap.getHeight();
        float cropX = 0.0f;
        float cropY = 0.0f;
        if (std::abs(bw - w) <= 2 || std::abs(bh - h) <= 2) {  // 絕對差：遠小於框的圖（小 XTC）照舊等比縮（codex）
          if (bw > w) cropX = (static_cast<float>(bw - w) + 0.5f) / static_cast<float>(bw);
          if (bh > h && bitmap.isTopDown()) cropY = (static_cast<float>(bh - h) + 0.5f) / static_cast<float>(bh);
        } else {  // 不是這個框的縮圖（舊尺寸等）：照舊依長寬比只裁左右，其餘交給 drawBitmap 縮放
          const float ratio = static_cast<float>(bw) / static_cast<float>(bh);
          const float tileRatio = static_cast<float>(w) / static_cast<float>(h);
          cropX = ratio > tileRatio ? 1.0f - (tileRatio / ratio) : 0.0f;
        }
        t = micros();
        renderer.drawBitmap(bitmap, x, y, w, h, cropX, cropY);
        sStats.bmpUs += micros() - t;
        real = true;
        sStats.real++;
      }
      file.close();
    }
  }
  uint32_t t = micros();
  if (!real) {
    renderer.fillRect(x, y, w, h, false);
    if (!FallbackCover::draw(renderer, x, y, w, h, title, author)) {
      LyraTheme::drawTitleCoverPlaceholder(renderer, x, y, w, h, title);
    }
    sStats.fallback++;
    sStats.fbUs += micros() - t;
    t = micros();
  }
  renderer.maskSmoothRoundedRectOutsideCorners(x, y, w, h, kRadius, kSmoothing, Color::White);
  renderer.drawSmoothRoundedRect(x, y, w, h, 1, kRadius, kSmoothing, true);
  if (favorite) drawFavoriteBadge(renderer, x, y, w);
  sStats.edgeUs += micros() - t;
  return real;
}

void drawFavoriteBadge(const GfxRenderer& renderer, const int coverX, const int coverY, const int coverW) {
  constexpr int kR = 11;  // 圓半徑
  const int cx = coverX + coverW - 14;
  const int cy = coverY + 14;
  renderer.fillSmoothRoundedRect(cx - kR, cy - kR, 2 * kR + 1, 2 * kR + 1, kR, kSmoothing, Color::White);
  renderer.drawSmoothRoundedRect(cx - kR, cy - kR, 2 * kR + 1, 2 * kR + 1, 1, kR, kSmoothing, true);
  // 五角星：外半徑 8、內半徑 3.6，尖端朝上（中心往下 1 px，視覺置中）
  static constexpr int kOx[10] = {0, 2, 8, 3, 5, 0, -5, -3, -8, -2};
  static constexpr int kOy[10] = {-8, -3, -2, 1, 6, 4, 6, 1, -2, -3};
  int xs[10];
  int ys[10];
  for (int k = 0; k < 10; ++k) {
    xs[k] = cx + kOx[k];
    ys[k] = cy + 1 + kOy[k];
  }
  renderer.fillPolygon(xs, ys, 10, true);
}

void drawRing(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const bool erase) {
  renderer.drawSmoothRoundedRect(x, y, w, h, 3, 16, kSmoothing, !erase);
}

}  // namespace CoverTile
