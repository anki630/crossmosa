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

  // v358：「花在 SD 介面（這一層）裡的時間」與次數（開書量測：建索引各段有多少時間在等 SD）。只算 arm 的那個 task ——
  //   別的 task 的讀寫不算進來，但等它放鎖的時間算（計時包住拿鎖，等鎖的部分另外累計在 lockUs）。
  //   ⚠️ 這是牆上時間：含 SdFat 自己的 CPU（FAT 走訪、memcpy）與等鎖；段的時間扣掉它，剩下的是解析／inflate 等 CPU
  //   【加上】這個 task 被別的 task 搶走的時間，不是純 CPU。
  //   沒 arm 時每個操作只多讀一個 atomic；arm 著的時候每個操作多兩三次讀時鐘（約 1–2 µs），所以只在建索引那一段開。
  //   readBytes＝實際讀到的位元組（下限：readFileToStream 不知道讀了多少，記 0）。
  //   沒計時的：available／position／getName（不碰卡）—— 它們整段的時間（等鎖與函式本身）都落在「剩下的」那一邊。
  struct IoStats {
    uint32_t reads = 0, readUs = 0, readBytes = 0;
    uint32_t writes = 0, writeUs = 0;  // 含 flush
    uint32_t seeks = 0, seekUs = 0;
    uint32_t metas = 0, metaUs = 0;  // 開檔、關檔、exists、mkdir、remove、rename、列目錄等
    uint32_t lockUs = 0;             // 上面各類裡「等 StorageLock」的部分（已含在各類的 Us 裡）
    // v359：寫入是一次卡很久還是每次都慢（v358 X3：畫冊三段各約 1 秒，bin 那段只寫 3 次）。
    //   writeSlow 可以相減；最久的單次不行 ——
    //   writeMaxUs／metaMaxUs 是 arm 以來，win* 是上一次 ioResetWindowPeaks() 以來。
    uint32_t writeSlow = 0;  // 單次 ≥ kIoSlowUs 的寫
    uint32_t writeMaxUs = 0, metaMaxUs = 0;
    uint32_t winWriteMaxUs = 0, winMetaMaxUs = 0;
  };
  static constexpr uint32_t kIoSlowUs = 50 * 1000;
  // on：歸零並只算呼叫這個的 task —— 已經有別的 task 在量就回 false（不動它的數字）；off：只有 owner 關得掉。
  static bool armIoStats(bool on);
  static IoStats ioStats();          // 目前累計；只有 arm 的那個 task 讀得到，別的 task 拿到全 0
  static void ioResetWindowPeaks();  // win* 歸零（分段的起點）；只有 owner 做得到

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
  // openNextFile() 回來的空檔柄是「看完了」還是「檔柄配置失敗」：配置失敗時沒有 Impl（2026-10-07，書架掃描用）
  bool allocFailed() const { return impl == nullptr; }
};

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
