#pragma once

// Formosa Cover：沒有封面的書，在歐風底框的書名區裡排書名（2026-10-07）。純邏輯：量寬度的函式由呼叫端給
//   （韌體用 GfxRenderer::getTextWidth，主機測試用假的等寬字），電腦端測試在 test/cover_title_layout。
// 規則（像素級試作時一條條撞出來的，工作區 docs/specs/2026-10-07-formosa-cover-theme.md §2.3）：
//   1. 平均分行：四個字排 2＋2，不出現孤字（貪婪折行會變「聊齋誌／異」）。
//   2. 看寬度也看高度：大字（14 粗）放不下就換小字（10 粗），再放不下就截斷加「…」。
//   3. 最多 3 行；行距＝字型行高 −2。

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace covertitle {

// 書名最多看這麼多字（codex 複查：最近閱讀的書名沒有長度上限，而 -fno-exceptions 下每個字一段字串＝配置數跟著長）。
//   書名區最多 3 行、每行不到十個字，超過的部分本來就會截成「…」。
constexpr size_t kMaxTitleChars = 48;

// UTF-8 拆成字元（每個字元一段位元組），最多 maxChars 個。壞位元組一個算一個字元，一定前進（不用 utf8NextCodepoint：遇
// NUL 不前進）。
inline std::vector<std::string> splitChars(const std::string& s, const size_t maxChars = kMaxTitleChars) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size() && out.size() < maxChars) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + n > s.size()) n = 1;
    for (size_t k = 1; k < n; ++k) {  // 後面不是延續位元組（10xxxxxx）＝殘缺字元：只吃這一個位元組
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
        n = 1;
        break;
      }
    }
    out.emplace_back(s.substr(i, n));
    i += n;
  }
  return out;
}

struct Layout {
  bool big = true;  // true＝大字（14 粗），false＝小字（10 粗）
  std::vector<std::string> lines;
  bool complete = true;  // 書名全部排出來、而且沒有從英數字單字中間換行（v378；fitWithAuthor 用它決定放不放作者）
};

// 斷行單位（v370 實機截圖：英文被從字中間切開「Sample B / ook 0176」）：
//   英數字（含 . , ' - : ! ? & ( ) 等）連成一個單字、不從中間斷；空白是可以斷的地方（斷在那裡就不畫）；
//   其他字元（中文、日文、全形標點）一個字一格，任兩個之間都能斷。
//   單字本身比書名區寬時，那個單字才退回一個字元一格。
struct Token {
  std::string text;
  bool space = false;
  bool wordPart = false;  // 太寬的單字拆成的字元、而且不是那個單字的最後一個 —— 在它後面換行＝從單字中間斷
};

// 把太寬的單字拆成字元；拆出來的除了最後一個都標 wordPart
inline void pushSplit(std::vector<Token>& toks, const std::string& word) {
  const std::vector<std::string> cs = splitChars(word);
  for (size_t i = 0; i < cs.size(); ++i) toks.push_back(Token{cs[i], false, i + 1 < cs.size()});
}

// 書名的空白先整理（v378 複查）：頭尾去掉、連續空白（含 tab／換行）併成一個 —— 否則 48 字上限可能全被空白吃掉，
//   或只差一個行尾空白就誤判「被截斷」補上「…」。
//   全形空白 U+3000 與不斷行空白 U+00A0 也算空白（第二輪複查：48 個全形空白＋「史記」會只剩「…」）。
inline std::string normalizeSpaces(const std::string& s) {
  std::string out;
  bool pending = false;
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    size_t n = 0;  // 這個空白佔幾個位元組（0＝不是空白）
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      n = 1;
    } else if (s.compare(i, 2, "\xC2\xA0") == 0) {
      n = 2;
    } else if (s.compare(i, 3, "\xE3\x80\x80") == 0) {
      n = 3;
    }
    if (n > 0) {
      pending = !out.empty();
      i += n;
      continue;
    }
    if (pending) out += ' ';
    pending = false;
    out += c;
    ++i;
  }
  return out;
}

