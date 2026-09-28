#pragma once
// 注音資料區塊（ZYDB v2）的格式常數。
// ⚠️ 格式的唯一來源是工作區的 fonts-src/zhuyin/zy_block.py（產生器＋Python 版解析器，檔頭註解有逐欄位的佈局）。
//    這裡與它逐欄位一致；改任何一邊都要同時改另一邊，並重跑 test/zhuyin_resolver（它拿 Python 匯出的答案逐字比對）。
// 設計：工作區 docs/specs/2026-09-24-zhuyin-polyphone-engine-plan.md（v3）。
//
// CRC-32 與資料集 ID（FNV-1a-64）都涵蓋 [4,16)（版本、旗標、總長、節數、保留）再接 [28,total)；
// 區塊長度必須剛好等於 total（它是 .cpfont 的最後一段）；各節不得重疊。
//
// ⭐ v2 的字形輸出（設計 v3 第 10 節）：0 ＝ 畫原碼位；**破音字一律輸出私用區字形**（它的原碼位字形不帶注音），
//    單音字一律 0。U+E000 + k ＝ 第 k 個破音字（依碼位排序）的預設讀音；替代讀音從 U+E000 + n 接下去。
//    → 舊韌體、引擎沒啟用：破音字不標注音、不會標錯。

#include <cstdint>

namespace zhuyin {

constexpr uint8_t kMagic[4] = {'Z', 'Y', 'D', 'B'};
constexpr uint16_t kVersion = 2;
constexpr uint16_t kFlagSandhi = 1;  // bit0：一、不標變調
constexpr uint16_t kKnownFlags = kFlagSandhi;

enum SectionId : uint16_t {
  kSecPuaMap = 1,      // 載入時驗證，不常駐
  kSecBigram = 2,      // 常駐
  kSecCheckpoint = 3,  // 常駐
  kSecGroups = 4,      // 讀卡
  kSecDefaults = 5,    // 常駐
  kSecRules = 6,       // 常駐
  kSecSandhi = 7,      // 常駐
  kSecTone4 = 8,       // 常駐
  kSecAttrib = 9,      // 不載入
  kSecPoly = 10,       // 常駐：破音字集合（算 rank）
  kSecSelfTest = 11,   // 不常駐：黃金句子＋參考模型的答案，載入後在裝置上跑一次（ZhuyinResolver::selfTest）
};

constexpr uint32_t kHeaderSize = 28;
constexpr uint32_t kSectionEntrySize = 12;
constexpr uint16_t kMaxSections = 16;

constexpr uint32_t kUroFirst = 0x4E00;
constexpr uint32_t kUroLast = 0x9FFF;
constexpr uint32_t kUroBitsBytes = (kUroLast - kUroFirst + 1) / 8;  // 2,624（分兩塊 1,312）
constexpr uint16_t kPuaFirst = 0xE000;
constexpr uint32_t kPolyBlockBits = 256;  // rank 的前綴計數每 256 位元一格（82 格）；1,312 B 的分塊剛好切在格線上

constexpr uint32_t kCheckpointBytes = 512;  // 產生器每累積 ≥ 512 B 的詞組就記一個定位點
constexpr uint8_t kMaxWord = 8;             // 最長詞（碼位數）
// 分批送出：非最後一批時，尾端 kCommitLag 個位置不送出；送出點前面緊鄰的「沒被詞涵蓋的一／不」也不送
// （它們的聲調要等右鄰定案，一串可以任意長）。設計 v3 §3.2。
constexpr uint32_t kCommitLag = 2u * kMaxWord + 2u;

// 規則旗標（每個旗標後面各跟一個字集，依位元順序）
constexpr uint8_t kRuleNextIn = 1;
constexpr uint8_t kRuleNextNotIn = 2;
constexpr uint8_t kRulePrevNotIn = 4;
constexpr uint8_t kRulePrevInOrDoubled = 8;
// P3：不帶字集的鄰字條件（副詞「地」要兩邊都是漢字；句尾語氣詞要後面不是漢字；句首嘆詞要前面不是漢字）。
// 「不是漢字」含沒有鄰字（段首、段尾）。同一邊的兩個位元同時設 ＝ 矛盾 → 載入拒絕。
constexpr uint8_t kRuleNextHan = 16;
constexpr uint8_t kRuleNextNotHan = 32;
constexpr uint8_t kRulePrevHan = 64;
constexpr uint8_t kRulePrevNotHan = 128;
constexpr uint8_t kRuleKnownFlags = 0xFF;

// 漢字（URO、擴充 A、相容區）。其他一律是硬邊界（標點、空白、拉丁字母、數字…）：辭典的詞只含漢字，
// 疊字也必須是漢字。與 zy_block.is_ideograph、zy_model.is_ideograph 相同。
inline bool isIdeograph(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

// 上限（與 zy_block.py 相同）：每一塊 RAM 配置都 ≤ 2 KB
constexpr uint32_t kMaxBlockBytes = 131072;
constexpr uint32_t kKeysPerChunk = 512;    // 雙字索引切塊：每塊 2 KB
constexpr uint32_t kMaxKeys = 4096;        // 8 塊
constexpr uint16_t kMaxCheckpoints = 255;  // × 8 B ≤ 2,040 B
constexpr uint16_t kMaxDefaults = 512;     // × 4 B = 2 KB
constexpr uint8_t kMaxRules = 64;
constexpr uint32_t kMaxRulesBytes = 2048;
constexpr uint32_t kMaxSandhiBytes = 512;
constexpr uint32_t kMaxPua = 6400;       // BMP 私用區 U+E000–F8FF（位元 800 B）
constexpr uint16_t kMaxPolyExtras = 64;  // U+4E00–9FFF 以外的破音字（字型裡有 1 個：䰰）
constexpr uint32_t kMaxSelfTestBytes = 2048;
constexpr uint16_t kMaxSelfTestLen = 64;    // 每句最多 64 字（跑的時候用堆疊緩衝）
constexpr uint16_t kMinSelfTestCases = 16;  // 自我測試至少 16 句、至少一句比 kCommitLag 長（codex P1.5 複查 5）
constexpr uint16_t kMaxToneExtras = 64;     // U+4E00–9FFF 以外、第四聲的單音字（TONE4 節尾端的清單）
constexpr uint8_t kMaxAltCount = 15;        // 每個破音字最多 15 個替代讀音（歸屬表每字 4 位元）
constexpr uint8_t kPendingExact = 32;       // 分批接續：前 32 個沒送出的碼位逐字比對，其餘看 64 位元雜湊
constexpr uint32_t kMaxGroupBytes = 1536;
constexpr uint32_t kMaxWindowBytes = kCheckpointBytes + kMaxGroupBytes;  // 2 KB

}  // namespace zhuyin
