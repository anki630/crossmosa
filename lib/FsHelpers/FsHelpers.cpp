#include "FsHelpers.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <vector>

namespace FsHelpers {

namespace {
bool isHexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

uint8_t hexValue(const char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
  return static_cast<uint8_t>(10 + (c - 'A'));
}
}  // namespace

std::string decodeUriEscapes(const std::string& path) {
  std::string decoded;
  decoded.reserve(path.size());

  for (size_t i = 0; i < path.size(); i++) {
    if (path[i] == '%' && i + 2 < path.size() && isHexDigit(path[i + 1]) && isHexDigit(path[i + 2])) {
      const uint8_t value = static_cast<uint8_t>((hexValue(path[i + 1]) << 4) | hexValue(path[i + 2]));
      decoded += static_cast<char>(value);
      i += 2;
      continue;
    }

    decoded += path[i];
  }

  return decoded;
}

std::string normalisePath(const std::string& path) {
  std::vector<std::string_view> components;
  components.reserve(8);  // Eight nested folders is more than we might expect

  size_t start = 0;
  for (size_t i = 0; i <= path.length(); ++i) {
    if (i == path.length() || path[i] == '/') {
      if (i > start) {
        std::string_view component(path.data() + start, i - start);
        if (component == "..") {
          if (!components.empty()) {
            components.pop_back();
          }
        } else {
          components.push_back(component);
        }
      }
      start = i + 1;
    }
  }

  if (components.empty()) {
    return "";
  }

  size_t total_len = 0;
  for (const auto& c : components) {
    total_len += c.length() + 1;
  }

  std::string result;
  result.reserve(total_len - 1);

  for (size_t i = 0; i < components.size(); ++i) {
    if (i > 0) {
      result += '/';
    }
    result.append(components[i].data(), components[i].length());
  }

  return result;
}

bool naturalLess(const std::string& str1, const std::string& str2) {
  // Naive natural sort: numeric-aware, case-insensitive
  const char* s1 = str1.c_str();
  const char* s2 = str2.c_str();

  // ctype functions require unsigned char values: passing a negative char (UTF-8
  // bytes above 0x7f with signed char) is undefined behavior
  const auto isDigit = [](const char c) { return isdigit(static_cast<unsigned char>(c)) != 0; };

  // Iterate while both strings have characters
  while (*s1 && *s2) {
    // Check if both are at the start of a number
    if (isDigit(*s1) && isDigit(*s2)) {
      // Skip leading zeros and track them
      while (*s1 == '0') s1++;
      while (*s2 == '0') s2++;

      // Count digits to compare lengths first
      int len1 = 0, len2 = 0;
      while (isDigit(s1[len1])) len1++;
      while (isDigit(s2[len2])) len2++;

      // Different length so return smaller integer value
      if (len1 != len2) return len1 < len2;

      // Same length so compare digit by digit
      for (int i = 0; i < len1; i++) {
        if (s1[i] != s2[i]) return s1[i] < s2[i];
      }

      // Numbers equal so advance pointers
      s1 += len1;
      s2 += len2;
    } else {
      // Regular case-insensitive character comparison
      const int c1 = tolower(static_cast<unsigned char>(*s1));
      const int c2 = tolower(static_cast<unsigned char>(*s2));
      if (c1 != c2) return c1 < c2;
      s1++;
      s2++;
    }
  }

  // One string is prefix of other
  return *s1 == '\0' && *s2 != '\0';
}

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    return naturalLess(str1, str2);
  });
}

bool checkFileExtension(std::string_view fileName, const char* extension) {
  const size_t extLen = strlen(extension);
  if (fileName.length() < extLen) {
    return false;
  }

  const size_t offset = fileName.length() - extLen;
  for (size_t i = 0; i < extLen; i++) {
    if (tolower(static_cast<unsigned char>(fileName[offset + i])) !=
        tolower(static_cast<unsigned char>(extension[i]))) {
      return false;
    }
  }
  return true;
}

