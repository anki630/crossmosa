#pragma once
// 注音資料區塊（ZYDB）的載入與查詢。純邏輯、不碰 Arduino／SD，電腦上可以編譯測試（test/zhuyin_resolver）。
//
// ⭐ 載入時全部驗過才啟用：檔頭（含總長與節數）、CRC、內容雜湊（資料集 ID）、節不重疊、每一節的邊界／筆數／排序、
//    PUAMAP 與 PUA 數量一致、POLY 與 PUAMAP 的基字逐一相符、整個詞組區逐字掃過一次，
//    每個輸出都要「屬於它那個字」（v2：破音字 → 自己的預設讀音或替代讀音範圍內；單音字 → 0）。
//    任何一項不過 → 回報原因、狀態清空、不啟用。
// ⭐ 沒有載入成功時，所有查詢一律「關著」（找不到、沒有規則、不是第四聲）—— 不會讀到半套狀態。
// ⭐ 自己不配記憶體：全部向呼叫端給的 Arena 要，每塊 ≤ 2 KB（kMax* 保證），配不到就 NoMemory。
//    常駐約 14 KB；詞組放在卡上，每次查詢最多讀「定位點 → 該組結尾」一段（≤ 2 KB，實測 538 B），單格快取。
//
// 使用前提（呼叫端負責）：
//   - BlockSource 的內容在 ZhuyinData 的整個生命週期內不變，而且比它活得久。
//   - Arena 比它活得久；載入失敗或重新載入時，整個 Arena 由呼叫端丟掉（只配不放，沒有回收）。
//   - 不可複製（窗口快取不能共用）。

#include <cstddef>
#include <cstdint>

#include "ZhuyinFormat.h"

namespace zhuyin {

// 區塊的讀取來源。offset 相對於區塊開頭，size() 就是區塊的長度（必須剛好等於檔頭的 total）。
class BlockSource {
 public:
  virtual ~BlockSource() = default;
  virtual uint32_t size() const = 0;
  virtual bool read(uint32_t offset, void* dst, uint32_t len) = 0;
};

// 只配不放的配置器。回傳的位址必須對齊 align；配不到回 nullptr（不可 abort）。
class Arena {
 public:
  virtual ~Arena() = default;
  virtual void* alloc(size_t bytes, size_t align) = 0;
};

enum class LoadStatus : uint8_t {
  Ok,
  TooSmall,
  BadMagic,
  BadVersion,
  BadFlags,
  BadReserved,
  BadLength,
  BadSectionTable,
  MissingSection,
  BadCrc,
  BadDatasetId,
  BadPuaMap,
  BadBigram,
  BadCheckpoint,
  BadGroups,
  BadDefaults,
  BadRules,
  BadSandhi,
  BadTone,
  BadPoly,
  BadSelfTest,
  NoMemory,
  ReadError,
};
const char* loadStatusName(LoadStatus s);

class ZhuyinResolver;

class ZhuyinData {
 public:
  ZhuyinData() = default;
  ZhuyinData(const ZhuyinData&) = delete;
  ZhuyinData& operator=(const ZhuyinData&) = delete;

  // 只做結構載入（全部驗過）。⭐ 載入成功還【不能】查詢：要再過 ZhuyinResolver::selfTest（或直接用
  // ZhuyinResolver::open ＝ 載入＋自我測試）才會變成可用；自我測試沒過 → 狀態清空（codex P1.5 複查 4）。
  LoadStatus load(BlockSource& src, Arena& arena);
  // 清空（回到沒載入）。⚠️ 放掉 arena 之前一定先叫：資料的指標都指進 arena，放掉之後還顯示「已載入」＝ 懸空（codex v339 第三輪）。
  void unload() { clearState(); }
  bool loaded() const { return state_ == State::Verified; }  // ＝ 可用（已通過自我測試）
  // 狀態每改變一次（清空、結構載入、通過自我測試）就加一。引擎登記處記下登記當時的序號：
  // 之後不同 ＝ 資料就地換過（連「重新載入成功」也算）→ 不可用，直到重新登記（codex 複查 ③ F5）。
  uint32_t stateSerial() const { return stateSerial_; }
  bool structurallyLoaded() const { return state_ != State::Unloaded; }

  uint64_t datasetId() const { return datasetId_; }
  uint16_t flags() const { return flags_; }
  uint16_t puaCount() const { return puaCount_; }
  uint32_t keyCount() const { return keyCount_; }
  uint32_t maxGroupBytes() const { return maxGroupBytes_; }
  uint32_t windowBytes() const { return windowBytes_; }
  uint32_t reads() const { return reads_; }

