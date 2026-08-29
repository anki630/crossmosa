#pragma once

#include <stdint.h>

#include <string>

#include "ImageToFramebufferDecoder.h"

// v162（P6，維護者拍板「最大化閱讀體驗」）：GIF 支援。
// 台灣/歐美通路的舊排版書（實測 Brandon Sanderson Sampler 有 119 張 GIF87a/89a，
// 含 528×798 的整頁地圖）此前在排版階段就被 ImageDecoderFactory 靜默丟棄。
// 解碼走 vendored 的 bitbank2/AnimatedGIF（lib/AnimatedGIF，Apache-2.0，MCU 實證），
// 只解【第一幀】——e-ink 不放動畫，第一幀就是內容。
// 管線形狀與 PngToFramebufferConverter 同構：逐行 callback → 調色盤→灰階 →
// Bresenham 縮放 → DirectPixelWriter + PixelCache（.pxc）。
class GifToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "GIF"; }
};
