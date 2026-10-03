#include "Epub.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <Serialization.h>
#include <Utf8.h>
#include <ZipEntryReader.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

#include "Epub/converters/ReadAheadCore.h"
#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

// ─── v350：固定版面的 spine 清單（<書的快取>/layout.bin）───────────────────────────────
// 為什麼不放進 book.bin：改 book.bin 的格式＝每本書重建索引，而重建索引會刪掉整個 sections/（全部重排）。
//   書庫實測 72% 的書有固定版面的項目，這個代價不能讓每本書都付。
// 格式 v2（對抗式複查之後重做）：
//   magic "CMFX"｜uint8 版本｜uint16 項數 N｜uint16 spine 數｜uint32 book.bin 大小｜uint32 book.bin 開頭 256 B 的雜湊｜
//   N 個遞增的 uint16 spine 索引｜uint32 前面全部位元組的 FNV-1a。
// 身分綁在 book.bin 上：book.bin 不論從哪條路重建過、清單沒跟著重寫 → 對不上 → load() 當場從 OPF 重算。
// 寫入：先寫 layout.tmp、每一步都確認成功、刪舊檔、改名。斷電留下的是 .tmp 或沒有檔，不會是新舊混合還通過檢查的檔。
namespace {
constexpr char kLayoutFile[] = "/layout.bin";
constexpr char kLayoutTmp[] = "/layout.tmp";
constexpr char kLayoutMagic[4] = {'C', 'M', 'F', 'X'};
constexpr uint8_t kLayoutVersion = 2;
constexpr size_t kLayoutHeaderBytes = 4 + 1 + 2 + 2 + 4 + 4;

uint32_t layoutFnv(const uint8_t* d, const size_t n, uint32_t h = 2166136261u) {
  for (size_t i = 0; i < n; i++) {
    h ^= d[i];
    h *= 16777619u;
  }
  return h;
}
}  // namespace

bool Epub::writeLayoutFile(const SpineIndexList& spines, const uint16_t bookSpineCount, const uint32_t binSize,
                           const uint32_t binHead) const {
  if (spines.size() > bookSpineCount) return false;
  uint8_t head[kLayoutHeaderBytes];
  const auto count = static_cast<uint16_t>(spines.size());
  memcpy(head, kLayoutMagic, 4);
  head[4] = kLayoutVersion;
  memcpy(head + 5, &count, 2);
  memcpy(head + 7, &bookSpineCount, 2);
  memcpy(head + 9, &binSize, 4);
  memcpy(head + 13, &binHead, 4);
  uint32_t crc = layoutFnv(head, sizeof(head));

  const std::string tmp = cachePath + kLayoutTmp;
  const std::string path = cachePath + kLayoutFile;
  HalFile f;
  if (!Storage.openFileForWrite("EBP", tmp, f)) return false;
  bool ok = f.write(head, sizeof(head)) == sizeof(head);
  for (size_t i = 0; ok && i < spines.size(); i++) {
    const uint16_t v = spines[i];
    ok = f.write(reinterpret_cast<const uint8_t*>(&v), 2) == 2;
    crc = layoutFnv(reinterpret_cast<const uint8_t*>(&v), 2, crc);
  }
  ok = ok && f.write(reinterpret_cast<const uint8_t*>(&crc), 4) == 4;
  f.close();
  if (!ok) {
    Storage.remove(tmp.c_str());
    return false;
  }
  // 刪舊檔到改名之間斷電：留下 .tmp、沒有 layout.bin → 下次開書當作沒有清單、重算並覆寫 .tmp。不會留下錯的清單。
  Storage.remove(path.c_str());  // 沒有舊檔時回 false，沒關係；改名失敗才算寫失敗
  if (!Storage.rename(tmp.c_str(), path.c_str())) {
    Storage.remove(tmp.c_str());
    return false;
  }
  return true;
}

bool Epub::loadLayoutFile(const uint16_t bookSpineCount, const uint32_t binSize, const uint32_t binHead) {
  fixedLayoutSpines_.clear();
  const std::string path = cachePath + kLayoutFile;
  HalFile f;
  if (!Storage.openFileForRead("EBP", path, f)) return false;
  const size_t sz = f.size();
  uint8_t head[kLayoutHeaderBytes] = {};
  uint16_t count = 0;
  uint16_t fileSpineCount = 0;
  uint32_t fileBinSize = 0;
  uint32_t fileBinHead = 0;
  bool ok = sz >= kLayoutHeaderBytes + 4 && f.read(head, sizeof(head)) == static_cast<int>(sizeof(head)) &&
            memcmp(head, kLayoutMagic, 4) == 0 && head[4] == kLayoutVersion;
  uint32_t crc = 0;
  if (ok) {
    memcpy(&count, head + 5, 2);
    memcpy(&fileSpineCount, head + 7, 2);
    memcpy(&fileBinSize, head + 9, 4);
    memcpy(&fileBinHead, head + 13, 4);
    ok = sz == kLayoutHeaderBytes + 2u * count + 4u && fileSpineCount == bookSpineCount && count <= bookSpineCount &&
         fileBinSize == binSize && fileBinHead == binHead;
    crc = layoutFnv(head, sizeof(head));  // 檔頭完整讀到之後才算
  }
  ok = ok && fixedLayoutSpines_.reserve(count);  // nothrow：裝不下就當作不知道
  for (uint16_t i = 0; ok && i < count; i++) {
    uint16_t v = 0;
    ok = f.read(reinterpret_cast<uint8_t*>(&v), 2) == 2 && v < bookSpineCount &&
         (fixedLayoutSpines_.empty() || v > fixedLayoutSpines_.back());
    if (ok) {
      crc = layoutFnv(reinterpret_cast<const uint8_t*>(&v), 2, crc);
      ok = fixedLayoutSpines_.push(v);  // 已 reserve，不會再配置
    }
  }
  uint32_t fileCrc = 0;
  ok = ok && f.read(reinterpret_cast<uint8_t*>(&fileCrc), 4) == 4 && fileCrc == crc;
  f.close();
  if (!ok) fixedLayoutSpines_.clear();  // 壞了或對不上：由呼叫端決定要不要重算（重算成功會覆寫這個檔）
  return ok;
}

