#include "ZhuyinData.h"

#include <cstring>
#include <initializer_list>
#include <new>

namespace zhuyin {

namespace {

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t rd64(const uint8_t* p) {
  return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32);
}
inline uint32_t popcount8(uint8_t v) { return static_cast<uint32_t>(__builtin_popcount(v)); }
constexpr uint32_t kPolyBlocks = (kUroLast - kUroFirst + 1) / kPolyBlockBits;  // 82
constexpr uint32_t kPolyBlockBytes = kPolyBlockBits / 8;                       // 32
// 每一塊 RAM 配置 ≤ 2 KB（上限情況；目前的資料約 18 KB／21 塊）
static_assert(kMaxCheckpoints * 8u <= 2048u && kMaxCheckpoints * 4u <= 2048u, "checkpoint tables");
static_assert(kMaxDefaults * 4u <= 2048u && kKeysPerChunk * 4u <= 2048u, "defaults / key chunks");
static_assert((kMaxPua / 2 + 1) / 2 <= 2048u && 2u * ((kMaxPua / 2 + 31) / 32) <= 2048u, "ownership tables");
static_assert(kMaxWindowBytes <= 2048u && kUroBitsBytes / 2 <= 2048u && kMaxToneExtras * 2u <= 2048u, "windows / bits");

// zlib 相容的 CRC-32（反射多項式 0xEDB88320），半位元組查表：16 格表、每位元組兩次查表。可以分段累加。
constexpr uint32_t nibbleEntry(uint32_t i) {
  uint32_t c = i;
  for (int k = 0; k < 4; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  return c;
}
constexpr uint32_t kCrcNibble[16] = {nibbleEntry(0),  nibbleEntry(1),  nibbleEntry(2),  nibbleEntry(3),
                                     nibbleEntry(4),  nibbleEntry(5),  nibbleEntry(6),  nibbleEntry(7),
                                     nibbleEntry(8),  nibbleEntry(9),  nibbleEntry(10), nibbleEntry(11),
                                     nibbleEntry(12), nibbleEntry(13), nibbleEntry(14), nibbleEntry(15)};
uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
  uint32_t c = ~crc;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    c = (c >> 4) ^ kCrcNibble[c & 15u];
    c = (c >> 4) ^ kCrcNibble[c & 15u];
  }
  return ~c;
}

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;
uint64_t fnvUpdate(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= kFnvPrime;
  }
  return h;
}

// 驗證掃描用的循序讀取器：256 B 的堆疊緩衝，逐段向來源要資料。
class SeqReader {
 public:
  SeqReader(BlockSource& src, uint32_t start, uint32_t length) : src_(src), start_(start), length_(length) {}
  uint32_t pos() const { return pos_; }
  bool failed() const { return failed_; }
  bool u8(uint8_t* v) {
    if (!ensure(1)) return false;
    *v = buf_[pos_ - bufStart_];
    pos_ += 1;
    return true;
  }
  bool u16(uint16_t* v) {
    uint8_t a, b;
    if (!u8(&a) || !u8(&b)) return false;
    *v = static_cast<uint16_t>(a | (b << 8));
    return true;
  }
  bool skip(uint32_t n) {
    if (n > length_ - pos_) return false;
    pos_ += n;
    return true;
  }

 private:
  bool ensure(uint32_t n) {
    if (n > length_ - pos_) return false;
    if (pos_ >= bufStart_ && pos_ + n <= bufStart_ + bufLen_) return true;
    const uint32_t want = (length_ - pos_) < sizeof(buf_) ? (length_ - pos_) : static_cast<uint32_t>(sizeof(buf_));
    if (!src_.read(start_ + pos_, buf_, want)) {
      failed_ = true;
      return false;
    }
    bufStart_ = pos_;
    bufLen_ = want;
    return true;
  }
  BlockSource& src_;
  uint32_t start_, length_;
  uint32_t pos_ = 0;
  uint8_t buf_[256] = {};
  uint32_t bufStart_ = 0, bufLen_ = 0;
  bool failed_ = false;
};

inline LoadStatus scanFail(const SeqReader& r, LoadStatus structural) {
  return r.failed() ? LoadStatus::ReadError : structural;
}

}  // namespace

const char* loadStatusName(LoadStatus s) {
  switch (s) {
    case LoadStatus::Ok:
      return "ok";
    case LoadStatus::TooSmall:
      return "too-small";
    case LoadStatus::BadMagic:
      return "magic";
    case LoadStatus::BadVersion:
      return "version";
    case LoadStatus::BadFlags:
      return "flags";
    case LoadStatus::BadReserved:
      return "reserved";
    case LoadStatus::BadLength:
      return "length";
    case LoadStatus::BadSectionTable:
      return "section-table";
    case LoadStatus::MissingSection:
      return "missing-section";
    case LoadStatus::BadCrc:
      return "crc";
    case LoadStatus::BadDatasetId:
      return "dataset-id";
    case LoadStatus::BadPuaMap:
      return "puamap";
    case LoadStatus::BadBigram:
      return "bigram";
    case LoadStatus::BadCheckpoint:
      return "checkpoint";
    case LoadStatus::BadGroups:
      return "groups";
    case LoadStatus::BadDefaults:
      return "defaults";
    case LoadStatus::BadRules:
      return "rules";
    case LoadStatus::BadSandhi:
      return "sandhi";
    case LoadStatus::BadTone:
      return "tone";
    case LoadStatus::BadPoly:
      return "poly";
    case LoadStatus::BadSelfTest:
      return "selftest";
    case LoadStatus::NoMemory:
      return "no-memory";
    case LoadStatus::ReadError:
      return "read";
  }
  return "?";
}

