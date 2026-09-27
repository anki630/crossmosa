#pragma once

#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>
#include <string>

class Txt {
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  bool loaded = false;
  size_t fileSize = 0;

  // v120:常駐檔柄。readContent 原本【每次呼叫都開一次檔】,而 SdFat 的開檔內含一次
  // exists(),等於把路徑從根目錄掃兩遍 —— v55 對 SD 字型量過是 12-18 ms,而純文字
  // 閱讀器每翻一頁至少呼叫一次。改成開一次就留著,之後只做 seek + read。
  // mutable:readContent 是 const,但它要能延遲開檔。
  mutable HalFile sharedFile_;
  mutable bool sharedFileOpen_ = false;
  // 注音（codex 整合複查 B2）：「seek＋read」要一整組不被插隊。閱讀器的主任務（書籤、QR、跳頁對齊行首）與繪製任務
  //   （排版、預取、注音游標讀上下文）都用這一個檔柄；HalFile 每個呼叫各自持 StorageLock，只保證單一呼叫不壞，
  //   不保證位置不被搬走 → 讀到別處的位元組。遞迴互斥鎖、靜態配置（同 SdCardFont::SharedFileLock），鎖內只做 SD 讀。
  mutable StaticSemaphore_t fileMutexStorage_{};
  mutable SemaphoreHandle_t fileMutex_ = nullptr;
  struct FileLock;

 public:
  explicit Txt(std::string path, std::string cacheBasePath);
  Txt(const Txt&) = delete;  // 序列鎖的靜態儲存在物件裡（fileMutexStorage_）
  Txt& operator=(const Txt&) = delete;

  ~Txt();
  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] size_t getFileSize() const { return fileSize; }

  void setupCacheDir() const;
  bool clearCache() const;

  // Cover image support - looks for cover.bmp/jpg/jpeg/png in same folder as txt file
  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] bool generateCoverBmp() const;
  [[nodiscard]] std::string findCoverImage() const;

  // Read content from file
  // ⚠️ 讀到一個位元組就回 true（短讀時緩衝區後段是沒寫過的記憶體）。要當成檔案內容用的地方一律用 readContentExact。
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;
  // 讀滿 length 個位元組才算成功（codex 整合複查 B1：短讀的殘值會被排版當成字、被注音游標當成上下文 → 可能給錯讀音）。
  [[nodiscard]] bool readContentExact(uint8_t* buffer, size_t offset, size_t length) const;

 private:
  bool seekShared(size_t offset) const;
};
