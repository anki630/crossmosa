#pragma once

#include <cstdint>
#include <string>
#define REPLACEMENT_GLYPH 0xFFFD

uint32_t utf8NextCodepoint(const unsigned char** string);
// Appends a Unicode codepoint to a std::string in UTF-8 encoding.
void utf8AppendCodepoint(uint32_t cp, std::string& out);
// Remove the last UTF-8 codepoint from a std::string and return the new size.
size_t utf8RemoveLastChar(std::string& str);
// Truncate string by removing N UTF-8 codepoints from the end.
void utf8TruncateChars(std::string& str, size_t numChars);

// Canonical composition (NFC) for the Latin / Vietnamese range: precomposes a
// base letter followed by combining diacritical mark(s) into a single codepoint.
// Needed because the device fonts have no combining-mark positioning, so text
// stored in NFD (e.g. some EPUB chapter titles) otherwise renders broken.
std::string utf8ComposeNfc(const std::string& in);

// Truncate a raw char buffer to the last complete UTF-8 codepoint boundary.
// Returns the new length (<= len). If the buffer ends mid-sequence, the
// incomplete trailing bytes are excluded.
int utf8SafeTruncateBuffer(const char* buf, int len);

// Returns true for CJK characters that allow line breaks on either side without hyphenation.
// Covers CJK Unified Ideographs, Hiragana, Katakana, Hangul Syllables, CJK punctuation,
// and fullwidth forms — the ranges where word boundaries are implicit per character.
inline bool utf8IsCjkBreakable(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x3000 && cp <= 0x303F)     // CJK Symbols and Punctuation
         || (cp >= 0x3040 && cp <= 0x309F)     // Hiragana
         || (cp >= 0x30A0 && cp <= 0x30FF)     // Katakana
         || (cp >= 0x3130 && cp <= 0x318F)     // Hangul Compatibility Jamo
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xAC00 && cp <= 0xD7AF)     // Hangul Syllables
         || (cp >= 0xD7B0 && cp <= 0xD7FF)     // Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2A6DF)   // CJK Extension B
         || (cp >= 0x2A700 && cp <= 0x2B73F);  // CJK Extension C
}

// Returns true for any codepoint in a CJK script block (Han, Kana, Hangul, Bopomofo,
// radicals, and CJK punctuation/compatibility/enclosed forms). Used for fallback font
// selection — deliberately broader than utf8IsCjkBreakable, whose ranges are tuned to
// implicit line-break opportunities and must not grow without rethinking layout.
inline bool utf8IsCjkCodepoint(const uint32_t cp) {
  return (cp >= 0x1100 && cp <= 0x11FF)        // Hangul Jamo
         || (cp >= 0x2E80 && cp <= 0x2FDF)     // CJK Radicals Supplement, Kangxi Radicals
         || (cp >= 0x3000 && cp <= 0x33FF)     // CJK punctuation, Kana, Bopomofo, Hangul Compat
                                               // Jamo, Kanbun, strokes, enclosed + compat forms
         || (cp >= 0x3400 && cp <= 0x4DBF)     // CJK Extension A
         || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK Unified Ideographs
         || (cp >= 0xA960 && cp <= 0xA97F)     // Hangul Jamo Extended-A
         || (cp >= 0xAC00 && cp <= 0xD7FF)     // Hangul Syllables, Hangul Jamo Extended-B
         || (cp >= 0xF900 && cp <= 0xFAFF)     // CJK Compatibility Ideographs
         || (cp >= 0xFE10 && cp <= 0xFE1F)     // Vertical Forms
         || (cp >= 0xFE30 && cp <= 0xFE4F)     // CJK Compatibility Forms
         || (cp >= 0xFF01 && cp <= 0xFF60)     // Fullwidth Latin / Punctuation
         || (cp >= 0xFF65 && cp <= 0xFFEF)     // Halfwidth Katakana / Hangul
         || (cp >= 0x20000 && cp <= 0x2EBEF)   // CJK Extensions B-F
         || (cp >= 0x2F800 && cp <= 0x2FA1F)   // CJK Compatibility Ideographs Supplement
         || (cp >= 0x30000 && cp <= 0x323AF);  // CJK Extensions G-H
}

// Returns true for Unicode combining diacritical marks that should not advance the cursor.
inline bool utf8IsCombiningMark(const uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036F)      // Combining Diacritical Marks
         || (cp >= 0x1DC0 && cp <= 0x1DFF)   // Combining Diacritical Marks Supplement
         || (cp >= 0x20D0 && cp <= 0x20FF)   // Combining Diacritical Marks for Symbols
         || (cp >= 0xFE20 && cp <= 0xFE2F);  // Combining Half Marks
}

// --- CJK 禁則(v118 從 lib/Epub/Epub/ParsedText.cpp 的匿名 namespace 搬來)---
// 原本住在 ParsedText.cpp 內、標頭未宣告,所以 src/activities/ 連結不到,而純文字
// 閱讀器與 GfxRenderer::wrappedText 因此一條禁則都沒有(句號、下引號可以跑到行首)。
// 禁則與 utf8IsCjkBreakable 是同一個問題的兩半,放在一起才不會又長出第二份實作。

inline bool isNoBreakBeforeCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
      return true;
    default:
      return false;
  }
}

inline bool isNoBreakAfterCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0x00AB:  // «
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
      return true;
    default:
      return false;
  }
}