void ZhuyinData::clearState() {
  setState(State::Unloaded);
  selfTesting_ = false;
  src_ = nullptr;
  flags_ = 0;
  datasetId_ = 0;
  for (auto& s : sec_) s = Section{};
  keyCount_ = 0;
  for (auto& k : keyChunks_) k = nullptr;
  cpCount_ = 0;
  cps_ = nullptr;
  defaultCount_ = 0;
  defaults_ = nullptr;
  ruleCount_ = 0;
  rules_ = nullptr;
  ruleRefs_ = nullptr;
  yi1_ = yi2_ = yi4_ = bu4_ = bu2_ = 0;
  sandhi_ = nullptr;
  yiPrevOff_ = yiNextOff_ = 0;
  yiPrevLen_ = yiNextLen_ = 0;
  uroBits_[0] = uroBits_[1] = nullptr;
  puaCount_ = 0;
  puaBits_ = nullptr;
  polyBits_[0] = polyBits_[1] = nullptr;
  polyPrefix_ = nullptr;
  polyExtras_ = nullptr;
  polyExtraCount_ = polyUroTotal_ = polyCount_ = 0;
  altCounts_ = altPrefix_ = windowCrc_ = toneExtras_ = nullptr;
  toneExtraCount_ = 0;
  selfTestCount_ = 0;
  maxGroupBytes_ = 0;
  windowBytes_ = 0;
  window_ = nullptr;
  windowCp_ = UINT32_MAX;
  windowLen_ = 0;
  reads_ = 0;
}

LoadStatus ZhuyinData::load(BlockSource& src, Arena& arena) {
  clearState();
  const LoadStatus s = loadImpl(src, arena);
  if (s != LoadStatus::Ok) {
    clearState();  // 失敗就全部清空：不留半套 count／指標給查詢讀
    return s;
  }
  setState(State::Structural);  // 還不能查詢：要過自我測試（ZhuyinResolver::selfTest／open）
  return s;
}

uint8_t* ZhuyinData::allocBytes(Arena& arena, size_t bytes, size_t align) {
  auto* p = static_cast<uint8_t*>(arena.alloc(bytes ? bytes : 1, align));
  if (p && (reinterpret_cast<uintptr_t>(p) % align) != 0) return nullptr;  // 違反對齊的配置器當成配不到
  return p;
}

LoadStatus ZhuyinData::loadImpl(BlockSource& src, Arena& arena) {
  const uint32_t size = src.size();
  if (size < kHeaderSize) return LoadStatus::TooSmall;
  uint8_t h[kHeaderSize];
  if (!src.read(0, h, kHeaderSize)) return LoadStatus::ReadError;
  if (std::memcmp(h, kMagic, 4) != 0) return LoadStatus::BadMagic;
  const uint16_t version = rd16(h + 4);
  const uint16_t flags = rd16(h + 6);
  const uint32_t total = rd32(h + 8);
  const uint16_t count = rd16(h + 12);
  if (version != kVersion) return LoadStatus::BadVersion;
  if (flags != kFlagSandhi) return LoadStatus::BadFlags;  // 產品定案：一、不一律標變調（不是開關），別的旗標都不認得
  if (rd16(h + 14) != 0) return LoadStatus::BadReserved;
  const uint32_t tableEnd = kHeaderSize + kSectionEntrySize * count;
  if (count == 0 || count > kMaxSections || total != size || total > kMaxBlockBytes || total < tableEnd) {
    return LoadStatus::BadLength;
  }

  // CRC 管「壞了沒」、資料集 ID 管「是不是同一份內容」：兩者都涵蓋 [4,16) 與 [28,total)。
  {
    uint32_t crc = crc32Update(0, h + 4, 12);
    uint64_t fnv = fnvUpdate(kFnvOffset, h + 4, 12);
    uint8_t buf[256];
    for (uint32_t off = kHeaderSize; off < total;) {
      const uint32_t n = (total - off) < sizeof(buf) ? (total - off) : static_cast<uint32_t>(sizeof(buf));
      if (!src.read(off, buf, n)) return LoadStatus::ReadError;
      crc = crc32Update(crc, buf, n);
      fnv = fnvUpdate(fnv, buf, n);
      off += n;
    }
    if (crc != rd32(h + 24)) return LoadStatus::BadCrc;
    if (fnv != rd64(h + 16)) return LoadStatus::BadDatasetId;
  }
  flags_ = flags;
  datasetId_ = rd64(h + 16);
  src_ = &src;

  {
    uint8_t t[kMaxSections * kSectionEntrySize];
    if (!src.read(kHeaderSize, t, kSectionEntrySize * count)) return LoadStatus::ReadError;
    uint32_t starts[kMaxSections], ends[kMaxSections];
    for (uint16_t i = 0; i < count; i++) {
      const uint8_t* e = t + kSectionEntrySize * i;
      const uint16_t id = rd16(e);
      const uint32_t off = rd32(e + 4);
      const uint32_t len = rd32(e + 8);
      if (rd16(e + 2) != 0 || off < tableEnd || off > total || len > total - off) return LoadStatus::BadSectionTable;
      // 依（起點, 終點）插入排序，之後檢查不重疊（連不認得的節也算）。排序鍵與 zy_block.py 相同：
      // 零長度的節落在別節開頭時，不論節表順序都收（只依起點排的話，收不收會看節表的順序）
      uint16_t j = i;
      while (j > 0 && (starts[j - 1] > off || (starts[j - 1] == off && ends[j - 1] > off + len))) {
        starts[j] = starts[j - 1];
        ends[j] = ends[j - 1];
        j--;
      }
      starts[j] = off;
      ends[j] = off + len;
      if (id == 0 || id > kMaxSections) continue;  // 不認得的節：向前相容，照樣在 CRC 範圍內
      if (sec_[id].present) return LoadStatus::BadSectionTable;
      sec_[id] = Section{off, len, true};
    }
    for (uint16_t i = 1; i < count; i++) {
      if (ends[i - 1] > starts[i]) return LoadStatus::BadSectionTable;
    }
  }
  for (uint16_t id : {kSecPuaMap, kSecBigram, kSecCheckpoint, kSecGroups, kSecDefaults, kSecRules, kSecSandhi,
                      kSecTone4, kSecPoly, kSecSelfTest}) {
    if (!sec_[id].present) return LoadStatus::MissingSection;
  }

  LoadStatus s;
  if ((s = loadTone(src, arena)) != LoadStatus::Ok) return s;  // 先載：其他節要用 PUA 數量檢查輸出
  if ((s = loadPoly(src, arena)) != LoadStatus::Ok) return s;  // 再來破音字集合：輸出的歸屬要看它
  if ((s = checkPuaMap(src, arena)) != LoadStatus::Ok) return s;
  if ((s = loadBigram(src, arena)) != LoadStatus::Ok) return s;
  if ((s = loadCheckpoints(src, arena)) != LoadStatus::Ok) return s;
  if ((s = scanGroups(src)) != LoadStatus::Ok) return s;
  if ((s = loadDefaults(src, arena)) != LoadStatus::Ok) return s;
  if ((s = loadRules(src, arena)) != LoadStatus::Ok) return s;
  if ((s = loadSandhi(src, arena)) != LoadStatus::Ok) return s;
  if ((s = checkSelfTest(src)) != LoadStatus::Ok) return s;
  window_ = allocBytes(arena, windowBytes_);
  if (!window_) return LoadStatus::NoMemory;
  if ((s = loadWindowCrcs(src, arena)) != LoadStatus::Ok) return s;
  return LoadStatus::Ok;
}

