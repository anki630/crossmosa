#pragma once
// 章節快取的「有效字型身分」（P2 設計第 1 節；codex 修訂 2、16）。
//
// 章節檔頭那一格（int，原本就是 fontId）：
//   - 非注音字型：sectionIdentity(fontId, 0) ＝ fontId 原值 → 檔頭逐位元組不變、不重排（不變量 2）；
//   - 注音字型：混合（fontId、engineIdentity）→ 引擎開／關、資料集、語意版號任何一個不同 → 身分不同 → 重排。
// fontId 本身已經雜湊了 cpfont 檔頭（含資料集 ID，zy_pack.py 寫在保留位元組 13–31）；這裡再混一次資料集是多一層保險。

#include <cstdint>

// 章節檔頭那一格是 int（fontId）；混合值用它的 32 位元（codex 複查 ③ 第二輪：把假設固定下來）
static_assert(sizeof(int) == 4, "section identity assumes a 32-bit int");

namespace zhuyin {

// ⭐ 韌體裡任何會改變「畫面上哪個字配哪個讀音」的改動都要加一：解析器、分批與送出規則、透明字元集合、
//   出版社標注的判定、一不串上限、替換位置的算法（UTF-8 解碼）、之後的 TXT 解析範圍與注音直排欄距……
//   電腦端 test/zhuyin_cache 的 SemanticsVersionTracksBehavior 會在輸出改變而版號沒動時變紅。
// 2（v342，B 路線）：bpmfvs 選擇符號照書（VS17 不標、VS18 起第 2、3…個讀音）；預先標注的章節（zhuyin-ivs
// 標記）沒有選擇符號
//   ＝ 第一個讀音。原本任何選擇符號都是「不猜、畫原字」→ 帶選擇符號的書，舊的章節快取要重排。
constexpr uint16_t ZHUYIN_SEMANTICS_VERSION = 2;

enum class EngineMode : uint8_t {
  Off = 1,  // 注音字型、但引擎沒啟用（哨兵、載入失敗、記憶體、建置途中降級）：版面一樣、破音字不標
  On = 2,
};

// 引擎身分：資料集 ＋ 語意版號 ＋ 模式。最低兩個位元就是模式（Off＝1、On＝2）→ 永不為
// 0，同一個資料集的開與關構造上不同。
uint32_t engineIdentity(uint64_t datasetId, EngineMode mode);

// 章節檔頭那一格。zhuyinIdentity == 0 ＝ 非注音 → 原值；否則混合（保證 ≠ fontId、≠
// 0，最低兩個位元沿用引擎身分的模式）。
int sectionIdentity(int fontId, uint32_t zhuyinIdentity);

// 載入章節時，檔頭那一格可不可以用（current／off／on ＝
// 這個字型現在的、引擎沒開時的、引擎開著時的引擎身分；非注音字型三個都是 0）：
//   - 跟現在的身分相同 → 可以（非注音字型只有這一條 → 跟以前逐位元組相同）；
//   - 引擎不在（current ＝ off）而檔案是「開」建的 → 也可以：版面跟「關」完全一樣（同字型、注音格同寬），清單由閘門擋住
//     （沒有可用的引擎就畫原字），引擎回來時同一份檔案的讀音又畫得出來 —— 不必為了引擎進出重排（codex
//     整合複查的建議）；
//   - 引擎在（current ＝ on）而檔案是「關」建的 → 不行：要重排，把注音補上。
bool sectionIdentityAccepted(int fileIdentity, int fontId, uint32_t current, uint32_t off, uint32_t on);

}  // namespace zhuyin
