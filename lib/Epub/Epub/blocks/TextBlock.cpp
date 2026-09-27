#include "TextBlock.h"

#include "../VerticalText.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <ZhuyinActive.h>

#include <cstring>
#include <new>

#include "../../../../src/fontIds.h"

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasFocus, const uint16_t textBytes) {
  // Layout documented in TextBlock.h: 16-bit arrays first, then 8-bit arrays, then text.
  size_t size = static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t));
  if (hasFocus) {
    size += static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(uint8_t));
  }
  return size + textBytes;
}

void TextBlock::bindArenaPointers() {
  uint8_t* base = arena.get();
  const size_t wc = numWords;
  textOffArr = reinterpret_cast<const uint16_t*>(base);
  xposArr = reinterpret_cast<const int16_t*>(base + wc * 2);
  size_t off = wc * 4;
  if (focusPresent) {
    focusSuffixXArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  stylesArr = base + off;
  off += wc;
  if (focusPresent) {
    focusBoundaryArr = base + off;
    off += wc;
  }
  textArr = reinterpret_cast<const char*>(base + off);
}

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusSuffixX, const BlockStyle& blockStyle,
                     std::vector<std::string> rubyTexts, const zhuyin::SwapBatch& swaps)
    : blockStyle(blockStyle), rubyTexts(std::move(rubyTexts)) {
  // Same invariant as deserialize(): a block never holds an all-empty rubyTexts, so a
  // ruby-less line costs nothing beyond its arena. The layout engine hands one over for
  // every line it extracts, ruby or not; release it here rather than carrying it for the
  // block's lifetime. Move-assigning an empty vector frees the buffer (clear() would not).
  if (!hasRuby()) {
    this->rubyTexts = std::vector<std::string>{};
  }

  // Focus annotations are optional: empty vectors mean no word in this block has a split.
  // When present, they must be sized in lockstep with words[].
  const bool hasFocus = !focusBoundary.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() || words.size() > 10000 ||
      (hasFocus && (words.size() != focusBoundary.size() || words.size() != focusSuffixX.size()))) {
    LOG_ERR("TXB", "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(focusBoundary.size()),
            static_cast<uint32_t>(focusSuffixX.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  focusPresent = hasFocus;
  if (numWords == 0) {
    if (swaps.count > 0) zhuyin::swapStats().dropCheck++;  // 空行不會有替換（排版端的錯）
    return;  // valid empty block, no arena
  }

  // Pass 1: total text size, one NUL per word. A line is at most a physical
  // row of the page, so uint16_t offsets are ample; reject anything larger.
  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    focusPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, focusPresent, textBytes);
  // 注音：先決定清單能不能掛（引擎、世代、字型、大小），才知道要配多大 —— 仍然只有一次配置。
  // 配不到 arena＋清單就退回只配本體、這一行不換（引擎讓記憶體，codex 複查 ③ F6）。
  uint16_t keep = 0;
  if (swaps.count > 0) {
    const auto eng = zhuyin::activeEngine();
    if (!zhuyin::engineUsable(eng) || eng.generation != swaps.generation || eng.fontId != swaps.fontId) {
      zhuyin::swapStats().dropStale++;
    } else if (swaps.count > zhuyin::kMaxLineSwaps ||
               swapTailOffset(size) + swapTailBytes(swaps.count) > zhuyin::kMaxLineArenaBytes) {
      zhuyin::swapStats().dropSize++;
    } else {
      keep = swaps.count;
    }
  }
  if (keep > 0) {
    arena = makeUniqueNoThrow<uint8_t[]>(swapTailOffset(size) + swapTailBytes(keep));
    if (!arena) {
      zhuyin::swapStats().listOom++;
      keep = 0;
    }
  }
  if (!arena) arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    focusPresent = false;
    isValid = false;
    return;
  }
  bindArenaPointers();

  // Pass 2: fill. Mutable aliases of the const views bound above.
  auto* textOff = const_cast<uint16_t*>(textOffArr);
  auto* xpos = const_cast<int16_t*>(xposArr);
  auto* styles = const_cast<uint8_t*>(stylesArr);
  auto* text = const_cast<char*>(textArr);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = wordXpos[i];
    styles[i] = static_cast<uint8_t>(wordStyles[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
  if (focusPresent) {
    auto* suffixX = const_cast<uint16_t*>(focusSuffixXArr);
    auto* boundary = const_cast<uint8_t*>(focusBoundaryArr);
    for (uint16_t i = 0; i < numWords; i++) {
      suffixX[i] = focusSuffixX[i];
      boundary[i] = focusBoundary[i];
    }
  }
  if (keep > 0 && attachSwaps(size, swaps.list, keep, swaps.generation)) {
    zhuyin::swapStats().built++;
  }
}

bool TextBlock::attachSwaps(const size_t baseArenaBytes, const zhuyin::Swap* swaps, const uint16_t n,
                            const uint32_t generation) {
  const auto eng = zhuyin::activeEngine();
  if (!zhuyin::engineUsable(eng) || zhuyin::checkSwaps(lineText(), swaps, n, *eng.data) != zhuyin::SwapCheck::Ok) {
    zhuyin::swapStats().dropCheck++;
    return false;
  }
  uint8_t* tail = arena.get() + swapTailOffset(baseArenaBytes);
  const uint16_t zero = 0;
  memcpy(tail, &generation, sizeof(generation));
  memcpy(tail + 4, &n, sizeof(n));
  memcpy(tail + 6, &zero, sizeof(zero));
  for (uint16_t i = 0; i < n; i++) {
    // placement new：明確開始每一筆的生命期（codex 複查 ③ F11）
    new (tail + 8 + i * sizeof(zhuyin::Swap)) zhuyin::Swap{swaps[i].word, swaps[i].pua, swaps[i].cp, 0};
  }
  swapTail = tail;
  return true;
}

uint16_t TextBlock::swapCount() const {
  if (!swapTail) return 0;
  uint16_t n = 0;
  memcpy(&n, swapTail + 4, sizeof(n));
  return n;
}

const zhuyin::Swap* TextBlock::swapList() const {
  return swapTail ? reinterpret_cast<const zhuyin::Swap*>(swapTail + 8) : nullptr;
}

uint32_t TextBlock::swapGeneration() const {
  uint32_t g = 0;
  if (swapTail) memcpy(&g, swapTail, sizeof(g));
  return g;
}

bool TextBlock::swapsPersistable(const zhuyin::ActiveEngine& eng) const {
  return swapTail && zhuyin::engineUsable(eng) && eng.generation == swapGeneration();
}

bool TextBlock::swapsPersistable() const { return swapsPersistable(zhuyin::activeEngine()); }

bool TextBlock::serializeSwapHeader(HalFile& file, const uint8_t* binding) const {
  const uint16_t n = swapCount();
  return serialization::writePodChecked(file, n) && serialization::writePodChecked(file, static_cast<uint16_t>(~n)) &&
         file.write(binding, zhuyin::kSwapBindingBytes) == zhuyin::kSwapBindingBytes;
}

bool TextBlock::serializeSwaps(HalFile& file, const uint8_t* binding) const {
  const uint16_t n = swapCount();
  const zhuyin::Swap* list = swapList();
  uint16_t crc = zhuyin::swapCrcBegin(binding, n, numWords, focusPresent ? 1 : 0, textBytes, arena.get(),
                                      arenaSize(numWords, focusPresent, textBytes));
  for (uint16_t i = 0; i < n; i++) {
    uint8_t rec[zhuyin::kSwapDiskBytes];
    zhuyin::encodeSwap(list[i], rec);
    if (file.write(rec, sizeof(rec)) != sizeof(rec)) return false;
    crc = zhuyin::crc16(rec, sizeof(rec), crc);
  }
  return serialization::writePodChecked(file, crc);
}

bool TextBlock::hasRuby() const {
  for (const auto& rt : rubyTexts) {
    if (!rt.empty()) return true;
  }
  return false;
}

// 直排的繪製。
//
// ⭐ **轉置編碼**（見 ParsedTextVertical.cpp）：
//     xpos[i]          → 沿欄的位移（Cell 已含 cellAscent，Rotated 不含）
//     focusSuffixX[i]  → 跨軸（欄內）位移
//     styles[i] bit 7  → 這個 token 要整串順時針旋轉
//   三個值全部在【排版階段】烤好，這裡只做查表與座標加法 ——
//   因為本函式一頁跑 **16 次**（掃描 1 ＋ BW 1 ＋ 灰階 7 帶 × 2 平面 14）。
//
// ⚠️ **ruby 在直排刻意不做，而且必須顯式跳過** —— 只要 rubyTexts 非空，橫排那段就會
//    用橫排座標亂畫三處（不是靜默不畫）。直排的 TextBlock 一律不帶 rubyTexts，
//    這裡再擋一次。實測未見真正的注音 ruby。
// ⚠️ **不要用 SUP/SUB** —— renderCharScaled 沒有帶剪枝，直向分帶算繪會出事。
const zhuyin::Swap* TextBlock::drawableSwaps(const int fontId, uint16_t* n) const {
  *n = 0;
  if (!swapTail) return nullptr;
  const auto eng = zhuyin::activeEngine();
  if (!zhuyin::engineUsable(eng) || eng.generation != swapGeneration() || eng.fontId != fontId) {
    zhuyin::swapStats().renderGated++;
    return nullptr;
  }
  *n = swapCount();
  return swapList();
}

const char* TextBlock::drawnWord(const uint16_t i, const zhuyin::Swap* swaps, const uint16_t n, uint16_t& next,
                                 char* buf, const size_t cap) const {
  const char* word = wordText(i);
  if (!swaps) return word;
  uint16_t k = next;
  while (k < n && swaps[k].word == i) k++;  // 清單照 (word, cp) 排好：這個字詞的那幾筆連在一起
  if (k == next) return word;
  const bool ok = zhuyin::applyWordSwaps(word, wordTextLen(i), swaps + next, static_cast<uint16_t>(k - next), buf, cap);
  next = k;
  if (!ok) {
    zhuyin::swapStats().renderSkipped++;
    return word;
  }
  return buf;
}

void TextBlock::renderVertical(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) return;
  renderer.noteVerticalDraw();  // 證人（B-22）：直排繪製分支確實被走到
  // 注音（P2 ④）：判一次閘門；換字在交給字型之前做（預取掃描那一趟也一樣走這裡 → 看得到私用區碼位）
  uint16_t swapN = 0;
  const zhuyin::Swap* swaps = drawableSwaps(fontId, &swapN);
  uint16_t nextSwap = 0;
  char zbuf[zhuyin::kSwapWordBuffer];
  for (uint16_t i = 0; i < numWords; i++) {
    const char* word = drawnWord(i, swaps, swapN, nextSwap, zbuf, sizeof(zbuf));
    const uint8_t raw = static_cast<uint8_t>(wordStyle(i));
    const bool rotated = (raw & vtext::STYLE_BIT_ROTATED) != 0;
    // ⚠️ 傳給字型之前必須遮掉第 7 位，否則字型會拿到它不認得的位元。
    // ⚠️⚠️ 而且要**再遮掉直排沒有實作的四個裝飾位**（UNDERLINE/STRIKETHROUGH/SUP/SUB）。
    //    先前只寫在註解裡說「不要用 SUP/SUB」—— 但註解攔不住 EPUB：
    //    `<sup>` 會讓 drawText 走 renderCharScaled，而那條路**沒有 glyphIntersectsStrip
    //    剪枝**，灰階七帶會對同一個字重複解碼七次；底線／刪除線則是橫排 render() 才畫，
    //    直排靜默不畫。留著位元只會讓行為在兩邊都說不清。
    //    → V1 明確地【不支援】：遮掉，行為與註解一致。要做就走直排自己的基線位移。
    constexpr uint8_t VERTICAL_UNSUPPORTED_STYLE_BITS =
        EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH | EpdFontFamily::SUP | EpdFontFamily::SUB;
    const auto style =
        static_cast<EpdFontFamily::Style>(raw & vtext::STYLE_MASK & ~VERTICAL_UNSUPPORTED_STYLE_BITS);
    const int along = xposArr[i];
    const int cross = focusPresent ? static_cast<int>(focusSuffixXArr[i]) : 0;
    if (rotated) {
      renderer.drawTextVerticalCW(fontId, x + cross, y + along, word, true, style);
    } else {
      renderer.drawText(fontId, x + cross, y + along, word, true, style, BidiUtils::BidiBaseDir::LTR);
    }
  }
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }

  // 直排：與橫排完全分流。座標是【轉置】的，而且每個字要不要旋轉在排版階段就烤好了。
  if (renderer.isVerticalLayout()) {
    renderVertical(renderer, fontId, x, y);
    return;
  }

  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);

  // Resolve ruby positions. Layout (extractLine) has already reserved extraStartOffset on the
  // left and extraEndOffset on the right, so the centered rubyX is always within the page margins.
  struct RubyDrawInfo {
    int x;
    std::string text;
    BidiUtils::BidiBaseDir baseDir;
  };
  const bool blockHasRuby = hasRuby();
  std::vector<RubyDrawInfo> rubies;
  if (blockHasRuby) {
    rubies.resize(numWords);
    for (uint16_t i = 0; i < numWords; i++) {
      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) == 0) {
        int groupWordCount = 1;
        while (i + groupWordCount < numWords && (wordStyle(i + groupWordCount) & EpdFontFamily::RUBY_CONTINUE) != 0) {
          groupWordCount++;
        }
        int groupActualWidth = 0;
        for (int k = 0; k < groupWordCount; ++k) {
          groupActualWidth += renderer.getTextAdvanceX(fontId, wordText(i + k), wordStyle(i + k));
        }
        const int rubyWidth = renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP);
        const int leaderWordX = xposArr[i] + x;
        const auto baseDir =
            static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(wordText(i), blockStyle.isRtl ? 1 : 0));
        rubies[i] = {leaderWordX - (rubyWidth - groupActualWidth) / 2, rubyTexts[i], baseDir};
        i += groupWordCount - 1;
      }
    }
  }

  struct DecorationLineTracker {
    EpdFontFamily::Style style;
    int yOffset;
    int startX = -1;
    int endX = -1;
    int yPos = 0;

    bool active() const { return startX != -1; }
    void reset() {
      startX = -1;
      endX = -1;
      yPos = 0;
    }
  };

  DecorationLineTracker decorationLines[] = {
      {EpdFontFamily::UNDERLINE, ascender + 2},
      {EpdFontFamily::STRIKETHROUGH, ascender * 4 / 5},
  };

  const auto flushDecoration = [&](DecorationLineTracker& line) {
    if (line.active()) {
      renderer.drawLine(line.startX, line.yPos, line.endX, line.yPos, 2, true);
      line.reset();
    }
  };
  const auto flushDecorations = [&]() {
    for (auto& line : decorationLines) {
      flushDecoration(line);
    }
  };

  // Loop-invariant: hoisted out of the word loop so rubyTexts is scanned once,
  // not once per word.
  const int rubyShift = getRubyShift(ascender);

  // 注音（P2 ④）：判一次閘門；每個字詞在交給字型之前換字（預取掃描那一趟也走這裡 → 看得到私用區碼位）。
  // 換字長度不變（3 位元組換 3 位元組），所以專注閱讀的位元組界線、字寬、裝飾線都不受影響。
  uint16_t swapN = 0;
  const zhuyin::Swap* swaps = drawableSwaps(fontId, &swapN);
  uint16_t nextSwap = 0;
  char zbuf[zhuyin::kSwapWordBuffer];

  for (uint16_t i = 0; i < numWords; i++) {
    const char* word = drawnWord(i, swaps, swapN, nextSwap, zbuf, sizeof(zbuf));
    const int wordX = xposArr[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyle(i);
    const auto baseDir =
        static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(word, blockStyle.isRtl ? 1 : 0));
    const uint8_t boundary = focusBoundary(i);

    // SUP/SUB shift the baseline passed to drawText; the glyph is also scaled 50% inside
    // drawText, so these offsets are chosen relative to the full-size ascender:
    //   SUP: raise by 40% of ascender — sits clearly above the cap-height
    //   SUB: lower by 25% of ascender — descends below baseline without clashing with ascenders below
    int wordY = y + rubyShift;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    const int drawX = wordX;

    if (boundary > 0) {
      // Focus split: draw bold prefix, then the regular suffix at a pre-computed x offset.
      // The bold prefix is bounded to 9 codepoints by the clamp on targetBoldChars in
      // ParsedText::addWord; 9 UTF-8 codepoints occupy at most 9 * 4 = 36 bytes, +1 for null = 37.
      // suffixX is computed at cache-creation time to avoid font metric lookups at render time.
      static constexpr size_t MAX_FOCUS_PREFIX_BYTES = 9 * 4 + 1;
      char boldBuf[40];
      static_assert(sizeof(boldBuf) >= MAX_FOCUS_PREFIX_BYTES,
                    "boldBuf too small for max focus prefix (9 codepoints * 4 UTF-8 bytes + null)");
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      const size_t boldLen =
          std::min<size_t>({static_cast<size_t>(boundary), static_cast<size_t>(wordTextLen(i)), sizeof(boldBuf) - 1});
      memcpy(boldBuf, word, boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, drawX, wordY, boldBuf, true, boldStyle, baseDir);
      const int suffixX = drawX + focusSuffixXArr[i];
      renderer.drawText(fontId, suffixX, wordY, word + boldLen, true, currentStyle, baseDir);
    } else {
      renderer.drawText(fontId, drawX, wordY, word, true, currentStyle, baseDir);
    }

    // Horizontal ruby text rendering
    if (blockHasRuby && i < rubyTexts.size() && !rubyTexts[i].empty() &&
        (wordStyle(i) & EpdFontFamily::RUBY_CONTINUE) == 0) {
      const int rubyY = wordY - ascender;
      renderer.drawText(fontId, rubies[i].x, rubyY, rubies[i].text.c_str(), true, EpdFontFamily::SUP,
                        rubies[i].baseDir);
    }

    if (scanning) {
      continue;
    }

    if (EpdFontFamily::hasTextDecoration(currentStyle)) {
      int lineStartX = drawX;
      int lineWidth = renderer.getTextWidth(fontId, word, currentStyle, baseDir);

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        lineWidth = (lineWidth + 1) / 2;
      }

      // Do not decorate the synthetic em-space used for paragraph indentation.
      if (wordTextLen(i) >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
          static_cast<uint8_t>(word[2]) == 0x83) {
        const char* visibleText = word + 3;
        lineStartX += renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        lineWidth = renderer.getTextWidth(fontId, visibleText, currentStyle, baseDir);
        if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
          lineWidth = (lineWidth + 1) / 2;
        }
      }

      for (auto& line : decorationLines) {
        if ((currentStyle & line.style) == 0) {
          flushDecoration(line);
          continue;
        }

        const int lineY = wordY + line.yOffset;
        if (line.active() && line.yPos != lineY) {
          flushDecoration(line);
        }
        if (!line.active()) {
          line.startX = lineStartX;
          line.yPos = lineY;
        }
        line.endX = lineStartX + lineWidth;
      }
    } else {
      flushDecorations();
    }
  }
  flushDecorations();
}