// 只為了固定版面清單解一次 OPF（manifest＋spine），不寫 spine、不動 book.bin。*spineCount＝解出來的 spine 項數，
// 呼叫端拿它跟 book.bin 的 spine 數對照（同一份 OPF → 必定相同；不同＝書換過或解析不完整，當作不知道）。
bool Epub::computeLayoutFromOpf(SpineIndexList* spines, uint32_t* spineCount, bool* overflow) const {
  std::string opfPath;
  if (!findContentOpfFile(&opfPath)) return false;
  size_t opfSize = 0;
  if (!getItemSize(opfPath, &opfSize)) return false;
  const std::string base = opfPath.substr(0, opfPath.find_last_of('/') + 1);  // 與 parseContentOpf 相同的算法
  ContentOpfParser parser(getCachePath(), base, opfSize, nullptr);
  parser.layoutOnly = true;
  if (!parser.setup()) return false;
  if (!readItemContentsToStream(opfPath, parser, 1024)) return false;
  *spines = std::move(parser.fixedLayoutSpines);
  *spineCount = parser.builtSpineCount;
  *overflow = parser.layoutOverflow;
  return true;
}

// load() 的尾端（book.bin 已經載入）：清單讀得到就用；讀不到、壞了、對不上 → 會畫頁的呼叫端要重算（標記，由它借好
// framebuffer 之後呼叫 computePendingLayout）。在重算之前清單是空的＝一般版面，所以漏呼叫也不會畫錯。
void Epub::resolveLayoutInfo(const bool mayCompute) {
  layoutStats_ = LayoutInfoStats{};
  layoutComputePending_ = false;
  layoutIdValid_ = bookMetadataCache && bookMetadataCache->identity(&layoutIdSize_, &layoutIdHead_);
  const int sc = bookMetadataCache ? bookMetadataCache->getSpineCount() : 0;
  if (!layoutIdValid_ || sc <= 0 || sc > 0xFFFF) {
    fixedLayoutSpines_.clear();
    layoutStats_.load = "noid";
    return;
  }
  layoutSpineCount_ = static_cast<uint16_t>(sc);
  if (loadLayoutFile(layoutSpineCount_, layoutIdSize_, layoutIdHead_)) {
    layoutStats_.load = "file";
    return;
  }
  layoutComputePending_ = mayCompute;
  layoutStats_.load = mayCompute ? "pending" : "missing";  // missing：主畫面、最近閱讀、同步 —— 不畫頁，不解 OPF
}

void Epub::computePendingLayout() {
  if (!layoutComputePending_) return;
  layoutComputePending_ = false;
  const uint32_t t0 = millis();
  SpineIndexList spines;
  uint32_t parsed = 0;
  bool overflow = false;
  if (computeLayoutFromOpf(&spines, &parsed, &overflow) && !overflow && parsed == layoutSpineCount_) {
    fixedLayoutSpines_ = std::move(spines);
    layoutStats_.load =
        writeLayoutFile(fixedLayoutSpines_, layoutSpineCount_, layoutIdSize_, layoutIdHead_) ? "opf" : "opf-nowrite";
  } else {
    // 解析失敗、清單記不完整、或 spine 數跟 book.bin 對不上：這一次照一般版面顯示，不寫檔（下次開書再試）。
    fixedLayoutSpines_.clear();
    layoutStats_.load = overflow ? "opf-overflow" : "opf-fail";
  }
  layoutStats_.ms = millis() - t0;
}

void Epub::demoteFixedLayoutSpine(const int spineIndex) {
  if (spineIndex < 0 || spineIndex > UINT16_MAX) return;
  if (!fixedLayoutSpines_.remove(static_cast<uint16_t>(spineIndex))) return;  // 只搬移、不重新配置
  // 用 load() 時記下的身分寫回（這裡在 render task 上，不碰 book.bin 的檔案位置；SD 存取由 HalStorage 的全域鎖排隊）。
  // 寫不成功：這次開書記得，下次開書再判一次。
  if (layoutIdValid_) writeLayoutFile(fixedLayoutSpines_, layoutSpineCount_, layoutIdSize_, layoutIdHead_);
}