// --- 直排（縦書き）的禁則 delta ---------------------------------------------
//
// ⚠️ **這裡刻意寫成上面兩個函式的【delta】，不是第二份表。**
//   Utf8.h:81 的註解已經寫過「放在一起才不會又長出第二份實作」——
//   直排的桌面預言機（tools/vertical-oracle/vtables.py）就長出過一份，
//   2026-09-08 的來源稽核抓到兩張表【互相】有對方沒有的東西：
//     既有的有而預言機沒有：半形 . , : ; ! ? ) ] }、»、〙、〛、（ [ {、«、〘、〚
//     預言機有而既有的沒有：分隔號 ／、間隔號 ·‧・、連接號 —–―─、…‥、彎引號 ”’〝
//   所以是互補，不是取代。
//
// ⚠️⚠️ **為什麼不直接加進上面兩個函式**：那會改變【橫排】的斷行位置 →
//   依教訓 A-11 就得 bump SECTION_FILE_VERSION，而 bump 的視窗正是記憶體壓力最高的時候。
//   這組 delta 只被直排路徑呼叫，橫排的 isNoBreakBefore/AfterCjkPunctuation 零改動；
//   而直排的快取由 section 檔頭的 verticalLayout 欄位（v104）與橫排分開失效。
//   ℹ️ **但橫排那份確實缺 clreq §6.1.1「基本處理」明列的連接號／間隔號／分隔號** ——
//      那是既有的缺口，值得單獨一版修（要 bump），本版不動。
//
// 來源：W3C《中文排版需求》clreq §6.1.1「行首行尾禁則」的【基本處理】等級
//   （clreq 明說這一級最推薦；另一級叫「GB 法」，是中國的做法，我們刻意不要）。

inline bool isNoBreakBeforeVertical(const uint32_t cp) {
  if (isNoBreakBeforeCjkPunctuation(cp)) return true;  // 重用既有，不複製
  switch (cp) {
    // 連接號（clreq 基本處理明列）
    case 0x2013:  // – en dash
    case 0x2014:  // — em dash（中文書常見）
    case 0x2015:  // ― horizontal bar
    case 0x2500:  // ─ box drawings light horizontal
                  //   ⭐ 教育部手冊自己拿來排破折號與夾注號乙式的就是它（實測比 … 還常見）
    case 0x007E:  // ~
    case 0xFF5E:  // ～（教育部連接號乙式）
    // 間隔號（clreq 建議 ·；台灣在用 ‧；・ 是日文 JIS 碼位，clreq 說不建議但中文書實際會用）
    case 0x00B7:  // ·
    case 0x2027:  // ‧
    case 0x30FB:  // ・
    // 分隔號 —— ⭐ clreq 基本處理【明列】，而兩張表原本都沒有（實測**多數**中文書都會用到）
    case 0x002F:  // /
    case 0xFF0F:  // ／
    // 刪節號（clreq TR 版列在基本處理）
    case 0x2025:  // ‥
    case 0x2026:  // …
    // 反向雙引號的【收】—— 與 isNoBreakAfterVertical 的 301D（開）成對
    // ⚠️ 這一個是 2026-09-08 的跨實作比對抓到的：我放了開引號卻漏了收引號
    case 0x301E:  // 〞
      return true;
    default:
      return false;
  }
}

inline bool isNoBreakAfterVertical(const uint32_t cp) {
  if (isNoBreakAfterCjkPunctuation(cp)) return true;  // 重用既有，不複製
  switch (cp) {
    case 0x301D:  // 〝 反向雙引號（開）
      return true;
    default:
      return false;
  }
}

// 可懸掛的標點 —— ⚠️ 這【不等於】行首禁則集合。
//
// clreq §6.1.3：「通常，行尾只可懸掛【一個】標點符號；適合行尾懸掛的標點符號有
//   【頓號、逗號及句號】。**簡體中文排版中**，其餘標點符號…**也可**進行行尾懸掛配置。」
// → 把整個禁則集合拿來當懸掛集合 ＝ 採用了簡體中文的做法。繁體只吊這三個。
//
// ℹ️ 現況：V1 的直排走【追い出し】不走懸掛（見帳本），所以這個集合暫時無人呼叫，
//    留著是因為 kinsoku_mode 的另一條路徑要用，而且它記錄了「為什麼只有三個」。
inline bool isHangablePunctuation(const uint32_t cp) {
  return cp == 0x3001    // 、頓號
      || cp == 0xFF0C    // ，逗號
      || cp == 0x3002;   // 。句號
}

inline bool containsCjkBreakableCodepoint(const std::string& text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (utf8IsCjkBreakable(cp)) {
      return true;
    }
  }
  return false;
}

inline bool hasCjkBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunctuation(leftCp) || isNoBreakBeforeCjkPunctuation(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

// 直排版本。**橫排的行為一個位元都不變** —— 只有 vertical=true 時才吃 delta。
//
// ⚠️⚠️ 2026-09-09：`isNoBreakBeforeVertical` / `isNoBreakAfterVertical` 在此之前
//    **零個呼叫者**（只有註解提到），也就是我寫的直排禁則從來沒有生效過，
//    實際跑的一直是橫排那張表。這是同一個 feature 裡第三次踩到「表寫了沒人叫」
//    （前兩次：verticalForm 的 24 個直排字形、isHangablePunctuation）。
//    → 見 memory `cross-check-must-use-the-production-path`。
inline bool hasCjkBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp, const bool vertical) {
  if (!vertical) return hasCjkBreakOpportunityBetween(leftCp, rightCp);
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterVertical(leftCp) || isNoBreakBeforeVertical(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}