bool ZhuyinData::validOut(uint16_t out) const {
  return out == 0 || (out >= kPuaFirst && static_cast<uint32_t>(out - kPuaFirst) < puaCount_);
}

// v2 不變量：單音字 → 0；破音字 → 自己的預設讀音（U+E000 + rank）或自己的替代讀音範圍 [altStart, altStart + count)。
// 歸屬表（每字 4 位元的替代讀音數＋每 32 字一格的前綴）載入時從 PUAMAP 算，約 1.2 KB（codex P1.5 複查 2：
// 原本替代讀音只查範圍 → 壞資料可以讓「一」畫成「不」的字形）。
bool ZhuyinData::ownedOut(uint32_t cp, uint16_t out) const {
  if (!validOut(out)) return false;
  if (!polyBit(cp)) return out == 0;
  if (out == 0) return false;
  const uint32_t r = polyRank(cp);
  if (out == kPuaFirst + r) return true;
  const uint32_t first = altStart(r);
  const uint32_t cnt = (altCounts_[r >> 1] >> ((r & 1) * 4)) & 0xFu;
  return out >= first && out < first + cnt;
}

uint32_t ZhuyinData::altStart(uint32_t rank) const {
  uint32_t acc = rd16(altPrefix_ + 2 * (rank / 32));
  for (uint32_t i = rank & ~31u; i < rank; i++) acc += (altCounts_[i >> 1] >> ((i & 1) * 4)) & 0xFu;
  return kPuaFirst + static_cast<uint32_t>(polyCount_) + acc;
}

bool ZhuyinData::outputOwned(uint32_t cp, uint16_t out) const { return usable() && ownedOut(cp, out); }

LoadStatus ZhuyinData::loadPoly(BlockSource& src, Arena& arena) {
  const Section& s = sec_[kSecPoly];
  if (s.length < kUroBitsBytes + 4) return LoadStatus::BadPoly;
  uint8_t head[4];
  if (!src.read(s.offset + kUroBitsBytes, head, 4)) return LoadStatus::ReadError;
  const uint16_t ne = rd16(head);
  if (rd16(head + 2) != 0 || ne > kMaxPolyExtras || s.length != kUroBitsBytes + 4 + 2u * ne) return LoadStatus::BadPoly;
  for (int c = 0; c < 2; c++) {
    uint8_t* p = allocBytes(arena, kUroChunkBytes);
    if (!p) return LoadStatus::NoMemory;
    if (!src.read(s.offset + kUroChunkBytes * c, p, kUroChunkBytes)) return LoadStatus::ReadError;
    polyBits_[c] = p;
  }
  if (ne) {
    uint8_t* p = allocBytes(arena, 2u * ne);
    if (!p) return LoadStatus::NoMemory;
    if (!src.read(s.offset + kUroBitsBytes + 4, p, 2u * ne)) return LoadStatus::ReadError;
    for (uint16_t i = 0; i < ne; i++) {
      const uint16_t e = rd16(p + 2 * i);
      // 額外清單只能是 URO 以外的漢字（擴充 A、相容區）：標點、拉丁字母、私用區都不行（codex P1.5 複查 8）
      if (!isIdeograph(e) || (e >= kUroFirst && e <= kUroLast) || (i > 0 && rd16(p + 2 * (i - 1)) >= e)) {
        return LoadStatus::BadPoly;
      }
    }
    polyExtras_ = p;
  }
  polyExtraCount_ = ne;
  uint8_t* pre = allocBytes(arena, 2u * kPolyBlocks);
  if (!pre) return LoadStatus::NoMemory;
  uint32_t acc = 0;
  for (uint32_t b = 0; b < kPolyBlocks; b++) {
    pre[2 * b] = static_cast<uint8_t>(acc & 0xFFu);
    pre[2 * b + 1] = static_cast<uint8_t>(acc >> 8);
    for (uint32_t j = 0; j < kPolyBlockBytes; j++) acc += popcount8(polyByte(b * kPolyBlockBytes + j));
  }
  polyPrefix_ = pre;
  polyUroTotal_ = static_cast<uint16_t>(acc);    // ≤ 20,992
  polyCount_ = static_cast<uint16_t>(acc + ne);  // checkPuaMap 逐一核對
  return LoadStatus::Ok;
}

bool ZhuyinData::polyBit(uint32_t cp) const {
  if (cp >= kUroFirst && cp <= kUroLast) {
    const uint32_t i = cp - kUroFirst;
    return (polyByte(i >> 3) >> (i & 7)) & 1u;
  }
  uint32_t lo = 0, hi = polyExtraCount_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint16_t v = rd16(polyExtras_ + 2 * mid);
    if (v < cp) {
      lo = mid + 1;
    } else if (v > cp) {
      hi = mid;
    } else {
      return true;
    }
  }
  return false;
}

