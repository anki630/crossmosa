#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>  // for oflag_t
#include <freertos/semphr.h>

#include <cassert>
#include <memory>
#include <string>
#include <vector>

class HalFile;

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  // v341：卡片的 CID（16 bytes）—— 淺睡眠醒來比對「還是不是同一張卡、有沒有被拔過」。含卡片序號：不可寫進 log。
  bool readCardId(uint8_t out[16]);
  // v341：握住儲存鎖（每一個 Storage／HalFile 操作都拿的那把遞迴鎖）。淺睡眠用它跨過 esp_light_sleep_start()，
  //   醒來檢查完卡片才放 —— 期間別的 task 一碰卡就會等（codex：繪製鎖不是 SD 鎖）。同一個 task 裡照常可以用 Storage。
  class ExclusiveHold {
   public:
    ExclusiveHold() {
      const BaseType_t got = xSemaphoreTakeRecursive(getInstance().storageMutex, portMAX_DELAY);
      assert(got == pdTRUE);  // portMAX_DELAY：只有鎖本身無效才會失敗
      (void)got;
    }
    ~ExclusiveHold() { xSemaphoreGiveRecursive(getInstance().storageMutex); }
    ExclusiveHold(const ExclusiveHold&) = delete;
    ExclusiveHold& operator=(const ExclusiveHold&) = delete;
  };
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

  // v194：Impl／HalFile 配置失敗的證人（先到先得）。src 讀走寫成 ALLOCFAIL。
  static char lastAllocFail[96];
  static void noteAllocFail(const char* where, size_t bytes);

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
  // 沒有這個 override，Print::write(buf,count) 會退化成逐 byte 呼叫 write(uint8_t)，
  // 而每個 byte 都要進一次 SD 的 semaphore。serializeJson(doc, halFile) 直接踩到。
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