inline bool isWordByte(const std::string& c) {
  if (c.size() != 1) return false;
  const unsigned char b = static_cast<unsigned char>(c[0]);
  return (b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || std::strchr(".,'-:;!?&()/#+", b);
}

inline std::vector<Token> tokenize(const std::vector<std::string>& chars) {
  std::vector<Token> out;
  for (const auto& c : chars) {
    if (c == " ") {
      if (!out.empty() && !out.back().space) out.push_back(Token{" ", true});
      continue;
    }
    if (isWordByte(c) && !out.empty() && !out.back().space && !out.back().text.empty() &&
        isWordByte(std::string(1, out.back().text.back())) && out.back().text.size() < 64) {
      out.back().text += c;
      continue;
    }
    out.push_back(Token{c, false});
  }
  while (!out.empty() && out.back().space) out.pop_back();
  return out;
}

// width(text,
// big)：那段字的像素寬；lineH(big)：該字型的行高。maxW／maxH：書名區可用寬高（authorH＝作者那一行要預留的高，0＝沒有作者）。
template <typename WidthFn, typename LineHFn>
Layout fit(const std::string& rawTitle, const int maxW, const int maxH, const int authorH, WidthFn width, LineHFn lineH,
           const int maxLines = 3) {
  const std::string title = normalizeSpaces(rawTitle);
  const std::vector<std::string> chars = splitChars(title);
  const bool clipped = [&] {  // 超過 kMaxTitleChars：只排前面那些，而且一定要走截斷加「…」
    size_t n = 0;
    for (const auto& c : chars) n += c.size();
    return n < title.size();
  }();
  const auto blockH = [&](const bool big, const int n) { return n * (lineH(big) - 2) + authorH; };
  if (chars.empty()) return Layout{true, {}};
  // 先試「單字不拆」（大字、小字），都不行才允許把太寬的單字拆成字元 —— 寧可換小字也不要從字中間斷
  for (int pass = 0; pass < 2 && !clipped; ++pass)
    for (const bool big : {true, false}) {
      // 斷行單位；第二輪才把太寬的單字拆成字元
      std::vector<Token> toks;
      for (Token& t : tokenize(chars)) {
        if (pass == 1 && !t.space && t.text.size() > 1 && width(t.text, big) > maxW) {
          pushSplit(toks, t.text);
        } else {
          toks.push_back(std::move(t));
        }
      }
      const int T = static_cast<int>(toks.size());
      // 寬度先各量一次再相加（中文沒有字距調整；英文單字整個量）—— 不在搜尋迴圈裡反覆量整行
      std::vector<int> w(T);
      for (int k = 0; k < T; ++k) w[k] = width(toks[k].text, big);
      // [a, b) 這一行：頭尾的空白不算
      const auto lineRange = [&](int a, int b, int& wa) {
        while (a < b && toks[a].space) ++a;
        while (b > a && toks[b - 1].space) --b;
        wa = 0;
        for (int k = a; k < b; ++k) wa += w[k];
        return b > a;
      };
      const auto lineText = [&](int a, int b) {
        while (a < b && toks[a].space) ++a;
        while (b > a && toks[b - 1].space) --b;
        std::string s;
        for (int k = a; k < b; ++k) s += toks[k].text;
        return s;
      };
      for (int n = 1; n <= maxLines && n <= T; ++n) {
        if (blockH(big, n) > maxH) break;
        // 找最平均的斷法（7 字 3 行＝3＋2＋2，不是 3＋3＋1）
        int best = -1, bestI = 0, bestJ = 0, bestFirst = -1, bestMin = -1;
        const auto consider = [&](int i, int j) {
          int w0, w1 = 0, w2 = 0;
          if (!lineRange(0, n == 1 ? T : i, w0)) return;
          if (n >= 2 && !lineRange(i, n == 2 ? T : j, w1)) return;
          if (n == 3 && !lineRange(j, T, w2)) return;
          const int m = std::max(w0, std::max(w1, w2));
          const int lo = n == 1 ? w0 : n == 2 ? std::min(w0, w1) : std::min(w0, std::min(w1, w2));
          if (m > maxW) return;
          // 最寬那行越窄越好 → 最窄那行越寬越好（不留孤字）→ 第一行長一點
          if (best < 0 || m < best || (m == best && (lo > bestMin || (lo == bestMin && w0 > bestFirst)))) {
            best = m, bestI = i, bestJ = j, bestFirst = w0, bestMin = lo;
          }
        };
        if (n == 1) {
          consider(T, T);
        } else if (n == 2) {
          for (int i = 1; i < T; ++i) consider(i, T);
        } else {
          for (int i = 1; i < T; ++i)
            for (int j = i + 1; j < T; ++j) consider(i, j);
        }
        if (best < 0) continue;
        std::vector<std::string> lines;
        lines.push_back(lineText(0, n == 1 ? T : bestI));
        if (n >= 2) lines.push_back(lineText(bestI, n == 2 ? T : bestJ));
        if (n == 3) lines.push_back(lineText(bestJ, T));
        // 量的是各段相加；整行實際寬度再確認一次（英文字距）
        bool ok = true;
        for (const auto& l : lines) ok = ok && width(l, big) <= maxW;
        // 換行點落在拆開的單字裡（第二輪才會有）＝不完整
        const bool mid = (n >= 2 && toks[bestI - 1].wordPart) || (n == 3 && toks[bestJ - 1].wordPart);
        if (ok) return Layout{big, std::move(lines), !mid};
      }
    }
  // 都放不下：小字、照斷行單位貪婪折行（v378：原本一個字元一格，英文會斷成「Sample B／ook」），
  //   截斷到放得下的行數，最後一行補「…」。比書名區寬的單字才拆成字元。
  Layout out{false, {}};
  int lines = maxLines;
  while (lines > 1 && blockH(false, lines) > maxH) --lines;
  std::vector<Token> toks;
  for (Token& t : tokenize(chars)) {
    if (!t.space && t.text.size() > 1 && width(t.text, false) > maxW) {
      pushSplit(toks, t.text);
    } else {
      toks.push_back(std::move(t));
    }
  }
  std::vector<std::vector<std::string>> rows(1);  // 每一行的單位（不含行首行尾的空白）
  const auto rowText = [](const std::vector<std::string>& r) {
    std::string t;
    for (const auto& u : r) t += u;
    return t;
  };
  size_t k = 0;
  bool pendingSpace = false;
  bool prevWordPart = false;  // 上一個放進去的單位是拆開單字的中段
  for (; k < toks.size(); ++k) {
    if (toks[k].space) {
      pendingSpace = !rows.back().empty();
      continue;
    }
    std::vector<std::string> trial = rows.back();
    if (pendingSpace) trial.push_back(" ");
    trial.push_back(toks[k].text);
    if (!rows.back().empty() && width(rowText(trial), false) > maxW) {
      if (static_cast<int>(rows.size()) == lines) break;
      if (prevWordPart) out.complete = false;
      rows.push_back({toks[k].text});
    } else {
      rows.back() = std::move(trial);
    }
    prevWordPart = toks[k].wordPart;
    pendingSpace = false;
  }
  if (k >= toks.size() && !clipped) {
    for (const auto& r : rows) out.lines.push_back(rowText(r));
    return out;  // 全部放得下（只是要小字）
  }
  out.complete = false;
  // 還有沒放進去的字 → 最後一行從後面整個單位拿掉，直到補上「…」放得下；只剩一個單位時才逐字元拿掉
  for (size_t r = 0; r + 1 < rows.size(); ++r) out.lines.push_back(rowText(rows[r]));
  std::vector<std::string> last = rows.back();
  while (!last.empty()) {
    while (!last.empty() && last.back() == " ") last.pop_back();
    if (last.empty()) break;
    if (width(rowText(last) + "…", false) <= maxW) {
      out.lines.push_back(rowText(last) + "…");
      return out;
    }
    if (last.size() == 1 && last[0].size() > 1) {
      std::vector<std::string> cs = splitChars(last[0]);
      cs.pop_back();
      last = cs;
    } else {
      last.pop_back();
    }
  }
  out.lines.push_back("…");
  return out;
}

// 排出來的書名有幾個看得到的字元（不算空白與「…」）—— 比較「帶作者」與「不帶作者」哪一份讓書名露得多
inline size_t visibleChars(const Layout& lay) {
  size_t n = 0;
  for (const auto& l : lay.lines)
    for (const auto& c : splitChars(l, l.size()))
      if (c != " " && c != "\xE2\x80\xA6") ++n;
  return n;
}

// 書名優先（v378）：作者只有在「不讓書名吃虧」時才放。帶作者排出來的書名不完整（截斷或斷字）或超出高度時，
//   改排不帶作者的那份 ——
//   但只有那份真的比較好（完整了，或露出更多字）才換；兩份一樣差（例如超長書名兩邊都截在第三行）就留作者。 v370
//   的寫法只看高度，而 fit() 為了塞進高度會先截斷書名 → 書名區寬而矮的款式變成「Sample B…」＋作者（v378
//   真字型測試抓到）。 authorH＝作者一行要預留的高（0＝沒有作者）；showAuthor 回傳要不要畫作者。
template <typename WidthFn, typename LineHFn>
Layout fitWithAuthor(const std::string& title, const int maxW, const int maxH, const int authorH, WidthFn width,
                     LineHFn lineH, bool& showAuthor) {
  Layout bare = fit(title, maxW, maxH, 0, width, lineH);
  showAuthor = false;
  if (authorH <= 0) return bare;
  Layout with = fit(title, maxW, maxH, authorH, width, lineH);
  const bool fits = static_cast<int>(with.lines.size()) * (lineH(with.big) - 2) + authorH <= maxH;
  if (!fits) return bare;
  const bool bareBetter = !with.complete && (bare.complete || visibleChars(bare) > visibleChars(with));
  if (bareBetter) return bare;
  showAuthor = true;
  return with;
}

}  // namespace covertitle