uint32_t ZhuyinData::polyRank(uint32_t cp) const {
  uint32_t lo = 0, hi = polyExtraCount_;  // 額外破音字中比 cp 小的個數
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (rd16(polyExtras_ + 2 * mid) < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (cp < kUroFirst) return lo;
  if (cp > kUroLast) return lo + polyUroTotal_;
  const uint32_t i = cp - kUroFirst;
  const uint32_t b = i / kPolyBlockBits;
  uint32_t cnt = rd16(polyPrefix_ + 2 * b);
  for (uint32_t byte = b * kPolyBlockBytes; byte < (i >> 3); byte++) cnt += popcount8(polyByte(byte));
  cnt += popcount8(static_cast<uint8_t>(polyByte(i >> 3) & ((1u << (i & 7)) - 1u)));
  return lo + cnt;
}

bool ZhuyinData::isPolyphonic(uint32_t cp) const { return usable() && polyBit(cp); }

// 自我測試節只驗結構與歸屬（答案本身對不對，要等解析器跑過才知道 —— 那是 ZhuyinResolver::selfTest 的事）
LoadStatus ZhuyinData::checkSelfTest(BlockSource& src) {
  const Section& s = sec_[kSecSelfTest];
  if (s.length < 4 || s.length > kMaxSelfTestBytes) return LoadStatus::BadSelfTest;
  SeqReader r(src, s.offset, s.length);
  uint16_t count, pad;
  if (!r.u16(&count) || !r.u16(&pad)) return scanFail(r, LoadStatus::BadSelfTest);
  if (pad != 0 || count < kMinSelfTestCases) return LoadStatus::BadSelfTest;
  bool longCase = false;  // 至少一句比送出延遲長（否則分批完全沒被測到）
  for (uint16_t c = 0; c < count; c++) {
    uint16_t n;
    if (!r.u16(&n)) return scanFail(r, LoadStatus::BadSelfTest);
    if (n == 0 || n > kMaxSelfTestLen) return LoadStatus::BadSelfTest;
    if (n > kCommitLag) longCase = true;
    uint16_t cps[kMaxSelfTestLen];
    for (uint16_t i = 0; i < n; i++) {
      if (!r.u16(&cps[i])) return scanFail(r, LoadStatus::BadSelfTest);
    }
    for (uint16_t i = 0; i < n; i++) {
      uint16_t out;
      if (!r.u16(&out)) return scanFail(r, LoadStatus::BadSelfTest);
      if (!ownedOut(cps[i], out)) return LoadStatus::BadSelfTest;
    }
  }
  if (r.pos() != s.length || !longCase) return LoadStatus::BadSelfTest;
  selfTestCount_ = count;
  return LoadStatus::Ok;
}

bool ZhuyinData::forEachSelfTest(SelfTestFn fn, void* ctx) {
  if (!usable() || !fn) return false;
  const Section& s = sec_[kSecSelfTest];
  SeqReader r(*src_, s.offset, s.length);
  uint16_t count, pad;
  if (!r.u16(&count) || !r.u16(&pad) || pad != 0 || count != selfTestCount_) return false;
  for (uint16_t c = 0; c < count; c++) {
    uint16_t n;
    if (!r.u16(&n) || n == 0 || n > kMaxSelfTestLen) return false;
    uint32_t cps[kMaxSelfTestLen];
    uint16_t expected[kMaxSelfTestLen];
    for (uint16_t i = 0; i < n; i++) {
      uint16_t v;
      if (!r.u16(&v)) return false;
      cps[i] = v;
    }
    for (uint16_t i = 0; i < n; i++) {
      if (!r.u16(&expected[i])) return false;
    }
    if (!fn(ctx, cps, expected, n)) return false;
  }
  return r.pos() == s.length;  // 第二次讀也要剛好讀完（卡在兩次讀之間變了 → 不算通過）
}

uint16_t ZhuyinData::polyDefault(uint32_t cp) const {
  if (!usable() || !polyBit(cp)) return 0;
  return static_cast<uint16_t>(kPuaFirst + polyRank(cp));
}

uint16_t ZhuyinData::variantOut(uint32_t cp, uint32_t k) const {
  if (!usable() || !polyBit(cp)) return 0;
  const uint32_t r = polyRank(cp);
  if (k == 0) return static_cast<uint16_t>(kPuaFirst + r);
  const uint32_t cnt = (altCounts_[r >> 1] >> ((r & 1) * 4)) & 0xFu;  // 同 ownedOut 的歸屬表
  if (k > cnt) return 0;
  return static_cast<uint16_t>(altStart(r) + (k - 1));
}

LoadStatus ZhuyinData::loadTone(BlockSource& src, Arena& arena) {
  const Section& s = sec_[kSecTone4];
  if (s.length < kUroBitsBytes + 4) return LoadStatus::BadTone;
  uint8_t head[4];
  if (!src.read(s.offset + kUroBitsBytes, head, 4)) return LoadStatus::ReadError;
  puaCount_ = rd16(head);
  if (rd16(head + 2) != 0 || puaCount_ > kMaxPua) return LoadStatus::BadTone;
  const uint32_t puaBytes = (static_cast<uint32_t>(puaCount_) + 7) / 8;
  // 尾端：URO 以外、第四聲的單音字清單（count u16、pad u16、count × u16 遞增）（codex P1.5 複查 9）
  const uint32_t extraAt = kUroBitsBytes + 4 + puaBytes;
  if (s.length < extraAt + 4) return LoadStatus::BadTone;
  uint8_t eh[4];
  if (!src.read(s.offset + extraAt, eh, 4)) return LoadStatus::ReadError;
  const uint16_t ne = rd16(eh);
  if (rd16(eh + 2) != 0 || ne > kMaxToneExtras || s.length != extraAt + 4 + 2u * ne) return LoadStatus::BadTone;
  for (int c = 0; c < 2; c++) {
    uint8_t* p = allocBytes(arena, kUroChunkBytes);
    if (!p) return LoadStatus::NoMemory;
    if (!src.read(s.offset + kUroChunkBytes * c, p, kUroChunkBytes)) return LoadStatus::ReadError;
    uroBits_[c] = p;
  }
  if (puaBytes) {
    uint8_t* p = allocBytes(arena, puaBytes);
    if (!p) return LoadStatus::NoMemory;
    if (!src.read(s.offset + kUroBitsBytes + 4, p, puaBytes)) return LoadStatus::ReadError;
    puaBits_ = p;
  }
  if (ne) {
    uint8_t* p = allocBytes(arena, 2u * ne);
    if (!p) return LoadStatus::NoMemory;
    if (!src.read(s.offset + extraAt + 4, p, 2u * ne)) return LoadStatus::ReadError;
    for (uint16_t i = 0; i < ne; i++) {
      const uint16_t e = rd16(p + 2 * i);
      if (!isIdeograph(e) || (e >= kUroFirst && e <= kUroLast) || (i > 0 && rd16(p + 2 * (i - 1)) >= e)) {
        return LoadStatus::BadTone;
      }
    }
    toneExtras_ = p;
  }
  toneExtraCount_ = ne;
  return LoadStatus::Ok;
}

// PUAMAP 不常駐，但要驗：n 個破音字、基字遞增；第 k 個的預設讀音是 U+E000 + k（隱含），
// 替代讀音從 U+E000 + n 起連續配、總數剛好等於 PUA 數量；而且 POLY 描述的正是這些基字、順序相同（rank(第 k 個) ＝ k）。
LoadStatus ZhuyinData::checkPuaMap(BlockSource& src, Arena& arena) {
  const Section& s = sec_[kSecPuaMap];
  SeqReader r(src, s.offset, s.length);
  uint16_t n;
  if (!r.u16(&n)) return scanFail(r, LoadStatus::BadPuaMap);
  if (s.length != 2u + 6u * n) return LoadStatus::BadPuaMap;
  if (n != polyCount_) return LoadStatus::BadPoly;
  // 歸屬表：每字 4 位元的替代讀音數（依 rank）＋每 32 字一格的前綴。n ≤ kMaxPua / 2 → ≤ 1,600 B、≤ 200 B
  uint8_t* counts = allocBytes(arena, (static_cast<uint32_t>(n) + 1) / 2);
  uint8_t* prefix = allocBytes(arena, 2u * ((static_cast<uint32_t>(n) + 31) / 32));
  if (!counts || !prefix) return LoadStatus::NoMemory;
  std::memset(counts, 0, (static_cast<uint32_t>(n) + 1) / 2);
  uint32_t altTotal = 0;
  uint32_t next = kPuaFirst + static_cast<uint32_t>(n);
  int32_t lastBase = -1;
  for (uint16_t i = 0; i < n; i++) {
    uint16_t base, first;
    uint8_t count, pad;
    if (!r.u16(&base) || !r.u16(&first) || !r.u8(&count) || !r.u8(&pad)) return scanFail(r, LoadStatus::BadPuaMap);
    if (pad != 0 || count == 0 || count > kMaxAltCount || !isIdeograph(base) ||
        static_cast<int32_t>(base) <= lastBase || first != next) {
      return LoadStatus::BadPuaMap;
    }
    if (!polyBit(base) || polyRank(base) != i) return LoadStatus::BadPoly;
    if ((i & 31u) == 0) {
      prefix[2 * (i / 32)] = static_cast<uint8_t>(altTotal & 0xFFu);
      prefix[2 * (i / 32) + 1] = static_cast<uint8_t>(altTotal >> 8);
    }
    counts[i >> 1] = static_cast<uint8_t>(counts[i >> 1] | (count << ((i & 1) * 4)));
    altTotal += count;
    lastBase = base;
    next = static_cast<uint32_t>(first) + count;
  }
  altCounts_ = counts;
  altPrefix_ = prefix;
  if (next != kPuaFirst + static_cast<uint32_t>(puaCount_)) return LoadStatus::BadPuaMap;
  return LoadStatus::Ok;
}

LoadStatus ZhuyinData::loadBigram(BlockSource& src, Arena& arena) {
  const Section& s = sec_[kSecBigram];
  if (s.length < 4) return LoadStatus::BadBigram;
  uint8_t head[4];
  if (!src.read(s.offset, head, 4)) return LoadStatus::ReadError;
  const uint32_t n = rd32(head);
  if (n > kMaxKeys || s.length != 4 + 4 * n) return LoadStatus::BadBigram;
  keyCount_ = n;
  for (uint32_t c = 0; c * kKeysPerChunk < n; c++) {
    const uint32_t cnt = (n - c * kKeysPerChunk) < kKeysPerChunk ? (n - c * kKeysPerChunk) : kKeysPerChunk;
    uint8_t* p = allocBytes(arena, 4 * cnt);
    if (!p) return LoadStatus::NoMemory;
    if (!src.read(s.offset + 4 + 4 * kKeysPerChunk * c, p, 4 * cnt)) return LoadStatus::ReadError;
    keyChunks_[c] = p;
  }
  for (uint32_t i = 1; i < n; i++) {
    if (keyAt(i - 1) >= keyAt(i)) return LoadStatus::BadBigram;
  }
  return LoadStatus::Ok;
}

uint32_t ZhuyinData::keyAt(uint32_t i) const { return rd32(keyChunks_[i / kKeysPerChunk] + 4 * (i % kKeysPerChunk)); }

LoadStatus ZhuyinData::loadCheckpoints(BlockSource& src, Arena& arena) {
  const Section& s = sec_[kSecCheckpoint];
  if (s.length < 2) return LoadStatus::BadCheckpoint;
  uint8_t head[2];
  if (!src.read(s.offset, head, 2)) return LoadStatus::ReadError;
  const uint16_t m = rd16(head);
  if (m > kMaxCheckpoints || s.length != 2u + 8u * m) return LoadStatus::BadCheckpoint;
  if ((keyCount_ > 0) != (m > 0)) return LoadStatus::BadCheckpoint;
  cpCount_ = m;
  if (!m) return LoadStatus::Ok;
  uint8_t* p = allocBytes(arena, 8u * m);
  if (!p) return LoadStatus::NoMemory;
  if (!src.read(s.offset + 2, p, 8u * m)) return LoadStatus::ReadError;
  cps_ = p;
  const uint32_t groupsLen = sec_[kSecGroups].length;
  for (uint16_t i = 0; i < m; i++) {
    const uint16_t ki = rd16(p + 8 * i);
    const uint32_t off = rd32(p + 8 * i + 4);
    if (rd16(p + 8 * i + 2) != 0 || ki >= keyCount_ || off >= groupsLen) return LoadStatus::BadCheckpoint;
    if (i == 0 && (ki != 0 || off != 0)) return LoadStatus::BadCheckpoint;
    if (i > 0 && (ki <= rd16(p + 8 * (i - 1)) || off <= rd32(p + 8 * (i - 1) + 4))) return LoadStatus::BadCheckpoint;
  }
  return LoadStatus::Ok;
}

// 整個詞組區逐字掃過一次：組數＝鍵數、每個詞 2–8 字且由長到短、輸出都在範圍內、定位點剛好落在組的開頭，
// 並算出「從定位點讀到任一組結尾」最多要讀多少 → 查詢時的窗口大小（≤ kMaxWindowBytes）。
LoadStatus ZhuyinData::scanGroups(BlockSource& src) {
  const Section& s = sec_[kSecGroups];
  SeqReader r(src, s.offset, s.length);
  uint32_t cpCur = 0, cpNext = 0;
  for (uint32_t g = 0; g < keyCount_; g++) {
    if (cpNext < cpCount_ && rd16(cps_ + 8 * cpNext) == g) {
      if (rd32(cps_ + 8 * cpNext + 4) != r.pos()) return LoadStatus::BadGroups;
      cpCur = cpNext++;
    }
    const uint32_t start = r.pos();
    uint8_t wc;
    if (!r.u8(&wc)) return scanFail(r, LoadStatus::BadGroups);
    if (wc == 0) return LoadStatus::BadGroups;
    uint8_t prevLen = kMaxWord;
    const uint32_t key = keyAt(g);
    // 辭典的詞只含漢字：硬邊界（標點之後重新開始解析＝整段解析）靠的就是這個（codex P1.5 複查 10）
    if (!isIdeograph(key >> 16) || !isIdeograph(key & 0xFFFFu)) return LoadStatus::BadGroups;
    uint32_t prevTail[kMaxWord] = {};
    for (uint8_t w = 0; w < wc; w++) {
      uint8_t len;
      if (!r.u8(&len)) return scanFail(r, LoadStatus::BadGroups);
      if (len < 2 || len > prevLen) return LoadStatus::BadGroups;
      const bool sameLen = (w > 0 && len == prevLen);
      prevLen = len;
      uint32_t chars[kMaxWord] = {key >> 16, key & 0xFFFFu};
      for (uint8_t t = 2; t < len; t++) {
        uint16_t c;
        if (!r.u16(&c)) return scanFail(r, LoadStatus::BadGroups);
        if (!isIdeograph(c)) return LoadStatus::BadGroups;
        chars[t] = c;
      }
      // 同長度的詞依碼位嚴格遞增（重複的詞會帶不同輸出、第一筆勝出 → 不收）
      if (sameLen) {
        int cmp = 0;
        for (uint8_t t = 2; t < len && cmp == 0; t++)
          cmp = chars[t] < prevTail[t] ? -1 : (chars[t] > prevTail[t] ? 1 : 0);
        if (cmp <= 0) return LoadStatus::BadGroups;
      }
      for (uint8_t t = 2; t < len; t++) prevTail[t] = chars[t];
      for (uint8_t j = 0; j < len; j++) {
        uint16_t out;
        if (!r.u16(&out)) return scanFail(r, LoadStatus::BadGroups);
        if (!ownedOut(chars[j], out)) return LoadStatus::BadGroups;
      }
    }
    const uint32_t gsize = r.pos() - start;
    if (gsize > kMaxGroupBytes) return LoadStatus::BadGroups;
    if (gsize > maxGroupBytes_) maxGroupBytes_ = gsize;
    const uint32_t span = r.pos() - rd32(cps_ + 8 * cpCur + 4);
    if (span > windowBytes_) windowBytes_ = span;
  }
  if (r.pos() != s.length || cpNext != cpCount_) return LoadStatus::BadGroups;
  if (windowBytes_ > kMaxWindowBytes) return LoadStatus::BadGroups;
  return LoadStatus::Ok;
}

LoadStatus ZhuyinData::loadDefaults(BlockSource& src, Arena& arena) {
  const Section& s = sec_[kSecDefaults];
  if (s.length < 2) return LoadStatus::BadDefaults;
  uint8_t head[2];
  if (!src.read(s.offset, head, 2)) return LoadStatus::ReadError;
  const uint16_t d = rd16(head);
  if (d > kMaxDefaults || s.length != 2u + 4u * d) return LoadStatus::BadDefaults;
  defaultCount_ = d;
  if (!d) return LoadStatus::Ok;
  uint8_t* p = allocBytes(arena, 4u * d);
  if (!p) return LoadStatus::NoMemory;
  if (!src.read(s.offset + 2, p, 4u * d)) return LoadStatus::ReadError;
  defaults_ = p;
  for (uint16_t i = 0; i < d; i++) {
    const uint16_t out = rd16(p + 4 * i + 2);
    if (out == 0 || !ownedOut(rd16(p + 4 * i), out)) return LoadStatus::BadDefaults;
    if (i > 0 && rd16(p + 4 * (i - 1)) >= rd16(p + 4 * i)) return LoadStatus::BadDefaults;
  }
  return LoadStatus::Ok;
}

LoadStatus ZhuyinData::loadRules(BlockSource& src, Arena& arena) {
  const Section& s = sec_[kSecRules];
  if (s.length < 1 || s.length > kMaxRulesBytes) return LoadStatus::BadRules;
  uint8_t* p = allocBytes(arena, s.length);
  if (!p) return LoadStatus::NoMemory;
  if (!src.read(s.offset, p, s.length)) return LoadStatus::ReadError;
  rules_ = p;
  const uint8_t n = p[0];
  if (n > kMaxRules) return LoadStatus::BadRules;
  RuleRef* refs = nullptr;
  if (n) {
    static_assert(sizeof(RuleRef) * kMaxRules <= 2048u, "rule table must fit one 2 KB block");
    uint8_t* mem = allocBytes(arena, sizeof(RuleRef) * n, alignof(RuleRef));
    if (!mem) return LoadStatus::NoMemory;
    // 逐個建構（陣列版 placement new 允許實作多吃前置空間，不能用）
    for (uint8_t i = 0; i < n; i++) new (mem + sizeof(RuleRef) * i) RuleRef{};
    refs = std::launder(reinterpret_cast<RuleRef*>(mem));
  }
  uint32_t pos = 1;
  for (uint8_t i = 0; i < n; i++) {
    if (s.length - pos < 6) return LoadStatus::BadRules;
    RuleRef r{};
    r.target = rd16(p + pos);
    r.out = rd16(p + pos + 2);
    r.flags = p[pos + 4];
    constexpr uint8_t kBothNext = kRuleNextHan | kRuleNextNotHan;
    constexpr uint8_t kBothPrev = kRulePrevHan | kRulePrevNotHan;
    if (p[pos + 5] != 0 || (r.flags & ~kRuleKnownFlags) || (r.flags & kBothNext) == kBothNext ||
        (r.flags & kBothPrev) == kBothPrev || !ownedOut(r.target, r.out)) {
      return LoadStatus::BadRules;
    }
    pos += 6;
    for (int b = 0; b < 4; b++) {
      if (!(r.flags & (1u << b))) continue;
      if (s.length - pos < 2) return LoadStatus::BadRules;
      const uint8_t len = p[pos];
      if (p[pos + 1] != 0 || s.length - pos - 2 < 2u * len) return LoadStatus::BadRules;
      for (uint8_t k = 0; k < len; k++) {
        const uint16_t v = rd16(p + pos + 2 + 2 * k);
        if (!isIdeograph(v) || (k > 0 && rd16(p + pos + 2 + 2 * (k - 1)) >= v)) return LoadStatus::BadRules;
      }
      r.setOff[b] = static_cast<uint16_t>(pos + 2);
      r.setLen[b] = len;
      pos += 2u + 2u * len;
    }
    refs[i] = r;
  }
  if (pos != s.length) return LoadStatus::BadRules;
  ruleCount_ = n;
  ruleRefs_ = refs;
  return LoadStatus::Ok;
}

LoadStatus ZhuyinData::loadSandhi(BlockSource& src, Arena& arena) {
  const Section& s = sec_[kSecSandhi];
  if (s.length < 14 || s.length > kMaxSandhiBytes) return LoadStatus::BadSandhi;
  uint8_t* p = allocBytes(arena, s.length);
  if (!p) return LoadStatus::NoMemory;
  if (!src.read(s.offset, p, s.length)) return LoadStatus::ReadError;
  sandhi_ = p;
  yi1_ = rd16(p);
  yi2_ = rd16(p + 2);
  yi4_ = rd16(p + 4);
  bu4_ = rd16(p + 6);
  bu2_ = rd16(p + 8);
  for (uint16_t o : {yi1_, yi2_, yi4_}) {
    if (!ownedOut(0x4E00, o)) return LoadStatus::BadSandhi;  // 一
  }
  for (uint16_t o : {bu4_, bu2_}) {
    if (!ownedOut(0x4E0D, o)) return LoadStatus::BadSandhi;  // 不
  }
  uint32_t pos = 10;
  for (int k = 0; k < 2; k++) {
    if (s.length - pos < 2) return LoadStatus::BadSandhi;
    const uint8_t len = p[pos];
    if (p[pos + 1] != 0 || s.length - pos - 2 < 2u * len) return LoadStatus::BadSandhi;
    for (uint8_t i = 0; i < len; i++) {
      const uint16_t v = rd16(p + pos + 2 + 2 * i);
      if (!isIdeograph(v) || (i > 0 && rd16(p + pos + 2 + 2 * (i - 1)) >= v)) return LoadStatus::BadSandhi;
    }
    if (k == 0) {
      yiPrevOff_ = static_cast<uint16_t>(pos + 2);
      yiPrevLen_ = len;
    } else {
      yiNextOff_ = static_cast<uint16_t>(pos + 2);
      yiNextLen_ = len;
    }
    pos += 2u + 2u * len;
  }
  if (pos != s.length) return LoadStatus::BadSandhi;
  return LoadStatus::Ok;
}

// 每個定位點的讀卡窗口：載入時讀一次、算 CRC-32 常駐（≤ 255 × 4 B）；查詢讀卡後比對（codex P1.5 複查 3：
// 詞組在卡上、查詢時才讀 → 載入後卡上內容變了，查詢會拿到「範圍內但不是它的」輸出。有了 CRC，變了就是查不到）。
LoadStatus ZhuyinData::loadWindowCrcs(BlockSource& src, Arena& arena) {
  if (!cpCount_) return LoadStatus::Ok;
  uint8_t* crcs = allocBytes(arena, 4u * cpCount_);
  if (!crcs) return LoadStatus::NoMemory;
  const Section& s = sec_[kSecGroups];
  for (uint16_t i = 0; i < cpCount_; i++) {
    const uint32_t off = rd32(cps_ + 8 * i + 4);
    const uint32_t n = (s.length - off) < windowBytes_ ? (s.length - off) : windowBytes_;
    if (!src.read(s.offset + off, window_, n)) return LoadStatus::ReadError;
    const uint32_t c = crc32Update(0, window_, n);
    crcs[4 * i] = static_cast<uint8_t>(c);
    crcs[4 * i + 1] = static_cast<uint8_t>(c >> 8);
    crcs[4 * i + 2] = static_cast<uint8_t>(c >> 16);
    crcs[4 * i + 3] = static_cast<uint8_t>(c >> 24);
  }
  windowCrc_ = crcs;
  return LoadStatus::Ok;
}

bool ZhuyinData::findKey(uint32_t key, uint32_t* index) const {
  if (!usable() || !index) return false;
  uint32_t lo = 0, hi = keyCount_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint32_t k = keyAt(mid);
    if (k < key) {
      lo = mid + 1;
    } else if (k > key) {
      hi = mid;
    } else {
      *index = mid;
      return true;
    }
  }
  return false;
}

