#pragma once
#include <cstdint>
#include <string>

// A single bookmark entry — a position in a book.
struct BookmarkEntry {
  std::string xpath;    // XPath-like progress string
  std::string summary;  // First few words of a page to help identify it
  float percentage;     // Progress percentage (0.0 to 1.0)

  uint16_t computedSpineIndex = 0;        // Spine index at the time of bookmarking
  uint16_t computedChapterPageCount = 0;  // Total page count of the chapter at the time of bookmarking
  uint16_t computedChapterProgress = 0;   // Number of pages into the chapter at the time of bookmarking

  // Exact visible-codepoint offset of the bookmarked page within its spine. Unlike the page
  // number above it is immune to re-pagination, so it lands on the right page under any
  // font/margin/orientation. Absent (hasVisibleTextOffset == false) for pre-offset bookmarks.
  bool hasVisibleTextOffset = false;
  uint32_t visibleTextOffset = 0;

  // v290：txt 的錨點。**刻意是新欄位，不是把 visibleTextOffset 借去用** ——
  //   EPUB 的那個是「章內的可見字位移」，txt 的是「整個檔案的位元組位移」，
  //   同型別不同語意。借用會讓兩邊都讀得到、也都讀錯，而編譯器一句話都不會說
  //   （memory: new-meaning-needs-new-field，就是上週才踩過的那個形狀）。
  // ℹ️ 書籤檔是 JSON、按名稱索引，所以加欄位**零遷移**：舊檔沒有這個鍵就是 false。
  bool hasByteOffset = false;
  uint32_t byteOffset = 0;
};