bool Epub::isSpineFixedLayout(const int spineIndex) const {
  if (spineIndex < 0 || spineIndex > UINT16_MAX) return false;
  return fixedLayoutSpines_.contains(static_cast<uint16_t>(spineIndex));
}

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, const bool writeSpineEntries) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             writeSpineEntries ? bookMetadataCache.get() : nullptr);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  bookMetadata.title = utf8ComposeNfc(opfParser.title);
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;
  bookMetadata.pageProgressionRtl = opfParser.pageProgressionRtl ? 1 : 0;
  // v350：建索引那一次收下固定版面的項目（只有寫 spine 的那一趟才有意義）；book.bin 建好之後寫進 layout.bin。
  if (writeSpineEntries) {
    builtLayoutSpines_ = std::move(opfParser.fixedLayoutSpines);
    builtLayoutOverflow_ = opfParser.layoutOverflow;
    builtLayoutSpineCount_ = opfParser.builtSpineCount;
  }

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    size_t coverPageSize;
    uint8_t* coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    if (coverPageData) {
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      free(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search for image references: xlink:href="..." (SVG) and src="..." (img)
      std::string imageRef;
      for (const char* pattern : {"xlink:href=\"", "src=\""}) {
        auto pos = coverPageHtml.find(pattern);
        while (pos != std::string::npos) {
          pos += strlen(pattern);
          const auto endPos = coverPageHtml.find('"', pos);
          if (endPos != std::string::npos) {
            const auto ref = std::string_view{coverPageHtml}.substr(pos, endPos - pos);
            // Cover BMP generation supports JPG/PNG only; skip GIF so an unsupported wrapper image
            // does not block a later supported cover reference.
            if (FsHelpers::hasPngExtension(ref) || FsHelpers::hasJpgExtension(ref)) {
              imageRef = ref;
              break;
            }
          }
          pos = coverPageHtml.find(pattern, pos);
        }
        if (!imageRef.empty()) break;
      }

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + imageRef));
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  size_t ncxSize;
  if (!getItemSize(tocNcxItem, &ncxSize)) {
    LOG_ERR("EBP", "Could not get size of toc ncx file");
    return false;
  }

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  // Stream the decompressed NCX straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc ncx file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  size_t navSize;
  if (!getItemSize(tocNavItem, &navSize)) {
    LOG_ERR("EBP", "Could not get size of toc nav file");
    return false;
  }

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  // Stream the decompressed nav document straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNavItem, navParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc nav file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::discoverCssFilesFromZip() {
  const std::string& opfDir = contentBasePath;
  ZipFile zf(filepath);

  if (!zf.enumerateFilePaths([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) {
          return;
        }

        if (!FsHelpers::hasCssExtension(filePath)) {
          return;
        }

        if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
          return;
        }

        LOG_DBG("EBP", "Discovered CSS file via ZIP enumeration: %.*s", (int)filePath.size(), filePath.data());
        cssFiles.push_back(std::string{filePath});
      })) {
    LOG_ERR("EBP", "Failed to enumerate ZIP file paths for CSS discovery");
  }
}

void Epub::parseCssFiles() const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  // See if we have a cached version of the CSS rules
  if (cssParser->hasCache()) {
    LOG_DBG("EBP", "CSS cache exists, skipping parseCssFiles");
    return;
  }

  // Some converters emit one byte-identical stylesheet per chapter (100+ .css
  // entries), and each parse costs a zip locate plus an SD extract round-trip.
  // Map every CSS path to its central-directory (CRC32, compressed size) in a
  // single scan and parse only the first of each identical pair. Rules merge
  // into one global set, so dropping exact duplicates cannot lose styles. A
  // path that never matches a directory entry keeps key 0 and always parses.
  std::vector<uint64_t> dedupKeys(cssFiles.size(), 0);
  if (cssFiles.size() > 1) {
    std::unordered_map<std::string, size_t> pathToIndex;
    pathToIndex.reserve(cssFiles.size());
    for (size_t i = 0; i < cssFiles.size(); i++) {
      pathToIndex.emplace(FsHelpers::normalisePath(cssFiles[i]), i);
    }
    ZipFile(filepath).enumerateFileEntries([&](std::string_view entryPath, uint32_t crc32, uint32_t compressedSize) {
      if (!FsHelpers::hasCssExtension(entryPath)) {
        return;
      }
      const auto it = pathToIndex.find(std::string{entryPath});
      if (it != pathToIndex.end()) {
        dedupKeys[it->second] = (static_cast<uint64_t>(crc32) << 32) | compressedSize;
      }
    });
  }
  std::vector<uint64_t> seenKeys;
  seenKeys.reserve(cssFiles.size());
  size_t skippedDuplicates = 0;

  // No cache yet - parse CSS files
  for (size_t cssIndex = 0; cssIndex < cssFiles.size(); cssIndex++) {
    const auto& cssPath = cssFiles[cssIndex];
    const uint64_t dedupKey = dedupKeys[cssIndex];
    if (dedupKey != 0) {
      if (std::find(seenKeys.begin(), seenKeys.end(), dedupKey) != seenKeys.end()) {
        skippedDuplicates++;
        continue;
      }
      seenKeys.push_back(dedupKey);
    }
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      continue;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        continue;
      }
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    HalFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    // Explicitly close() file before reopening for reading
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    cssParser->loadFromStream(tempCssFile);
    // Explicitly close() file before calling Storage.remove()
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
  }

  // Save to cache for next time
  if (!cssParser->saveToCache()) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
  }

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files (%zu identical duplicates skipped)", cssParser->ruleCount(),
          cssFiles.size(), skippedDuplicates);
  cssParser->clear();
}

