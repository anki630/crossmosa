#include "ZipFile.h"

#include <HalStorage.h>
#include <Arduino.h>
#include <InflateStream.h>
#include <Logging.h>

#include <algorithm>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

struct ZipInflateCtx {
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

// v251：抽圖（readFileToStream）失敗的證人 —— 與 ZipEntryReader 共用 g_zipStreamStats.openFailStage／openFailMax，
// 由 ImageBlock 印在 IMGFAIL render-extract 那一行。必須在釋放任何東西之前呼叫（量的是失敗當下）。
// 步驟：2 開 zip 3 找項目 4 資料偏移 5 方法不支援 6 跳位 7 inflate 狀態／視窗 8 讀取緩衝 9 輸出緩衝 10 讀不到 11 寫不進 12 大小不符 13 解壓錯誤
void noteExtractFail(const uint8_t stage) {
  if (g_zipStreamStats.openFailStage != 0) return;  // 先到先得
  g_zipStreamStats.openFailStage = stage;
#if defined(ESP_PLATFORM)
  g_zipStreamStats.openFailMax = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
#endif
}

// v251：緩衝配不到就減半到 1KB（只影響一次讀寫多少，不影響正確性）。*size 回傳實際大小。
// 小於 1KB 的請求照原樣配（codex：不要替呼叫端放大，例如只探檔頭的呼叫）。
uint8_t* mallocLadder(size_t* size) {
  const size_t kMin = *size < 1024 ? (*size > 0 ? *size : 1) : 1024;
  for (;;) {
    if (auto* p = static_cast<uint8_t*>(malloc(*size))) return p;
    if (*size == kMin) return nullptr;
    *size = *size / 2 > kMin ? *size / 2 : kMin;
  }
}

size_t zipFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipInflateCtx*>(vctx);
  if (ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const uint32_t t0 = static_cast<uint32_t>(micros());  // v247 儀器
  const size_t bytesRead = ctx->file->read(ctx->readBuf, toRead);
  g_zipStreamStats.readUs += static_cast<uint32_t>(micros()) - t0;
  g_zipStreamStats.readBytes += static_cast<uint32_t>(bytesRead);
  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}
}  // namespace

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  file.seek(zipDetails.centralDirOffset);

  uint32_t sig;
  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;  // End of list

    FileStatSlim fileStat = {};

    file.seekCur(6);
    file.read(&fileStat.method, 2);
    file.seekCur(8);
    file.read(&fileStat.compressedSize, 4);
    file.read(&fileStat.uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat.localHeaderOffset, 4);

    if (nameLen < sizeof(itemName)) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';
      fileStatSlimCache.emplace(itemName, fileStat);
    } else {
      // Skip over oversized entry names to avoid writing past fixed buffer.
      file.seekCur(nameLen);
    }

    // Skip the rest of this entry (extra field + comment)
    file.seekCur(m + k);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  // Phase 1: Try scanning from cursor position first
  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  file.seek(startPos);

  uint32_t sig;
  char itemName[256];

  while (true) {
    uint32_t entryStart = file.position();

    if (file.read(&sig, 4) != 4 || sig != 0x02014b50) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        file.seek(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entryStart >= startPos) {
      break;
    }

    file.seekCur(6);
    file.read(&fileStat->method, 2);
    file.seekCur(8);
    file.read(&fileStat->compressedSize, 4);
    file.read(&fileStat->uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat->localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      if (strcmp(itemName, filename) == 0) {
        // Found it! Update cursor to next entry
        file.seekCur(m + k);
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }
    } else {
      // Name too long, skip it
      file.seekCur(nameLen);
    }

    // Skip extra field + comment
    file.seekCur(m + k);
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  file.seek(fileSize - scanRange);
  file.read(buffer, scanRange);

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    constexpr uint32_t signature = 0x06054b50;
    if (*reinterpret_cast<uint32_t*>(&buffer[i]) == signature) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  zipDetails.totalEntries = *reinterpret_cast<uint16_t*>(&buffer[foundOffset + 10]);
  zipDetails.centralDirOffset = *reinterpret_cast<uint32_t*>(&buffer[foundOffset + 16]);
  zipDetails.isSet = true;

  free(buffer);
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(1024));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      free(data);
      return nullptr;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = 1024;

    // One-shot mode: `data` holds the entire output, so back-references
    // resolve inside it and no 32KB window is allocated.
    InflateStream inflate;
    if (!inflate.init(false)) {
      LOG_ERR("ZIP", "Failed to init inflate stream");
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    inflate.setFill(zipFillCallback, &ctx);

    if (!inflate.read(data, inflatedDataSize)) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(fileReadBuffer);
      free(data);
      return nullptr;
    }
    free(fileReadBuffer);

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize, const bool allowEarlyStop) {
  g_zipStreamStats = ZipStreamStats{};
  const uint32_t setupStartUs = static_cast<uint32_t>(micros());  // v247 儀器
  const ScopedOpenClose zip{*this};
  if (!zip) {
    noteExtractFail(2);
    return false;
  }

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    noteExtractFail(3);
    return false;
  }

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) {
    noteExtractFail(4);
    return false;
  }

  if (!file.seek(fileOffset)) {  // v251：原本沒檢查
    noteExtractFail(6);
    return false;
  }
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  g_zipStreamStats.setupUs = static_cast<uint32_t>(micros()) - setupStartUs;
  g_zipStreamStats.method = fileStat.method;
  g_zipStreamStats.compressed = fileStat.compressedSize;
  g_zipStreamStats.uncompressed = fileStat.uncompressedSize;

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    size_t bufSize = chunkSize;
    const auto buffer = mallocLadder(&bufSize);  // v251：配不到減半
    if (!buffer) {
      noteExtractFail(8);
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const uint32_t tr = static_cast<uint32_t>(micros());  // v247 儀器
      const size_t dataRead = file.read(buffer, remaining < bufSize ? remaining : bufSize);
      g_zipStreamStats.readUs += static_cast<uint32_t>(micros()) - tr;
      g_zipStreamStats.readBytes += static_cast<uint32_t>(dataRead);
      if (dataRead == 0) {
        noteExtractFail(10);
        LOG_ERR("ZIP", "Could not read more bytes");
        free(buffer);
        return false;
      }

      const uint32_t tw = static_cast<uint32_t>(micros());  // v247 儀器
      const size_t wrote = out.write(buffer, dataRead);
      g_zipStreamStats.writeUs += static_cast<uint32_t>(micros()) - tw;
      if (wrote != dataRead) {
        free(buffer);
        if (allowEarlyStop) return true;  // sink has what it needs
        noteExtractFail(11);
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    // v251：缺一不可的 inflate 狀態＋環形視窗【先】配，兩個緩衝最後配、配不到就減半（同 v250 的 ZipEntryReader）。
    //   diag250：背景排版期間抽 10KB 的章首 PNG 全部失敗 —— 失敗當下 52K 那塊還在（FONTREL img … max=51188），
    //   但排版把小洞都佔了，讀取 8K＋輸出 8K＋狀態 8.4K 全切進 52K 塊，最後的 32KB 視窗（要找 ≥34,816 的塊）必失敗。
    //   視窗依解壓後大小取 2 的次方（小檔不需要 32KB，見 InflateStream::init）。
    InflateStream inflate;
    if (!inflate.init(true, inflatedDataSize)) {
      noteExtractFail(7);
      LOG_ERR("ZIP", "Failed to init inflate stream");
      return false;
    }

    size_t readSize = chunkSize;
    auto* fileReadBuffer = mallocLadder(&readSize);
    if (!fileReadBuffer) {
      noteExtractFail(8);
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      return false;
    }

    size_t outSize = chunkSize;
    auto* outputBuffer = mallocLadder(&outSize);
    if (!outputBuffer) {
      noteExtractFail(9);
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer");
      free(fileReadBuffer);
      return false;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = readSize;
    inflate.setFill(zipFillCallback, &ctx);

    bool success = false;
    size_t totalProduced = 0;

    while (true) {
      size_t produced;
      const uint32_t readBefore = g_zipStreamStats.readUs;  // v247 儀器：readAtMost 內含 fill 讀取
      const uint32_t ti = static_cast<uint32_t>(micros());
      const InflateStream::Status status = inflate.readAtMost(outputBuffer, outSize, &produced);
      g_zipStreamStats.inflateUs +=
          (static_cast<uint32_t>(micros()) - ti) - (g_zipStreamStats.readUs - readBefore);

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        noteExtractFail(12);
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        const uint32_t tw = static_cast<uint32_t>(micros());  // v247 儀器
        const size_t wrote = out.write(outputBuffer, produced);
        g_zipStreamStats.writeUs += static_cast<uint32_t>(micros()) - tw;
        if (wrote != produced) {
          if (allowEarlyStop) {
            success = true;  // sink has what it needs
          } else {
            noteExtractFail(11);
            LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          }
          break;
        }
      }

      if (status == InflateStream::Status::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          noteExtractFail(12);
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStream::Status::Error) {
        noteExtractFail(13);
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
      // InflateStream::Status::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    return success;  // inflate destructor frees the decompressor state + window
  }

  noteExtractFail(5);
  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}
