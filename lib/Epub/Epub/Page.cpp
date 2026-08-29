#include "Page.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>
#include <new>

char Page::lastAllocFail[96] = {0};

void Page::noteAllocFail(const char* where, size_t bytes) {
  // v194：lib 不能碰 DiagLog；先到先得，src 讀走寫成 ALLOCFAIL。
  LOG_ERR("PGE", "ALLOCFAIL where=%s bytes=%u max=%u", where, static_cast<unsigned>(bytes),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (lastAllocFail[0] != '\0') return;
  snprintf(lastAllocFail, sizeof(lastAllocFail), "where=%s bytes=%u max=%u", where, static_cast<unsigned>(bytes),
           static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

namespace {

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::shared_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const int fontId, const int xOffset, const int yOffset, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

}  // namespace

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: null TextBlock");
    return nullptr;
  }

  auto* line = new (std::nothrow) PageLine(std::move(tb), xPos, yPos);
  if (!line) {
    // v194：LOG_ERR 在這台等於丟掉。與 PageImage／Page 同一套 noteAllocFail。
    Page::noteAllocFail("PageLine:deserialize", sizeof(PageLine));
    return nullptr;
  }
  return std::unique_ptr<PageLine>(line);
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

void PageImage::renderPlaceholder(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  imageBlock->renderPlaceholder(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  if (!ib) {
    // v194：ImageBlock 反序列化失敗（含配不到）。LOG_ERR 等於丟掉，走既有 noteFailure。
    ImageBlock::noteFailure("pageimage-null-ib");
    return nullptr;
  }
  // v194：throwing new 在 -fno-exceptions 下 OOM 會 abort；載入路徑配不到就維持上一頁。
  auto* img = new (std::nothrow) PageImage(std::move(ib), xPos, yPos);
  if (!img) {
    Page::noteAllocFail("PageImage:deserialize", sizeof(PageImage));
    return nullptr;
  }
  return std::unique_ptr<PageImage>(img);
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
}

bool PageHorizontalRule::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, thickness);
  return true;
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, width);
  serialization::readPod(file, thickness);

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto* rule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, yPos);
  if (!rule) {
    // v194：LOG_ERR 在這台等於丟掉。與 PageImage／Page 同一套 noteAllocFail。
    Page::noteAllocFail("PageHorizontalRule:deserialize", sizeof(PageHorizontalRule));
    return nullptr;
  }
  return std::unique_ptr<PageHorizontalRule>(rule);
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, [](const PageElement&) { return true; });
}

void Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

void Page::renderWithImagePlaceholders(GfxRenderer& renderer, const int fontId, const int xOffset,
                                       const int yOffset) const {
  for (const auto& element : elements) {
    if (element->getTag() == TAG_PageImage) {
      static_cast<const PageImage&>(*element).renderPlaceholder(renderer, xOffset, yOffset);
    } else {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

bool Page::serialize(HalFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  serialization::writePod(file, fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  // v194：載入路徑配不到 Page 就回 nullptr，呼叫端走 PAGELOAD deferred／維持上一頁。
  auto page = std::unique_ptr<Page>(new (std::nothrow) Page());
  if (!page) {
    noteAllocFail("Page:deserialize", sizeof(Page));
    return nullptr;
  }

  uint16_t count;
  serialization::readPod(file, count);

  // Reserve up front so a page load costs one allocation for the element vector
  // instead of a grow-copy-free cycle every doubling. `count` is untrusted (it
  // comes straight off the SD cache), so clamp it: a real page holds a few dozen
  // elements, while a corrupt header could ask for 65535 * sizeof(shared_ptr) and
  // abort() on the failed allocation (vector's operator new is throwing, and this
  // firmware builds with -fno-exceptions). Under-reserving is harmless -- the
  // push_back path below still grows normally.
  static constexpr uint16_t RESERVE_CAP = 256;
  page->elements.reserve(std::min(count, RESERVE_CAP));

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  serialization::readPod(file, fnCount);
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  // v187：fnCount×288 B 連續（throwing resize）。瀕死時守不過就跳過註腳資料、頁照常回傳（這次沒有註腳選單）。
  if (fnCount > 0 && heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) <
                         static_cast<size_t>(fnCount) * sizeof(FootnoteEntry) + 1024) {
    LOG_ERR("PGE", "Skipping %u footnotes (low memory)", fnCount);
    Page::footnoteDrops = static_cast<uint16_t>(Page::footnoteDrops + fnCount);
    file.seekCur(static_cast<int64_t>(fnCount) * static_cast<int64_t>(sizeof(FootnoteEntry::number) + sizeof(FootnoteEntry::href)));
    return page;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  return page;
}

uint16_t Page::footnoteDrops = 0;

void Page::addFootnote(const char* number, const char* href) {
  if (footnotes.size() >= MAX_FOOTNOTES_PER_PAGE) return;  // Cap per-page footnotes
  if (footnotes.size() == footnotes.capacity()) {
    // 成長要配新塊、複製、再放舊塊：先確認連續塊夠，不夠就丟這條註腳，別在建置視窗 abort。
    // libstdc++ 的成長律：size + max(size,1)（1,2,4,8,16）。
    const size_t nextCap = footnotes.capacity() ? footnotes.capacity() * 2 : 1;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < nextCap * sizeof(FootnoteEntry) + 2048) {
      LOG_ERR("PGE", "Dropping footnote (low memory for vector growth)");
      footnoteDrops++;
      return;
    }
  }
  FootnoteEntry entry;
  strncpy(entry.number, number, sizeof(entry.number) - 1);
  entry.number[sizeof(entry.number) - 1] = '\0';
  strncpy(entry.href, href, sizeof(entry.href) - 1);
  entry.href[sizeof(entry.href) - 1] = '\0';
  footnotes.push_back(entry);
}
