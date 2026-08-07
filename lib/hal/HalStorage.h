#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>  // for oflag_t
#include <freertos/semphr.h>

#include <memory>
#include <string>
#include <vector>

class HalFile;

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file);
  bool removeDir(const char* path);

  static HalStorage& getInstance() { return instance; }

  class StorageLock;  // private class, used internally

 private:
  static HalStorage instance;

  bool initialized = false;
  SemaphoreHandle_t storageMutex = nullptr;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  explicit HalFile(std::unique_ptr<Impl> impl);

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&);
  HalFile& operator=(HalFile&&);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  // Same write-back as flush(), but reports whether it worked.
  //
  // FsFile::flush() is `void flush() { sync(); }` (SdFat FsLib/FsFile.h:262):
  // it throws away sync()'s bool (FsFile.h:809-810), so a caller that has to
  // make a durability promise -- SMB2's WRITE_THROUGH, MS-SMB2 2.2.21, which
  // asserts "on stable storage before the response" -- had no way to keep it.
  // Exposing the existing return value here is the sanctioned way to reach an
  // SDK capability; the alternative would be reaching around the HAL and its
  // mutex.
  bool sync();
  // This handle's MODIFY date/time, read from its FAT directory entry, in
  // FAT's own packed 16-bit format -- the exact pair SdFat returns
  // (FsFile::getModifyDateTime(), FsLib/FsFile.h:305-309, forwarding to
  // FatFile::getModifyDateTime(), FatLib/FatFile.cpp:310-322). Returns false
  // when the entry cannot be read, leaving *date/*time untouched. Decode with
  // src/util/FatTimestamp.h.
  //
  // Added for the SMB2 server: an SMB client wants a real modification time
  // per file, and reporting a constant instead makes every book in the Files
  // app share one date, which destroys sorting -- the main thing the feature
  // is for. Exposing the SDK capability HERE, rather than reaching around the
  // HAL to SdFat from a network handler, is the same choice `sync()` above
  // documents: the wrapped call inherits the storage mutex and the HAL's
  // error contract, and SdFat is not thread-safe (CLAUDE.md's HAL section).
  //
  // THE SHARE ROOT IS A KNOWN NON-ANSWER, and callers must not use it: the
  // FAT root directory has no directory entry to read. FatFile::openRoot()
  // memsets the file object (FatFile.cpp:697-724), leaving m_dirSector = 0,
  // and FatFile::dirEntry()'s sync() succeeds for a clean read-only handle --
  // so getModifyDateTime() on the root returns TRUE having read sector 0 (the
  // boot sector / MBR) as though it were a directory entry. Garbage, reported
  // as success. There is no cheap way to detect that from here; the SMB
  // handler skips the root instead, and the desktop stub returns false for it
  // so the fallback path stays exercised.
  bool getModifyDateTime(uint16_t* date, uint16_t* time);
  // Set this file's length to `length` bytes. Returns false on failure,
  // leaving the file unchanged.
  //
  // ⚠️ THIS CANNOT EXTEND A FILE, and that is a property of the filesystem,
  // not of this wrapper. Both implementations are literally
  //     bool truncate(uint64_t length) { return seekSet(length) && truncate(); }
  // (FatLib/FatFile.h:957, ExFatLib/ExFatFile.h:786) and seekSet() refuses any
  // position past end-of-file (FatFile.cpp:1184-1188, ExFatFile.cpp:715-719).
  // So a length greater than the current size returns false and changes
  // nothing -- it does NOT zero-extend the way POSIX ftruncate() does. A
  // caller that needs growth has to write the bytes (see extendWithZeros() in
  // src/network/SmbFileHandlers.cpp).
  //
  // SIDE EFFECT: on success the file position is left at the new end of file
  // (truncate() sets m_fileSize = m_curPosition after seekSet moved there).
  // Anything that seeks before its next read/write is unaffected.
  //
  // Added for the SMB2 server's FILE_END_OF_FILE_INFORMATION. Exposing the
  // SDK capability here rather than reaching around the HAL is the same choice
  // sync() and getModifyDateTime() above document.
  // Write this file's timestamps into its directory entry. `flags` is a
  // bitwise OR of SdFat's T_ACCESS / T_CREATE / T_WRITE (common/
  // FsApiConstants.h:79-83, already included by this header). Returns false on
  // failure, changing nothing.
  //
  // The parameters mirror SdFat's own setter exactly
  // (FsFile::timestamp(), FsLib/FsFile.h:844-850) rather than taking the
  // packed FAT pair getModifyDateTime() returns, for a layering reason: the
  // decomposition needs a calendar algorithm, that algorithm lives in
  // src/util/FatTimestamp, and lib/hal must not depend on src/. The caller
  // decomposes; this wraps.
  //
  // ⚠️ RANGE IS 1980-2099, NARROWER THAN THE GETTER'S. FatFile::timestamp()
  // refuses `year < 1980 || year > 2099` outright (FatLib/FatFile.cpp:
  // 1278-1283), while the packed date the getter reads has a 7-bit year field
  // that reaches 2107. FatTimestamp::fromUnixSeconds() enforces the narrower
  // bound so callers get a clear refusal instead of a write that fails.
  //
  // Works on directories as well as files (isFileOrSubDir(), same line).
  //
  // Added for the SMB2 server's FILE_BASIC_INFORMATION: a client that has just
  // copied a file stamps its timestamps, and there is no honest way to answer
  // that request without being able to write them -- accepting it and doing
  // nothing is the "report unverified success" antipattern this project has
  // already paid for twice.
  bool setTimestamp(uint8_t flags, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                    uint8_t second);
  bool truncate(uint64_t length);
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  // v53:必須覆寫 Print 的緩衝版,否則任何走 Print 介面的寫入(serializeJson(doc, halFile) 等)
  // 會落到基底類別的「逐位元組迴圈」——每個 byte 一次遞迴 mutex + 一次 1-byte SdFat 寫入,
  // 且每 byte 都開一個讓其他 task 插隊驅逐 SdFat 單一 sector cache 的窗口。
  size_t write(const uint8_t* buf, size_t count) override { return write(static_cast<const void*>(buf), count); }
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;
};

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
