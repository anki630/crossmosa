#include "LanguageRegistry.h"

#include <algorithm>
#include <array>

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"

// 繁中自訂版的斷字表狀態(v122)：
//   v11  砍掉 9 種非英文語言(de/ru/sv/uk/pl/es/fr/it/fi，約 323KB)—— 為了騰 flash 給中文字集。
//   v27  連英文 trie(約 27KB)也砍了，理由是「使用者只讀中文」。
//   v122 把英文加回來 —— 社群回報有人用這個韌體讀英文書，而 flash 還有約 590KB 餘裕。
//        非英文的 9 種【維持移除】：它們合計 323KB，而且沒有實際使用者需求；
//        generated/ 底下的 trie 原始檔全部留著，要哪一種就把 include 與 entry 加回來即可。
// 中文不受影響：CJK 不斷字(斷字只作用在拉丁字母上，見 isLatinLetter)。

namespace {
// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);

using EntryArray = std::array<LanguageEntry, 1>;

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator}}};
  return kEntries;
}

}  // namespace

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(),
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  return (it != allEntries.end()) ? it->hyphenator : nullptr;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