namespace {
// v359：一段的起點＝累計快照＋把「單次最久」的視窗歸零（最久不能相減，只能每段重來）
HalStorage::IoStats ioMark() {
  HalStorage::ioResetWindowPeaks();
  return HalStorage::ioStats();
}

// v358：兩個累計快照相減＝這一段的 SD 操作（IndexProfile::io）。v359：單次最久取 ioMark() 以來的視窗。
HalStorage::IoStats ioSince(const HalStorage::IoStats& a) {
  const HalStorage::IoStats b = HalStorage::ioStats();
  HalStorage::IoStats d;
  d.reads = b.reads - a.reads;
  d.readUs = b.readUs - a.readUs;
  d.readBytes = b.readBytes - a.readBytes;
  d.writes = b.writes - a.writes;
  d.writeUs = b.writeUs - a.writeUs;
  d.seeks = b.seeks - a.seeks;
  d.seekUs = b.seekUs - a.seekUs;
  d.metas = b.metas - a.metas;
  d.metaUs = b.metaUs - a.metaUs;
  d.lockUs = b.lockUs - a.lockUs;
  d.writeSlow = b.writeSlow - a.writeSlow;
  d.writeMaxUs = b.winWriteMaxUs;
  d.metaMaxUs = b.winMetaMaxUs;
  return d;
}
}  // namespace

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());
  indexProfile_ = IndexProfile{};  // v357：每次 load() 重來（同一個物件第二次 load 不留上一次的數字）

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser.reset(new CssParser(cachePath));

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      // Rebuild CSS cache when missing or when cache version changed (loadFromCache removes stale file)
      if (!cssParser->hasCache() || !cssParser->loadFromCache()) {
        LOG_DBG("EBP", "CSS rules cache missing or stale, attempting to parse CSS files");
        cssParser->deleteCache();

        BookMetadataCache::BookMetadata cachedMetadata = bookMetadataCache->coreMetadata;
        if (!parseContentOpf(cachedMetadata, /*writeSpineEntries=*/false)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
          // continue anyway - book will work without CSS and we'll still load any inline style CSS
        } else {
          discoverCssFilesFromZip();
        }
        bookMetadataCache.reset();
        parseCssFiles();
        bookMetadataCache.reset(new BookMetadataCache(cachePath));
        if (!bookMetadataCache->load()) {
          LOG_ERR("EBP", "Failed to reload cache after CSS rebuild");
          return false;
        }
        // Invalidate section caches so they are rebuilt with the new CSS
        Storage.removeDir((cachePath + "/sections").c_str());
      }
    }
    // Release the resolved CSS rule map: it is only needed transiently while building
    // section caches, and createSectionFile reloads it from cache on demand. Holding it
    // resident pins tens of KB for the whole reading session (more on warm resume into
    // an already-cached chapter, where createSectionFile never runs to clear it).
    cssParser->clear();
    // v350：固定版面的項目。讀不到、壞了、跟這份 book.bin 對不上 → 會畫頁的呼叫端（buildIfMissing）當場從 OPF 重算。
    //   ⚠️ 必須在任何章節被載入之前決定：清單錯了，章節會用錯的版心排、還會被當成有效的快取留下來（複查）。
    resolveLayoutInfo(buildIfMissing);
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();
  indexProfile_.built = true;  // v357：「這次有試著建索引」（成敗看 BOOKIDX 的 ok=；分段計時由閱讀器印成 BOOKIDX）
  // v358：只算這個 task 的 SD 操作，歸零開始（下面的守衛在每條離開路徑停掉）。別的 task 正在量就不量（ioArmed=0）。
  indexProfile_.ioArmed = HalStorage::armIoStats(true);
  // total 在任何離開路徑都結算（失敗時各段只有做完的那幾段有數字，total 仍是到失敗為止的時間；codex）。
  struct IndexTotalGuard {
    IndexProfile& p;
    uint32_t t0;
    ~IndexTotalGuard() {
      p.totalMs = millis() - t0;
      if (p.ioArmed) {
        // arm 時歸零過 → 累計就是整段；單次最久是 arm 以來的 writeMaxUs／metaMaxUs（不是 win*）
        p.io[IndexProfile::kIoAll] = HalStorage::ioStats();
        HalStorage::armIoStats(false);
      }
    }
  } indexTotalGuard{indexProfile_, indexingStart};

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  const HalStorage::IoStats ioOpf0 = ioMark();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  const uint32_t cssFindStart = millis();
  indexProfile_.opfMs = cssFindStart - opfStart;
  indexProfile_.io[IndexProfile::kIoOpf] = ioSince(ioOpf0);
  discoverCssFilesFromZip();
  indexProfile_.cssFindMs = millis() - cssFindStart;
  indexProfile_.cssFiles = static_cast<uint16_t>(cssFiles.size());
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  const uint32_t tocStart = millis();
  const HalStorage::IoStats ioToc0 = ioMark();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);
  indexProfile_.tocMs = millis() - tocStart;
  indexProfile_.io[IndexProfile::kIoToc] = ioSince(ioToc0);

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  const HalStorage::IoStats ioBin0 = ioMark();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  indexProfile_.binMs = millis() - buildStart;
  indexProfile_.io[IndexProfile::kIoBin] = ioSince(ioBin0);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  if (!skipLoadingCss) {
    // Parse CSS before reloading book.bin to leave more heap for CSS rule-table growth.
    const uint32_t cssStart = millis();
    const HalStorage::IoStats ioCss0 = ioMark();
    bookMetadataCache.reset();
    parseCssFiles();
    const uint32_t secRmStart = millis();
    indexProfile_.cssParseMs = secRmStart - cssStart;
    indexProfile_.io[IndexProfile::kIoCss] = ioSince(ioCss0);
    Storage.removeDir((cachePath + "/sections").c_str());
    indexProfile_.sectionsRmMs = millis() - secRmStart;
  }

  // Reload the cache from disk so it's in the correct state
  const uint32_t reloadStart = millis();
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (!bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }
  indexProfile_.reloadMs = millis() - reloadStart;
  indexProfile_.spines = static_cast<uint16_t>(bookMetadataCache->getSpineCount());
  indexProfile_.tocs = static_cast<uint16_t>(bookMetadataCache->getTocCount());
  const uint32_t layoutStart = millis();
  const HalStorage::IoStats ioLayout0 = ioMark();

  // v350：這一趟從 OPF 收到的固定版面項目 → layout.bin（一般的書寫一個「沒有」的檔，之後開書不必再解 OPF）。
  //   只有記得完整、spine 數跟剛建好的 book.bin 相同時才採用；否則這次照一般版面顯示、不寫檔（下次開書重算）。
  {
    layoutStats_ = LayoutInfoStats{};
    layoutIdValid_ = bookMetadataCache->identity(&layoutIdSize_, &layoutIdHead_);
    const int sc = bookMetadataCache->getSpineCount();
    bool valid = layoutIdValid_ && !builtLayoutOverflow_ && sc > 0 && sc <= 0xFFFF &&
                 builtLayoutSpineCount_ == static_cast<uint32_t>(sc);
    for (size_t k = 0; valid && k < builtLayoutSpines_.size(); k++) {
      valid = builtLayoutSpines_[k] < sc && (k == 0 || builtLayoutSpines_[k] > builtLayoutSpines_[k - 1]);
    }
    if (valid) {
      layoutSpineCount_ = static_cast<uint16_t>(sc);
      fixedLayoutSpines_ = std::move(builtLayoutSpines_);  // 搬，不複製（不同時握兩份）
      layoutStats_.load = writeLayoutFile(fixedLayoutSpines_, layoutSpineCount_, layoutIdSize_, layoutIdHead_)
                              ? "index"
                              : "index-nowrite";
    } else {
      fixedLayoutSpines_.clear();
      layoutStats_.load = builtLayoutOverflow_ ? "index-overflow" : "index-fail";
    }
    builtLayoutSpines_.clear();  // 還記憶體（SpineIndexList::clear 會釋放）
  }
  indexProfile_.layoutMs = millis() - layoutStart;
  indexProfile_.io[IndexProfile::kIoLayout] = ioSince(ioLayout0);

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

