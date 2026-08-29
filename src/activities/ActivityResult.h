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
  uint8_t pageTurnOption = 0;
  // v119/v161（txt 選單）：pending 語意的字級。EPUB 選單不用（其字級走 Text Settings），保持 0。
  uint8_t fontSize = 0;
  // v184：清除快取時的選擇（0=只清快取保留進度、1=連進度一起重設）。
  uint8_t resetProgress = 0;
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