  // 雙字索引：key ＝ (第一字 << 16) | 第二字
  bool findKey(uint32_t key, uint32_t* index) const;

  // 在第 index 組裡找「從 cps[0] 開始、最長的那個詞」（cps[0..1] 就是 key），輸出複製到 outs[0..*len)。
  // 回傳 false ＝ 沒載入、讀卡失敗或讀到的內容不一致（輸出超出範圍也算）；true 且 *len==0 ＝ 沒有詞符合。
  bool matchLongest(uint32_t index, const uint32_t* cps, size_t avail, uint8_t* len, uint16_t (&outs)[kMaxWord]);

  uint16_t defaultOut(uint32_t cp) const;

  // 規則：依表的順序，第一條符合的勝出。has* ＝ 那個位置有沒有字。
  bool applyRules(uint32_t cp, bool hasPrev, uint32_t prev, bool hasPrev2, uint32_t prev2, bool hasNext,
                  uint32_t next, uint16_t* out) const;

  uint16_t yi1() const { return yi1_; }
  uint16_t yi2() const { return yi2_; }
  uint16_t yi4() const { return yi4_; }
  uint16_t bu4() const { return bu4_; }
  uint16_t bu2() const { return bu2_; }
  bool yiPrevPlain(uint32_t cp) const;
  bool yiNextPlain(uint32_t cp) const;

  // 變調用的「第四聲」：out≠0 看 PUA 的位元，out==0 看基字（U+4E00–9FFF）的位元，其他一律 false。
  bool tone4(uint32_t cp, uint16_t out) const;

  // v2：破音字（原碼位字形不帶注音）。沒載入一律 false。
  bool isPolyphonic(uint32_t cp) const;
  // v2 不變量：這個輸出屬於這個字嗎？（單音字 → 0；破音字 → 自己的預設讀音或自己的替代讀音範圍）。沒載入一律 false。
  bool outputOwned(uint32_t cp, uint16_t out) const;
  // 破音字的預設讀音字形 ＝ U+E000 + rank(cp)（rank ＝ 比它小的破音字個數）；不是破音字或沒載入 → 0。
  uint16_t polyDefault(uint32_t cp) const;
  // v342（B 路線，bpmfvs 選擇符號）：第 k 個讀音的字形。k == 0 → 預設讀音（＝ polyDefault）；k ≥ 1 → 第 k 個替代讀音
  //   （替代讀音的順序 ＝ 字型的讀音表，與 bpmfvs 的 VS18、VS19… 一致）；沒有這個讀音、不是破音字或沒載入 → 0。
  uint16_t variantOut(uint32_t cp, uint32_t k) const;
  uint16_t polyCount() const { return polyCount_; }

  // 自我測試節：逐句讀出（碼位、參考模型的答案），交給 fn；fn 回 false 就停。讀卡失敗或沒載入回 false。
  // 由 ZhuyinResolver::selfTest 使用（它負責解析與比對）。不配記憶體。
  // 堆疊（-fstack-usage，riscv32 -Os 實測框大小）：這一層 736 B＋回呼 608＋feed 80＋run 144＋讀卡 → 整條約 1.9 KB。
  using SelfTestFn = bool (*)(void* ctx, const uint32_t* cps, const uint16_t* expected, size_t n);
  bool forEachSelfTest(SelfTestFn fn, void* ctx);
  uint16_t selfTestCount() const { return selfTestCount_; }

 private:
  friend class ZhuyinResolver;  // 自我測試要在「結構已載入、還沒驗證」時查詢，過了才把狀態切成 Verified
  enum class State : uint8_t { Unloaded, Structural, Verified };
  bool usable() const { return state_ == State::Verified || (state_ == State::Structural && selfTesting_); }
  State state_ = State::Unloaded;
  uint32_t stateSerial_ = 0;
  void setState(State s) {
    state_ = s;
    stateSerial_++;
  }
  bool selfTesting_ = false;

 private:
  struct Section {
    uint32_t offset = 0;
    uint32_t length = 0;
    bool present = false;
  };
  struct RuleRef {
    uint16_t target;
    uint16_t out;
    uint8_t flags;
    uint16_t setOff[4];  // 相對於 rules_ 的位移（指向第一個值）
    uint8_t setLen[4];
  };

