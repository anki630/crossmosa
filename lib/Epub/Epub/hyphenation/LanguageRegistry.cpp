#include "LanguageRegistry.h"

#include <algorithm>
#include <array>

#include "HyphenationCommon.h"
#include "generated/hyph-en.trie.h"

namespace {

// CrossMosa L4：只保留英文斷字。中文不斷字，其餘語言的 trie 合計約 323 KB
// （最大的 de 一個就 206,259 bytes），而我們的 flash 要留給漢字。
// 英文那份【要留】——v122 因社群回報加回來的。
//
// ⚠️ 這裡原本是 `std::array<LanguageEntry, 10>`，大小【硬寫】。若只刪初始化列表的 9 筆
//    而忘了改 10：編譯完全通過（std::array 初始化列表不足會 value-initialize 補齊），
//    尾端 9 筆全是 {nullptr, nullptr, nullptr}。接著下面的
//    `primaryTag == entry.primaryTag` 就是 std::string == (const char*)nullptr → strlen(nullptr)。
//    而 Section.cpp:407 每次章節重排都無條件呼叫 setPreferredLanguage(epub->getLanguage())，
//    中文書的 "zh" 匹配不到任何一筆 → find_if 掃到尾端 nullptr → 【開任何一本中文書必當機】，
//    且與「斷字有沒有開」無關。
//    → 改用 CTAD 推導大小，讓它在結構上不可能與內容脫鉤。不要改回寫死的數字。
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);

const auto& entries() {
  static const std::array kEntries = {LanguageEntry{"english", "en", &englishHyphenator}};
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
