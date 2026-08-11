#pragma once

#include <HalStorage.h>

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

 public:
  explicit Txt(std::string path, std::string cacheBasePath);

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
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;
};