bool TextBlock::serialize(HalFile& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }
  using serialization::writePodChecked;

  // Word data: scalars, then the arena verbatim -- its in-memory layout is
  // exactly the on-disk layout (see TextBlock.h), so one write covers all
  // per-word arrays and the text blob.
  // 每個欄位都檢查寫入長度（codex 複查 ③ F8）：寫到一半失敗就回 false，不讓後面恢復的寫入把殘缺的一頁補成「成功」。
  if (!writePodChecked(file, numWords) || !writePodChecked(file, static_cast<uint8_t>(focusPresent ? 1 : 0)) ||
      !writePodChecked(file, textBytes)) {
    LOG_ERR("TXB", "Serialization failed: short write (header)");
    return false;
  }
  if (numWords > 0) {
    const size_t size = arenaSize(numWords, focusPresent, textBytes);
    if (file.write(arena.get(), size) != size) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  // Ruby text data
  for (size_t i = 0; i < numWords; i++) {
    serialization::writeString(file, (i < rubyTexts.size()) ? rubyTexts[i] : std::string());
  }

  // Style (alignment + margins/padding/indent)
  const bool styleOk =
      writePodChecked(file, blockStyle.alignment) && writePodChecked(file, blockStyle.textAlignDefined) &&
      writePodChecked(file, blockStyle.marginTop) && writePodChecked(file, blockStyle.marginBottom) &&
      writePodChecked(file, blockStyle.marginLeft) && writePodChecked(file, blockStyle.marginRight) &&
      writePodChecked(file, blockStyle.paddingTop) && writePodChecked(file, blockStyle.paddingBottom) &&
      writePodChecked(file, blockStyle.paddingLeft) && writePodChecked(file, blockStyle.paddingRight) &&
      writePodChecked(file, blockStyle.textIndent) && writePodChecked(file, blockStyle.textIndentDefined) &&
      writePodChecked(file, blockStyle.isRtl) && writePodChecked(file, blockStyle.directionDefined);
  if (!styleOk) {
    LOG_ERR("TXB", "Serialization failed: short write (style)");
    return false;
  }
  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file, const uint16_t zhuyinSwaps, const uint8_t* binding,
                                                  const zhuyin::PagePlace& place) {
  using serialization::readPodChecked;
  if (zhuyinSwaps > 0 && !binding) {
    LOG_ERR("TXB", "Deserialization failed: zhuyin swaps without a binding");
    return nullptr;
  }
  // 每個欄位都檢查讀到的長度（codex 複查 ③ F8）：截斷 → nullptr，不讓未初始化的值流下去
  uint16_t wc = 0;
  uint8_t hasFocus = 0;
  uint16_t textBytes = 0;
  if (!readPodChecked(file, wc) || !readPodChecked(file, hasFocus) || !readPodChecked(file, textBytes)) {
    LOG_ERR("TXB", "Deserialization failed: truncated header");
    return nullptr;
  }

  // Sanity checks: cap the arena allocation and reject impossible geometry
  // (every word carries at least its NUL terminator).
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }
  if ((wc == 0 && textBytes != 0) || (wc > 0 && textBytes < wc)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wc);
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "OOM: TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;
  block->focusPresent = hasFocus != 0;

  // 標籤 4：先決定清單能不能掛（引擎可用、綁定相同、大小）—— 配置仍然只有一次。
  // 掛不上也照樣把那幾筆讀掉、驗 CRC（完整性在每一條路徑都驗，codex 複查 ③ F9）。
  const zhuyin::ActiveEngine eng = zhuyinSwaps > 0 ? zhuyin::activeEngine() : zhuyin::ActiveEngine{};
  const size_t baseSize = wc > 0 ? arenaSize(wc, block->focusPresent, textBytes) : 0;
  uint16_t keep = 0;
  uint32_t* dropCounter = nullptr;
  if (zhuyinSwaps > 0) {
    uint8_t want[zhuyin::kSwapBindingBytes] = {};
    if (zhuyin::engineUsable(eng)) zhuyin::encodeBinding(zhuyin::bindingOf(eng, place), want);
    if (wc == 0) {
      dropCounter = &zhuyin::swapStats().dropCheck;  // 空行不會有替換
    } else if (!zhuyin::engineUsable(eng)) {
      dropCounter = &zhuyin::swapStats().dropNoEngine;
    } else if (memcmp(binding, want, sizeof(want)) != 0) {
      dropCounter = &zhuyin::swapStats().dropBinding;  // 別的資料集／語意版號／字型／位置算的
    } else if (zhuyinSwaps > zhuyin::kMaxLineSwaps ||
               swapTailOffset(baseSize) + swapTailBytes(zhuyinSwaps) > zhuyin::kMaxLineArenaBytes) {
      dropCounter = &zhuyin::swapStats().dropSize;  // 過大的筆數也算語意失敗：讀完、驗 CRC，這一行不換（第二輪 F2）
    } else {
      keep = zhuyinSwaps;
    }
  }

  if (wc > 0) {
    if (keep > 0) {
      block->arena = makeUniqueNoThrow<uint8_t[]>(swapTailOffset(baseSize) + swapTailBytes(keep));
      if (!block->arena) {  // 引擎讓記憶體：只配本體、這一行不換（codex 複查 ③ F6）；在失敗的當下記
        zhuyin::swapStats().listOom++;
        dropCounter = nullptr;
        keep = 0;
      }
    }
    if (!block->arena) block->arena = makeUniqueNoThrow<uint8_t[]>(baseSize);
    if (!block->arena) {
      LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(baseSize));
      return nullptr;
    }
    if (file.read(block->arena.get(), baseSize) != static_cast<int>(baseSize)) {
      LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(baseSize));
      return nullptr;
    }
    block->bindArenaPointers();

    // Validate offsets before anything dereferences wordText(): offset 0 first,
    // strictly increasing, in bounds, and every word NUL-terminated (word i ends
    // at the byte before offset i+1; the last word at the last text byte).
    const uint16_t* textOff = block->textOffArr;
    const char* text = block->textArr;
    if (textOff[0] != 0 || text[textBytes - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
      return nullptr;
    }
    for (uint16_t i = 1; i < wc; i++) {
      if (textOff[i] <= textOff[i - 1] || textOff[i] >= textBytes || text[textOff[i] - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
        return nullptr;
      }
    }
  }

  // Ruby text data. Ruby is a CJK feature, so for nearly every book every entry here
  // is the empty string. Materializing the vector regardless costs wordCount * 24 bytes
  // (sizeof(std::string)) plus a heap block per line, held for as long as the page is
  // resident -- several KB of DRAM on a full page, none of it ever read. An empty
  // rubyTexts is already the "no ruby" representation: hasRuby() reports false and every
  // other reader is guarded by `i < rubyTexts.size()`, so allocate lazily and only once a
  // non-empty annotation actually shows up.
  //
  // `scratch` is reused across words: readString() resizes it to the incoming length and
  // overwrites every byte, so a moved-from value carries nothing into the next iteration.
  std::string scratch;
  for (uint16_t i = 0; i < wc; i++) {
    if (!serialization::readStringChecked(file, scratch)) {  // 長度欄位或內容讀不齊 → 這一頁壞了（第二輪 F8）
      LOG_ERR("TXB", "Deserialization failed: truncated ruby text %u", i);
      return nullptr;
    }
    if (scratch.empty()) continue;
    if (block->rubyTexts.empty()) {
      block->rubyTexts.resize(wc);
    }
    block->rubyTexts[i] = std::move(scratch);
  }

  // Style (alignment + margins/padding/indent)
  BlockStyle& blockStyle = block->blockStyle;
  const bool styleOk =
      readPodChecked(file, blockStyle.alignment) && readPodChecked(file, blockStyle.textAlignDefined) &&
      readPodChecked(file, blockStyle.marginTop) && readPodChecked(file, blockStyle.marginBottom) &&
      readPodChecked(file, blockStyle.marginLeft) && readPodChecked(file, blockStyle.marginRight) &&
      readPodChecked(file, blockStyle.paddingTop) && readPodChecked(file, blockStyle.paddingBottom) &&
      readPodChecked(file, blockStyle.paddingLeft) && readPodChecked(file, blockStyle.paddingRight) &&
      readPodChecked(file, blockStyle.textIndent) && readPodChecked(file, blockStyle.textIndentDefined) &&
      readPodChecked(file, blockStyle.isRtl) && readPodChecked(file, blockStyle.directionDefined);
  if (!styleOk) {
    LOG_ERR("TXB", "Deserialization failed: truncated style");
    return nullptr;
  }

  if (zhuyinSwaps > 0) {
    // 標籤 4 的尾段：每一筆＋CRC。完整性（讀不到、CRC 不符）→ 這一頁壞了；完整性過了但掛不上 → 這一行不換。
    // 先確定檔案裡真的還有那麼多位元組：筆數最多 65535，壞掉的筆數不該讓這裡空轉幾萬圈才發現讀不到。
    const size_t remaining = file.size() > file.position() ? file.size() - file.position() : 0;
    if (static_cast<size_t>(zhuyinSwaps) * zhuyin::kSwapDiskBytes + sizeof(uint16_t) > remaining) {
      LOG_ERR("TXB", "Deserialization failed: zhuyin swaps truncated (%u)", zhuyinSwaps);
      return nullptr;
    }
    uint16_t crc = zhuyin::swapCrcBegin(binding, zhuyinSwaps, wc, hasFocus, textBytes, block->arena.get(), baseSize);
    uint8_t* tail = keep ? block->arena.get() + swapTailOffset(baseSize) : nullptr;
    for (uint16_t i = 0; i < zhuyinSwaps; i++) {
      uint8_t rec[zhuyin::kSwapDiskBytes];
      if (file.read(rec, sizeof(rec)) != static_cast<int>(sizeof(rec))) {
        LOG_ERR("TXB", "Deserialization failed: zhuyin swap %u truncated", i);
        return nullptr;
      }
      crc = zhuyin::crc16(rec, sizeof(rec), crc);
      if (tail) new (tail + 8 + i * sizeof(zhuyin::Swap)) zhuyin::Swap(zhuyin::decodeSwap(rec));
    }
    uint16_t stored = 0;
    if (!readPodChecked(file, stored)) {
      LOG_ERR("TXB", "Deserialization failed: zhuyin swap CRC truncated");
      return nullptr;
    }
    if (stored != crc) {
      LOG_ERR("TXB", "Deserialization failed: zhuyin line CRC");
      return nullptr;
    }
    if (!keep) {
      if (dropCounter) (*dropCounter)++;  // 記憶體那一種已經在配置失敗的當下記過了
      return block;
    }
    const auto* list = reinterpret_cast<const zhuyin::Swap*>(tail + 8);
    if (zhuyin::checkSwaps(block->lineText(), list, keep, *eng.data) != zhuyin::SwapCheck::Ok) {
      zhuyin::swapStats().dropCheck++;
      return block;
    }
    const uint16_t zero = 0;
    memcpy(tail, &eng.generation, sizeof(eng.generation));
    memcpy(tail + 4, &keep, sizeof(keep));
    memcpy(tail + 6, &zero, sizeof(zero));
    block->swapTail = tail;
    zhuyin::swapStats().loaded++;
  }

  return block;
}
