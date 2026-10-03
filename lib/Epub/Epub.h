#pragma once

#include <HalStorage.h>
#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/SpineList.h"
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
  //   src：zip＝直接從書裡串流解碼／sd＝抽到 SD 再解／png／exists＝已有縮圖／none＝沒走到解碼（原因看
  //   thumbFailReason）。 note：串流沒成功、退回 SD
  //   的原因（mem-pre＝開之前記憶體就不夠／open／io／mem／size），沒退回為空。
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

  // v350：EPUB 3 固定版面（rendition:layout-pre-paginated）的 spine 項目。存在書的快取目錄的 layout.bin，
  //   **不在 book.bin 裡** —— book.bin 改格式會讓每一本書重建索引，而重建索引會刪掉那本書全部排好的章節。
  //   ⚠️ 實測（2026-09-30，書庫 304 本）：**72% 的書有固定版面的項目**（封面、插圖頁、章名圖），不是少數 ——
  //   所以舊韌體建過索引的書不重建索引：load() 發現沒有清單（或清單壞了、對不上這份 book.bin）就讀一次 OPF 只收清單，
  //   寫進 layout.bin；book.bin 與排好的章節都不動（只有固定版面那幾章的版心變了，會在讀到時各自重排）。
  struct LayoutInfoStats {
    // file＝讀到 layout.bin／opf＝從 OPF
    // 重算並寫回（opf-nowrite：寫不成功）／opf-fail、opf-overflow＝重算不成（這次當一般版面）／
    // index＝剛建索引（index-nowrite／index-fail／index-overflow
    // 同上）／missing＝不畫頁的呼叫端沒去重算／noid＝book.bin 讀不到身分
    const char* load = "-";
    uint32_t ms = 0;  // 從 OPF 重算花的時間（只有 opf* 才有）
  };
  // load() 之後：清單讀不到、壞了、或對不上這份 book.bin → 要從 OPF 重算（只有 load(buildIfMissing=true) 才會標記）。
  //   重算由呼叫端決定時機 —— 閱讀器借 framebuffer 之後才呼叫 computePendingLayout()（跟建索引一樣，OPF 解析吃記憶體；
  //   複查：只看檔案在不在來猜要不要借是錯的，壞掉但還在的檔一樣要重算）。沒呼叫＝這次照一般版面顯示，安全。
  bool layoutComputePending() const { return layoutComputePending_; }
  void computePendingLayout();
  bool isSpineFixedLayout(int spineIndex) const;
  uint16_t fixedLayoutSpineCount() const { return static_cast<uint16_t>(fixedLayoutSpines_.size()); }
  const LayoutInfoStats& layoutInfoStats() const { return layoutStats_; }
  // v357：建索引（book.bin）這一次的分段毫秒（開書量測：大書建索引 2–8.6 秒，而這一段原本只有 LOG_DBG ＝ 實機看不到）。
  //   lib 記、閱讀器讀走印成 BOOKIDX。built＝這次 load() 真的建了索引；沒建的時候其他欄位沒有意義。
  struct IndexProfile {
    bool built = false;
    uint32_t opfMs = 0;         // 解 content.opf（manifest＋spine，寫 spine 暫存）
    uint32_t cssFindMs = 0;     // 從 zip 目錄找 CSS 檔
    uint32_t tocMs = 0;         // 解目錄（nav 或 ncx）
    uint32_t binMs = 0;         // 組 book.bin（查 zip 大小、spine↔目錄對應）
    uint32_t cssParseMs = 0;    // 解 CSS 檔（含放掉 metadata cache；skipLoadingCss 時是 0）
    uint32_t sectionsRmMs = 0;  // 刪掉舊的章節快取目錄
    uint32_t reloadMs = 0;      // 重讀剛建好的 book.bin
    uint32_t layoutMs = 0;      // 固定版面清單寫進 layout.bin
    // 從開始建到離開 load()（成功＝layout.bin 寫完；各段之和之外還有 pass 之間的收尾、暫存檔清理）
    uint32_t totalMs = 0;
    uint16_t spines = 0;
    uint16_t tocs = 0;
    uint16_t cssFiles = 0;
    // v358：各段花在 SD 介面裡的時間與次數（只算建索引這個 task；HalStorage::IoStats）。段的毫秒扣掉這些，剩下的是
    //   解析、inflate 等 CPU 加上被別的 task 搶走的時間。只量花時間的段：cssfind／secrm／reload 都在 100 ms 以下。
    //   kIoAll＝從開始建到離開 load()。ioArmed＝false：別的 task 正在量（不會發生，但不假設），io 沒有意義。
    enum IoPhase { kIoOpf, kIoToc, kIoBin, kIoCss, kIoLayout, kIoAll, kIoPhases };
    bool ioArmed = false;
    HalStorage::IoStats io[kIoPhases];
  };
  const IndexProfile& indexProfile() const { return indexProfile_; }
  // 閱讀器排完一個固定版面的章，發現它不是「整頁只有圖」（有字、或不止一頁）→ 退回一般版面，並寫回 layout.bin，
  //   之後開書不必再試。只在 render task 持 RenderLock 時呼叫（清單的讀取點都在同一把鎖裡）。
  void demoteFixedLayoutSpine(int spineIndex);

 private:
  mutable ThumbStats thumbStats_;
  SpineIndexList fixedLayoutSpines_;  // 遞增；一般的書是空的（nothrow 陣列，見 SpineList.h）
  SpineIndexList builtLayoutSpines_;  // 建索引那一次從 OPF 收來的，book.bin 建好之後寫進 layout.bin
  bool layoutComputePending_ = false;
  bool builtLayoutOverflow_ = false;
  uint32_t builtLayoutSpineCount_ = 0;
  LayoutInfoStats layoutStats_;
  IndexProfile indexProfile_;
  // layout.bin 綁定的身分（load() 時從 book.bin 取一次；demote 寫回時用，不再碰 book.bin 的檔案位置）
  bool layoutIdValid_ = false;
  uint32_t layoutIdSize_ = 0;
  uint32_t layoutIdHead_ = 0;
  uint16_t layoutSpineCount_ = 0;
  void resolveLayoutInfo(bool mayCompute);
  bool loadLayoutFile(uint16_t bookSpineCount, uint32_t binSize, uint32_t binHead);
  bool writeLayoutFile(const SpineIndexList& spines, uint16_t bookSpineCount, uint32_t binSize, uint32_t binHead) const;
  bool computeLayoutFromOpf(SpineIndexList* spines, uint32_t* spineCount, bool* overflow) const;

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
