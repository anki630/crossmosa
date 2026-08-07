#include "ParsedText.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
// Paragraph-level direction: scan the first N words to find base direction.
constexpr size_t RTL_PARAGRAPH_PROBE_WORDS = 3;
// Per-word: scan enough chars to see through leading neutrals (quotes, numbers)
// before giving up. 64 is a hedge for pathological cases like long numeric tokens.
constexpr int RTL_PER_WORD_PROBE_DEPTH = 64;
constexpr size_t MIN_JUSTIFY_GAPS = 1;

// Byte-level pre-check: Hebrew UTF-8 lead bytes 0xD6-0xD7, Arabic/Syriac 0xD8-0xDB.
bool mayContainRtlBytes(const char* str) {
  for (const auto* p = reinterpret_cast<const unsigned char*>(str); *p; ++p) {
    if (*p >= 0xD6 && *p <= 0xDB) return true;
  }
  return false;
}

// Returns the first rendered codepoint of a word (skipping leading soft hyphens).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD) return cp;  // skip soft hyphens
  }
}

// Returns the last codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  if (word.empty()) return 0;
  // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

bool isNoBreakBeforeCjkPunctuation(const uint32_t cp) {
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

bool isNoBreakAfterCjkPunctuation(const uint32_t cp) {
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

bool containsCjkBreakableCodepoint(const std::string& text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (utf8IsCjkBreakable(cp)) {
      return true;
    }
  }
  return false;
}

bool hasCjkBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunctuation(leftCp) || isNoBreakBeforeCjkPunctuation(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

std::vector<size_t> cjkCharacterBreakByteOffsets(const std::string& text) {
  struct CodepointBoundary {
    uint32_t cp;
    size_t endOffset;
  };

  std::vector<CodepointBoundary> codepoints;
  codepoints.reserve(text.size());
  bool hasCjkBreakable = false;

  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) break;
    if (utf8IsCjkBreakable(cp)) {
      hasCjkBreakable = true;
    }
    codepoints.push_back({cp, static_cast<size_t>(ptr - start)});
  }

  if (!hasCjkBreakable || codepoints.size() < 2) return {};

  std::vector<size_t> allowedOffsets;
  allowedOffsets.reserve(codepoints.size() - 1);
  for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
    const uint32_t current = codepoints[i].cp;
    const uint32_t next = codepoints[i + 1].cp;
    if (!hasCjkBreakOpportunityBetween(current, next)) continue;
    allowedOffsets.push_back(codepoints[i].endOffset);
  }
  return allowedOffsets;
}

int computeJustifyExtra(const int spareSpace, const size_t gapCount) {
  if (gapCount < MIN_JUSTIFY_GAPS || spareSpace <= 0) return 0;
  // Distribute the spare space evenly across gaps. Do NOT bail out to 0 when the
  // per-gap stretch is large: a sparse line (few words on a wide page) legitimately
  // needs big gaps to reach the margin. Returning 0 there disables justification for
  // that line, leaving it right-aligned (RTL) / left-aligned (LTR) — the mismatched
  // alignment bug. Match the un-capped behavior of the old code.
  return spareSpace / static_cast<int>(gapCount);
}

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen = false) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style);
}

// Checks if a UTF-8 codepoint should be counted as part of a word for Focus Reading
bool isWordCharacter(uint32_t cp) {
  // ASCII range (Catches 95%+ of characters immediately)
  if (cp < 128) {
    // Bitwise trick: (cp | 0x20) converts uppercase ASCII to lowercase.
    // This checks for A-Z and a-z mathematically, avoiding memory lookups and <cctype>
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '\'';
  }

  // General Punctuation Block, Currency, Math, Arrows, & Symbols (0x2000 - 0x2BFF)
  if (cp >= 0x2000 && cp <= 0x2BFF) {
    // Explicitly allow smart quotes, reject all other general punctuation (em-dashes, etc.)
    return cp == 0x2018 || cp == 0x2019;
  }

  // Latin-1 Punctuation Block (0x00A1 - 0x00BF)
  if (cp >= 0x00A1 && cp <= 0x00BF) {
    // Allow ordinal indicators and micro sign, reject the rest (¡, ¿, «, », etc.)
    return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
  }

  // Rejects Two-em dash, Three-em dash, Double oblique hyphen, etc.
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;

  // Rejects Modifier Minus (0x02D7), Small Hyphen (0xFE63), and Fullwidth Hyphen (0xFF0D)
  if (cp == 0x02D7 || cp == 0xFE63 || cp == 0xFF0D) return false;
  // Assume all other Unicode ranges (accented letters, Cyrillic, Greek, etc.) are valid

  return true;
}

}  // namespace

// Small safety margin above the ACTUAL allocation size at each guarded grow/layout, so we refuse
// only when the real contiguous allocation genuinely would not fit (plus slack for the sibling
// vectors / per-word string buffers). Kept small so a merely fragmented-but-healthy heap during
// reading is not needlessly failed to an index-failed popup (the first v6 cut was too aggressive).
static constexpr size_t LOW_HEAP_HEADROOM = 2 * 1024;

namespace {
bool g_boldBodyText = false;
}  // namespace