  void clearState();
  LoadStatus loadImpl(BlockSource& src, Arena& arena);
  LoadStatus checkPuaMap(BlockSource& src, Arena& arena);
  LoadStatus loadBigram(BlockSource& src, Arena& arena);
  LoadStatus loadCheckpoints(BlockSource& src, Arena& arena);
  LoadStatus scanGroups(BlockSource& src);
  LoadStatus loadDefaults(BlockSource& src, Arena& arena);
  LoadStatus loadRules(BlockSource& src, Arena& arena);
  LoadStatus loadSandhi(BlockSource& src, Arena& arena);
  LoadStatus loadTone(BlockSource& src, Arena& arena);
  LoadStatus loadPoly(BlockSource& src, Arena& arena);
  LoadStatus checkSelfTest(BlockSource& src);
  LoadStatus loadWindowCrcs(BlockSource& src, Arena& arena);
  uint32_t altStart(uint32_t rank) const;  // 第 rank 個破音字的第一個替代讀音
  bool polyBit(uint32_t cp) const;       // 不看 loaded_（載入途中要用）
  uint32_t polyRank(uint32_t cp) const;  // 同上
  uint8_t polyByte(uint32_t i) const { return polyBits_[i / kUroChunkBytes][i % kUroChunkBytes]; }
  bool ownedOut(uint32_t cp, uint16_t out) const;
  static uint8_t* allocBytes(Arena& arena, size_t bytes, size_t align = 1);
  bool validOut(uint16_t out) const;
  uint32_t keyAt(uint32_t i) const;
  static bool inSet(const uint8_t* values, uint8_t len, uint32_t cp);

  BlockSource* src_ = nullptr;
  uint16_t flags_ = 0;
  uint64_t datasetId_ = 0;
  Section sec_[kMaxSections + 1];

  static constexpr uint32_t kMaxKeyChunks = kMaxKeys / kKeysPerChunk;
  uint32_t keyCount_ = 0;
  const uint8_t* keyChunks_[kMaxKeyChunks] = {};

  uint16_t cpCount_ = 0;
  const uint8_t* cps_ = nullptr;  // cpCount_ × (keyIndex u16, pad u16, offset u32)

  uint16_t defaultCount_ = 0;
  const uint8_t* defaults_ = nullptr;  // (base u16, out u16)

  uint8_t ruleCount_ = 0;
  const uint8_t* rules_ = nullptr;
  const RuleRef* ruleRefs_ = nullptr;

  uint16_t yi1_ = 0, yi2_ = 0, yi4_ = 0, bu4_ = 0, bu2_ = 0;
  const uint8_t* sandhi_ = nullptr;
  uint16_t yiPrevOff_ = 0, yiNextOff_ = 0;
  uint8_t yiPrevLen_ = 0, yiNextLen_ = 0;

  static constexpr uint32_t kUroChunkBytes = kUroBitsBytes / 2;
  const uint8_t* uroBits_[2] = {};
  uint16_t puaCount_ = 0;
  const uint8_t* puaBits_ = nullptr;

  const uint8_t* polyBits_[2] = {};       // 2 × 1,312 B：bit i ＝ U+4E00+i 是破音字
  const uint8_t* polyPrefix_ = nullptr;   // 82 × u16（每 256 位元之前的破音字個數），載入時算
  const uint8_t* polyExtras_ = nullptr;   // U+4E00–9FFF 以外的破音字，u16 遞增
  const uint8_t* altCounts_ = nullptr;    // 每個破音字的替代讀音數（4 位元一個，依 rank），載入時從 PUAMAP 算
  const uint8_t* altPrefix_ = nullptr;    // 每 32 個破音字之前的替代讀音總數（u16）
  const uint8_t* windowCrc_ = nullptr;    // 每個定位點的讀卡窗口 CRC-32（載入時算）：讀卡後比對，卡在載入之後變了就不輸出
  const uint8_t* toneExtras_ = nullptr;   // U+4E00–9FFF 以外、第四聲的單音字（u16 遞增）
  uint16_t toneExtraCount_ = 0;
  uint16_t polyExtraCount_ = 0;
  uint16_t polyUroTotal_ = 0;
  uint16_t polyCount_ = 0;

  uint16_t selfTestCount_ = 0;

  uint32_t maxGroupBytes_ = 0;
  uint32_t windowBytes_ = 0;
  uint8_t* window_ = nullptr;
  uint32_t windowCp_ = UINT32_MAX;  // 目前窗口是哪一個定位點讀進來的
  uint32_t windowLen_ = 0;
  uint32_t reads_ = 0;
};

}  // namespace zhuyin