bool Epub::hasRtlPageProgression() const {
  // ⚠️ 與 getLanguage 同一組守衛：快取還沒載入時解參考會當機。
  //    載不到就回 false ＝ 退回橫排，不會誤判成直排。
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return false;
  if (bookMetadataCache->coreMetadata.pageProgressionRtl == 0) return false;

  // ⚠️⚠️ **`rtl` 不等於直排。** `page-progression-direction="rtl"` 講的是「書頁由右往左翻」，
  //    而那對【直排的中日文書】與【橫排的希伯來文／阿拉伯文書】**同樣成立**。
  //    只看這個旗標，一本阿拉伯小說會被排成中文的直欄，而且 bidi 重排在直排路徑上不生效
  //    —— 整本書變成不可讀（複查從三個面向各自抓到；本韌體確實帶著希伯來文與阿拉伯文
  //    的內建字型與介面翻譯，所以這不是假想的情境）。
  //    → 明確排除由右至左【書寫】的語言。判斷用 `dc:language` 的主要子標籤。
  const std::string& lang = getLanguage();
  static constexpr const char* kRtlScriptLangs[] = {"he", "iw", "ar", "fa", "ur", "yi", "ps", "sd", "dv", "ug", "ku"};
  for (const char* code : kRtlScriptLangs) {
    const size_t n = strlen(code);
    if (lang.size() >= n && strncasecmp(lang.c_str(), code, n) == 0 &&
        (lang.size() == n || lang[n] == '-' || lang[n] == '_')) {
      return false;
    }
  }
  // ⚠️ 沒有 `dc:language` 的書仍然照 rtl 判直排 —— 那是現行行為，而中文書漏標語言
  //    遠比阿拉伯文書漏標常見。使用者手動指定即可。
  return true;
}

