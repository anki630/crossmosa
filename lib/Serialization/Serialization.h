#pragma once
#include <HalStorage.h>

#include <iostream>

namespace serialization {
// v53:字串長度守衛。長度欄位直接來自 SD 卡,壞掉一個位元就會讓 s.resize(len) 要求上百 MB;
// -fno-exceptions 下配置失敗 = abort() → 那本書/那個快取檔變成「開一次當一次」且永久。
// 任何合法字串(書名/作者/章節名/路徑/URL)都遠小於此值。
constexpr uint32_t MAX_SERIALIZED_STRING = 4096;

template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
void readPod(HalFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  if (len > MAX_SERIALIZED_STRING) {  // 壞檔守衛(見 MAX_SERIALIZED_STRING)
    s.clear();
    is.setstate(std::ios::failbit);
    return;
  }
  s.resize(len);
  is.read(&s[0], len);
}

inline void readString(HalFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);
  // 壞檔守衛:長度不得超過上限,也不得超過檔案剩餘位元組
  const size_t fileSize = file.size();
  const size_t pos = file.position();
  const size_t remaining = fileSize > pos ? fileSize - pos : 0;
  if (len > remaining) {  // 長度欄位壞掉(payload 根本不存在):放棄,不配置
    s.clear();
    return;
  }
  if (len > MAX_SERIALIZED_STRING) {
    // 長度合理但超出上限(舊檔的超長字串):跳過 payload 以維持串流同步,
    // 後續欄位照樣解析得到——只有這一個字串會是空的。
    s.clear();
    file.seek(pos + len);
    return;
  }
  s.resize(len);
  file.read(&s[0], len);
}
}  // namespace serialization