bool ZhuyinData::matchLongest(uint32_t index, const uint32_t* cps, size_t avail, uint8_t* len,
                              uint16_t (&outs)[kMaxWord]) {
  if (len) *len = 0;
  if (!usable() || !len || !cps || index >= keyCount_ || avail < 2) return false;
  // 公開介面：index 必須就是 cps 前兩字的鑰匙（codex P1.5 複查 14）
  if (cps[0] > 0xFFFFu || cps[1] > 0xFFFFu || keyAt(index) != ((cps[0] << 16) | cps[1])) return false;
  // 最後一個 keyIndex ≤ index 的定位點
  uint32_t lo = 0, hi = cpCount_;
  while (hi - lo > 1) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (rd16(cps_ + 8 * mid) <= index) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const uint32_t cp = lo;
  const uint32_t cpOff = rd32(cps_ + 8 * cp + 4);
  const Section& s = sec_[kSecGroups];
  if (windowCp_ != cp) {
    const uint32_t n = (s.length - cpOff) < windowBytes_ ? (s.length - cpOff) : windowBytes_;
    windowCp_ = UINT32_MAX;
    if (!src_->read(s.offset + cpOff, window_, n)) return false;
    if (crc32Update(0, window_, n) != rd32(windowCrc_ + 4 * cp)) return false;  // 卡在載入之後變了：查不到，不輸出
    windowCp_ = cp;
    windowLen_ = n;
    reads_++;
  }
  // 查詢時照樣逐一檢查（卡在載入之後壞掉也不越界、不輸出範圍外的字形）
  uint32_t pos = 0;
  for (uint32_t g = rd16(cps_ + 8 * cp); g <= index; g++) {
    if (pos >= windowLen_) return false;
    const uint8_t wc = window_[pos++];
    for (uint8_t w = 0; w < wc; w++) {
      if (pos >= windowLen_) return false;
      const uint8_t wl = window_[pos];
      if (wl < 2 || wl > kMaxWord) return false;
      const uint32_t need = 1u + 2u * (wl - 2u) + 2u * wl;
      if (need > windowLen_ - pos) return false;
      if (g == index && wl <= avail) {
        bool same = true;
        for (uint8_t t = 0; t + 2 < wl && same; t++) same = (rd16(window_ + pos + 1 + 2 * t) == cps[2 + t]);
        if (same) {
          const uint8_t* o = window_ + pos + 1 + 2u * (wl - 2u);
          for (uint8_t j = 0; j < wl; j++) {
            outs[j] = rd16(o + 2 * j);
            if (!validOut(outs[j])) return false;
          }
          *len = wl;
          return true;  // 詞依長度由長到短排：第一個符合的就是最長的
        }
      }
      pos += need;
    }
  }
  return true;
}