std::string Epub::getCoverBmpPath(bool cropped) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

bool Epub::generateCoverBmp(bool cropped) const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from JPG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    HalFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      thumbFailReason_ = "jpg-tmp-write";
      return false;
    }
    readItemContentsToStream(coverImageHref, coverJpg, 1024);
    // Explicitly close() file before reopening for reading
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }

    HalFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    coverBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    HalFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      thumbFailReason_ = "png-tmp-write";
      return false;
    }
    readItemContentsToStream(coverImageHref, coverPng, 1024);
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      return false;
    }

    HalFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      return false;
    }
    const bool success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    coverBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from PNG cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  LOG_ERR("EBP", "Cover image is not a supported format, skipping");
  return false;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

namespace {
// v258：主畫面縮圖直接從書裡串流解碼的來源（與閱讀器 v248 的 DecodeFile 同形：項目讀取器＋預讀層）。
//   整包配在堆積上：ZipEntryReader 的 InflateStream 狀態很大，不能放在任務堆疊。
struct ThumbZipSource {
  ZipEntryReader zip;
  ReadAheadCore<ZipEntryReader> ra;
  std::unique_ptr<uint8_t[]> buf;
};

int32_t thumbZipRead(void* ctx, uint8_t* buf, int32_t len) {
  if (len <= 0) return 0;
  return static_cast<ThumbZipSource*>(ctx)->ra.read(buf, static_cast<size_t>(len));
}

bool thumbZipSeek(void* ctx, int32_t pos) {
  return static_cast<ThumbZipSource*>(ctx)->ra.seek(static_cast<size_t>(pos));
}
}  // namespace

