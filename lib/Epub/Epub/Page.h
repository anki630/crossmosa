#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "FootnoteEntry.h"
#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,
  TAG_PageHorizontalRule = 3,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) = 0;
  virtual bool serialize(HalFile& file) = 0;
  virtual PageElementTag getTag() const = 0;  // Add type identification
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  const std::shared_ptr<TextBlock>& getBlock() const { return block; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(HalFile& file);
};

// New PageImage class
class PageImage final : public PageElement {
  std::shared_ptr<ImageBlock> imageBlock;

 public:
  PageImage(std::shared_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), imageBlock(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  void renderPlaceholder(GfxRenderer& renderer, int xOffset, int yOffset) const;
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageImage; }
  static std::unique_ptr<PageImage> deserialize(HalFile& file);
  const ImageBlock& getImageBlock() const { return *imageBlock; }
};

class PageHorizontalRule final : public PageElement {
  uint16_t width;
  uint8_t thickness;

 public:
  PageHorizontalRule(uint16_t width, uint8_t thickness, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), width(width), thickness(thickness) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(HalFile& file) override;
  PageElementTag getTag() const override { return TAG_PageHorizontalRule; }
  static std::unique_ptr<PageHorizontalRule> deserialize(HalFile& file);
};

class Page {
 public:
  // the list of block index and line numbers on this page
  std::vector<std::shared_ptr<PageElement>> elements;
  std::vector<FootnoteEntry> footnotes;
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = 16;

  // Zero-based visible-codepoint offset where this page starts. Not part of the serialized page
  // body (it lives in the section's visible-offset LUT); Section::loadPage* fills it in from the
  // build LUT or the on-disk LUT while the page file is already open, so the reader can persist
  // progress without a second section-file open per page turn.
  uint32_t visibleTextOffset = 0;

  // v187：項目 288 B；vector 倍增 8→16 在建置視窗要 4,608 B 連續（throwing）——成長前先看連續塊。
  void addFootnote(const char* number, const char* href);
  // v187 證人：建置時（addFootnote／parser pendingFootnotes）或載入時（deserialize）因記憶體丟掉的註腳數。
  // lib 不能碰 DiagLog，由閱讀器讀走後歸零、寫成 FNDROP。
  static uint16_t footnoteDrops;

  // v194：nothrow 配不到 Page／PageImage 時的證人（先到先得）。src 讀走寫成 ALLOCFAIL。
  static char lastAllocFail[96];
  static void noteAllocFail(const char* where, size_t bytes);
  // v249（codex 複查）：noteAllocFail 被呼叫的累計次數。Section 用「deserialize 前後有沒有變」判斷這次失敗是不是記憶體不足
  // —— 原本看 lastAllocFail[0]，但那是給主迴圈讀走清掉的診斷緩衝，讀走的時機剛好卡在中間就會誤判成壞檔、
  // 在記憶體最緊的時候刪章節快取重排（v152 要避免的正是這個）。別的 task 同時失敗只會讓它多判一次「記憶體不足」（重試），無害。
  static uint32_t allocFailCount();

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  void renderImages(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  void renderWithImagePlaceholders(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  bool serialize(HalFile& file) const;
  static std::unique_ptr<Page> deserialize(HalFile& file);

  // Check if page contains any images (used to force full refresh)
  bool hasImages() const {
    return std::any_of(elements.begin(), elements.end(),
                       [](const std::shared_ptr<PageElement>& el) { return el->getTag() == TAG_PageImage; });
  }

  // v267：這一頁有幾張圖。灰階帶高的預配只給【單圖頁】—— 多圖頁只有第一張能進像素快取的 RAM slot，
  //   用外框估會高估它，反而可能把尾段製造出來（見 EpubReaderActivity 的 planGrayStrip）。
  size_t imageCount() const {
    return static_cast<size_t>(std::count_if(elements.begin(), elements.end(),
                                             [](const std::shared_ptr<PageElement>& el) {
                                               return el->getTag() == TAG_PageImage;
                                             }));
  }

  bool hasImagesNeedingDecode() const {
    return std::any_of(elements.begin(), elements.end(), [](const std::shared_ptr<PageElement>& element) {
      return element->getTag() == TAG_PageImage &&
             static_cast<const PageImage&>(*element).getImageBlock().needsDecode();
    });
  }

  // Get bounding box of all images on the page (union of image rects)
  // Returns false if no images. Coordinates are relative to page origin.
  bool getImageBoundingBox(int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) const {
    bool found = false;
    int16_t minX = INT16_MAX, minY = INT16_MAX, maxX = INT16_MIN, maxY = INT16_MIN;
    for (const auto& el : elements) {
      if (el->getTag() == TAG_PageImage) {
        const auto& img = static_cast<const PageImage&>(*el);
        int16_t x = img.xPos;
        int16_t y = img.yPos;
        int16_t right = x + img.getImageBlock().getWidth();
        int16_t bottom = y + img.getImageBlock().getHeight();
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, right);
        maxY = std::max(maxY, bottom);
        found = true;
      }
    }
    if (found) {
      outX = minX;
      outY = minY;
      outW = maxX - minX;
      outH = maxY - minY;
    }
    return found;
  }
};
