#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  // v184：清除快取時的選擇（0=只清快取保留進度、1=連進度一起重設）。
  uint8_t resetProgress = 0;
  // v288 移除兩個【已死】的欄位，理由記著免得有人又加回來：
  //   ・`pageTurnOption` —— 自動翻頁已移除（見 EpubReaderMenuActivity 的說明）。
  //   ・`fontSize` —— v287 把 txt 選單裡與「文字設定」重複的字級拿掉之後就沒有生產者了。
  // ⚠️ 留著全零的死欄位比刪掉更危險：之後有人讀它會安靜地拿到 0，而不是編譯錯誤。
  // ⚠️ 下面的初始化一律用【具名初始化】—— 這個 struct 被兩個閱讀器的選單共用，
  //    位置式初始化在增刪欄位時會安靜地對錯位（這一版就差點踩到）。
};

struct ChapterResult {
  int spineIndex = 0;
  std::string anchor;
};

struct PercentResult {
  int percent = 0;
};

struct IntervalResult {
  uint32_t value = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct ProgressChangeResult {
  int spineIndex = 0;
  int page = 0;
  int totalPages = 0;
  std::string xpath;
  float percentage = 0.0f;
  bool hasSavedProgress = false;
  // Exact visible-codepoint offset within spineIndex, when the source (a bookmark) has one.
  // Preferred over xpath/percentage on resolution: it is immune to re-pagination.
  bool hasVisibleTextOffset = false;
  uint32_t visibleTextOffset = 0;
  // v290：txt 的位元組位移（同 BookmarkEntry，刻意是新欄位不是借用上面那個）。
  bool hasByteOffset = false;
  uint32_t byteOffset = 0;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct FilePathResult {
  std::string path;
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult, IntervalResult,
                 PageResult, ProgressChangeResult, NetworkModeResult, FootnoteResult, FilePathResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