uint16_t ZhuyinData::defaultOut(uint32_t cp) const {
  if (!usable()) return 0;
  uint32_t lo = 0, hi = defaultCount_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint16_t b = rd16(defaults_ + 4 * mid);
    if (b < cp) {
      lo = mid + 1;
    } else if (b > cp) {
      hi = mid;
    } else {
      return rd16(defaults_ + 4 * mid + 2);
    }
  }
  return 0;
}

bool ZhuyinData::inSet(const uint8_t* values, uint8_t len, uint32_t cp) {
  if (!values) return false;
  uint32_t lo = 0, hi = len;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint16_t v = rd16(values + 2 * mid);
    if (v < cp) {
      lo = mid + 1;
    } else if (v > cp) {
      hi = mid;
    } else {
      return true;
    }
  }
  return false;
}

bool ZhuyinData::applyRules(uint32_t cp, bool hasPrev, uint32_t prev, bool hasPrev2, uint32_t prev2, bool hasNext,
                            uint32_t next, uint16_t* out) const {
  if (!usable() || !out) return false;
  for (uint8_t i = 0; i < ruleCount_; i++) {
    const RuleRef& r = ruleRefs_[i];
    if (r.target != cp) continue;
    bool ok = true;
    if (r.flags & kRuleNextIn) ok = hasNext && inSet(rules_ + r.setOff[0], r.setLen[0], next);
    if (ok && (r.flags & kRuleNextNotIn)) ok = hasNext && !inSet(rules_ + r.setOff[1], r.setLen[1], next);
    if (ok && (r.flags & kRulePrevNotIn)) ok = !(hasPrev && inSet(rules_ + r.setOff[2], r.setLen[2], prev));
    if (ok && (r.flags & kRulePrevInOrDoubled)) {
      const bool doubled = hasPrev && hasPrev2 && prev == prev2 && isIdeograph(prev);  // 疊字必須是漢字（「……地」不算）
      ok = doubled || (hasPrev && inSet(rules_ + r.setOff[3], r.setLen[3], prev));
    }
    const bool nextHan = hasNext && isIdeograph(next);
    const bool prevHan = hasPrev && isIdeograph(prev);
    if (ok && (r.flags & kRuleNextHan)) ok = nextHan;
    if (ok && (r.flags & kRuleNextNotHan)) ok = !nextHan;
    if (ok && (r.flags & kRulePrevHan)) ok = prevHan;
    if (ok && (r.flags & kRulePrevNotHan)) ok = !prevHan;
    if (ok) {
      *out = r.out;
      return true;
    }
  }
  return false;
}