bool Epub::generateThumbBmp(int height, const bool deferSdFallbackOnMemory) const {
  const auto thumbPath = getThumbBmpPath(height);
  thumbFailReason_ = "";  // v174：失敗原因給 src 端的 THUMBFAIL 診斷行（lib 不反向依賴 DiagLog）
  thumbStats_ = ThumbStats{};
  const uint32_t thumbT0 = millis();
  const ScopedCleanup stampTotal{[this, thumbT0]() { thumbStats_.totalMs = millis() - thumbT0; }};

  // Already generated, return true.
  //
  // ...except that "exists" is not the same as "usable". The tail of this function
  // deliberately writes a ZERO-BYTE file as a "don't try again" marker, and the
  // coverItemHref-is-empty branch falls through into it -- but an empty coverItemHref
  // is TRANSIENT state (it comes from book.bin), not a property of the book. v127's
  // namespace-prefix bug made every affected book parse to an empty spine AND an empty
  // coverItemHref, so each one got a permanent no-cover marker; fixing the parser could
  // not undo it, because this function returned early on the marker's mere existence.
  // A zero-byte marker must therefore not outlive the condition that produced it: once
  // a cover href is known, drop the marker and regenerate. Books that genuinely have no
  // cover keep theirs and still short-circuit, so this costs them nothing.
  if (Storage.exists(thumbPath.c_str())) {
    bool emptyMarker = false;
    HalFile existing;
    if (Storage.openFileForRead("EBP", thumbPath, existing)) {
      emptyMarker = existing.fileSize() == 0;
      // Explicit close() required before the Storage.remove() below.
      existing.close();
    }
    const bool coverKnown =
        bookMetadataCache && bookMetadataCache->isLoaded() && !bookMetadataCache->coreMetadata.coverItemHref.empty();
    if (!emptyMarker || !coverKnown) {
      thumbStats_.src = "exists";
      return true;
    }
    LOG_DBG("EBP", "Stale empty thumb marker but a cover is now known; regenerating");
    Storage.remove(thumbPath.c_str());
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    // v196：分因——快取檔不存在 vs 存在但載入失敗（只改診斷字串，不改行為）。
    // v196（複查）：這條是【低記憶體失敗路徑】，不可以在這裡配置 std::string ——
    // -fno-exceptions 下配置失敗＝abort，會把「縮圖產不出來」升級成整機重開。用固定緩衝。
    char cachePathBuf[160];
    const int n = snprintf(cachePathBuf, sizeof(cachePathBuf), "%s/book.bin", getCachePath().c_str());
    const bool truncated = (n < 0 || static_cast<size_t>(n) >= sizeof(cachePathBuf));
    const bool missing = !truncated && !Storage.exists(cachePathBuf);
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache %s", missing ? "missing" : "load-failed");
    thumbFailReason_ = missing ? "cache-missing" : "cache-load-failed";
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
    thumbFailReason_ = "no-cover-href";
  } else if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from JPG cover image");
    // Use smaller target size for Continue Reading card (half of screen: 240x400)
    // Generate 1-bit BMP for fast home screen rendering (no gray passes needed)
    // v174：2:3（Kobo 1600×2400／紙本 6×9）；0.6 太瘦，標準封面左右各裁 5%
    const int THUMB_TARGET_WIDTH = (height * 2 + 1) / 3;
    const int THUMB_TARGET_HEIGHT = height;

    // v258（diag257：開過書回主畫面「載入中」6–8 秒）：原本先把封面整個抽到 SD 暫存檔再讀回來解碼
    //   （v247 實機同一張 672KB 封面：抽＋寫 SD 約 2.7 秒、讀回 1.1 秒）。改成先試直接從書裡串流，
    //   與閱讀器 v248 的圖片同一條讀取器。開不起來（記憶體）、讀檔出錯、或轉檔器因記憶體放棄 → 退回下面的舊路。
    //   解碼器自己判定壞圖（不是 I/O、不是記憶體）就不退回：同一張圖從 SD 解也一樣壞，退回只是每次進主畫面做兩次。
    //   codex（記憶體）：串流的峰值＝項目讀取器約 49KB（解壓狀態 8,364＋視窗 32,768＋讀取緩衝
    //   ≤8KB）＋預讀（有餘裕才配） ＋轉檔器的第一段門檻（v259 起縮圖路徑＝解碼器 20KB＋保留
    //   16KB）。開之前先量：連這個都不夠就直接走舊路。 精確的需求要讀完 JPEG
    //   檔頭才知道（轉檔器第二段門檻）；那一段沒過的代價只有開讀取器＋讀檔頭（約 0.1 秒）再退回。 v259：diag258
    //   有一本在這裡沒過（當時門檻 109KB）→ 退回舊路 6.7 秒；檢查當下與開讀取器之後的數字都記進證人。
    constexpr size_t kStreamPeakFree = 49 * 1024 + 36 * 1024;
    constexpr size_t kStreamMinLargest = 40 * 1024;  // 32KB 解壓視窗要一整塊
    const uint32_t zipT0 = millis();
    const size_t preFree = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    const size_t preLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    thumbStats_.preFreeKb = static_cast<uint32_t>(preFree / 1024);
    thumbStats_.preMaxKb = static_cast<uint32_t>(preLargest / 1024);
    if (preFree < kStreamPeakFree || preLargest < kStreamMinLargest) {
      thumbStats_.note = "mem-pre";
    } else {
      auto zs = makeUniqueNoThrow<ThumbZipSource>();
      const uint32_t openT0 = millis();
      const bool opened = zs && openItemReader(coverImageHref, zs->zip, 8 * 1024);
      if (!opened) {
        thumbStats_.note = "open";
      } else if (zs->zip.size() > 0x7FFFFFFFu) {
        // codex 第二輪：這種項目【不能】退回舊路（舊路會把它整個抽到 SD）。直接判失敗；轉檔器本來就只收 2048×3072
        // 以內的圖。
        thumbStats_.src = "zip";
        thumbStats_.note = "size";
        thumbFailReason_ = "jpg-too-big";
        return false;
      } else {
        const size_t itemSize = zs->zip.size();
        // 預讀緩衝：最多 16KB。同時看最大塊（留 32KB 給之後才配的解碼器緩衝）與總量（留 52KB＋8KB
        // 給轉檔器的總量檢查）；
        //   配不到就直讀（cap=0）。
        constexpr size_t kMaxReadAhead = 16 * 1024;
        constexpr size_t kMinReadAhead = 4 * 1024;
        constexpr size_t kHeadroom = 32 * 1024;
        constexpr size_t kConverterFree = 60 * 1024;
        size_t cap = itemSize < kMaxReadAhead ? itemSize : kMaxReadAhead;
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        thumbStats_.openFreeKb = static_cast<uint32_t>(freeNow / 1024);
        thumbStats_.openMaxKb = static_cast<uint32_t>(largest / 1024);
        const size_t freeRoom = freeNow > kConverterFree ? freeNow - kConverterFree : 0;
        while (cap > kMinReadAhead && (cap + kHeadroom > largest || cap > freeRoom)) cap /= 2;
        if (cap + kHeadroom > largest || cap > freeRoom) cap = 0;
        if (cap > 0) {
          zs->buf = makeUniqueNoThrow<uint8_t[]>(cap);
          if (!zs->buf) cap = 0;
        }
        zs->ra.attach(&zs->zip, itemSize, zs->buf.get(), cap);
        thumbStats_.openMs = millis() - openT0;
        thumbStats_.itemBytes = static_cast<uint32_t>(itemSize);
        thumbStats_.readAheadKb = static_cast<uint32_t>(cap / 1024);

        HalFile thumbBmp;
        if (!Storage.openFileForWrite("EBP", thumbPath, thumbBmp)) {
          thumbFailReason_ = "jpg-thumb-open";
          return false;
        }
        JpegToBmpConverter::Source source;
        source.ctx = zs.get();
        source.read = &thumbZipRead;
        source.seek = &thumbZipSeek;
        source.size = static_cast<int32_t>(itemSize);
        const uint32_t convT0 = millis();
        thumbStats_.converted = true;
        bool success = JpegToBmpConverter::jpegSourceTo1BitBmpStreamWithSize(source, thumbBmp, THUMB_TARGET_WIDTH,
                                                                             THUMB_TARGET_HEIGHT);
        // 解碼器常在 EOI 就停：把項目剩下的部分解完，確認沒有截斷、沒有讀錯（同 v248
        // verifyActiveDecodeSourceComplete）。
        const bool ioError = zs->ra.hadError() || zs->zip.hadError() || (success && !zs->zip.verifyComplete());
        if (ioError) success = false;
        thumbStats_.convMs = millis() - convT0;
        thumbStats_.restarts = zs->zip.restarts();
        thumbBmp.close();
        thumbStats_.zipMs = millis() - zipT0;
        if (success) {
          thumbStats_.src = "zip";
          return true;
        }
        Storage.remove(thumbPath.c_str());
        // codex：不要從錯誤字串推「是不是記憶體」—— 轉檔器每個配置出口都會標 memFail。
        const bool memError = JpegToBmpConverter::lastInfo().memFail;
        if (!ioError && !memError) {
          LOG_ERR("EBP", "Failed to generate thumb BMP from streamed JPG cover image");
          thumbStats_.src = "zip";
          thumbFailReason_ = "jpg-convert";
          return false;
        }
        thumbStats_.note = ioError ? "io" : "mem";
      }
    }  // zs 在這裡釋放（約 50KB），舊路才有記憶體
    thumbStats_.zipMs = millis() - zipT0;
    thumbStats_.converted = false;
    // v261：記憶體類的串流失敗先交回呼叫端（卸字型後重試串流），不直接走 4–6 秒的舊路。
    if (deferSdFallbackOnMemory && (strcmp(thumbStats_.note, "mem-pre") == 0 || strcmp(thumbStats_.note, "mem") == 0 ||
                                    strcmp(thumbStats_.note, "open") == 0)) {
      thumbStats_.src = "zip";
      thumbStats_.deferredForMemory = true;
      thumbFailReason_ = "stream-mem-deferred";
      return false;
    }

    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";
    thumbStats_.src = "sd";
    const uint32_t extractT0 = millis();

    HalFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverJpg, 1024);
    // Explicitly close() file before reopening for reading
    coverJpg.close();
    thumbStats_.openMs = millis() - extractT0;

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      thumbFailReason_ = "jpg-tmp-read";
      return false;
    }

    HalFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      thumbFailReason_ = "jpg-thumb-open";
      return false;
    }
    thumbStats_.itemBytes = static_cast<uint32_t>(coverJpg.size());
    const uint32_t convT0 = millis();
    thumbStats_.converted = true;
    const bool success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverJpg, thumbBmp, THUMB_TARGET_WIDTH,
                                                                             THUMB_TARGET_HEIGHT);
    thumbStats_.convMs = millis() - convT0;
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    thumbBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from JPG cover image");
      thumbFailReason_ = "jpg-convert";
      Storage.remove(getThumbBmpPath(height).c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from PNG cover image");
    thumbStats_.src = "png";  // v258：PNG 照舊抽到 SD 再解（PNG 沒有 DCT 縮放可用）
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    HalFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverPng, 1024);
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      thumbFailReason_ = "png-tmp-read";
      return false;
    }

    HalFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      thumbFailReason_ = "png-thumb-open";
      return false;
    }
    // v174：2:3（Kobo 1600×2400／紙本 6×9）；0.6 太瘦，標準封面左右各裁 5%
    int THUMB_TARGET_WIDTH = (height * 2 + 1) / 3;
    int THUMB_TARGET_HEIGHT = height;
    const bool success =
        PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverPng, thumbBmp, THUMB_TARGET_WIDTH, THUMB_TARGET_HEIGHT);
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    thumbBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from PNG cover image");
      thumbFailReason_ = "png-convert";
      Storage.remove(getThumbBmpPath(height).c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else {
    LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail");
    thumbFailReason_ = "unsupported-format";
  }

  // Write an empty bmp file to avoid generation attempts in the future
  HalFile thumbBmp;
  Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp);
  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize,
                                    const bool allowEarlyStop) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize, allowEarlyStop);
}

bool Epub::extractItemToFile(const std::string& itemHref, const std::string& destPath) const {
  HalFile out;
  if (!Storage.openFileForWrite("EBP", destPath, out)) {
    return false;
  }
  // 上游 #2959：大圖主宰惰性抽取；緩衝跟 section 串流一樣 8KB，SD 讀寫次數減半。但抽取發生在圖片頁
  // 首次 render，可能撞上背景建置視窗（最大連續塊 5–25KB）——堆緊時退回 4KB（複查）。
  const size_t chunk = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) >= 32 * 1024 ? 8192 : 4096;
  const bool ok = readItemContentsToStream(itemHref, out, chunk);
  out.flush();
  out.close();
  if (!ok) {
    Storage.remove(destPath.c_str());
  }
  return ok;
}

bool Epub::openItemReader(const std::string& itemHref, ZipEntryReader& reader, const size_t readBufSize) const {
  if (itemHref.empty()) return false;
  const std::string path = FsHelpers::normalisePath(itemHref);
  return reader.open(filepath, path.c_str(), readBufSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const { return getSpineItem(spineIndex).cumulativeSize; }

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  const std::string target = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawTarget));

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
