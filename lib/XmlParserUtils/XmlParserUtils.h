#pragma once

#include <expat.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Safely tear down an expat parser: stop processing, clear callbacks, free, and null the pointer.
inline void destroyXmlParser(XML_Parser& parser) {
  if (!parser) return;
  XML_StopParser(parser, XML_FALSE);
  XML_SetElementHandler(parser, nullptr, nullptr);
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  parser = nullptr;
}

// Every parser in this tree creates its expat parser with XML_ParserCreate(nullptr), i.e. with
// namespace processing DISABLED, so element names arrive exactly as the document spelled them --
// prefix and all. Most EPUBs bind the relevant namespace as the default (bare <spine>, <navPoint>),
// but any toolchain that re-serialises the XML (Python ElementTree in particular) emits explicit
// prefixes like <ns0:spine>. Matching a hardcoded prefix list can only ever chase instances, since
// the prefix is arbitrary; compare the LOCAL name instead.
//
// This is the identity function on unprefixed names -- it returns the same pointer -- so documents
// that already parsed keep parsing byte-identically.
//
// ⚠️ When converting a comparison, remember to strip the prefix from the TARGET string too if it
// has one (ContentOpfParser's "dc:title"/"dc:creator"/"dc:language" did): comparing a local name
// against a prefixed literal never matches, and the resulting breakage is silent.
inline const char* xmlLocalName(const char* name) {
  const char* const colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}

// v345（帳本 D14）：XML 1.0 不准的控制字元（0x00–0x08、0x0B、0x0C、0x0E–0x1F；TAB／LF／CR 合法）在餵 expat 之前濾掉。
//   實例（a field report）：一本 EPUB 的 OPF 作者名中間夾了 0x08 —— expat 照規格把整份 OPF 判成不合法
//   （not well-formed (invalid token)）→ 開書直接回首頁；閱星曈官方韌體會略過這種字元，照樣開得了。
//   這些字元看不見，合法的文件裡本來就不會有 → 對沒有它們的文件，輸出與原本逐位元組相同。
//   ⚠️ UTF-16 文件不濾：0x08 這類位元組在 UTF-16 裡是字碼的一部分（例如 U+4E08 的低位元組），濾掉就把字弄壞。
//      由每份文件的前兩個位元組判斷：BOM（FF FE／FE FF），或其中有 0x00（沒有 BOM 的 UTF-16 開頭一定帶 0x00，
//      不論第一個字是 "<" 還是空白；合法的 UTF-8 XML 根本不可能有 0x00）。
//   用法：每份文件開始時重設（`filter = XmlControlCharFilter{}`），每一塊餵 expat 之前 `n = filter.apply(buf, n)`。
//   長度會變短，但本樹沒有任何解析器用 expat 的位元組索引（XML_GetCurrentByteIndex）對回檔案位置。
inline std::atomic<uint32_t>& xmlControlDropCounter() {
  static std::atomic<uint32_t> n{0};
  return n;
}
// 證人用：自上次取走後，所有解析器一共濾掉幾個控制字元（src 端印在 BOOKOPEN／BUILD end；lib 不依賴 DiagLog）。
//   全域計數 → 要歸屬到某個動作，就在動作【開始】先取走一次歸零（codex v345：否則 OPDS／同步濾掉的會算到下一次開書）。
inline uint32_t takeXmlControlDrops() { return xmlControlDropCounter().exchange(0, std::memory_order_relaxed); }

class XmlControlCharFilter {
 public:
  // 就地壓縮 buf[0..n)，回傳剩下的長度。
  size_t apply(char* buf, const size_t n) {
    if (!decided_) {
      if (n == 0) return 0;
      if (!haveFirst_) {
        first_ = static_cast<unsigned char>(buf[0]);
        haveFirst_ = true;
        if (n >= 2) {
          decide(static_cast<unsigned char>(buf[1]));
        } else if (droppable(first_) && first_ != 0x00) {
          // 第一塊只有這一個位元組、而且它本身就是要濾的字元（codex v345）：UTF-16 的開頭不會是它 → 照 UTF-8 濾掉
          decided_ = true;
        } else {
          return n;  // 還判斷不了；這個位元組不是要濾的字元（或是 0x00）→ 原樣交出，下一塊再判斷（實務上第一塊都 ≥
                     // 1KB）
        }
      } else {
        decide(static_cast<unsigned char>(buf[0]));  // 第一個位元組是上一塊的
      }
    }
    if (utf16_) return n;
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
      const auto c = static_cast<unsigned char>(buf[r]);
      if (droppable(c)) continue;
      buf[w++] = buf[r];
    }
    if (w != n) xmlControlDropCounter().fetch_add(static_cast<uint32_t>(n - w), std::memory_order_relaxed);
    return w;
  }

 private:
  static bool droppable(const unsigned char c) { return c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D; }
  void decide(const unsigned char second) {
    decided_ = true;
    utf16_ =
        (first_ == 0xFF && second == 0xFE) || (first_ == 0xFE && second == 0xFF) || first_ == 0x00 || second == 0x00;
  }
  bool decided_ = false;
  bool haveFirst_ = false;
  bool utf16_ = false;
  unsigned char first_ = 0;
};