void ParsedText::setBoldBodyText(const bool enabled) { g_boldBodyText = enabled; }

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle_, const bool underline,
                         const bool attachToPrevious) {
  if (word.empty()) return;
  // Bold reading mode: promote every body word to the bold face (REGULAR->BOLD,
  // ITALIC->BOLD_ITALIC). Already-bold headings are unchanged; fonts without a bold
  // face fall back via SdCardFont::resolveStyle / EpdFontFamily::getFont.
  const EpdFontFamily::Style fontStyle =
      g_boldBodyText ? static_cast<EpdFontFamily::Style>(fontStyle_ | EpdFontFamily::BOLD) : fontStyle_;

  // The device fonts carry no combining-mark positioning, so EPUB text stored in NFD
  // (a base letter followed by separate combining accents -- common for Vietnamese,
  // and used for many EPUB <h1> chapter headings) renders with the marks detached or
  // misplaced. Compose to NFC here, the single funnel every word passes through, so a
  // precomposed glyph is used instead. This runs once per word at layout time (the
  // result is cached in the section file) and is a cheap no-op for mark-free text.
  word = utf8ComposeNfc(word);

  EpdFontFamily::Style baseStyle = fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }
  const bool wordStartsRtl = !hasRtlWord && mayContainRtlBytes(word.c_str()) &&
                             BidiUtils::startsWithRtl(word.c_str(), RTL_PER_WORD_PROBE_DEPTH);

  const auto ensureTokenCapacity = [&](const size_t additionalTokens) {
    if (oom_) return;
    if (additionalTokens == 0) return;
    const size_t requiredSize = words.size() + additionalTokens;
    if (words.capacity() >= requiredSize) return;

    size_t newCapacity = words.capacity();
    if (newCapacity < 16) {
      newCapacity = 16;
    }
    while (newCapacity < requiredSize) {
      newCapacity *= 2;
    }

    // std::vector<std::string> growth uses throwing operator new; under -fno-exceptions an OOM
    // aborts the whole device. Guard the reserve (mirrors Dictionary.cpp / KOReaderSyncClient).
    // Try the amortized doubled capacity first; if the largest free block can't hold it, fall back
    // to the minimal exact size before degrading, so a merely-fragmented-but-healthy heap still
    // renders. words[] dominates; the parallel style/bool vectors fit within the headroom.
    if (ESP.getMaxAllocHeap() < newCapacity * sizeof(std::string) + LOW_HEAP_HEADROOM) {
      newCapacity = requiredSize;
      if (ESP.getMaxAllocHeap() < newCapacity * sizeof(std::string) + LOW_HEAP_HEADROOM) {
        LOG_ERR("PTX", "token reserve refused: %u words need %u bytes, maxAlloc=%u",
                static_cast<unsigned>(requiredSize), static_cast<unsigned>(newCapacity * sizeof(std::string)),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
        oom_ = true;
        return;
      }
    }

    words.reserve(newCapacity);
    wordStyles.reserve(newCapacity);
    wordContinues.reserve(newCapacity);
    wordNoSpaceBefore.reserve(newCapacity);
    wordIsFocusSuffix.reserve(newCapacity);
  };

  // Every append funnels through here so one heap guard covers all paths (CJK
  // per-char split, single token, focus split). On refusal the token is dropped
  // and oom_ latches; the parser sees hasOom() and abandons the build. Because
  // ensureTokenCapacity reserves all five vectors together, a token that passes
  // the guard cannot reallocate mid-push and desync them.
  const auto pushToken = [&](std::string token, const bool continues, const bool noSpaceBefore,
                             const bool isFocusSuffix) {
    ensureTokenCapacity(1);
    if (oom_) return;
    words.push_back(std::move(token));
    wordStyles.push_back(baseStyle);
    wordContinues.push_back(continues);
    wordNoSpaceBefore.push_back(noSpaceBefore);
    wordIsFocusSuffix.push_back(isFocusSuffix);
  };

  bool effectiveAttachToPrevious = attachToPrevious;
  bool effectiveNoSpaceBefore = false;
  if (attachToPrevious && !words.empty() &&
      hasCjkBreakOpportunityBetween(lastCodepoint(words.back()), firstCodepoint(word))) {
    effectiveAttachToPrevious = false;
    effectiveNoSpaceBefore = true;
  }

  if (auto breakOffsets = cjkCharacterBreakByteOffsets(word); !breakOffsets.empty()) {
    // CJK-heavy paragraphs can push hundreds of tiny tokens quickly when CSS toggles
    // inline styles. Reserve once up front to avoid repeated vector growth reallocations.
    ensureTokenCapacity(breakOffsets.size() + 1);
    bool firstToken = true;
    size_t tokenStart = 0;
    for (const size_t breakOffset : breakOffsets) {
      if (breakOffset <= tokenStart || breakOffset > word.size()) continue;
      pushToken(word.substr(tokenStart, breakOffset - tokenStart), firstToken ? effectiveAttachToPrevious : false,
                firstToken ? effectiveNoSpaceBefore : true, false);
      firstToken = false;
      tokenStart = breakOffset;
    }
    if (tokenStart < word.size()) {
      pushToken(word.substr(tokenStart), firstToken ? effectiveAttachToPrevious : false,
                firstToken ? effectiveNoSpaceBefore : true, false);
    }
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  if (containsCjkBreakableCodepoint(word)) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, false);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // Already-bold text should stay fully bold; focus splitting would make its suffix regular later.
  if (!this->focusReadingEnabled || (baseStyle & EpdFontFamily::BOLD) != 0) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, false);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // --- FOCUS READING LOGIC BELOW ---

  // Worst case: a segment boundary on each byte (highly punctuated UTF-8 text). Reserving up front
  // (token count <= word.length()) means the direct emplace_backs below never reallocate. If the
  // reserve was refused under low heap, oom_ is set: drop the word instead of aborting on a throw.
  ensureTokenCapacity(word.length());
  if (oom_) return;

  // Lambda helper to process and push individual sub-segments of the string
  // Use std::string_view to avoid heap allocations when slicing
  auto processSegment = [&](std::string_view segment, bool isWord, bool attach, bool noSpaceBefore) {
    if (!isWord) {
      // Punctuation and Numbers stay regular
      words.emplace_back(segment);
      wordStyles.push_back(baseStyle);
      wordContinues.push_back(attach);
      wordNoSpaceBefore.push_back(noSpaceBefore);
      wordIsFocusSuffix.push_back(false);
    } else {
      size_t charCount = 0;
      const unsigned char* countPtr = reinterpret_cast<const unsigned char*>(segment.data());
      const unsigned char* countEnd = countPtr + segment.length();

      while (countPtr < countEnd) {
        utf8NextCodepoint(&countPtr);
        charCount++;
      }

      // Target 45% for 1-bold at 4 chars and 3-bold at 7 chars with floor truncation
      constexpr size_t FOCUS_READING_PERCENT = 45;
      size_t targetBoldChars = (charCount * FOCUS_READING_PERCENT) / 100;
      targetBoldChars = std::clamp<size_t>(targetBoldChars, 1, 9);

      if (targetBoldChars >= charCount) {
        // Whole segment is bold - no suffix split needed
        words.emplace_back(segment);
        wordStyles.push_back(static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD));
        wordContinues.push_back(attach);
        wordNoSpaceBefore.push_back(noSpaceBefore);
        wordIsFocusSuffix.push_back(false);
      } else {
        countPtr = reinterpret_cast<const unsigned char*>(segment.data());
        for (size_t i = 0; i < targetBoldChars; ++i) {
          utf8NextCodepoint(&countPtr);
        }
        size_t splitByteOffset = countPtr - reinterpret_cast<const unsigned char*>(segment.data());

        // Bold prefix
        words.emplace_back(segment.substr(0, splitByteOffset));
        wordStyles.push_back(static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD));
        wordContinues.push_back(attach);
        wordNoSpaceBefore.push_back(noSpaceBefore);
        wordIsFocusSuffix.push_back(false);

        // Regular suffix - marked so extractLine can merge it back into single TextBlock entry
        words.emplace_back(segment.substr(splitByteOffset));
        wordStyles.push_back(baseStyle);
        wordContinues.push_back(true);
        wordNoSpaceBefore.push_back(false);
        wordIsFocusSuffix.push_back(true);
      }
    }
  };

  // Tokenize the string by alternating states (Word vs. Non-Word)
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* end = ptr + word.length();

  const unsigned char* segmentStart = ptr;
  uint32_t firstCp = utf8NextCodepoint(&ptr);  // Consume the first char to determine initial state
  bool inWordSegment = isWordCharacter(firstCp);

  bool isFirstSegment = true;

  while (ptr < end) {
    const unsigned char* currentCpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    bool isWordChar = isWordCharacter(cp);

    // Whenever the character type flips, slice off the segment we just completed and process it
    if (isWordChar != inWordSegment) {
      size_t segmentLen = currentCpStart - segmentStart;
      std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);

      // Only the very first segment inherits the original attachToPrevious flag.
      // Every subsequent segment MUST attach=true so it glues seamlessly to the prefix.
      processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                     isFirstSegment ? effectiveNoSpaceBefore : false);

      // Setup for the next segment
      segmentStart = currentCpStart;
      inWordSegment = isWordChar;
      isFirstSegment = false;
    }
  }

  // Process the final remaining segment
  size_t segmentLen = end - segmentStart;
  std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);
  processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                 isFirstSegment ? effectiveNoSpaceBefore : false);
  if (wordStartsRtl) {
    hasRtlWord = true;
  }
}

