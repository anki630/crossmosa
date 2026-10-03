#pragma once

#include <BufferedFile.h>
#include <HalStorage.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>

class BookMetadataCache {
 public:
  struct BookMetadata {
    std::string title;
    std::string author;
    std::string language;
    std::string coverItemHref;
    std::string textReferenceHref;
    // `<spine page-progression-direction="rtl">`。用來實作「文字方向 ＝ 依出版社」。
    // ⚠️ 存進 book.bin 是為了**不必為了一個位元重解 OPF** —— 快取命中時 OPF 根本不會被讀。
    uint8_t pageProgressionRtl = 0;
  };

  struct SpineEntry {
    std::string href;
    uint32_t cumulativeSize;
    int16_t tocIndex;

    SpineEntry() : cumulativeSize(0), tocIndex(-1) {}
    SpineEntry(std::string href, const uint32_t cumulativeSize, const int16_t tocIndex)
        : href(std::move(href)), cumulativeSize(cumulativeSize), tocIndex(tocIndex) {}
  };

  struct TocEntry {
    std::string title;
    std::string href;
    std::string anchor;
    uint8_t level;
    int16_t spineIndex;

    TocEntry() : level(0), spineIndex(-1) {}
    TocEntry(std::string title, std::string href, std::string anchor, const uint8_t level, const int16_t spineIndex)
        : title(std::move(title)),
          href(std::move(href)),
          anchor(std::move(anchor)),
          level(level),
          spineIndex(spineIndex) {}
  };

 private:
  std::string cachePath;
  uint32_t lutOffset;
  uint16_t spineCount;
  uint16_t tocCount;
  bool loaded;
  bool buildMode;

  HalFile bookFile;
  // Temp file handles during build
  HalFile spineFile;
  HalFile tocFile;
  // Buffers the per-entry tmp-file writes during the OPF/TOC passes: those
  // writes interleave with zip-inflate SD reads, and unbuffered they thrash
  // SdFat's shared sector cache (one 512B transaction per 4-byte pod). One
  // wrapper serves whichever pass is active (spine, then toc).
  std::unique_ptr<serialization::BufferedFileWriter> passOut;

  // Index for fast href→spineIndex lookup (used only for large EPUBs)
  struct SpineHrefIndexEntry {
    uint64_t hrefHash;  // FNV-1a 64-bit hash
    uint16_t hrefLen;   // length for collision reduction
    int16_t spineIndex;
  };
  std::deque<SpineHrefIndexEntry> spineHrefIndex;
  bool useSpineHrefIndex = false;

  // v357：400 → 1（每一本書都走「掃一遍、雜湊比對」的快路徑）。上游只給 400 章以上的書用，小書走逐項查：
  //   每查一章就從 zip 中央目錄一筆一筆掃（每筆約 13 次幾個位元組的讀取／跳位置），目錄對應也逐條重掃 spine 暫存檔。
  //   電腦端量（work/epub-host）：110 章的畫冊建索引時 EPUB 被讀 8.3 萬次、跳位置 3.7 萬次（目錄本身只有 12.5 KB），
  //   實機建索引 4.9–8.6 秒。快路徑的記憶體是每章約 16–20 位元組（110 章約 2 KB；3000 章約 60 KB，但那種書本來就
  //   走這條路）—— 上游擔心 OOM 的是另一個「整份目錄進 RAM」的做法（見 buildBookBin 的註解）。
  //   比對只看 FNV-64＋長度（不逐字比）：意外碰撞約 1e-13，刻意做的碰撞只會讓進度百分比或目錄章名錯（codex
  //   審過，接受）。 改前後書庫 304 本逐本比對 book.bin 等快取檔逐位元組相同。
  static constexpr uint16_t LARGE_SPINE_THRESHOLD = 1;

  // FNV-1a 64-bit hash function
  static uint64_t fnvHash64(const std::string& s) {
    uint64_t hash = 14695981039346656037ull;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  uint32_t writeSpineEntry(HalFile& file, const SpineEntry& entry) const;
  uint32_t writeTocEntry(HalFile& file, const TocEntry& entry) const;
  SpineEntry readSpineEntry(HalFile& file) const;
  TocEntry readTocEntry(HalFile& file) const;

 public:
  BookMetadata coreMetadata;

  explicit BookMetadataCache(std::string cachePath)
      : cachePath(std::move(cachePath)), lutOffset(0), spineCount(0), tocCount(0), loaded(false), buildMode(false) {}
  ~BookMetadataCache() = default;

  // Building phase (stream to disk immediately)
  bool beginWrite();
  bool beginContentOpfPass();
  void createSpineEntry(const std::string& href);
  bool endContentOpfPass();
  bool beginTocPass();
  void createTocEntry(const std::string& title, const std::string& href, const std::string& anchor, uint8_t level);
  bool endTocPass();
  bool endWrite();
  bool cleanupTmpFiles() const;

  // Post-processing to update mappings and sizes
  bool buildBookBin(const std::string& epubPath, const BookMetadata& metadata);

  // Reading phase (read mode)
  bool load();
  SpineEntry getSpineEntry(int index);
  TocEntry getTocEntry(int index);
  int getSpineCount() const { return spineCount; }
  int getTocCount() const { return tocCount; }
  bool isLoaded() const { return loaded; }
  // v350：layout.bin（固定版面清單）綁定用的身分 —— book.bin 的大小與開頭 256 bytes 的 FNV-1a。
  //   book.bin 不論從哪條路重建過，這兩個數字幾乎一定會變 → 清單對不上就重算。只在載入之後呼叫（會移動檔案位置；
  //   getSpineEntry／getTocEntry 每次都自己 seek，不受影響）。
  bool identity(uint32_t* size, uint32_t* headHash);
};
