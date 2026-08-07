#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

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
