#pragma once
#include <HalStorage.h>

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

// v247 儀器：readFileToStream 的時間分解（第一次打開圖片頁時「從 EPUB 抽圖到 SD」佔 38%，v246 實機）。
// 每次 readFileToStream 開頭歸零；呼叫端（ImageBlock 的懶抽取）在返回後立刻讀走。只做加法與 micros()。
struct ZipStreamStats {
  uint32_t setupUs = 0;    // 開 zip ＋ 找項目 ＋ 算資料偏移 ＋ seek
  uint32_t readUs = 0;     // 讀 zip 本體（stored 的 file.read；deflated 的 fill 回呼）
  uint32_t readBytes = 0;
  uint32_t inflateUs = 0;  // readAtMost 扣掉 fill 讀取＝解壓 CPU
  uint32_t writeUs = 0;    // out.write（寫到 SD 的抽出檔）
  uint32_t compressed = 0;
  uint32_t uncompressed = 0;
  uint16_t method = 0xFFFF;  // 0 stored、8 deflated
  // v250（ZipEntryReader 串流開啟）：壓縮讀取緩衝實際配到的位元組數；開啟失敗在哪一步、當時最大連續塊。
  uint16_t inBufBytes = 0;
  // 0＝沒失敗。1 ZipFile 物件 2 開 zip 3 找項目 4 資料偏移 5 大小／方法 6 stored 跳位 7 inflate（跳位／狀態／視窗） 8 讀取緩衝
  // v251 抽圖（readFileToStream）另有：9 輸出緩衝 10 讀不到 11 寫不進 12 大小不符 13 解壓錯誤
  uint8_t openFailStage = 0;
  uint32_t openFailMax = 0;
};
inline ZipStreamStats g_zipStreamStats;

class ZipFile {
 public:
  struct FileStatSlim {
    uint16_t method;             // Compression method
    uint32_t compressedSize;     // Compressed size
    uint32_t uncompressedSize;   // Uncompressed size
    uint32_t localHeaderOffset;  // Offset of local file header
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint16_t totalEntries;
    bool isSet;
  };

  // Target for batch uncompressed size lookup (sorted by hash, then len)
  struct SizeTarget {
    uint64_t hash;   // FNV-1a 64-bit hash of normalized path
    uint16_t len;    // Length of path for collision reduction
    uint16_t index;  // Caller's index (e.g. spine index)
  };

  // FNV-1a 64-bit hash computed from char buffer (no std::string allocation)
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  friend class ZipEntryReader;  // v248：圖片解碼直接從書裡讀，需要檔柄、項目查找與資料偏移
  const std::string& filePath;
  HalFile file;
  ZipDetails zipDetails = {0, 0, false};
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  // Cursor for sequential central-dir scanning optimization
  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();

 public:
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  ~ZipFile() = default;
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  bool getInflatedFileSize(const char* filename, size_t* size);
  // Batch lookup: scan ZIP central dir once and fill sizes for matching targets.
  // targets must be sorted by (hash, len). sizes[target.index] receives uncompressedSize.
  // Returns number of targets matched.
  int fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  // allowEarlyStop: a short write from `out` is treated as the sink asking to
  // stop (returns true) instead of a write failure — used by header probes
  // that only need the first bytes of an entry.
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize, bool allowEarlyStop = false);

  template <typename F>
  bool enumerateFilePaths(F&& callback) {
    if (!fileStatSlimCache.empty()) {
      for (const auto& entry : fileStatSlimCache) {
        callback(std::string_view{entry.first});
      }
      return true;
    }

    return enumerateFileEntries([&callback](std::string_view path, uint32_t, uint32_t) { callback(path); });
  }

  // Callback receives (path, crc32, compressedSize) for each central-directory
  // entry. Always scans the central directory: the slim-stat cache does not
  // hold CRCs.
  template <typename F>
  bool enumerateFileEntries(F&& callback) {
    const bool wasOpen = isOpen();
    if (!wasOpen && !open()) {
      return false;
    }

    if (!loadZipDetails()) {
      if (!wasOpen) {
        close();
      }
      return false;
    }

    file.seek(zipDetails.centralDirOffset);

    uint32_t sig;
    char itemName[256];

    while (file.available()) {
      file.read(&sig, 4);
      if (sig != 0x02014b50) {
        break;
      }

      file.seekCur(12);
      uint32_t crc32, compressedSize;
      file.read(&crc32, 4);
      file.read(&compressedSize, 4);
      file.seekCur(4);
      uint16_t nameLen, m, k;
      file.read(&nameLen, 2);
      file.read(&m, 2);
      file.read(&k, 2);
      file.seekCur(12);

      if (nameLen < sizeof(itemName)) {
        file.read(itemName, nameLen);
        itemName[nameLen] = '\0';
        callback(std::string_view{itemName, nameLen}, crc32, compressedSize);
      } else {
        file.seekCur(nameLen);
      }

      file.seekCur(m + k);
    }

    if (!wasOpen) {
      close();
    }
    return true;
  }
};