bool hasJpgExtension(std::string_view fileName) {
  return checkFileExtension(fileName, ".jpg") || checkFileExtension(fileName, ".jpeg");
}

bool hasPngExtension(std::string_view fileName) { return checkFileExtension(fileName, ".png"); }

bool hasBmpExtension(std::string_view fileName) { return checkFileExtension(fileName, ".bmp"); }

bool hasGifExtension(std::string_view fileName) { return checkFileExtension(fileName, ".gif"); }

bool hasEpubExtension(std::string_view fileName) { return checkFileExtension(fileName, ".epub"); }

bool hasXtcExtension(std::string_view fileName) {
  return checkFileExtension(fileName, ".xtc") || checkFileExtension(fileName, ".xtch");
}

bool hasTxtExtension(std::string_view fileName) { return checkFileExtension(fileName, ".txt"); }

bool hasMarkdownExtension(std::string_view fileName) { return checkFileExtension(fileName, ".md"); }

bool hasCssExtension(std::string_view fileName) { return checkFileExtension(fileName, ".css"); }

std::string extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen) {
  if (maxLen == 0) {
    return;
  }

  // ⚠️ **只輸出【完整】的 UTF-8 字元 —— 一個位元組都不要半截。**
  //    （2026-09-08：截圖對長中文書名靜默失敗，實機 diag210 的 SCRFAIL）
  //
  //    檔名帶著半截 UTF-8 時 mkdir／open 會失敗，而失敗只寫 LOG_ERR ＝
  //    這台機器沒有序列埠，等於什麼都沒發生。某本中文書一直沒事：9 bytes。
  //
  //    ⚠️ 這是第【三】版寫法，前兩版都被回歸測試抓到：
  //      · 原版：無條件在第 maxLen-1 個位元組截斷 → 中文書名 2/3 機率切在字中間。
  //      · 第二版：追蹤「下一個位元組不是續接位元組」當邊界 —— 但 `\0` 也不是續接
  //        位元組，於是**上游就已經壞掉的輸入**（實機正是這種：傳進來的 title
  //        本身就是截斷過的 63 bytes、結尾是孤零零一個 0xE5）被判成合法，原樣輸出。
  //      · 現在：逐字元前進，續接位元組不齊、或整個字元放不下，就到此為止。
  //        判準與「為什麼會壞」無關，只跟「這個字元完不完整」有關。
  size_t outLen = 0;
  size_t i = 0;
  while (input[i] != '\0' && outLen < maxLen - 1) {
    const auto lead = static_cast<unsigned char>(input[i]);
    size_t seqLen;
    if (lead < 0x80) {
      seqLen = 1;
    } else if (lead >= 0xF0) {
      seqLen = 4;
    } else if (lead >= 0xE0) {
      seqLen = 3;
    } else if (lead >= 0xC0) {
      seqLen = 2;
    } else {
      break;  // 落單的續接位元組 = 輸入本身壞掉，到此為止
    }

    bool complete = true;
    for (size_t k = 1; k < seqLen; k++) {
      if ((static_cast<unsigned char>(input[i + k]) & 0xC0) != 0x80) {
        complete = false;
        break;
      }
    }
    if (!complete) break;                 // 續接位元組不齊（含被 \0 截斷）
    if (outLen + seqLen > maxLen - 1) break;  // 放不下整個字元，寧可短一點

    for (size_t k = 0; k < seqLen; k++) {
      const char c = input[i + k];
      // 只有單位元組字元需要換掉 FAT 不接受的符號；多位元組序列原樣保留。
      if (seqLen == 1 && (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                          c == '>' || c == '|' || c == ' ' || (c > 0x00 && c <= 0x1f))) {
        output[outLen] = '-';
      } else {
        output[outLen] = c;
      }
      outLen++;
    }
    i += seqLen;
  }
  output[outLen] = '\0';
}

}  // namespace FsHelpers