bool ZhuyinData::yiPrevPlain(uint32_t cp) const { return usable() && inSet(sandhi_ + yiPrevOff_, yiPrevLen_, cp); }
bool ZhuyinData::yiNextPlain(uint32_t cp) const { return usable() && inSet(sandhi_ + yiNextOff_, yiNextLen_, cp); }

bool ZhuyinData::tone4(uint32_t cp, uint16_t out) const {
  if (!usable()) return false;
  if (out) {
    if (out < kPuaFirst) return false;
    const uint32_t i = static_cast<uint32_t>(out) - kPuaFirst;
    if (i >= puaCount_ || !puaBits_) return false;
    return (puaBits_[i >> 3] >> (i & 7)) & 1u;
  }
  if (cp < kUroFirst || cp > kUroLast) {
    uint32_t lo = 0, hi = toneExtraCount_;  // URO 以外的單音字
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      const uint16_t v = rd16(toneExtras_ + 2 * mid);
      if (v < cp) {
        lo = mid + 1;
      } else if (v > cp) {
        hi = mid;
      } else {
        return true;
      }
    }
    return false;
  }
  const uint32_t i = cp - kUroFirst;
  const uint32_t byte = i >> 3;
  return (uroBits_[byte / kUroChunkBytes][byte % kUroChunkBytes] >> (i & 7)) & 1u;
}

}  // namespace zhuyin
