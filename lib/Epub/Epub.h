#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;
class ZipEntryReader;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  void parseCssFiles() const;

  // v174：generateThumbBmp 最近一次失敗的出口（靜態字串），給主畫面 THUMBFAIL 診斷行用。
  mutable const char* thumbFailReason_ = "";

 public:
  // v258：generateThumbBmp 最近一次走哪條路、各段花多久（主畫面 THUMBGEN 證人；lib 不反向依賴 DiagLog）。
  //   src：zip＝直接從書裡串流解碼／sd＝抽到 SD 再解／png／exists＝已有縮圖／none＝沒走到解碼（原因看 thumbFailReason）。
  //   note：串流沒成功、退回 SD 的原因（mem-pre＝開之前記憶體就不夠／open／io／mem／size），沒退回為空。
  //   openMs：zip＝開項目讀取器；sd＝把封面抽到 SD。convMs：轉檔器整段（讀取＋解碼＋縮放＋抖色＋寫 BMP）。
  struct ThumbStats {
    const char* src = "none";
    const char* note = "";
    uint32_t totalMs = 0;
    uint32_t openMs = 0;
    uint32_t convMs = 0;
    uint32_t itemBytes = 0;
    uint32_t restarts = 0;
    uint32_t readAheadKb = 0;
    uint32_t zipMs = 0;      // 串流那一次（含失敗）花的時間；退回 SD 時 openMs／convMs 是 SD 那一次的
    bool converted = false;  // 最後一次有呼叫 JPEG 轉檔器（JpegToBmpConverter::lastInfo() 屬於這本書）
    uint32_t preFreeKb = 0;  // v259：串流前檢查當下的總量／最大塊（KB）
    uint32_t preMaxKb = 0;
    uint32_t openFreeKb = 0;  // v259：開完項目讀取器之後的總量／最大塊（KB）
    uint32_t openMaxKb = 0;
    bool deferredForMemory = false;  // v261：見 generateThumbBmp 的 deferSdFallbackOnMemory
  };
  const ThumbStats& thumbStats() const { return thumbStats_; }

 private:
  mutable ThumbStats thumbStats_;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  // v248：開一個「可讀可跳」的書內項目讀取器（圖片直接從書裡解碼，不先抽到 SD）。路徑正規化與 extractItemToFile 相同。
  bool openItemReader(const std::string& itemHref, ZipEntryReader& reader, size_t readBufSize) const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  // `<spine page-progression-direction="rtl">`：實作「文字方向 ＝ 依出版社」用。
  bool hasRtlPageProgression() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  // v261：deferSdFallbackOnMemory＝串流因記憶體失敗（mem-pre／open／mem）時不走「抽到 SD」舊路，直接回 false 並標
  //   thumbStats().deferredForMemory —— 呼叫端（主畫面）先卸字型再呼叫一次（diag260：差約 1KB 就退回舊路 5.3 秒）。
  bool generateThumbBmp(int height, bool deferSdFallbackOnMemory = false) const;
  const char* thumbFailReason() const { return thumbFailReason_; }
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize,
                                bool allowEarlyStop = false) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
