#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  // v120 儀器:最後一次解碼失敗的圖片路徑(空字串 = 沒有待回報的失敗)。
  // 由 src 端讀走並清空 —— lib 不能反向依賴 src/util/DiagLog。
  static char lastFailPath[96];

  // v125 儀器:最後一次【通過邊界檢查、真的畫下去】的圖片幾何(空字串 = 這次 render 沒有圖)。
  // 為什麼需要它:失敗碼答不出「圖被畫出來了但小到看不見」。v123 的修正會把溢出的圖縮到
  // 這一頁剩下的高度,如果某本書的上緣邊距吃掉幾乎整頁,縮完就是一條線 —— 那是「成功」,
  // 不會有任何失敗碼。同樣由 src 端讀走並清空。
  static char lastDrawGeom[64];

  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  // v125:統一的失敗記錄點。三個出口(檔案不存在 / 大小為 0 / 沒有解碼器)在此之前完全沒有
  // 紀錄,而它們正是「白框 + log 全空」的形狀 —— diag124 零 IMGFAIL 就是這樣來的。
  // 先到先得:不覆寫還沒被讀走的紀錄,免得同一頁的第二張圖蓋掉第一張的原因(v122 踩過)。
  static void noteRenderFailure(int stage, const std::string& path, int w, int h, int x, int y);

  bool imageExists() const;
  bool hasValidCache() const;
  bool needsDecode() const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
  static void clearSessionRenderFailures();

  // Low-memory relief hook. A first-time image decode allocates the ~44 KB PNG /
  // ~20 KB JPEG decoder; under heap pressure (the resident SD reading font holds
  // tens of KB) that fails and the image falls back to a placeholder. The reader
  // wires this so the decode can temporarily free RAM (unload the SD reading
  // font) and restore it. Plain function pointers keep lib/Epub independent of
  // the app's font system. reliefFn frees memory; restoreFn reloads it.
  using MemoryReliefFn = void (*)(void* ctx);
  static void setMemoryReliefHooks(MemoryReliefFn reliefFn, MemoryReliefFn restoreFn, void* ctx);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;
};