int ParsedText::resolveFirstLineIndent(const bool isFirstLine, const GfxRenderer& renderer, const int fontId) const {
  if (!isFirstLine || !isNaturalAlign) {
    return 0;
  }
  if (blockStyle.textIndentDefined) {
    if (blockStyle.textIndent < 0 || !extraParagraphSpacing) {
      return blockStyle.textIndent;
    }
    return 0;
  }
  if (!extraParagraphSpacing) {
    return renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR) * 3;
  }
  return 0;
}
// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Per-paragraph RTL auto-detection: only when CSS/HTML didn't explicitly set direction.
  // Explicit dir="ltr" must be respected and not overridden by content heuristic.
  if (!blockStyle.directionDefined && hasRtlWord) {
    // Check the first few words for RTL letter codepoints (no heap allocation).
    const size_t wordsToScan = std::min(words.size(), RTL_PARAGRAPH_PROBE_WORDS);
    for (size_t i = 0; i < wordsToScan; ++i) {
      if (BidiUtils::startsWithRtl(words[i].c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
        blockStyle.isRtl = true;
        break;
      }
    }
  }

  isNaturalAlign =
      blockStyle.alignment == CssTextAlign::Justify ||
      (blockStyle.isRtl ? blockStyle.alignment == CssTextAlign::Right : blockStyle.alignment == CssTextAlign::Left);

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For SD card fonts this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  if (renderer.isSdCardFont(fontId)) {
    // Style mask: only ask the SD font to load advances for styles actually
    // used in this paragraph. Style index is the low two bits (regular/bold/
    // italic/bold-italic); the underline bit is irrelevant to advance metrics.
    uint8_t styleMask = 0;
    for (auto s : wordStyles) {
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(s) & 0x03));
    }
    if (styleMask == 0) styleMask = 0x01;  // defensive: regular only
    renderer.ensureSdCardFontReady(fontId, words, hyphenationEnabled, styleMask);
  }

  // Layout allocates O(N) DP + width tables (computeLineBreaks/calculateWordWidths);
  // those throwing allocations abort under -fno-exceptions. Refuse and degrade if the
  // largest free block can't hold them, latching oom_ for the parser to abandon on.
  // The layout DP/width tables (dp + ans + wordWidths + lineBreakIndices) total ~14 bytes/word,
  // allocated while the words vectors stay resident. Guard against that real need (not the rare
  // hyphenation words-realloc worst case, which is bounded by the small soft-flush block size) so
  // a normal paragraph is not refused on a modestly-fragmented but adequate heap.
  if (oom_) return;
  if (!words.empty() && ESP.getMaxAllocHeap() < words.size() * 14 + LOW_HEAP_HEADROOM) {
    LOG_ERR("PTX", "layout refused: %u words, maxAlloc=%u bytes", static_cast<unsigned>(words.size()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    oom_ = true;
    return;
  }

  const int pageWidth = viewportWidth;
  auto wordWidths = calculateWordWidths(renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  if (hyphenationEnabled) {
    // Use greedy layout that can split words mid-loop when a hyphenated prefix fits.
    lineBreakIndices =
        computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore);
  }
  // Either branch above may have refused (set oom_) partway through, returning {} or a partial
  // result; bail out before the size()-1 below underflows on an empty vector.
  if (oom_) return;
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, lineBreakIndices, processLine, renderer,
                fontId);
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    wordNoSpaceBefore.erase(wordNoSpaceBefore.begin(), wordNoSpaceBefore.begin() + consumed);
    wordIsFocusSuffix.erase(wordIsFocusSuffix.begin(), wordIsFocusSuffix.begin() + consumed);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(words.size());

  for (size_t i = 0; i < words.size(); ++i) {
    wordWidths.push_back(measureWordWidth(renderer, fontId, words[i], wordStyles[i]));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                                  std::vector<bool>& noSpaceBeforeVec) {
  if (words.empty()) {
    return {};
  }

  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, fontId, wordWidths, /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  const size_t totalWordCount = words.size();

  // The fallback-hyphenation loop above can grow `words` past what layoutAndExtractLines's
  // upfront guard checked (that check runs before this loop, against the pre-split word
  // count -- see the comment there). Each individual split is itself guarded inside
  // hyphenateWordAtIndex, but nothing re-validates the *cumulative* result before the O(N)
  // tables below are sized to the post-split totalWordCount: dp (int) + ans (size_t) +
  // lineBreakIndices, reserved below to its worst case (one entry per word, size_t) so its
  // later push_back loop never has to grow on its own. Re-check here before any of the three
  // bare (throwing) vector allocations run.
  if (ESP.getMaxAllocHeap() < totalWordCount * (sizeof(int) + 2 * sizeof(size_t)) + LOW_HEAP_HEADROOM) {
    LOG_ERR("PTX", "computeLineBreaks refused: %u words post-hyphenation, maxAlloc=%u bytes",
            static_cast<unsigned>(totalWordCount), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    oom_ = true;
    return {};
  }

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i) && noSpaceBeforeVec[j]) {
        gap = 0;
      } else if (j > static_cast<size_t>(i) && !continuesVec[j]) {
        gap =
            renderer.getSpaceAdvance(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      } else if (j > static_cast<size_t>(i) && continuesVec[j]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        gap = renderer.getKerning(fontId, lastCodepoint(words[j - 1]), firstCodepoint(words[j]), wordStyles[j - 1]);
      }
      currlen += wordWidths[j] + gap;

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (j + 1 < totalWordCount && continuesVec[j + 1]) {
        continue;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1). Reserved to
  // the worst case (one line per word) up front -- guaranteed to fit by the check above -- so
  // this loop's push_back can never itself trigger an unguarded reallocation.
  std::vector<size_t> lineBreakIndices;
  lineBreakIndices.reserve(totalWordCount);
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, std::vector<uint16_t>& wordWidths,
                                                            std::vector<bool>& continuesVec,
                                                            std::vector<bool>& noSpaceBeforeVec) {
  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  bool isFirstLine = true;

  // lineBreakIndices grows once per output line. Unlike computeLineBreaks, hyphenation here is
  // interleaved with line-building rather than a single upfront pass, so the final word/line
  // count isn't known until this loop finishes and a single upfront reserve can't be sized
  // correctly. Guard each grow individually instead, mirroring hyphenateWordAtIndex's own
  // doubled-then-minimal reserve pattern, so this vector's push_back below can never itself
  // hit an unguarded (throwing) reallocation.
  const auto growLineBreakIndicesIfNeeded = [&]() {
    if (lineBreakIndices.size() < lineBreakIndices.capacity()) return true;
    const size_t doubled = lineBreakIndices.capacity() < 16 ? 16 : lineBreakIndices.capacity() * 2;
    if (ESP.getMaxAllocHeap() >= doubled * sizeof(size_t) + LOW_HEAP_HEADROOM) {
      lineBreakIndices.reserve(doubled);
      return true;
    }
    const size_t minimal = lineBreakIndices.size() + 1;
    if (ESP.getMaxAllocHeap() >= minimal * sizeof(size_t) + LOW_HEAP_HEADROOM) {
      lineBreakIndices.reserve(minimal);
      return true;
    }
    return false;
  };

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      int spacing = 0;
      if (!isFirstWord && noSpaceBeforeVec[currentIndex]) {
        spacing = 0;
      } else if (!isFirstWord && !continuesVec[currentIndex]) {
        spacing = renderer.getSpaceAdvance(fontId, lastCodepoint(words[currentIndex - 1]),
                                           firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      } else if (!isFirstWord && continuesVec[currentIndex]) {
        // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
        spacing = renderer.getKerning(fontId, lastCodepoint(words[currentIndex - 1]),
                                      firstCodepoint(words[currentIndex]), wordStyles[currentIndex - 1]);
      }
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentIndex < wordWidths.size() && continuesVec[currentIndex]) {
      --currentIndex;
    }

    if (!growLineBreakIndicesIfNeeded()) {
      LOG_ERR("PTX", "computeHyphenatedLineBreaks refused: %u lines so far, maxAlloc=%u bytes",
              static_cast<unsigned>(lineBreakIndices.size()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      oom_ = true;
      break;
    }
    lineBreakIndices.push_back(currentIndex);
    isFirstLine = false;
  }

  return lineBreakIndices;
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];

  // Collect candidate breakpoints (byte offsets and hyphen requirements).
  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  // libstdc++ basic_string's small-string-optimization buffer (_S_local_capacity).
  // word.substr(0, offset) below allocates a temporary string once offset exceeds this;
  // under -fno-exceptions a failed (throwing) allocation aborts the device. Guard it the
  // same way the actual split further down guards its own remainder buffer: skip the
  // candidate rather than risk an abort — the loop already treats "this candidate isn't
  // usable" (too wide, no improvement) as a reason to try the next one.
  constexpr size_t kStringSsoBytes = 15;

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }
    if (offset > kStringSsoBytes && ESP.getMaxAllocHeap() < offset + LOW_HEAP_HEADROOM) {
      continue;  // Not enough heap to safely measure this candidate; try another.
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const int prefixWidth = measureWordWidth(renderer, fontId, word.substr(0, offset), style, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Splitting mutates eight things, every one of which can call the default (throwing)
  // operator new: the remainder's own string content buffer, a second copy of that same
  // buffer (words.insert() below copies rather than moves, since the local `remainder`
  // is reused for measureWordWidth further down), and one grow-by-one on each of six
  // parallel vectors (words, wordStyles, wordIsFocusSuffix, wordContinues,
  // wordNoSpaceBefore, wordWidths). Under -fno-exceptions a failure in any of these
  // aborts the whole device instead of returning null — exactly the "rare hyphenation
  // words-realloc worst case" that layoutAndExtractLines's own guard (above) explicitly
  // excludes from its estimate. Estimate the worst-case total up front and refuse the
  // whole split atomically before any allocation happens: checking (and reserving) one
  // vector at a time would let an earlier reserve() consume real heap before a later
  // check refuses, stranding that capacity bump on a split that never happened.
  const size_t remainderLen = word.size() - chosenOffset;
  // Two content buffers of remainderLen: the substr() further down and the insert() copy
  // after it. kStringSsoBytes is declared above, by the candidate-measurement loop.
  const size_t stringContentBytes = remainderLen > kStringSsoBytes ? remainderLen * 2 : 0;

  const auto doubledCap = [](const size_t capacity) { return capacity < 16 ? 16 : capacity * 2; };
  // {doubled, minimal} byte cost to grow a vector of `size`/`capacity` by one element,
  // or {0, 0} if its current capacity already covers it.
  const auto cost = [&](const size_t size, const size_t capacity, const size_t elemBytes) {
    struct Cost {
      size_t doubled, minimal;
    } c{0, 0};
    if (size >= capacity) {
      c.doubled = doubledCap(capacity) * elemBytes;
      c.minimal = (size + 1) * elemBytes;
    }
    return c;
  };
  // vector<bool> is bit-packed (~1 bit/element), not 1 byte/element.
  const auto boolCost = [&](const std::vector<bool>& vec) {
    auto c = cost(vec.size(), vec.capacity(), 1);
    c.doubled = (c.doubled + 7) / 8;
    c.minimal = (c.minimal + 7) / 8;
    return c;
  };

  const auto wordsC = cost(words.size(), words.capacity(), sizeof(std::string));
  const auto stylesC = cost(wordStyles.size(), wordStyles.capacity(), sizeof(EpdFontFamily::Style));
  const auto widthsC = cost(wordWidths.size(), wordWidths.capacity(), sizeof(uint16_t));
  const auto focusC = boolCost(wordIsFocusSuffix);
  const auto continuesC = boolCost(wordContinues);
  const auto noSpaceC = boolCost(wordNoSpaceBefore);

  const size_t doubledTotal = stringContentBytes + wordsC.doubled + stylesC.doubled + widthsC.doubled +
                              focusC.doubled + continuesC.doubled + noSpaceC.doubled;
  const size_t maxAlloc = ESP.getMaxAllocHeap();
  bool useMinimal = false;
  if (doubledTotal > 0 && maxAlloc < doubledTotal + LOW_HEAP_HEADROOM) {
    // Try the amortized doubled capacity first; if the largest free block can't hold it,
    // fall back to the minimal exact size before refusing (mirrors ensureTokenCapacity's
    // two-tier attempt).
    const size_t minimalTotal = stringContentBytes + wordsC.minimal + stylesC.minimal + widthsC.minimal +
                                focusC.minimal + continuesC.minimal + noSpaceC.minimal;
    if (maxAlloc < minimalTotal + LOW_HEAP_HEADROOM) {
      LOG_ERR("PTX", "hyphenateWordAtIndex: split refused (%u bytes), maxAlloc=%u",
              static_cast<unsigned>(minimalTotal), static_cast<unsigned>(maxAlloc));
      return false;
    }
    useMinimal = true;
  }

  const auto reserveIfNeeded = [&](auto& vec) {
    if (vec.size() < vec.capacity()) return;
    vec.reserve(useMinimal ? vec.size() + 1 : doubledCap(vec.capacity()));
  };
  reserveIfNeeded(words);
  reserveIfNeeded(wordStyles);
  reserveIfNeeded(wordIsFocusSuffix);
  reserveIfNeeded(wordContinues);
  reserveIfNeeded(wordNoSpaceBefore);
  reserveIfNeeded(wordWidths);

  // Split the word at the selected breakpoint and append a hyphen if required.
  // Re-index rather than reuse `word`: the reservations above may have reallocated it.
  std::string remainder = words[wordIndex].substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  words.insert(words.begin() + wordIndex + 1, remainder);
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, style);
  // The hyphen remainder is not a focus suffix - it starts fresh on the next line.
  wordIsFocusSuffix.insert(wordIsFocusSuffix.begin() + wordIndex + 1, false);

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);
  wordNoSpaceBefore.insert(wordNoSpaceBefore.begin() + wordIndex + 1, false);

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const std::vector<uint16_t>& wordWidths,
                             const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                             const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  const int firstLineIndent = resolveFirstLineIndent(breakIndex == 0, renderer, fontId);

  // Build line data by moving from the original vectors using index range
  std::vector<std::string> lineWords;
  lineWords.reserve(lineWordCount);
  std::vector<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; ++i) {
    std::string word = std::move(words[lastBreakAt + i]);
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
    lineWords.push_back(std::move(word));
    lineWordStyles.push_back(wordStyles[lastBreakAt + i]);
  }

  // Calculate total word width for this line, count actual word gaps,
  // and accumulate total natural gap widths (including space kerning adjustments).
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += wordWidths[lastBreakAt + wordIdx];
    // Count gaps: each word after the first creates a gap, unless it's a continuation
    if (wordIdx > 0 && noSpaceBeforeVec[lastBreakAt + wordIdx]) {
      // Unicode break opportunity with no inserted Latin-style space. It is still
      // a stretchable gap for justified CJK/Korean text.
      actualGapCount++;
    } else if (wordIdx > 0 && !continuesVec[lastBreakAt + wordIdx]) {
      actualGapCount++;
      totalNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                                   firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    } else if (wordIdx > 0 && continuesVec[lastBreakAt + wordIdx]) {
      // Non-breaking space tokens (" " with continues=true) are visible, stretchable spaces —
      // count them as justifiable gaps so justifyExtra is distributed to them too.
      if (lineWords[wordIdx] == " ") {
        actualGapCount++;
      }
      // Cross-boundary kerning for continuation words (e.g. nonbreaking spaces, attached punctuation)
      totalNaturalGaps += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx - 1]),
                                              firstCodepoint(lineWords[wordIdx]), lineWordStyles[wordIdx - 1]);
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  // For RTL, implicit/default Left alignment becomes Right alignment.
  // Explicit text-align:left must remain left for CSS correctness.
  const CssTextAlign effectiveAlignment =
      (blockStyle.isRtl && !blockStyle.textAlignDefined && blockStyle.alignment == CssTextAlign::Left)
          ? CssTextAlign::Right
          : blockStyle.alignment;

  // For justified text, compute per-gap extra to distribute remaining space evenly
  const int spareSpace = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                               ? computeJustifyExtra(spareSpace, actualGapCount)
                               : 0;

  // BiDi processing: reorder words with UAX#9 in full-line context.
  visualOrderScratch.clear();
  visualOrderScratch.reserve(lineWordCount);
  // Skip expensive visual-order resolution for pure LTR paragraphs that have no RTL words.
  const bool shouldResolveVisualOrder = blockStyle.isRtl || hasRtlWord;
  const bool willReorder =
      shouldResolveVisualOrder && BidiUtils::computeVisualWordOrder(lineWords, blockStyle.isRtl, visualOrderScratch);

  std::vector<int16_t> lineXPos;
  lineXPos.reserve(lineWordCount);

  if (willReorder) {
    reorderedWordsScratch.clear();
    reorderedStylesScratch.clear();
    reorderedWidthsScratch.clear();
    reorderedContinuesScratch.clear();
    reorderedNoSpaceBeforeScratch.clear();
    reorderedFocusSuffixScratch.clear();
    reorderedWordsScratch.reserve(visualOrderScratch.size());
    reorderedStylesScratch.reserve(visualOrderScratch.size());
    reorderedWidthsScratch.reserve(visualOrderScratch.size());
    reorderedContinuesScratch.reserve(visualOrderScratch.size());
    reorderedNoSpaceBeforeScratch.reserve(visualOrderScratch.size());
    reorderedFocusSuffixScratch.reserve(visualOrderScratch.size());

    for (size_t i = 0; i < visualOrderScratch.size(); ++i) {
      const uint16_t src = visualOrderScratch[i];
      reorderedWordsScratch.push_back(std::move(lineWords[src]));
      reorderedStylesScratch.push_back(lineWordStyles[src]);
      reorderedWidthsScratch.push_back(wordWidths[lastBreakAt + src]);
      reorderedFocusSuffixScratch.push_back(wordIsFocusSuffix[lastBreakAt + src]);

      // Continuation means "no break/gap between two adjacent logical tokens".
      // After visual reordering (common in RTL), an adjacent logical pair can appear
      // as either (prev -> curr) or (curr -> prev) in visual order; preserve both.
      bool continues = false;
      if (i > 0) {
        const size_t prevSrc = visualOrderScratch[i - 1];
        const size_t currSrc = src;
        const bool forwardAdjacent = currSrc == prevSrc + 1;
        const bool reverseAdjacent = prevSrc == currSrc + 1;

        if (forwardAdjacent && continuesVec[lastBreakAt + currSrc]) {
          continues = true;
        } else if (reverseAdjacent && continuesVec[lastBreakAt + prevSrc]) {
          continues = true;
        }
      }
      reorderedContinuesScratch.push_back(continues);
      reorderedNoSpaceBeforeScratch.push_back(!continues && noSpaceBeforeVec[lastBreakAt + src]);
    }

    int reorderedWordWidthSum = 0;
    size_t reorderedGapCount = 0;
    int reorderedNaturalGaps = 0;
    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      reorderedWordWidthSum += reorderedWidthsScratch[wordIdx];
      if (wordIdx > 0 && reorderedNoSpaceBeforeScratch[wordIdx]) {
        // Unicode break opportunity with no inserted Latin-style space. It is still
        // a stretchable gap for justified CJK/Korean text.
        reorderedGapCount++;
      } else if (wordIdx > 0 && !reorderedContinuesScratch[wordIdx]) {
        reorderedGapCount++;
        reorderedNaturalGaps += renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                                         firstCodepoint(reorderedWordsScratch[wordIdx]),
                                                         reorderedStylesScratch[wordIdx - 1]);
      } else if (wordIdx > 0 && reorderedContinuesScratch[wordIdx]) {
        if (reorderedWordsScratch[wordIdx] == " ") {
          reorderedGapCount++;
        }
        reorderedNaturalGaps +=
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx - 1]),
                                firstCodepoint(reorderedWordsScratch[wordIdx]), reorderedStylesScratch[wordIdx - 1]);
      }
    }

    const int reorderedSpare = effectivePageWidth - reorderedWordWidthSum - reorderedNaturalGaps;
    const int reorderedJustifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                          ? computeJustifyExtra(reorderedSpare, reorderedGapCount)
                                          : 0;

    const int justifyContribution = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                        ? reorderedJustifyExtra * static_cast<int>(reorderedGapCount)
                                        : 0;
    const int contentWidth = reorderedWordWidthSum + reorderedNaturalGaps + justifyContribution;

    int xpos = 0;
    if (blockStyle.isRtl) {
      if (effectiveAlignment == CssTextAlign::Right || effectiveAlignment == CssTextAlign::Justify) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    } else {
      xpos = firstLineIndent;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    }

    for (size_t wordIdx = 0; wordIdx < reorderedWidthsScratch.size(); wordIdx++) {
      lineXPos.push_back(static_cast<int16_t>(xpos));
      xpos += reorderedWidthsScratch[wordIdx];

      const bool nextIsContinuation =
          wordIdx + 1 < reorderedWidthsScratch.size() && reorderedContinuesScratch[wordIdx + 1];
      if (nextIsContinuation) {
        int advance =
            renderer.getKerning(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                firstCodepoint(reorderedWordsScratch[wordIdx + 1]), reorderedStylesScratch[wordIdx]);
        // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
        // no-break space must not receive justifyExtra, or the line over-stretches by one
        // gap and the last word is pushed past the right margin (issue #2185).
        if (wordIdx > 0 && reorderedWordsScratch[wordIdx] == " " && reorderedContinuesScratch[wordIdx] &&
            effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          advance += reorderedJustifyExtra;
        }
        xpos += advance;
      } else if (wordIdx + 1 < reorderedWidthsScratch.size()) {
        const bool nextNoSpace = reorderedNoSpaceBeforeScratch[wordIdx + 1];
        int gap = nextNoSpace ? 0
                              : renderer.getSpaceAdvance(fontId, lastCodepoint(reorderedWordsScratch[wordIdx]),
                                                         firstCodepoint(reorderedWordsScratch[wordIdx + 1]),
                                                         reorderedStylesScratch[wordIdx]);
        if (effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
          gap += reorderedJustifyExtra;
        }
        xpos += gap;
      }
    }

    lineWords.swap(reorderedWordsScratch);
    lineWordStyles.swap(reorderedStylesScratch);
  } else {
    // Standard LTR/RTL positioning loop when no visual reordering is needed
    if (blockStyle.isRtl) {
      // RTL: position words from right to left
      int xpos = effectivePageWidth;
      if (effectiveAlignment == CssTextAlign::Left) {
        // Explicit left alignment in RTL context
        xpos = lineWordWidthSum + totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth + lineWordWidthSum + totalNaturalGaps) / 2;
      }
      // For Right and Justify, start from right edge (xpos = effectivePageWidth)

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        xpos -= wordWidths[lastBreakAt + wordIdx];
        lineXPos.push_back(static_cast<int16_t>(xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          // Cross-boundary kerning for continuation words
          int advance = renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                            firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          // wordIdx > 0: see the LTR branch — a leading no-break space is not a justifiable gap.
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos -= advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace
                      ? 0
                      : renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                 firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos -= gap;
        }
      }
    } else {
      // LTR: position words from left to right
      int xpos = firstLineIndent;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
      }

      for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
        lineXPos.push_back(static_cast<int16_t>(xpos));

        const bool nextIsContinuation = wordIdx + 1 < lineWordCount && continuesVec[lastBreakAt + wordIdx + 1];
        if (nextIsContinuation) {
          int advance = wordWidths[lastBreakAt + wordIdx];
          advance += renderer.getKerning(fontId, lastCodepoint(lineWords[wordIdx]),
                                         firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          // wordIdx > 0 mirrors the gap accounting above (which skips index 0): a leading
          // no-break space must not receive justifyExtra, or the line over-stretches by one
          // gap and the last word is pushed past the right margin (issue #2185).
          if (wordIdx > 0 && lineWords[wordIdx] == " " && continuesVec[lastBreakAt + wordIdx] &&
              effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            advance += justifyExtra;
          }
          xpos += advance;
        } else {
          int gap = 0;
          bool nextNoSpace = false;
          if (wordIdx + 1 < lineWordCount) {
            nextNoSpace = noSpaceBeforeVec[lastBreakAt + wordIdx + 1];
            gap = nextNoSpace
                      ? 0
                      : renderer.getSpaceAdvance(fontId, lastCodepoint(lineWords[wordIdx]),
                                                 firstCodepoint(lineWords[wordIdx + 1]), lineWordStyles[wordIdx]);
          }
          if (wordIdx + 1 < lineWordCount && effectiveAlignment == CssTextAlign::Justify && !isLastLine) {
            gap += justifyExtra;
          }
          xpos += wordWidths[lastBreakAt + wordIdx] + gap;
        }
      }
    }
  }

  const auto isFocusSuffixAt = [&](const size_t idx) {
    return willReorder ? reorderedFocusSuffixScratch[idx] : wordIsFocusSuffix[lastBreakAt + idx];
  };

  // Fast path: when no word on this line was split for focus reading, skip the merge work
  // entirely and pass empty boundary/suffixX vectors. TextBlock pays zero per-word RAM cost
  // for these annotations when the vectors are empty.
  bool lineHasFocusSplit = false;
  for (size_t i = 0; i < lineWordCount; i++) {
    if (isFocusSuffixAt(i)) {
      lineHasFocusSplit = true;
      break;
    }
  }

  if (!lineHasFocusSplit) {
    // TextBlock flattens the vectors into its arena; they stay owned here and die at return.
    auto block = std::make_shared<TextBlock>(lineWords, lineXPos, lineWordStyles, std::vector<uint8_t>{},
                                             std::vector<uint16_t>{}, blockStyle);
    if (!block->valid()) {
      LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
      return;
    }
    processLine(std::move(block));
    return;
  }

  // Slow path: merge focus suffix tokens back into their preceding word entry so each
  // original word occupies one TextBlock slot. Splits are recorded as per-word annotations
  // applied at render time, cutting the token count significantly when the feature is active.
  std::vector<std::string> outWords;
  std::vector<int16_t> outXPos;
  std::vector<EpdFontFamily::Style> outStyles;
  std::vector<uint8_t> outBoundaries;
  std::vector<uint16_t> outSuffixX;
  outWords.reserve(lineWordCount);
  outXPos.reserve(lineWordCount);
  outStyles.reserve(lineWordCount);
  outBoundaries.reserve(lineWordCount);
  outSuffixX.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; i++) {
    if (isFocusSuffixAt(i) && !outWords.empty()) {
      // Focus suffix: merge string into the preceding bold-prefix entry.
      outWords.back() += lineWords[i];
    } else {
      // Normal word: check for a following focus suffix to record the byte boundary.
      uint8_t boundary = 0;
      uint16_t suffixX = 0;
      if (i + 1 < lineWordCount && isFocusSuffixAt(i + 1)) {
        boundary = static_cast<uint8_t>(std::min(lineWords[i].size(), size_t{255}));
        // Suffix x offset = layout-time advance of the bold prefix, already known from xpos table.
        const int suffixDelta = static_cast<int>(lineXPos[i + 1]) - static_cast<int>(lineXPos[i]);
        suffixX = static_cast<uint16_t>(suffixDelta > 0 ? suffixDelta : 0);
      }
      outWords.push_back(std::move(lineWords[i]));
      outXPos.push_back(lineXPos[i]);
      // For focus entries with a suffix, strip BOLD from the stored style.
      // Render re-applies it to the prefix portion only, via the boundary field.
      const EpdFontFamily::Style storedStyle =
          boundary > 0 ? static_cast<EpdFontFamily::Style>(lineWordStyles[i] & ~EpdFontFamily::BOLD)
                       : lineWordStyles[i];
      outStyles.push_back(storedStyle);
      outBoundaries.push_back(boundary);
      outSuffixX.push_back(suffixX);
    }
  }

  auto block = std::make_shared<TextBlock>(outWords, outXPos, outStyles, outBoundaries, outSuffixX, blockStyle);
  if (!block->valid()) {
    LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
    return;
  }
  processLine(std::move(block));
}
