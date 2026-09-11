#pragma once
#include <Print.h>

#include <algorithm>
#include <deque>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  HalFile tempItemStore;
  std::string coverItemId;
  bool hasExplicitStartReference = false;

  // Index for fast idref→href lookup (binary search over .items.bin)
  struct ItemIndexEntry {
    uint32_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  std::deque<ItemIndexEntry> itemIndex;
  bool useItemIndex = false;

  // FNV-1a hash function
  static uint32_t fnvHash(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (char c : s) {
      hash ^= static_cast<uint8_t>(c);
      hash *= 16777619u;
    }
    return hash;
  }

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths
  // ⭐ `<spine page-progression-direction="rtl">` —— 這是「這本書是直排（或右翻）」
  //    在 EPUB 裡最便宜、也最可靠的訊號：不必開任何內文檔、不必解 CSS。
  //    實測（每本書取多個內文檔投票，避免被橫排的前置頁誤導）：
  //    **凡是根層 CSS 真的解析成直排的書，ppd=rtl 全部命中 —— 召回 100%，零遺漏。**
  //    ⚠️ 反過來會誤判：另有一部分書 ppd=rtl 而 CSS 完全沒提方向（多半是印刷版直排、
  //      轉檔時只留下翻頁方向），少數甚至 ppd=rtl 卻明說橫排。使用者手動指定即可。
  //    ⚠️⚠️ **不要拿「CSS 任何地方提到 vertical」當分母**：日式雙模板同時定義
  //      `.hltr` 與 `.vrtl`，而多數書選了橫排 —— 用那個分母算會得到「只做 OPF 漏 38%」
  //      的錯誤結論（本專案的帳本一度就是這樣寫的，2026-09-10 重測更正）。
  bool pageProgressionRtl = false;

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache)
      : cachePath(cachePath), baseContentPath(baseContentPath), remainingSize(xmlSize), cache(cache) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
