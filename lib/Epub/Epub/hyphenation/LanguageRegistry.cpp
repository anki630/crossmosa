#include "LanguageRegistry.h"

#include "HyphenationCommon.h"

// 繁中自訂版：斷字表已全部移除。v11 先砍 9 種非英文語言（~323KB），v27 連英文 trie（~27KB）
// 也移除——使用者只讀中文（不斷字），英文內文退回不斷字，斷字設定項已一併自 SettingsList 拿掉。
// 要還原：加回 generated/hyph-en.trie.h 的 include、englishHyphenator 物件與 entries 表，
// 並把 SettingsList.h 的 STR_HYPHENATION Toggle 加回。

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  (void)primaryTag;
  return nullptr;
}

LanguageEntryView getLanguageEntries() { return LanguageEntryView{nullptr, 0}; }
