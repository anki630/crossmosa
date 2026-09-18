#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <Arduino.h>
#include "SdCardFont.h"

#include <Breadcrumb.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <memory>

#include "EpdFontFamily.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

namespace {

// FNV-1a hash for content-based font ID generation
constexpr uint32_t FNV_OFFSET = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
// CPFONT_VERSION is defined as a #define in SdCardFont.h so it can be
// stringified into FONT_MANIFEST_URL.
constexpr uint32_t HEADER_SIZE = 32;
constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;

// Helper to read little-endian values from byte buffer
inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

// Walks a null-terminated UTF-8 string and appends each unique codepoint to
// codepoints[0..cpCount-1] via O(n²) dedup.  Returns true if the buffer
// reached maxCount (cap hit), false if all codepoints fit.
bool collectUniqueCodepoints(const char* text, uint32_t* codepoints, uint32_t& cpCount, uint32_t maxCount,
                             bool (*cpFilter)(uint32_t) = nullptr) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (cpFilter && !cpFilter(cp)) continue;  // v262：呼叫端不需要的碼位（直排的漢字）不進表
    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (cpCount >= maxCount) return true;
      codepoints[cpCount++] = cp;
    }
  }
  return false;
}

const char* asCStr(const std::string& s) { return s.c_str(); }
const char* asCStr(const char* s) { return s; }

// resetStyleMiniData retention bounds (see the PerStyle comment in the header).
constexpr size_t MINI_RETAIN_MIN_FREE_HEAP = 40 * 1024;
// v188：留不留也要看【最大連續塊】（CLAUDE.md 硬限制 2：殺手是連續塊不是 free 總量）。
// diag187_2 的四次「記憶體不足」：總 free 48KB（>40KB → 留）而最大連續塊只剩 28KB。
// 但這個量是在 mini 還駐留時量的（自我參照）：門檻設 32KB 會在三分之一的頁觸發、變成每頁配丟的
// 碎片機（驗證者用 diag187_2 的 pmax 算過）。16KB（＝背景建置的地板）只在堆真的出事時觸發（~6% 頁）；
// 建置視窗本身由 releaseMiniData() 明確處理，這裡只是最後防線。每次 clearCache 只量一次。
constexpr size_t MINI_RETAIN_MIN_MAXBLOCK = 16 * 1024;
constexpr uint8_t MINI_UNDERUSE_RUNS_BEFORE_FREE = 3;

// Keep-if-fits buffer reuse: only reallocate when the needed size exceeds the
// current capacity. Freeing + reallocating slightly different sizes every page
// turn punches non-coalescing holes in the heap (the freed block rarely fits the
// next page's need), eroding the largest contiguous block all session. With
// reuse, capacities converge on the book's max page after a few turns and page
// turns stop touching the allocator. Only three small instantiations exist
// (interval/glyph/byte arrays), so template bloat is negligible.
// v142：把上游那個 keep-if-fits 補完 —— 它擋掉了「縮小重配」，但【沒擋掉放大重配】。
//
// 實機量到的殘留（v141，只讀書、無網路、讀約 20 頁）：
//    33800 build-start  p2 f=115,616 max=115,616
//   120552 build-start  p2 f= 71,244 max= 27,476     <- 87 秒內最大連續塊蝕掉 88,140
//   [92283] Failed to allocate mini bitmap (33422 bytes)
//   [95791] Failed to allocate mini bitmap (38071 bytes)
//   [99810] Failed to allocate mini bitmap (31569 bytes)
// 字圖要 31–38KB 連續，而 p2 最大只剩 27,476 -> 配不到 -> 字型降級 -> 最後 abort。
//
// 為什麼配【剛好夠】會這樣：每一頁的用字不同，所需大小逐頁小幅變動。配 exact size 時，
// 只要某一頁比歷史高水位大一點就重配一次；而剛釋放的舊塊【永遠差一點裝不下】新的，
// 於是配置器往前找新位置，舊塊變成洞。上游的註解自己描述了這個機制
// （"punches non-coalescing holes… eroding the largest contiguous block all session"），
// 只是 keep-if-fits 只解掉一半。
//
// 修法：成長時【向上取整到 8KB 級距】。容量因此在兩三次之內收斂到「這本書最大的一頁」，
// 之後翻頁完全不碰配置器。溢配上限是 8KB（有界），換掉的是無界的碎片化。
//   實測序列 30,000 -> 32,768 / 33,422 -> 40,960 / 38,071 命中 => 只重配兩次。
// 這與 v55/v138 的 advance table 同一個形狀（一次配到位、之後就地重用），那個已實機驗證。
//
// ⚠️ 級距用【位元組】換算成元素數，因為呼叫端的 needed 有的是位元組（miniBitmap）、
//    有的是元素數（miniIntervals / miniGlyphs / kern classes）。
// ⚠️ CapT 可能是 uint16_t（kern class 計數）—— 夾限，否則截斷後 capacity 會記成比實際
//    配置【小】的值，下一次判斷就錯，反而變成每頁都重配。
// ⚠️ miniBitmap【不走這裡】（v244 起走 growMiniBitmap）：這裡是先 delete 再 new，
//    配不到時舊的已經沒了 —— 對幾百位元組的陣列無所謂，對 30–45KB 的 bitmap 會讓降級階梯每頁重切一塊。
template <typename T, typename CapT>
bool ensureArrayCapacity(T*& buf, CapT& capacity, const uint32_t needed) {
  if (buf && capacity >= needed) return true;

  constexpr uint32_t kGrowGranularityBytes = 8 * 1024;
  constexpr uint32_t kStep =
      (kGrowGranularityBytes / sizeof(T)) > 0 ? static_cast<uint32_t>(kGrowGranularityBytes / sizeof(T)) : 1u;
  constexpr uint32_t kCapMax = static_cast<uint32_t>(static_cast<CapT>(-1));

  // ⚠️ 只有【配置本身 ≥ 8KB】才取整。小陣列（kern class 上限 256 個位元組）取整會變成
  //    為了 256 bytes 配 8KB，兩個就浪費 16KB —— 而小配置本來就不是碎片化的來源。
  //    這一條是桌面對拍抓到的：第一版無條件取整，kern 的容量直接跳到 8,192。
  uint32_t grown = needed;
  const bool worthRounding = static_cast<uint64_t>(needed) * sizeof(T) >= kGrowGranularityBytes;
  if (worthRounding && needed <= kCapMax - (kStep - 1)) {  // 取整不會溢位才做
    grown = ((needed + kStep - 1) / kStep) * kStep;
  }
  if (grown > kCapMax) grown = kCapMax;
  if (grown < needed) return false;               // needed 本身就超過 CapT 能表示：誠實失敗

  delete[] buf;
  buf = new (std::nothrow) T[grown > 0 ? grown : 1];
  capacity = buf ? static_cast<CapT>(grown) : 0;
  return buf != nullptr;
}

// v244：mini-bitmap 專用的成長（取代 ensureArrayCapacity ＋ v243 的階梯配置）。
//
// v243 實機（diag243.log，EPUB NotoSerifTC 22pt）量到的病：取整放不下時，階梯在最大塊裡切一塊「剛好」的，
// 留下 ≥4KB 的尾巴；下一頁 prewarm 之前才載入的排版物件（小配置）正好坐在尾巴裡，buffer 放掉也合併不回來
// → 最大塊每頁降 4KB（34,804 → 30,708 → 26,612 → 22,516），連續 18 頁降級。
//
// ⭐ 大小選「TLSF 區間邊界 − 12」（IDF 5.5.2 tlsf.c／multi_heap_poisoning.c 讀碼）：malloc(n) 在池內實際要 n＋12
//   （CONFIG_HEAP_POISONING_LIGHT），mapping_search 把它【往上】取到區間邊界才去找；放掉的洞則歸在【往下】取整的那一格。
//   n＋12 在邊界上時兩者同格，放掉再要回同樣大小一定找得到那個洞；不在邊界上時要看洞有沒有碰巧併到旁邊的碎片。
//   `heap_caps_get_largest_free_block()` 回報的值（tlsf_fit_size − 12）天生就是這種大小；8KB 級距也是邊界
//   （≤64KB 的區間寬 ≤2,048，都整除 8,192），所以取整成 8KB − 12。
//   ⚠️ 12 綁著 sdkconfig 的 poisoning 等級；換掉只會失去這個性質，不影響正確性。
//
// 形狀：先放掉舊的（前後的空塊都能合併進來 —— realloc 只能往後長，codex 複查指出那樣會比 v242 更常降級），
//   取整配得到就照舊；配不到就把【當下最大的一塊整塊】吃下（它包含剛放掉、合併過的區域，所以通常 ≥ 原容量）。
// 不留保留量（codex 兩輪複查＋桌機重播後定案）：
//   - 第一版「吃完別處剩 <16KB 就原地退 4KB」會【製造】它要防的東西：退出去的尾巴緊貼 buffer，下一次成長時正好被
//     下一頁的排版物件坐住，放掉 buffer 也拿不回來 → 容量永久少 4KB，重播裡 9 次／2.2KB 變 14 次／4.9KB。
//   - 真的見底時已有兩道既有防線：下一頁 clearCache 的 heap-tight 地板（最大塊 <16KB 就整個放掉 mini）；
//     miss ring 的字圖配置有事前檢查＋nothrow（v167），配不到只是那個字不畫，不會 abort。
//   - v243 實機 21 筆，吃完之後別處都還有 19,444–25,588（post=，本版照印，繼續盯）。
//
// ✅ 桌機重播（工作區 tools/tlsf-minibitmap-sim：IDF 5.5.2 的 tlsf.c 原檔 ＋ poisoning 模型 ＋ 每頁
//    「clearCache 清 miss ring → 載入下一頁排版物件 → prewarm → 放掉排版物件 → 被丟的字進 ring」的順序）：
//    v243 策略逐位元組重現實機階梯（34,804 → 30,708 → 26,612 → 22,516 → 跳回）；這個策略在四種情境
//    （有無小碎片 × v243 的 18 頁失敗序列／疏密交錯 72 頁）都不下降，失敗頁 12–35 → 9、每頁平均丟字 5.5–13.4KB → 2.2KB。
//    ⚠️ 重播用的是【自己抄的一份策略】，不是編這個檔（它在匿名命名空間、帶 Arduino 相依）—— 改這裡要同步改那份。
//
// 回傳值只給證人用；成敗看 buf／capacity 與 needed 的關係。
constexpr uint32_t kPoisonOverhead = 12;

struct MiniBitmapGrowth {
  bool grabbed = false;   // 取整配不到，改吃下當下最大的一塊
  size_t largest = 0;     // 吃之前的最大塊（已含剛放掉的舊 buffer）
};

MiniBitmapGrowth growMiniBitmap(uint8_t*& buf, uint32_t& capacity, const uint32_t needed) {
  MiniBitmapGrowth g;
  if (buf && capacity >= needed) return g;

  constexpr uint32_t kStep = 8 * 1024;
  uint32_t want = needed > 0 ? needed : 1;
  if (want >= kStep - kPoisonOverhead && want <= UINT32_MAX - (kPoisonOverhead + kStep - 1)) {  // 溢位防呆（codex）
    want = ((want + kPoisonOverhead + kStep - 1) / kStep) * kStep - kPoisonOverhead;
  }

  heap_caps_free(buf);  // 先放：這塊內容每次重建都會整個重讀，不必 realloc 搬（也省掉兩塊並存的尖峰）
  buf = nullptr;
  capacity = 0;

  if (void* p = heap_caps_malloc(want, MALLOC_CAP_DEFAULT)) {
    buf = static_cast<uint8_t*>(p);
    capacity = want;
    return g;
  }

  // 查詢與配置之間別的 task 可能動到堆積 → 失敗就重查一次（第二次也失敗＝這頁沒有 buffer，走既有的 exhausted 路徑）。
  for (uint8_t attempt = 0; attempt < 2 && !buf; attempt++) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (attempt == 0) g.largest = largest;
    if (largest == 0) break;
    // 通常 largest < want（否則上面已經配到）；別的 task 剛好放了一塊大的時就照 want 配。
    const uint32_t grab = largest < want ? static_cast<uint32_t>(largest) : want;
    if (void* p = heap_caps_malloc(grab, MALLOC_CAP_DEFAULT)) {
      buf = static_cast<uint8_t*>(p);
      capacity = grab;
      g.grabbed = true;
    }
  }
  return g;
}

}  // namespace

namespace {
// v253：CJK 等寬字寬。範圍只取 U+4E00–9FFF：常用字全在這段；擴充 A、相容表意字很少出現，照舊走查表。
constexpr uint32_t CJK_FAST_FIRST = 0x4E00;
constexpr uint32_t CJK_FAST_LAST = 0x9FFF;
// 舊路徑為某字面【成功】讀過這麼多個範圍內字寬才掃。v253 實機：一筆舊讀取約 3.4ms、掃描 578–766ms。
// v254：【內文字面】128（約 0.45 秒）—— 中文書的內文一定用得回來，早點掃只是少付掃描前的逐字讀
//   （v253 開機後第一個大章掃描前付了 1.3–2.5 秒，其中一半是直排替粗體讀的，已另修）。
// 其他字面（粗體等）256：codex 算過回本點約 300 筆，粗體可能只出現一兩百個字，掃了是白付 0.6–0.8 秒。
constexpr uint32_t CJK_SCAN_TRIGGER_BODY = 128;
constexpr uint32_t CJK_SCAN_TRIGGER = 256;
constexpr uint32_t CJK_VERIFY_SAMPLES = 16;
constexpr uint8_t CJK_MAX_RETRIES = 1;
// 單次掃描的時間上限：正常卡約 0.7 秒；SD 卡慢到這個程度就放棄（不再試），照舊查表。
// 每一次 SD 操作之前都檢查（取樣、整段、抽驗）；單一個卡住的 SD 呼叫本身無法從這裡中斷（與其他讀取相同）。
constexpr int64_t CJK_SCAN_DEADLINE_US = 3000000;
// 兩次掃描之間的最短間隔：避免同一個建置步驟裡背對背掃兩個字面（每個都持 RenderLock）。
constexpr uint32_t CJK_SCAN_SPACING_MS = 2000;
// 掃描借 cpScratch_（uint32_t[4096+2]＝16,392B）當讀取緩衝：一次 1,024 筆（16,384B）。
constexpr uint32_t CJK_SCAN_SCRATCH_SLOTS = 4096 + 2;
constexpr uint32_t CJK_SCAN_IO_RECORDS = (CJK_SCAN_SCRATCH_SLOTS * sizeof(uint32_t)) / sizeof(EpdGlyph);
constexpr size_t GLYPH_ADVANCE_OFFSET = offsetof(EpdGlyph, advanceX);

enum : uint8_t {
  CJK_WHY_OK = 0,
  CJK_WHY_NONE,     // 這個字面在範圍內沒有字
  CJK_WHY_OPEN,     // 以下三個是 SD 讀取失敗（可重試一次）
  CJK_WHY_SEEK,
  CJK_WHY_READ,
  CJK_WHY_NOMAJ,    // 取樣找不到過半的字寬
  CJK_WHY_ZERO,     // 標準字寬是 0（getAdvance 的 0 是「沒查到」，不能拿來當答案）
  CJK_WHY_EXC,      // 例外超過 CJK_MAX_EXCEPTIONS
  CJK_WHY_SUM,      // 讀到的字寬總和與「標準字寬＋例外」算回來的不同（記帳錯）
  CJK_WHY_VERIFY,   // 抽驗與逐筆讀的結果不同
  CJK_WHY_VREAD,    // 抽驗時讀取失敗（可重試一次）
  CJK_WHY_SLOW,     // 超過時間上限
  CJK_WHY_RUNTIME,  // 頁面預載讀到的字寬與快路徑不同（prewarmStyle 的核對）
  CJK_WHY_ABORT,    // v255：使用者按了鍵／有畫面在等 → 中止，不算失敗，下個允許的時機重來
};
constexpr const char* CJK_WHY_NAME[] = {"ok",  "none",   "open",  "seek", "read", "nomaj",  "zero",
                                        "exc", "sum",    "verify", "vread", "slow", "runtime", "abort"};
}  // namespace

char SdCardFont::lastAllocFail[128] = {0};
void (*SdCardFont::buildProbeHook_)(uint8_t) = nullptr;
uint32_t SdCardFont::advanceMissCount_ = 0;
uint32_t SdCardFont::advanceSdReadCount_ = 0;
uint64_t SdCardFont::advanceSdReadUs_ = 0;
uint32_t SdCardFont::advanceRejectCount_ = 0;
uint32_t SdCardFont::advanceEvictCount_ = 0;
uint8_t SdCardFont::advanceSdProbeDepth_ = 0;
uint32_t SdCardFont::advanceCjkHitCount_ = 0;
TaskHandle_t SdCardFont::scanWindowOwner_ = nullptr;
uint8_t SdCardFont::scanWindowDepth_ = 0;
bool (*SdCardFont::scanAbortHook_)(void*) = nullptr;
void* SdCardFont::scanAbortCtx_ = nullptr;
namespace {
portMUX_TYPE g_scanWindowMux = portMUX_INITIALIZER_UNLOCKED;  // v255：擁有者／深度／鉤子／參數一起讀寫
}
uint32_t SdCardFont::advanceScanDeferred_ = 0;
uint32_t SdCardFont::advanceCjkCrossChecks_ = 0;
uint32_t SdCardFont::advanceCjkCrossMismatch_ = 0;
uint32_t SdCardFont::advanceFetchReadCount_ = 0;
uint64_t SdCardFont::advanceFetchUs_ = 0;
uint64_t SdCardFont::advanceScanUs_ = 0;
char SdCardFont::lastAdvScan[256] = {0};

void SdCardFont::resetAdvanceDiag() {
  advanceMissCount_ = 0;
  advanceSdReadCount_ = 0;
  advanceSdReadUs_ = 0;
  advanceRejectCount_ = 0;
  advanceEvictCount_ = 0;
  advanceCjkHitCount_ = 0;
  advanceFetchReadCount_ = 0;
  advanceFetchUs_ = 0;
  advanceScanUs_ = 0;
  advanceScanDeferred_ = 0;
}

// v255（codex 第二輪）：允許視窗屬於【開啟它的那個 task】。擁有者、深度、中止鉤子與它的參數在同一個臨界區內一起設、
//   一起讀 —— 別的 task 在這段時間排版不會拿到允許、也不會拿到錯配的鉤子參數。巢狀開啟保留最外層的鉤子。
void SdCardFont::openCjkScanWindow(bool (*abortFn)(void*), void* abortCtx) {
  const TaskHandle_t me = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&g_scanWindowMux);
  if (scanWindowDepth_ == 0) {
    scanWindowOwner_ = me;
    scanAbortHook_ = abortFn;
    scanAbortCtx_ = abortCtx;
  }
  if (scanWindowOwner_ == me && scanWindowDepth_ < 255) ++scanWindowDepth_;
  portEXIT_CRITICAL(&g_scanWindowMux);
}

void SdCardFont::closeCjkScanWindow() {
  const TaskHandle_t me = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&g_scanWindowMux);
  if (scanWindowOwner_ == me && scanWindowDepth_ > 0 && --scanWindowDepth_ == 0) {
    scanWindowOwner_ = nullptr;
    scanAbortHook_ = nullptr;
    scanAbortCtx_ = nullptr;
  }
  portEXIT_CRITICAL(&g_scanWindowMux);
}

void SdCardFont::setAdvanceSdProbe(bool on) {
  // v192：巢狀深度，進 +1 出 -1；布林旗標會讓內層提早清成 false。
  if (on) {
    if (advanceSdProbeDepth_ < 255) ++advanceSdProbeDepth_;
  } else if (advanceSdProbeDepth_ > 0) {
    --advanceSdProbeDepth_;
  }
}

void SdCardFont::noteAllocFail(const char* what, unsigned bytes, unsigned defMax, unsigned defFree) {
  if (breadcrumbPending(lastAllocFail)) return;  // 先到先得
  char local[sizeof(lastAllocFail)];
  snprintf(local, sizeof(local), "%s bytes=%u defMax=%u defFree=%u", what, bytes, defMax, defFree);
  breadcrumbPublish(lastAllocFail, sizeof(lastAllocFail), local);  // v249：跨 task 交接（見 Breadcrumb.h）
}

void SdCardFont::setBuildProbeHook(void (*fn)(uint8_t)) { buildProbeHook_ = fn; }

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  delete[] s.miniDropped;
  s.miniDropped = nullptr;
  s.miniDroppedCount = 0;
  s.miniDroppedCapacity = 0;
  delete[] s.miniIntervals;
  s.miniIntervals = nullptr;
  delete[] s.miniGlyphs;
  s.miniGlyphs = nullptr;
  heap_caps_free(s.miniBitmap);  // v244：growMiniBitmap 用 heap_caps_malloc 配的
  s.miniBitmap = nullptr;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniIntervalCapacity = 0;
  s.miniGlyphCapacity = 0;
  s.miniBitmapCapacity = 0;
  s.miniBitmapUsed = 0;
  s.miniUnderuseRuns = 0;
  freeStyleMiniKern(s);
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::resetStyleMiniData(PerStyle& s, const bool heapTight) {
  // Retention is a bet that the next scope needs similar data. Don't hold it
  // when the heap is tight: the arenas are rebuildable for one page's worth of
  // allocations, and this floor keeps retained fonts out of the way of section
  // builds and the render path's own floors.
  if (heapTight) {
    freeStyleMiniData(s);
    return;
  }
  // Underuse hysteresis, on the bitmap arena (the dominant allocation): an
  // outlier page (e.g. three styles cramped together) would otherwise pin its
  // high-water arena for the rest of the book. Keep while the page used at
  // least 3/4 of capacity; release only after several consecutive rebuilds
  // below that, so alternating dense/sparse pages never thrash. Evaluated at
  // most once per rebuild (a scope both constructs and destructs through here,
  // and subset hits load nothing new to judge).
  if (s.miniHysteresisPending && s.miniBitmapCapacity > 0 && s.miniBitmapUsed > 0) {
    s.miniHysteresisPending = false;
    if (s.miniBitmapUsed < s.miniBitmapCapacity - s.miniBitmapCapacity / 4) {
      if (++s.miniUnderuseRuns >= MINI_UNDERUSE_RUNS_BEFORE_FREE) {
        LOG_DBG("SDCF", "mini release (underuse): used=%u cap=%u", s.miniBitmapUsed, s.miniBitmapCapacity);
        freeStyleMiniData(s);
        return;
      }
    } else {
      s.miniUnderuseRuns = 0;
    }
  }
  // Data (intervals/glyphs/bitmaps/kern) deliberately survives the scope: the
  // next prewarm subset-checks against it, which is what lets the idle prewarm
  // of page N+1 serve the actual page turn with zero SD reads.
}

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  delete[] s.kernLeftClasses;
  s.kernLeftClasses = nullptr;
  delete[] s.kernRightClasses;
  s.kernRightClasses = nullptr;
  delete[] s.ligaturePairs;
  s.ligaturePairs = nullptr;
  s.kernLigLoaded = false;
}

void SdCardFont::freeStyleMiniKern(PerStyle& s) {
  delete[] s.miniKernLeftClasses;
  s.miniKernLeftClasses = nullptr;
  delete[] s.miniKernRightClasses;
  s.miniKernRightClasses = nullptr;
  delete[] s.miniKernMatrix;
  s.miniKernMatrix = nullptr;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  s.miniKernLeftCapacity = 0;
  s.miniKernRightCapacity = 0;
  s.miniKernMatrixCapacity = 0;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  delete[] s.fullIntervals;
  s.fullIntervals = nullptr;
  delete[] s.bmpIntervals;
  s.bmpIntervals = nullptr;
  s.intervalsAreBmp16 = false;
  freeStyleKernLigatureData(s);
  s.present = false;
}

// --- Global free/cleanup ---

// v255：sharedFile_ 的使用者（prewarmStyle、onGlyphMiss、fetchAdvancesForCodepoints、buildMiniKernMatrix）
// 各自的「seek＋read」序列要不被別的使用者插隊。RenderLock 管得住排版與畫頁，但字典頁的 onEnter 不持 RenderLock
// 也會量字寬（codex 複查）。HalFile 每個呼叫各自持 StorageLock，只保證單一呼叫不壞，不保證位置不被搬走。
// 遞迴互斥鎖（同一 task 巢狀進來不自鎖），靜態配置、不佔堆積。鎖內只做 SD 讀寫，不去拿 RenderLock，不會形成環。
struct SdCardFont::SharedFileLock {
  SemaphoreHandle_t m;
  explicit SharedFileLock(const SdCardFont& f) : m(f.sharedFileMutex_) {
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
  }
  ~SharedFileLock() {
    if (m) xSemaphoreGiveRecursive(m);
  }
  SharedFileLock(const SharedFileLock&) = delete;
  SharedFileLock& operator=(const SharedFileLock&) = delete;
};

SdCardFont::SdCardFont() { sharedFileMutex_ = xSemaphoreCreateRecursiveMutexStatic(&sharedFileMutexStorage_); }

// v255：SD seek／read 出錯之後把共用檔柄關掉，下一次 ensureFileOpen 重開 —— 不讓一次暫時性錯誤毒化整個 session。
// 呼叫端必須持有 SharedFileLock。
void SdCardFont::dropSharedFile() { sharedFile_ = HalFile{}; }

bool SdCardFont::ensureFileOpen() {
  if (sharedFile_) return true;
  if (!filePath_[0]) return false;
  if (!Storage.openFileForRead("SDCF", filePath_, sharedFile_)) {
    LOG_ERR("SDCF", "Failed to open shared .cpfont handle: %s", filePath_);
    return false;
  }
  return true;
}

void SdCardFont::freeAll() {
  delete[] cpScratch_;
  cpScratch_ = nullptr;
  {
    const SharedFileLock fileLock(*this);  // v255：別的 task 正在用它讀的話，等讀完再關
    sharedFile_ = HalFile{};  // 關閉共用檔柄（移動指派讓舊的解構→close）
  }
  clearOverflow();
  clearPersistentCache();  // v253：含掃描結果
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    delete[] overflow_[i].bitmap;
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const {
  // Kern data uses the per-page mini tables (renumbered class IDs). The full
  // kern matrix is never resident — see PerStyle::miniKernMatrix comment.
  data.kernLeftClasses = s.miniKernLeftClasses;
  data.kernRightClasses = s.miniKernRightClasses;
  data.kernMatrix = s.miniKernMatrix;
  data.kernLeftEntryCount = s.miniKernLeftEntryCount;
  data.kernRightEntryCount = s.miniKernRightEntryCount;
  data.kernLeftClassCount = s.miniKernLeftClassCount;
  data.kernRightClassCount = s.miniKernRightClassCount;
  // Ligatures are small (typically < 1KB) so they stay resident.
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.header.ligaturePairCount;
}

bool SdCardFont::loadStyleKernLigatureData(PerStyle& s) {
  if (s.kernLigLoaded) return true;
  bool hasKern = s.header.kernLeftEntryCount > 0;
  bool hasLig = s.header.ligaturePairCount > 0;
  if (!hasKern && !hasLig) {
    s.kernLigLoaded = true;
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (hasKern) {
    // Load only the small class-lookup tables (~3KB each). The full matrix
    // (~36KB contiguous for Literata) is built per-page from SD in
    // buildMiniKernMatrix().
    s.kernLeftClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernLeftEntryCount];
    s.kernRightClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernRightEntryCount];

    if (!s.kernLeftClasses || !s.kernRightClasses) {
      LOG_ERR("SDCF", "Failed to allocate kern classes (%u+%u bytes)", s.header.kernLeftEntryCount * 3u,
              s.header.kernRightEntryCount * 3u);
      freeStyleKernLigatureData(s);
      return false;
    }

    if (!file.seekSet(s.kernLeftFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to kern data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (file.read(reinterpret_cast<uint8_t*>(s.kernLeftClasses), leftSz) != static_cast<int>(leftSz) ||
        file.read(reinterpret_cast<uint8_t*>(s.kernRightClasses), rightSz) != static_cast<int>(rightSz)) {
      LOG_ERR("SDCF", "Failed to read kern classes");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  if (hasLig) {
    s.ligaturePairs = new (std::nothrow) EpdLigaturePair[s.header.ligaturePairCount];
    if (!s.ligaturePairs) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
    if (!file.seekSet(s.ligatureFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to ligature data");
      freeStyleKernLigatureData(s);
      return false;
    }
    size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(s.ligaturePairs), sz) != static_cast<int>(sz)) {
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      freeStyleKernLigatureData(s);
      return false;
    }
  }

  s.kernLigLoaded = true;

  // Make ligatures visible to the stub (used when no mini data built yet).
  // Kern stays nullptr on the stub — it is only wired in miniData via
  // applyKernLigaturePointers() after buildMiniKernMatrix() runs.
  s.stubData.ligaturePairs = s.ligaturePairs;
  s.stubData.ligaturePairCount = s.header.ligaturePairCount;

  LOG_DBG("SDCF", "Kern classes + lig loaded: kernL=%u, kernR=%u, ligs=%u", s.header.kernLeftEntryCount,
          s.header.kernRightEntryCount, s.header.ligaturePairCount);
  return true;
}

// --- Per-page mini kern matrix ---

// Local copy of EpdFont.cpp's lookupKernClass (that one is file-static there).
// Returns the 1-based class ID for `cp`, or 0 if the codepoint has no kerning class.
static uint8_t miniLookupKernClass(const EpdKernClassEntry* entries, uint16_t count, uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) return 0;
  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;
  const auto it =
      std::lower_bound(entries, end, target, [](const EpdKernClassEntry& e, uint16_t v) { return e.codepoint < v; });
  return (it != end && it->codepoint == target) ? it->classId : 0;
}

// Build a small per-page kern matrix containing ONLY the (leftClass, rightClass)
// pairs reachable from codepoints in the current text. Class IDs are renumbered
// to a dense 1..N range so the resulting matrix is usedLeft × usedRight (typical
// Latin page: ~25×25 bytes) instead of the font's full ~180×200 (~36KB).
//
// Correctness: EpdFont::getKerning only touches `kernLeftClasses` /
// `kernRightClasses` / `kernMatrix` / the count fields — we swap all of them to
// the mini versions together in applyKernLigaturePointers, so a codepoint not
// on this page simply returns class 0 (no kerning), which was the pre-existing
// behavior for any codepoint outside the kern classes.
bool SdCardFont::buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount) {
  // No freeStyleMiniKern here: it zeroed the capacities, which forced the
  // ensureArrayCapacity calls below to reallocate every page and defeated the
  // buffer reuse. prewarmStyle is the only caller and the success path
  // overwrites the contents and all four counts, so keeping the buffers is
  // safe. The early returns zero the counts (buffers kept) so a page with no
  // applicable kern pairs kerns as none instead of through the previous
  // page's tables.
  const auto resetMiniKernCounts = [&s]() {
    s.miniKernLeftEntryCount = 0;
    s.miniKernRightEntryCount = 0;
    s.miniKernLeftClassCount = 0;
    s.miniKernRightClassCount = 0;
  };
  if (!s.kernLeftClasses || !s.kernRightClasses || s.header.kernLeftEntryCount == 0 ||
      s.header.kernRightEntryCount == 0) {
    resetMiniKernCounts();
    return true;  // font has no kern classes — nothing to build
  }

  // Step 1: mark used left/right classes via a 256-wide bitmap (class IDs are uint8_t).
  bool usedLeft[256] = {};
  bool usedRight[256] = {};
  for (uint32_t i = 0; i < cpCount; i++) {
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]);
    if (lc) usedLeft[lc] = true;
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]);
    if (rc) usedRight[rc] = true;
  }

  // Step 2: build renumber maps (oldClassId -> newClassId, 1-based) and
  // reverse maps (newClassId -> oldClassId) for the SD read step.
  uint8_t leftRenumber[256] = {};
  uint8_t rightRenumber[256] = {};
  uint8_t newToOldLeft[256] = {};
  uint8_t newToOldRight[256] = {};
  uint8_t numLeft = 0, numRight = 0;
  for (int i = 1; i < 256; i++) {
    if (usedLeft[i]) {
      numLeft++;
      leftRenumber[i] = numLeft;
      newToOldLeft[numLeft] = static_cast<uint8_t>(i);
    }
    if (usedRight[i]) {
      numRight++;
      rightRenumber[i] = numRight;
      newToOldRight[numRight] = static_cast<uint8_t>(i);
    }
  }
  if (numLeft == 0 || numRight == 0) {
    resetMiniKernCounts();
    return true;  // no kern pairs applicable on this page
  }

  // Step 3: count how many codepoint→classId entries the mini class tables need.
  // Each resident class table has one entry per kerned codepoint in the page.
  uint16_t miniLeftCount = 0;
  uint16_t miniRightCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]) != 0) miniLeftCount++;
    if (miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]) != 0) miniRightCount++;
  }

  // Step 4: size the three mini buffers (reused across pages when they fit; the
  // per-page sizes vary by a few entries, which as free+realloc churn was punching
  // non-coalescing holes in the heap every page turn).
  const uint32_t matrixBytes = static_cast<uint32_t>(numLeft) * numRight;
  if (!ensureArrayCapacity(s.miniKernLeftClasses, s.miniKernLeftCapacity, miniLeftCount) ||
      !ensureArrayCapacity(s.miniKernRightClasses, s.miniKernRightCapacity, miniRightCount) ||
      !ensureArrayCapacity(s.miniKernMatrix, s.miniKernMatrixCapacity, matrixBytes)) {
    LOG_ERR("SDCF", "Failed to allocate mini kern (%u+%u+%u bytes)", miniLeftCount * 3u, miniRightCount * 3u,
            matrixBytes);
    freeStyleMiniKern(s);
    return false;
  }

  // Step 5: populate mini class tables. `codepoints` is already sorted (see
  // prewarm()) so the output is sorted by codepoint — required for binary
  // search in lookupKernClass during render.
  uint16_t lIdx = 0, rIdx = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    uint32_t cp = codepoints[i];
    if (cp > 0xFFFF) continue;  // kern class entries are uint16_t
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, cp);
    if (lc) {
      s.miniKernLeftClasses[lIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernLeftClasses[lIdx].classId = leftRenumber[lc];
      lIdx++;
    }
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, cp);
    if (rc) {
      s.miniKernRightClasses[rIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernRightClasses[rIdx].classId = rightRenumber[rc];
      rIdx++;
    }
  }

  // Step 6: read the full matrix's rows for each used left class, keep only
  // columns for used right classes. One SD seek + one read per used left class;
  // a row is kernRightClassCount bytes (~200 for Literata).
  // v255：共用檔柄（prewarmStyle 讀字圖用的同一個；呼叫時字圖已讀完，下面每列都自己 seek）。
  //   原本每一次畫頁預載都另開一次 .cpfont（12–18ms，A-4）只為了讀這幾列字距。
  const SharedFileLock fileLock(*this);  // v255
  if (!ensureFileOpen()) {
    LOG_ERR("SDCF", "Failed to open .cpfont for mini kern: %s", filePath_);
    freeStyleMiniKern(s);
    return false;
  }
  HalFile& file = sharedFile_;

  std::unique_ptr<int8_t[]> rowBuf(new (std::nothrow) int8_t[s.header.kernRightClassCount]);
  if (!rowBuf) {
    LOG_ERR("SDCF", "Failed to allocate row buffer (%u bytes)", s.header.kernRightClassCount);
    freeStyleMiniKern(s);
    return false;
  }

  for (uint8_t newL = 1; newL <= numLeft; newL++) {
    const uint8_t oldL = newToOldLeft[newL];
    const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * s.header.kernRightClassCount;
    if (!file.seekSet(rowFileOff)) {
      LOG_ERR("SDCF", "Failed to seek to kern row %u", oldL);
      dropSharedFile();  // v255
      freeStyleMiniKern(s);
      return false;
    }
    if (file.read(reinterpret_cast<uint8_t*>(rowBuf.get()), s.header.kernRightClassCount) !=
        static_cast<int>(s.header.kernRightClassCount)) {
      LOG_ERR("SDCF", "Failed to read kern row %u", oldL);
      dropSharedFile();  // v255
      freeStyleMiniKern(s);
      return false;
    }
    int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * numRight;
    for (uint8_t newR = 1; newR <= numRight; newR++) {
      miniRow[newR - 1] = rowBuf[newToOldRight[newR] - 1u];
    }
  }

  s.miniKernLeftEntryCount = lIdx;
  s.miniKernRightEntryCount = rIdx;
  s.miniKernLeftClassCount = numLeft;
  s.miniKernRightClassCount = numRight;

  LOG_DBG("SDCF", "Built mini kern: %u×%u matrix (%u bytes, full was %u×%u = %u bytes)", numLeft, numRight, matrixBytes,
          s.header.kernLeftClassCount, s.header.kernRightClassCount,
          static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount);
  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.stubData.coverageHandler = &SdCardFont::onCoverageQuery;
}

bool SdCardFont::onCoverageQuery(void* ctx, const uint32_t codepoint) {
  const auto* octx = static_cast<OverflowContext*>(ctx);
  const PerStyle& s = octx->self->styles_[octx->styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return false;  // coverage index freed/never loaded
  return octx->self->findGlobalGlyphIndex(s, codepoint) >= 0;
}

// --- Compute per-style file offsets from a base data offset ---

void SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset) {
  s.intervalsFileOffset = baseOffset;
  s.glyphsFileOffset = s.intervalsFileOffset + s.header.intervalCount * sizeof(EpdUnicodeInterval);
  s.kernLeftFileOffset = s.glyphsFileOffset + s.header.glyphCount * sizeof(EpdGlyph);
  s.kernRightFileOffset = s.kernLeftFileOffset + s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  s.kernMatrixFileOffset = s.kernRightFileOffset + s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
  s.ligatureFileOffset =
      s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
  s.bitmapFileOffset = s.ligatureFileOffset + s.header.ligaturePairCount * sizeof(EpdLigaturePair);
}

// --- Load ---

bool SdCardFont::load(const char* path) {
  freeAll();
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  HalFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion != CPFONT_VERSION) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u)", fileVersion, CPFONT_VERSION);
    return false;
  }

  // Begin content hash: accumulate global header
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", styleCount);
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      file.close();
      freeAll();
      return false;
    }

    auto& s = styles_[styleId];
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating.
    // Kern class counts are uint8 (bounded by type). Entry counts are uint16
    // but in practice a sane font has far fewer than 4096 per-side kern entries.
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    static constexpr uint32_t MAX_KERN_ENTRIES = 4096;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS ||
        s.header.kernLeftEntryCount > MAX_KERN_ENTRIES || s.header.kernRightEntryCount > MAX_KERN_ENTRIES) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (iv=%u, gl=%u, kL=%u, kR=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount, s.header.kernLeftEntryCount, s.header.kernRightEntryCount);
      file.close();
      freeAll();
      return false;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    computeStyleFileOffsets(s, dataOffset);
  }

  styleCount_ = styleCount;
  contentHash_ = hash;

  // Load full intervals into RAM for each present style. BMP-only fonts with
  // fewer than 65536 glyphs use a compact 6-byte interval table instead of the
  // on-disk 12-byte table; large sparse CJK subsets otherwise keep tens of KB
  // of always-resident heap just for lookup metadata.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to intervals for style %u", i);
      freeAll();
      return false;
    }

    // Validate interval contents before any later code (findGlobalGlyphIndex,
    // glyph reads) trusts them. A malformed file could otherwise drive
    // out-of-range glyph indices into bogus on-disk reads.
    bool canUseBmp16 = s.header.glyphCount <= UINT16_MAX;
    uint32_t expectedOffset = 0;
    uint32_t prevLast = 0;
    EpdUnicodeInterval iv{};
    for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
      if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
        LOG_ERR("SDCF", "Failed to read interval %u for style %u", j, i);
        freeAll();
        return false;
      }
      if (iv.first > iv.last) {
        LOG_ERR("SDCF", "Style %u: invalid interval %u (first 0x%lX > last 0x%lX)", i, j,
                static_cast<unsigned long>(iv.first), static_cast<unsigned long>(iv.last));
        file.close();
        freeAll();
        return false;
      }
      const uint32_t span = iv.last - iv.first + 1;
      const bool overlapsPrev = (j > 0 && iv.first <= prevLast);
      const bool spanTooBig = (span > s.header.glyphCount);
      const bool offsetMismatch = (iv.offset != expectedOffset);
      const bool offsetOverruns = (iv.offset > s.header.glyphCount - span);
      if (overlapsPrev || spanTooBig || offsetMismatch || offsetOverruns) {
        LOG_ERR("SDCF", "Style %u: invalid interval layout at %u (overlap=%d span=%u offMis=%d offOver=%d)", i, j,
                overlapsPrev, span, offsetMismatch, offsetOverruns);
        file.close();
        freeAll();
        return false;
      }
      if (iv.first > UINT16_MAX || iv.last > UINT16_MAX || iv.offset > UINT16_MAX) {
        canUseBmp16 = false;
      }
      expectedOffset += span;
      prevLast = iv.last;
    }

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek back to intervals for style %u", i);
      freeAll();
      return false;
    }

    if (canUseBmp16) {
      s.bmpIntervals = new (std::nothrow) PerStyle::BmpInterval16[s.header.intervalCount];
      if (!s.bmpIntervals) {
        LOG_ERR("SDCF", "Failed to allocate compact intervals for style %u", i);
        freeAll();
        return false;
      }
      for (uint32_t j = 0; j < s.header.intervalCount; ++j) {
        if (file.read(reinterpret_cast<uint8_t*>(&iv), sizeof(iv)) != sizeof(iv)) {
          LOG_ERR("SDCF", "Failed to read compact interval %u for style %u", j, i);
          freeAll();
          return false;
        }
        s.bmpIntervals[j] = {static_cast<uint16_t>(iv.first), static_cast<uint16_t>(iv.last),
                             static_cast<uint16_t>(iv.offset)};
      }
      s.intervalsAreBmp16 = true;
    } else {
      s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, i);
        freeAll();
        return false;
      }
      size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
        LOG_ERR("SDCF", "Failed to read intervals for style %u", i);
        freeAll();
        return false;
      }
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, CPFONT_VERSION, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, kernL=%u, kernR=%u, ligs=%u", i,
            h.intervalCount, h.glyphCount, h.advanceY, h.ascender, h.descender, h.kernLeftEntryCount,
            h.kernRightEntryCount, h.ligaturePairCount);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const uint32_t first = s.intervalsAreBmp16 ? s.bmpIntervals[mid].first : s.fullIntervals[mid].first;
    const uint32_t last = s.intervalsAreBmp16 ? s.bmpIntervals[mid].last : s.fullIntervals[mid].last;
    if (codepoint < first) {
      right = mid - 1;
    } else if (codepoint > last) {
      left = mid + 1;
    } else {
      const uint32_t offset = s.intervalsAreBmp16 ? s.bmpIntervals[mid].offset : s.fullIntervals[mid].offset;
      return static_cast<int32_t>(offset + (codepoint - first));
    }
  }
  return -1;
}

// --- Prewarm ---

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints from UTF-8 text (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  // Heap-allocated: MAX_PAGE_GLYPHS * 4 = 2048 bytes, too large for stack (limit < 256 bytes)
  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[MAX_PAGE_GLYPHS]);
  if (!codepoints) {
    LOG_ERR("SDCF", "Failed to allocate codepoint buffer (%u bytes)", MAX_PAGE_GLYPHS * 4);
    return -1;
  }
  uint32_t cpCount = 0;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < MAX_PAGE_GLYPHS) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      codepoints[cpCount++] = cp;
    }
  }

  // Always include the replacement character
  {
    bool hasReplacement = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == REPLACEMENT_GLYPH) {
        hasReplacement = true;
        break;
      }
    }
    if (!hasReplacement && cpCount < MAX_PAGE_GLYPHS) {
      codepoints[cpCount++] = REPLACEMENT_GLYPH;
    }
  }

  // Add ligature output codepoints from all styles being prewarmed.
  // Skip during metadata-only prewarm (layout measurement) to avoid loading
  // kern/lig data for all styles upfront (~22KB per style). Kern/lig is
  // loaded per-style in prewarmStyle() during the full render prewarm instead.
  if (!metadataOnly) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
      auto& s = styles_[si];

      loadStyleKernLigatureData(s);
      if (s.ligaturePairs && s.header.ligaturePairCount > 0) {
        for (uint8_t li = 0; li < s.header.ligaturePairCount && cpCount < MAX_PAGE_GLYPHS; li++) {
          uint32_t leftCp = s.ligaturePairs[li].pair >> 16;
          uint32_t rightCp = s.ligaturePairs[li].pair & 0xFFFF;
          uint32_t outCp = s.ligaturePairs[li].ligatureCp;

          bool hasLeft = false, hasRight = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == leftCp) hasLeft = true;
            if (codepoints[i] == rightCp) hasRight = true;
            if (hasLeft && hasRight) break;
          }
          if (!hasLeft || !hasRight) continue;

          bool hasOut = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == outCp) {
              hasOut = true;
              break;
            }
          }
          if (!hasOut) {
            codepoints[cpCount++] = outCp;
          }
        }
      }
    }
  }

  // Sort codepoints for ordered interval building
  std::sort(codepoints.get(), codepoints.get() + cpCount);

  // Prewarm each requested style
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    totalMissed += prewarmStyle(si, codepoints.get(), cpCount, metadataOnly);
  }

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly) {
  auto& s = styles_[styleIdx];

  // Idle-prewarm hit: mini data persists across PrewarmScopes (resetStyleMiniData
  // keeps it), so when the previous scope -- typically the idle prewarm of this
  // exact page -- already loaded every requested codepoint the font covers, this
  // page needs zero SD reads. A mini built metadata-only cannot serve a full
  // request (no bitmaps). Any uncovered codepoint falls through to the rebuild.
  if (s.miniGlyphCount > 0 && !(s.miniMetadataOnly && !metadataOnly)) {
    bool covered = true;
    int missedInMini = 0;
    for (uint32_t i = 0; i < cpCount && covered; i++) {
      const uint32_t cp = codepoints[i];
      bool inMini = false;
      for (uint32_t iv = 0; iv < s.miniIntervalCount; iv++) {
        if (cp < s.miniIntervals[iv].first) break;  // intervals sorted ascending
        if (cp <= s.miniIntervals[iv].last) {
          inMini = true;
          break;
        }
      }
      if (inMini) continue;
      // v154（codex P1-1）：被降級丟棄的碼位視為 covered —— 它已由 miss ring 承接，
      // 重建整套 mini 也只會在同樣的堆積條件下再丟一次。二分搜尋（miniDropped 已排序）。
      if (s.miniDroppedCount > 0) {
        const uint32_t* lo = s.miniDropped;
        const uint32_t* hi = s.miniDropped + s.miniDroppedCount;
        const uint32_t* it = std::lower_bound(lo, hi, cp);
        if (it != hi && *it == cp) continue;
      }
      if (findGlobalGlyphIndex(s, cp) < 0) {
        missedInMini++;  // not in font coverage: the rebuild couldn't load it either
      } else {
        covered = false;
      }
    }
    if (covered) {
      return missedInMini;
    }
  }

  // Map codepoints to global glyph indices for this style
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };
  CpGlyphMapping* mappings = new (std::nothrow) CpGlyphMapping[cpCount];
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    int32_t idx = findGlobalGlyphIndex(s, codepoints[i]);
    if (idx >= 0) {
      mappings[validCount].codepoint = codepoints[i];
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  int missed = static_cast<int>(cpCount - validCount);

  if (validCount == 0) {
    freeStyleMiniData(s);
    delete[] mappings;
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Build mini intervals from sorted codepoints. Reset counts and fall back to the
  // stub until the rebuild completes, but KEEP the existing buffers (keep-if-fits
  // reuse) — the free-and-realloc-per-page pattern here was a primary fragmenter.
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;

  if (!ensureArrayCapacity(s.miniIntervals, s.miniIntervalCapacity, validCount)) {
    LOG_ERR("SDCF", "Failed to allocate mini intervals for style %u", styleIdx);
    delete[] mappings;
    return static_cast<int>(cpCount);
  }

  s.miniIntervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      s.miniIntervals[s.miniIntervalCount].first = mappings[rangeStart].codepoint;
      s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
      s.miniIntervals[s.miniIntervalCount].offset = rangeStart;
      s.miniIntervalCount++;
      rangeStart = i;
    }
  }

  // Mini glyph array (reused across pages when it fits)
  if (!ensureArrayCapacity(s.miniGlyphs, s.miniGlyphCapacity, validCount)) {
    LOG_ERR("SDCF", "Failed to allocate mini glyphs for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  s.miniGlyphCount = validCount;

  // Build sorted read order for sequential I/O
  uint32_t* readOrder = new (std::nothrow) uint32_t[validCount];
  if (!readOrder) {
    LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
  std::sort(readOrder, readOrder + validCount,
            [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });

  // v154：共用檔柄 —— 每頁 prewarm 省一次 12–18ms 的開檔。
  // v255：鎖到函式結束（含後面的 buildMiniKernMatrix，遞迴鎖）。
  const SharedFileLock fileLock(*this);
  if (!ensureFileOpen()) {
    delete[] readOrder;
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  HalFile& file = sharedFile_;

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;

  // Read glyph metadata. lastReadIndex tracks sequential reads to skip redundant
  // seeks; INT32_MIN guarantees the first iteration always seeks to the correct
  // offset (otherwise when gIdx == 0, the "gIdx != lastReadIndex + 1" check would
  // be false and we'd read from the file's current position — the header — which
  // decodes to a garbage EpdGlyph with a massive advanceX, inflating any word
  // containing that codepoint beyond page width).
  int32_t lastReadIndex = INT32_MIN;
  for (uint32_t i = 0; i < validCount; i++) {
    uint32_t mapIdx = readOrder[i];
    int32_t gIdx = mappings[mapIdx].globalIndex;

    uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
    if (gIdx != lastReadIndex + 1) {
      if (!file.seekSet(fileOff)) {
        LOG_ERR("SDCF", "Prewarm: failed to seek to glyph %d (style %u)", gIdx, styleIdx);
        dropSharedFile();  // v255
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      seekCount++;
    }
    if (file.read(reinterpret_cast<uint8_t*>(&s.miniGlyphs[mapIdx]), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
      LOG_ERR("SDCF", "Prewarm: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
      dropSharedFile();  // v255
      delete[] readOrder;
      delete[] mappings;
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }
    lastReadIndex = gIdx;
  }

  // v253：頁面預載剛讀進來的字形紀錄是現成的對照組 —— 範圍內的字都拿來核對掃描結果，零額外 I/O。
  // 掃描後的抽驗只看 16 個字＋前 8 個例外；這裡是每一頁實際出現的字。對不上＝快路徑停用（之後照舊查表）。
  if (cjk_[styleIdx].state == CJK_SCAN_READY) {
    for (uint32_t i = 0; i < validCount; i++) {
      uint16_t fast = 0;
      if (!cjkAdvanceLookup(styleIdx, mappings[i].codepoint, &fast)) continue;
      ++advanceCjkCrossChecks_;
      if (fast != s.miniGlyphs[i].advanceX) {
        ++advanceCjkCrossMismatch_;
        CjkAdvance& c = cjk_[styleIdx];
        c.state = CJK_SCAN_UNUSABLE;
        c.why = CJK_WHY_RUNTIME;
        publishAdvScanWitness();
        break;
      }
    }
  }

  uint32_t totalBitmapSize = 0;

  if (!metadataOnly) {
    s.miniDroppedCount = 0;  // v154：每次重建重新決定 dropped 集合（ladder 會再填）
    // Compute total bitmap size
    for (uint32_t i = 0; i < validCount; i++) {
      totalBitmapSize += s.miniGlyphs[i].dataLength;
    }

    const MiniBitmapGrowth growth = growMiniBitmap(s.miniBitmap, s.miniBitmapCapacity, totalBitmapSize);
    if (growth.grabbed && s.miniBitmapCapacity >= totalBitmapSize) {
      // 取整配不到，但吃下當下最大的一塊之後裝得下 → 這頁完全不降級。
      stats_.bitmapExactRescues++;
      // 證人走 SDCFFAIL 既有的印出點（兩個閱讀器都有），不新增印出站點（CLAUDE.md B-22）。
      if (!breadcrumbPending(lastAllocFail)) {
        char local[sizeof(lastAllocFail)];
        snprintf(local, sizeof(local), "mini-rescue bytes=%u defMax=%u cap=%u post=%u",
                 static_cast<unsigned>(totalBitmapSize), static_cast<unsigned>(growth.largest),
                 static_cast<unsigned>(s.miniBitmapCapacity),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
        breadcrumbPublish(lastAllocFail, sizeof(lastAllocFail), local);
      }
    }
    if (!s.miniBitmap || s.miniBitmapCapacity < totalBitmapSize) {
      // v154（P1 效能止血①）：miniBitmap 降級階梯 —— 搬回舊樹（v55 系）的作法。
      //
      // 原本這裡整段放棄（freeStyleMiniData + return cpCount）＝整頁的字全走
      // glyph-miss ring（8 格）→ 每個字每一趟灰階都從 SD 讀。實機 v153 量到的帳單：
      //   SEG tiled lsb=12,630 msb=11,872 total=26,787  （27 秒一頁）
      //   SDCFFAIL mini-bitmap bytes=33,190 defMax=14,324
      // 而 v130（舊樹）同一本書順暢 —— 差的就是這個階梯。
      //
      // 作法：丟掉「最占空間」的字（丟越少個越好），剩下的字仍走快路徑。
      // 排版不受影響 —— advanceX 走獨立的 advanceTable_，這裡只關 render。
      // 被丟的字由 glyph-miss ring 承接（OVERFLOW_CAPACITY 本版一併 8→32，
      // 帳本記過兩者是配套：ring 太小時被丟的字每一趟都重新 miss）。
      //
      // v244：階梯裡不再配置 —— growMiniBitmap 已經吃下當下最大的一塊（大小對齊區間邊界，下次回收得回來），
      //   這裡只丟到裝得進它為止。v243 是「照最大塊 − 4KB 切剛好的」，尾巴被佔住就每頁把最大塊切小一截。
      stats_.bitmapAllocFailures++;
      // v249：整行在本地組好，階梯跑完一次發佈（原本先寫基本欄位、之後再補，主任務可能在中間讀走）。
      char crumb[sizeof(lastAllocFail)];
      int crumbLen = snprintf(crumb, sizeof(crumb), "mini-bitmap bytes=%u defMax=%u defFree=%u",
                              static_cast<unsigned>(totalBitmapSize), static_cast<unsigned>(growth.largest),
                              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)));

      // 依 bitmap 大小遞減排序（readOrder 此刻的 glyph-index 排序已用完，可以重用；
      // 下面讀 bitmap 前反正會再按 dataOffset 重排）。
      std::sort(readOrder, readOrder + validCount,
                [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataLength > s.miniGlyphs[b].dataLength; });

      // dropped 標記借用 mappings[].globalIndex（metadata 已讀完，它不再被使用）。
      const uint32_t cap = s.miniBitmap ? s.miniBitmapCapacity : 0;
      uint32_t dropCursor = 0;
      uint32_t remaining = totalBitmapSize;
      while (dropCursor < validCount && remaining > cap) {
        const uint32_t idx = readOrder[dropCursor++];
        // dataLength==0 的字（空格等）不佔 bitmap，丟了只是白佔 ring —— 跳過。
        if (s.miniGlyphs[idx].dataLength == 0) continue;
        remaining -= s.miniGlyphs[idx].dataLength;
        mappings[idx].globalIndex = -1;  // 丟棄標記
      }
      const bool bitmapOk = s.miniBitmap != nullptr && remaining <= cap;
      if (bitmapOk && crumbLen > 0 && crumbLen < static_cast<int>(sizeof(crumb))) {
        // 證人：手上這塊多大、留下多少、有沒有整塊吃、配完之後別處的最大塊。
        snprintf(crumb + crumbLen, sizeof(crumb) - crumbLen, " cap=%u kept=%u grab=%u post=%u",
                 static_cast<unsigned>(cap), static_cast<unsigned>(remaining), growth.grabbed ? 1u : 0u,
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
      }
      breadcrumbPublish(lastAllocFail, sizeof(lastAllocFail), crumb);  // 先到先得（交接內判斷）

      if (!bitmapOk) {
        LOG_ERR("SDCF", "mini bitmap ladder exhausted (%u bytes) style %u", totalBitmapSize, styleIdx);
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }

      // 就地壓縮 mappings/miniGlyphs（保持碼位遞增 —— mappings 本來就按碼位序），
      // 再重建 intervals：被丟的字從 mini 消失 → render 時 interval 查不到 → 走 miss ring。
      // 壓縮會就地搬移，先把「哪些碼位被丟」快照下來（壓縮後就讀不到了）。
      const uint32_t validCountOrig = validCount;
      // 借 readOrder 前段當快照緩衝不行（它還要重建）—— 用臨時 VLA 風格 new。
      uint32_t* origCp = new (std::nothrow) uint32_t[validCount];
      uint8_t* origDropFlag = new (std::nothrow) uint8_t[validCount];
      if (origCp && origDropFlag) {
        for (uint32_t i = 0; i < validCount; i++) {
          origCp[i] = mappings[i].codepoint;
          origDropFlag[i] = mappings[i].globalIndex < 0 ? 1 : 0;
        }
      }
      uint32_t kept = 0;
      for (uint32_t i = 0; i < validCount; i++) {
        if (mappings[i].globalIndex < 0) continue;
        if (kept != i) {
          mappings[kept] = mappings[i];
          s.miniGlyphs[kept] = s.miniGlyphs[i];
        }
        kept++;
      }
      const uint32_t dropped = validCount - kept;
      // codex P1-1：把被丟的碼位記成排序集合（mappings 原本就按碼位序，被丟的挑出來
      // 仍是遞增），coverage 檢查據此不再重建。codex P3：**不**把 dropped 算進 missed ——
      // 它們仍會由 miss ring 畫出來，不是「字型沒有」。
      if (origCp && origDropFlag &&
          ensureArrayCapacity(s.miniDropped, s.miniDroppedCapacity, dropped > 0 ? dropped : 1)) {
        uint16_t dc = 0;
        for (uint32_t i = 0; i < validCountOrig; i++) {
          if (origDropFlag[i]) s.miniDropped[dc++] = origCp[i];
        }
        s.miniDroppedCount = dc;
        stats_.bitmapGlyphsDropped += dropped;
      } else {
        stats_.bitmapGlyphsDropped += dropped;
        s.miniDroppedCount = 0;  // 記不下就退回「每次重建」的舊行為，不是錯誤
      }
      validCount = kept;
      s.miniGlyphCount = validCount;

      delete[] origCp;
      delete[] origDropFlag;

      s.miniIntervalCount = 0;
      uint32_t rs = 0;
      for (uint32_t i = 1; i <= validCount; i++) {
        if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
          s.miniIntervals[s.miniIntervalCount].first = mappings[rs].codepoint;
          s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
          s.miniIntervals[s.miniIntervalCount].offset = rs;
          s.miniIntervalCount++;
          rs = i;
        }
      }
      totalBitmapSize = remaining;
      // ⚠️ 壓縮讓 readOrder 裡的舊索引全部失效（陣列已就地搬移）——下面的 bitmap 讀取
      // 迴圈會拿它按 dataOffset 重排。必須重建成 0..validCount-1，否則讀進錯的格子
      // = 字形錯置（畫錯字、不當機、不留 log —— 最陰險的那種）。
      for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
      LOG_ERR("SDCF", "mini bitmap degraded: kept=%u dropped=%u bytes=%u style %u", kept, dropped, remaining, styleIdx);
    }
    s.miniBitmapUsed = totalBitmapSize;  // underuse-hysteresis signal for resetStyleMiniData

    // Read bitmap data sorted by file offset
    std::sort(readOrder, readOrder + validCount,
              [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset; });

    uint32_t miniBitmapOffset = 0;
    uint32_t lastBitmapEnd = UINT32_MAX;
    for (uint32_t i = 0; i < validCount; i++) {
      uint32_t mapIdx = readOrder[i];
      EpdGlyph& glyph = s.miniGlyphs[mapIdx];

      if (glyph.dataLength == 0) {
        glyph.dataOffset = miniBitmapOffset;
        continue;
      }

      uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
      if (fileOff != lastBitmapEnd) {
        if (!file.seekSet(fileOff)) {
          LOG_ERR("SDCF", "Prewarm: failed to seek to bitmap (style %u)", styleIdx);
          dropSharedFile();  // v255
          delete[] readOrder;
          delete[] mappings;
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
        seekCount++;
      }
      if (file.read(s.miniBitmap + miniBitmapOffset, glyph.dataLength) != static_cast<int>(glyph.dataLength)) {
        LOG_ERR("SDCF", "Prewarm: short bitmap read (style %u)", styleIdx);
        dropSharedFile();  // v255
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      lastBitmapEnd = fileOff + glyph.dataLength;

      glyph.dataOffset = miniBitmapOffset;
      miniBitmapOffset += glyph.dataLength;
    }
  }

  uint32_t sdTime = millis() - sdStart;
  delete[] readOrder;
  delete[] mappings;

  // Full render prewarm: load the persistent kern classes + ligatures (one-time
  // per style, small — the big matrix is NOT loaded here) and then build the
  // per-page mini kern matrix restricted to class pairs reachable from this
  // page's codepoints. Skip during metadata-only prewarm — layout only needs
  // advanceX and the mini kern would be thrown away before rendering.
  bool kernLigOk = false;
  if (!metadataOnly) {
    if (loadStyleKernLigatureData(s)) {
      kernLigOk = buildMiniKernMatrix(s, codepoints, cpCount);
    }
  }

  // Populate miniData and swap
  s.miniMetadataOnly = metadataOnly;
  // codex P2：降級的 mini（capacity 是 8K 取整、used 是砍過的量）會被 underuse 判定
  // 誤殺 -> 反覆釋放重建。降級這輪跳過 hysteresis。
  s.miniHysteresisPending = !metadataOnly && s.miniDroppedCount == 0;
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.miniData.bitmap = s.miniBitmap;
  s.miniData.glyph = s.miniGlyphs;
  s.miniData.intervals = s.miniIntervals;
  s.miniData.intervalCount = s.miniIntervalCount;
  s.miniData.advanceY = s.header.advanceY;
  s.miniData.ascender = s.header.ascender;
  s.miniData.descender = s.header.descender;
  s.miniData.is2Bit = s.header.is2Bit;
  if (kernLigOk) {
    applyKernLigaturePointers(s, s.miniData);
  }
  s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.miniData.coverageHandler = &SdCardFont::onCoverageQuery;

  s.epdFont.data = &s.miniData;

  // Accumulate stats
  stats_.sdReadTimeMs += sdTime;
  stats_.seekCount += seekCount;
  stats_.uniqueGlyphs += validCount;
  stats_.bitmapBytes += totalBitmapSize;

  return missed;
}

// --- Cache management ---

void SdCardFont::clearCache() {
  clearOverflow();
  // Note: advance table is intentionally preserved here. It persists across
  // layout passes so repeated section indexing amortizes SD reads. Use
  // clearPersistentCache() to wipe it.
  // 一次量完（heap walk 持有 heap 鎖，別每個字面各走一趟）。
  const bool heapTight = ESP.getFreeHeap() < MINI_RETAIN_MIN_FREE_HEAP ||
                         heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < MINI_RETAIN_MIN_MAXBLOCK;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    resetStyleMiniData(styles_[i], heapTight);
    applyGlyphMissCallback(i);
  }
}

size_t SdCardFont::releaseMiniData() {
  clearOverflow();
  size_t bytes = 0;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    PerStyle& s = styles_[i];
    if (!s.present) continue;
    bytes += static_cast<size_t>(s.miniBitmapCapacity) + static_cast<size_t>(s.miniGlyphCapacity) * sizeof(s.miniGlyphs[0]) +
             static_cast<size_t>(s.miniIntervalCapacity) * sizeof(s.miniIntervals[0]);
    freeStyleMiniData(s);
    applyGlyphMissCallback(i);
  }
  return bytes;
}

size_t SdCardFont::retainedMiniBitmapCapacity() const {
  size_t best = 0;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    const PerStyle& s = styles_[i];
    if (!s.present || !s.miniBitmap) continue;
    if (s.miniBitmapCapacity > best) best = s.miniBitmapCapacity;
  }
  return best;
}

// --- Advance table ---

void SdCardFont::resetCjkAdvances() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) cjk_[i] = CjkAdvance{};
  lastCjkScanEndMs_ = 0;
  cjkScannedOnce_ = false;
}

bool SdCardFont::cjkAdvanceLookup(const uint8_t styleIdx, const uint32_t codepoint, uint16_t* const outAdvance) const {
  if (codepoint < CJK_FAST_FIRST || codepoint > CJK_FAST_LAST) return false;
  const CjkAdvance& c = cjk_[styleIdx];
  if (c.state != CJK_SCAN_READY) return false;
  // 字型沒有的字交給舊路徑：那邊會換成替代字形的字寬（fetchAdvancesForCodepoints 的 replacementIdx）。
  if (findGlobalGlyphIndex(styles_[styleIdx], codepoint) < 0) return false;
  const uint16_t off = static_cast<uint16_t>(codepoint - CJK_FAST_FIRST);
  uint16_t adv = c.uniform;
  uint32_t lo = 0;
  uint32_t hi = c.exceptionCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (c.exceptions[mid].cpOffset < off) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < c.exceptionCount && c.exceptions[lo].cpOffset == off) adv = c.exceptions[lo].advanceX;
  if (outAdvance) *outAdvance = adv;
  return true;
}

// v253：為什麼要掃。v252 BUILDPROF：一本中文小說的一章 56 頁排版 8.3 秒，其中 7.1 秒在拿字寬 ——
//   ① 每段文字把新出現的字逐筆 seek＋read 字形紀錄（字散在 336KB 的表裡，幾乎每筆一個磁區）；
//   ② 表每章清空、上限 768，中文一章兩三千個不同的字，滿了之後每個字退回 getGlyph（連字圖一起讀，約 14ms）。
// 而中文字型的漢字字寬幾乎全部相同。所以一次循序讀完那一段（約 0.7 秒，之後不再讀），
// 記住標準字寬與例外，查詢結果與逐筆讀【逐位元組相同】—— 例外也照實記，不是近似。
//
// 自我核對三層，對不上就放棄（照舊查表），不會帶著錯的字寬繼續排版：
//   ① 總和：讀到的每一筆字寬加總，必須等於「標準字寬 × 字數 ＋ 例外的差額」—— 抓記帳錯（例外漏記、記錯值）；
//   ② 抽驗：均勻分布 16 個字＋全部例外，走產品的查找（findGlobalGlyphIndex＋整筆 EpdGlyph）逐筆讀，
//      與掃描的區段算術是兩條獨立的路 —— 抓位置錯（區段、索引、碼位對錯）；
//   ③ 執行期：每一頁預載讀進來的字形紀錄都拿來比（prewarmStyle）。
// ⚠️ ③ 抓到時，之前已排好的頁不會重排（只停用快路徑）。邏輯本身由桌機測試對五套字型的每個碼位驗過
//    （tools/advance-scan-check），③ 是給「裝置上才會發生的分歧」的證人。
void SdCardFont::scanCjkAdvances(const uint8_t si, uint8_t* const io, const uint32_t ioRecords,
                                 bool (*const abortFn)(void*), void* const abortCtx) {
  CjkAdvance& c = cjk_[si];
  const PerStyle& s = styles_[si];
  const int64_t t0 = esp_timer_get_time();
  HalFile file;
  bool opened = false;
  // 所有出口都經過這裡：關檔、記時間、決定狀態、放 witness。
  const auto finish = [&](const uint8_t why) {
    if (opened) file.close();
    const int64_t dt = esp_timer_get_time() - t0;
    advanceScanUs_ += static_cast<uint64_t>(dt);
    c.scanMs = static_cast<uint32_t>(dt / 1000);
    c.why = why;
    lastCjkScanEndMs_ = millis();
    cjkScannedOnce_ = true;
    if (why == CJK_WHY_OK) {
      c.state = CJK_SCAN_READY;
    } else if (why == CJK_WHY_ABORT) {
      c.state = CJK_SCAN_NONE;  // 條件仍滿足，下一個允許的時機（至少隔 CJK_SCAN_SPACING_MS）重掃；不佔重試次數
    } else {
      const bool retryable =
          why == CJK_WHY_OPEN || why == CJK_WHY_SEEK || why == CJK_WHY_READ || why == CJK_WHY_VREAD;
      if (retryable && c.retries < CJK_MAX_RETRIES) {
        ++c.retries;
        c.state = CJK_SCAN_NONE;
        c.oldPathCjk = 0;  // 再累積一輪才重試，不在同一段文字裡連續重掃
      } else {
        c.state = CJK_SCAN_UNUSABLE;
      }
    }
    publishAdvScanWitness();
  };
  const auto intervalAt = [&s](const uint32_t i, uint32_t& first, uint32_t& last, uint32_t& offset) {
    if (s.intervalsAreBmp16) {
      first = s.bmpIntervals[i].first;
      last = s.bmpIntervals[i].last;
      offset = s.bmpIntervals[i].offset;
    } else {
      first = s.fullIntervals[i].first;
      last = s.fullIntervals[i].last;
      offset = s.fullIntervals[i].offset;
    }
  };
  const auto advAt = [io](const uint32_t k) { return readU16(io + k * sizeof(EpdGlyph) + GLYPH_ADVANCE_OFFSET); };

  c.state = CJK_SCAN_NONE;  // 掃描途中快路徑一律不用（例外陣列還沒填完）
  c.exceptionCount = 0;
  c.verified = 0;
  c.covered = 0;
  c.uniform = 0;
  if (!s.present || (s.intervalsAreBmp16 ? !s.bmpIntervals : !s.fullIntervals) || ioRecords == 0) {
    return finish(CJK_WHY_NONE);
  }

  // 1) 範圍內有幾個字、哪個區段最長（取樣用）。load() 已驗過區段排序、不重疊、offset 連續且不越界。
  uint32_t first = 0, last = 0, offset = 0;
  uint32_t bestIv = 0, bestSpan = 0;
  for (uint32_t i = 0; i < s.header.intervalCount; i++) {
    intervalAt(i, first, last, offset);
    if (last < CJK_FAST_FIRST) continue;
    if (first > CJK_FAST_LAST) break;
    const uint32_t span = std::min(last, CJK_FAST_LAST) - std::max(first, CJK_FAST_FIRST) + 1;
    c.covered += span;
    if (span > bestSpan) {
      bestSpan = span;
      bestIv = i;
    }
  }
  if (c.covered == 0) return finish(CJK_WHY_NONE);

  if (!Storage.openFileForRead("SDCF", filePath_, file)) return finish(CJK_WHY_OPEN);
  opened = true;

  // 2) 標準字寬＝最長區段開頭一批裡過半的那個值（Boyer–Moore 多數決，再數一次確認真的過半）。
  {
    intervalAt(bestIv, first, last, offset);
    const uint32_t lo = std::max(first, CJK_FAST_FIRST);
    const uint32_t n = std::min(bestSpan, ioRecords);
    const uint32_t glyphIdx = offset + (lo - first);
    if (esp_timer_get_time() - t0 > CJK_SCAN_DEADLINE_US) return finish(CJK_WHY_SLOW);  // 開檔本身可能就很慢
    if (!file.seekSet(s.glyphsFileOffset + glyphIdx * sizeof(EpdGlyph))) return finish(CJK_WHY_SEEK);
    if (file.read(io, n * sizeof(EpdGlyph)) != static_cast<int>(n * sizeof(EpdGlyph))) return finish(CJK_WHY_READ);
    uint16_t cand = 0;
    uint32_t votes = 0;
    for (uint32_t k = 0; k < n; k++) {
      const uint16_t adv = advAt(k);
      if (votes == 0) {
        cand = adv;
        votes = 1;
      } else if (adv == cand) {
        ++votes;
      } else {
        --votes;
      }
    }
    uint32_t occ = 0;
    for (uint32_t k = 0; k < n; k++) {
      if (advAt(k) == cand) ++occ;
    }
    if (occ * 2 <= n) return finish(CJK_WHY_NOMAJ);
    if (cand == 0) return finish(CJK_WHY_ZERO);
    c.uniform = cand;
  }

  // 3) 整段循序讀，記下每個字寬不等於標準值的碼位（區段依碼位排序 → 例外自然排好序）。
  uint64_t rawSum = 0;
  uint32_t filePosIdx = UINT32_MAX;  // 檔案位置（以字形索引計）；取樣之後未知，第一段一定 seek
  for (uint32_t i = 0; i < s.header.intervalCount; i++) {
    intervalAt(i, first, last, offset);
    if (last < CJK_FAST_FIRST) continue;
    if (first > CJK_FAST_LAST) break;
    const uint32_t lo = std::max(first, CJK_FAST_FIRST);
    const uint32_t hi = std::min(last, CJK_FAST_LAST);
    uint32_t glyphIdx = offset + (lo - first);
    uint32_t cp = lo;
    uint32_t remaining = hi - lo + 1;
    if (glyphIdx != filePosIdx && !file.seekSet(s.glyphsFileOffset + glyphIdx * sizeof(EpdGlyph))) {
      return finish(CJK_WHY_SEEK);
    }
    while (remaining > 0) {
      if (esp_timer_get_time() - t0 > CJK_SCAN_DEADLINE_US) return finish(CJK_WHY_SLOW);
      const uint32_t n = std::min(remaining, ioRecords);
      if (file.read(io, n * sizeof(EpdGlyph)) != static_cast<int>(n * sizeof(EpdGlyph))) return finish(CJK_WHY_READ);
      for (uint32_t k = 0; k < n; k++, cp++) {
        const uint16_t adv = advAt(k);
        rawSum += adv;
        if (adv == c.uniform) continue;
        if (c.exceptionCount >= CJK_MAX_EXCEPTIONS) return finish(CJK_WHY_EXC);
        c.exceptions[c.exceptionCount].cpOffset = static_cast<uint16_t>(cp - CJK_FAST_FIRST);
        c.exceptions[c.exceptionCount].advanceX = adv;
        ++c.exceptionCount;
      }
      remaining -= n;
      glyphIdx += n;
      if (buildProbeHook_) buildProbeHook_(7);  // 每批約 16KB：讓建置探針（含按鍵輪詢）照常跑
      if (abortFn && abortFn(abortCtx)) return finish(CJK_WHY_ABORT);  // v255
    }
    filePosIdx = glyphIdx;
  }

  // 4) 總和核對。
  {
    uint64_t expect = static_cast<uint64_t>(c.uniform) * (c.covered - c.exceptionCount);
    for (uint16_t k = 0; k < c.exceptionCount; k++) expect += c.exceptions[k].advanceX;
    if (expect != rawSum) return finish(CJK_WHY_SUM);
  }

  // 5) 抽驗：均勻 16 個字＋全部例外（最多 32 個）。查找要 READY 才回答；任何失敗 finish 會改掉狀態。
  c.state = CJK_SCAN_READY;
  const auto verifyOne = [&](const uint32_t cp) -> uint8_t {
    if (esp_timer_get_time() - t0 > CJK_SCAN_DEADLINE_US) return CJK_WHY_SLOW;
    if (abortFn && abortFn(abortCtx)) return CJK_WHY_ABORT;  // v255
    const int32_t gi = findGlobalGlyphIndex(s, cp);
    if (gi < 0) return CJK_WHY_VERIFY;
    EpdGlyph g{};
    if (!file.seekSet(s.glyphsFileOffset + static_cast<uint32_t>(gi) * sizeof(EpdGlyph))) return CJK_WHY_VREAD;
    if (file.read(reinterpret_cast<uint8_t*>(&g), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) return CJK_WHY_VREAD;
    uint16_t fast = 0;
    if (!cjkAdvanceLookup(si, cp, &fast) || fast != g.advanceX) return CJK_WHY_VERIFY;
    ++c.verified;
    return CJK_WHY_OK;
  };
  for (uint32_t j = 0; j < CJK_VERIFY_SAMPLES; j++) {
    uint32_t ord = static_cast<uint32_t>(static_cast<uint64_t>(j) * (c.covered - 1) / (CJK_VERIFY_SAMPLES - 1));
    uint32_t cp = 0;
    for (uint32_t i = 0; i < s.header.intervalCount; i++) {
      intervalAt(i, first, last, offset);
      if (last < CJK_FAST_FIRST) continue;
      if (first > CJK_FAST_LAST) break;
      const uint32_t lo = std::max(first, CJK_FAST_FIRST);
      const uint32_t span = std::min(last, CJK_FAST_LAST) - lo + 1;
      if (ord < span) {
        cp = lo + ord;
        break;
      }
      ord -= span;
    }
    const uint8_t why = verifyOne(cp);
    if (why != CJK_WHY_OK) return finish(why);
  }
  for (uint16_t k = 0; k < c.exceptionCount; k++) {
    const uint8_t why = verifyOne(CJK_FAST_FIRST + c.exceptions[k].cpOffset);
    if (why != CJK_WHY_OK) return finish(why);
  }

  // 6) 表裡在掃描之前讀進來的範圍內項目已經多餘（值相同），壓掉，讓出格子給標點與範圍外的字。
  //    穩定的原地過濾：剩下的仍依碼位排序。
  if (AdvanceEntry* const tbl = advanceTable_[si]) {
    uint32_t w = 0;
    for (uint32_t r = 0; r < advanceTableSize_[si]; r++) {
      if (cjkAdvanceLookup(si, tbl[r].codepoint, nullptr)) continue;
      tbl[w++] = tbl[r];
    }
    advanceTableSize_[si] = w;
  }
  finish(CJK_WHY_OK);
}

void SdCardFont::publishAdvScanWitness() const {
  // 一行含所有掃過的字面，開頭是字型的內容雜湊（區分換字型／換字級）與這個 task 的堆疊最低剩餘量（hwm，bytes：
  // 掃描在很深的排版呼叫鏈裡跑，codex 要求實機量）。每個字面最長 54 字元
  // （" s3=rt:runtime u=65535 e=32+ n=20992 ms=99999 v=48 t=1"），四個＋開頭 240 < 255；超過時最後一字元改成 '~'。
  // line 用 static：這個函式在排版／畫頁的深處被呼叫，256B 放堆疊不划算；呼叫端（掃描、prewarm 核對）都在 RenderLock 內串行。
  static char line[sizeof(lastAdvScan)];
  int n = snprintf(line, sizeof(line), "font=%08x hwm=%u", static_cast<unsigned>(contentHash_),
                   static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  size_t pos = n > 0 ? static_cast<size_t>(n) : 0;
  bool truncated = false;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    const CjkAdvance& c = cjk_[i];
    if (c.state == CJK_SCAN_NONE && c.retries == 0 && c.scanMs == 0 && c.why == CJK_WHY_OK) continue;  // 還沒掃過
    const char* const st = c.state == CJK_SCAN_READY ? "ok" : (c.state == CJK_SCAN_UNUSABLE ? "na" : "rt");
    const char* const why = c.why < sizeof(CJK_WHY_NAME) / sizeof(CJK_WHY_NAME[0]) ? CJK_WHY_NAME[c.why] : "?";
    const unsigned ms = c.scanMs > 99999 ? 99999u : static_cast<unsigned>(c.scanMs);
    n = snprintf(line + pos, sizeof(line) - pos, " s%u=%s%s%s u=%u e=%u%s n=%u ms=%u v=%u t=%u", static_cast<unsigned>(i),
                 st, c.state == CJK_SCAN_READY ? "" : ":", c.state == CJK_SCAN_READY ? "" : why,
                 static_cast<unsigned>(c.uniform), static_cast<unsigned>(c.exceptionCount),
                 c.why == CJK_WHY_EXC ? "+" : "", static_cast<unsigned>(c.covered), ms,
                 static_cast<unsigned>(c.verified), static_cast<unsigned>(c.retries));
    if (n < 0) break;
    if (pos + static_cast<size_t>(n) >= sizeof(line)) {
      truncated = true;
      pos = sizeof(line) - 1;
      break;
    }
    pos += static_cast<size_t>(n);
  }
  if (truncated) line[sizeof(line) - 2] = '~';
  // 新的一行含所有字面的狀態，所以先丟掉還沒被讀走的舊行再放（Breadcrumb 本身是先到先得）。
  char discard[1];
  breadcrumbTake(lastAdvScan, sizeof(lastAdvScan), discard, sizeof(discard));
  breadcrumbPublish(lastAdvScan, sizeof(lastAdvScan), line);
}

void SdCardFont::clearPersistentCache() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    delete[] advanceTable_[i];
    advanceTable_[i] = nullptr;
    advanceTableSize_[i] = 0;
  }
  // v253：掃描結果跟著表一起丟。READY 的前提是那個字面的表已配置（觸發條件），
  // 表沒了還留著 READY，renderer 的 hasAdvanceTable() 閘就可能擋掉快路徑、而 fetch 又略過漢字。
  resetCjkAdvances();
}

uint32_t SdCardFont::resetAdvanceTables(uint32_t* const keptOut) {
  // v193：一次配滿 768 格之後就地重用；只把 size 歸零，下一章直接往同一塊寫。
  // v253：CJK 掃描就緒的字面，表裡只剩標點、拉丁字母與範圍外的字 —— 整本書就那一兩百個。
  //   每章清掉只是讓下一章把同一批再從 SD 讀一次（一筆一次 seek＋read）。不到 1/4 滿就留著（新章仍有
  //   ≥577 格）；超過才照 v193 清（清的理由是「表滿會把新章的字擋在外面」）。
  uint32_t used = 0;
  uint32_t kept = 0;
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    used += advanceTableSize_[i];
    if (cjk_[i].state == CJK_SCAN_READY && advanceTableSize_[i] < ADVANCE_CACHE_LIMIT / 4) {
      kept += advanceTableSize_[i];
      continue;
    }
    advanceTableSize_[i] = 0;
  }
  if (keptOut) *keptOut = kept;
  return used;
}

bool SdCardFont::advanceTableLookup(uint8_t styleIdx, uint32_t codepoint, uint16_t* outAdvance) const {
  const AdvanceEntry* table = advanceTable_[styleIdx];
  const uint32_t size = advanceTableSize_[styleIdx];
  if (!table || size == 0) return false;
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    if (outAdvance) *outAdvance = table[lo].advanceX;
    return true;
  }
  return false;
}

void SdCardFont::mergeIntoAdvanceTable(uint8_t styleIdx, const AdvanceEntry* sortedNew, uint32_t newCount) {
  if (newCount == 0) return;
  uint32_t oldSize = advanceTableSize_[styleIdx];
  if (oldSize >= ADVANCE_CACHE_LIMIT) {
    // v192：表滿拒收整批新項（newCount>0 才走到這裡）；不是擠掉舊項。
    ++advanceRejectCount_;
    return;
  }

  // v55：一次配足終局大小，之後【原地】反向合併 —— 不再「配新的 → 複製 → 刪舊的」。
  //
  // 為什麼：舊寫法每次成長都要一塊更大的連續空間，新配置永遠不可能重用剛釋放的舊洞，
  // 於是一路往堆積前方走。閱讀穩態下主池最大塊只有約 3.7KB，所以表長到 3.7KB 之後每次
  // 成長都落進【備援池 p2】，長到 768 筆上限後就永遠不再重配也不釋放 —— 一塊約 9KB 的
  // 長壽殘骸卡在 p2 中段，把最大連續塊【從 115,616 砍到 42,312】（v54 實機實測，
  // 整個 session 數值一字不變；v58/v59/v60/v126 四份 POOL 傾印佐證修後為「兩顆約 6,400B
  // 的表靠在池頭、無楔子」）。而每頁字圖需要 45,350 → 必然失敗 → 整頁字型降級 → 25 秒翻頁。
  //
  // 一次配足 6,144B 峰值不變（那本來就是終局值，只是提前到位），但配置只發生一次、
  // 位置固定，不再製造遞增的洞。
  //
  // ⚠️⚠️ 【這兩半必須同時存在】只搬「原地反向合併」而不搬「一次配滿 ADVANCE_CACHE_LIMIT」，
  //    就會在 mergedCap > oldSize 時寫出配置範圍外 —— 堆積越界寫，與殺掉第一台機器的
  //    v131 同型（那次也是「配置大小」與「寫入大小」的隱含前提脫鉤）。
  //
  // 2026-08-25 實機 crash 佐證（v137，中文 EPUB 讀約 95 秒後）：
  //   [SDCF] buildAdvanceTable: failed to allocate codepoint buffer (16384 bytes)  ×6
  //   接著 std::__new_allocator<FootnoteEntry>::allocate -> operator new -> terminate -> abort()
  // 最大連續塊已掉到 16KB 以下。上游用的正是被 v55 否決掉的成長搬遷寫法。
  if (!advanceTable_[styleIdx]) {
    advanceTable_[styleIdx] = new (std::nothrow) AdvanceEntry[ADVANCE_CACHE_LIMIT];
    if (!advanceTable_[styleIdx]) {
      LOG_ERR("SDCF", "advanceTable: alloc failed (%u entries) style %u", ADVANCE_CACHE_LIMIT, styleIdx);
      return;
    }
    advanceTableSize_[styleIdx] = 0;
    oldSize = 0;
  }

  AdvanceEntry* const tbl = advanceTable_[styleIdx];
  uint32_t mergedCap = oldSize + newCount;
  if (mergedCap > ADVANCE_CACHE_LIMIT) mergedCap = ADVANCE_CACHE_LIMIT;

  // 反向合併：兩個來源都已排序，從尾端往前寫進同一塊緩衝。寫入位置 k-1 恆 >= 讀取位置 i-1
  // （k >= i 在整個迴圈都成立），所以不會覆寫還沒讀到的舊資料 —— 原地合併成立的關鍵不變量。
  //
  // 超過上限時【丟棄尾端】，與上游的前向實作逐位元組等價（桌面對拍 7,000 組含重複碼位全同）：
  // 前向填充從最小的開始、填滿即停 => 保留最小的 mergedCap 個；
  // 反向填充天然保留「最大的 mergedCap 個」，故要先空轉跳過最大的 drop 個。
  uint32_t i = oldSize;
  uint32_t j = newCount;
  uint32_t k = mergedCap;
  uint32_t drop = (oldSize + newCount > mergedCap) ? (oldSize + newCount - mergedCap) : 0;
  while (drop > 0 && (i > 0 || j > 0)) {
    if (i > 0 && (j == 0 || tbl[i - 1].codepoint > sortedNew[j - 1].codepoint)) {
      --i;
      ++advanceEvictCount_;  // v192：超量合併丟掉既有高碼位項；與表滿拒收（areject）不是同一條路
    } else {
      --j;
      ++advanceRejectCount_;  // v192：超量合併丟掉新項目，同樣是新字進不去
    }
    --drop;
  }
  while (k > 0 && (i > 0 || j > 0)) {
    if (i > 0 && (j == 0 || tbl[i - 1].codepoint > sortedNew[j - 1].codepoint)) {
      tbl[--k] = tbl[--i];
    } else {
      tbl[--k] = sortedNew[--j];
    }
  }
  // 來源總數 < mergedCap 時 k>0，前 k 格是未使用的殘留，把有效資料往前搬齊。
  if (k > 0) {
    for (uint32_t m = k; m < mergedCap; m++) tbl[m - k] = tbl[m];
    mergedCap -= k;
  }
  advanceTableSize_[styleIdx] = mergedCap;
}

bool SdCardFont::hasAdvanceTable() const {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (advanceTable_[i]) return true;
  }
  return false;
}

uint16_t SdCardFont::getAdvance(uint32_t codepoint, uint8_t style) const {
  style &= (MAX_STYLES - 1);
  uint16_t cjkAdv = 0;
  if (cjkAdvanceLookup(style, codepoint, &cjkAdv)) {  // v253：掃描過的漢字，值與表／逐筆讀相同
    ++advanceCjkHitCount_;
    return cjkAdv;
  }
  if (!advanceTable_[style]) {
    ++advanceMissCount_;  // v192：量測路徑沒命中才可能去打 SD
    return 0;
  }
  const AdvanceEntry* table = advanceTable_[style];
  const uint32_t size = advanceTableSize_[style];
  // Binary search sorted by codepoint
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    return table[lo].advanceX;
  }
  ++advanceMissCount_;
  return 0;
}

// Given a sorted array of unique codepoints, resolve glyph indices per style,
// batch-read advanceX from SD, and merge into the persistent advance table.
// Caller owns the codepoints buffer.
int SdCardFont::fetchAdvancesForCodepoints(uint32_t* codepoints, uint32_t cpCount, uint8_t styleMask) {
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    const auto& s = styles_[si];

    // Stop fetching once the cache is full — further inserts would be dropped
    // by the merge anyway. The renderer fast path tolerates missing entries
    // (returns 0); the slow path is still correct for those codepoints.
    if (advanceTableSize_[si] >= ADVANCE_CACHE_LIMIT) {
      for (uint32_t i = 0; i < cpCount; i++) {
        if (cjkAdvanceLookup(si, codepoints[i], nullptr)) continue;  // v253：掃描已涵蓋，不需要進表
        if (advanceTableLookup(si, codepoints[i], nullptr)) continue;
        // v192：確定有新碼位要插、表滿插不進才算；整批早已在表裡不准假陽性。
        ++advanceRejectCount_;
        break;
      }
      continue;
    }

    // For each codepoint in `codepoints`, skip those already cached, then
    // resolve to a glyph index. Build a parallel array sorted by glyph index
    // for sequential SD reads.
    struct CpIdx {
      uint32_t codepoint;
      int32_t glyphIndex;
    };
    std::unique_ptr<CpIdx[]> mappings(new (std::nothrow) CpIdx[cpCount]);
    if (!mappings) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate mappings for style %u", si);
      totalMissed += cpCount;
      continue;
    }

    uint32_t needCount = 0;
    uint32_t missedThisStyle = 0;
    const int32_t replacementIdx = findGlobalGlyphIndex(s, REPLACEMENT_GLYPH);
    for (uint32_t i = 0; i < cpCount; i++) {
      const uint32_t cp = codepoints[i];
      if (cjkAdvanceLookup(si, cp, nullptr)) continue;    // v253：掃描已涵蓋
      if (advanceTableLookup(si, cp, nullptr)) continue;  // already cached
      int32_t idx = findGlobalGlyphIndex(s, cp);
      if (idx < 0) {
        if (replacementIdx < 0) {
          missedThisStyle++;
          continue;
        }
        idx = replacementIdx;
      }
      mappings[needCount].codepoint = cp;
      mappings[needCount].glyphIndex = idx;
      needCount++;
    }
    totalMissed += static_cast<int>(missedThisStyle);

    if (needCount == 0) continue;

    // Sort by glyph index so SD reads are mostly sequential.
    std::sort(mappings.get(), mappings.get() + needCount,
              [](const CpIdx& a, const CpIdx& b) { return a.glyphIndex < b.glyphIndex; });

    // v253：這一段（開檔到關檔，含所有出口）的時間與讀取筆數進 BUILDPROF 的 fetchms／fetchn。
    struct FetchProf {
      int64_t t0 = esp_timer_get_time();
      uint32_t reads = 0;
      ~FetchProf() {
        SdCardFont::advanceFetchUs_ += static_cast<uint64_t>(esp_timer_get_time() - t0);
        SdCardFont::advanceFetchReadCount_ += reads;
      }
    } fetchProf;

    // v255：用常駐的共用檔柄（sharedFile_），不再每次呼叫、每個字重各開一次檔。
    //   v254 實機：一章 110 筆讀取卻花 1,197ms（≈11ms／筆）—— 一段只補一兩個新字也付一次 12–18ms 的開檔（A-4）。
    //   sharedFile_ 本來就被 prewarmStyle 與 onGlyphMiss（排版量字寬的退路也走它）共用，
    //   都在 RenderLock 串行之下；這裡每次讀取前都自己 seek（lastReadIndex 初值 INT32_MIN），不依賴前一個使用者留下的位置。
    std::unique_ptr<AdvanceEntry[]> staged(new (std::nothrow) AdvanceEntry[needCount]);
    if (!staged) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate staging for style %u", si);
      continue;
    }
    const SharedFileLock fileLock(*this);
    if (!ensureFileOpen()) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to open .cpfont for style %u", si);
      continue;
    }
    HalFile& file = sharedFile_;

    uint32_t fetched = 0;
    EpdGlyph tempGlyph;
    for (uint32_t i = 0; i < needCount; i++) {
      int32_t gIdx = mappings[i].glyphIndex;
      uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
      // v255：每一筆都自己 seek（原本連號的字省略 seek）—— 迴圈尾端的 buildProbeHook_ 回呼出去，
      //   不依賴「回來時檔案位置還在上一筆後面」（codex 複查）。這裡的字本來就散，省略 seek 的機會很少。
      if (!file.seekSet(fileOff)) {
        LOG_ERR("SDCF", "buildAdvanceTable: failed to seek to glyph %d (style %u)", gIdx, si);
        dropSharedFile();
        break;
      }
      if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
        LOG_ERR("SDCF", "buildAdvanceTable: short glyph read (style %u, glyph %d)", si, gIdx);
        dropSharedFile();
        break;
      }
      ++fetchProf.reads;
      // v253：掃描的觸發條件只算【成功讀到】的範圍內字（codex：算「打算讀的」會在讀取失敗時提早觸發）。
      // mappings 裡的 glyphIndex 可能是替代字形（字型沒有這個字），那種不算。
      if (mappings[i].codepoint >= CJK_FAST_FIRST && mappings[i].codepoint <= CJK_FAST_LAST &&
          gIdx == findGlobalGlyphIndex(s, mappings[i].codepoint) && cjk_[si].oldPathCjk < UINT32_MAX) {
        ++cjk_[si].oldPathCjk;
      }
      staged[fetched].codepoint = mappings[i].codepoint;
      staged[fetched].advanceX = tempGlyph.advanceX;
      fetched++;
      // v191：打點必須在 seek+read 【之後】——打在迴圈開頭時，單一 glyph 的那次讀取（以及每批最後一個）
      // 沒有後續 site 7 收尾，空窗會被下一個字寬探針收走而繼續誤報成 gapsite=2，儀器目的落空（複查抓到）。
      if (buildProbeHook_) buildProbeHook_(7);
    }

    if (fetched > 0) {
      // Sort staged by codepoint, then merge into the persistent table.
      std::sort(staged.get(), staged.get() + fetched,
                [](const AdvanceEntry& a, const AdvanceEntry& b) { return a.codepoint < b.codepoint; });
      mergeIntoAdvanceTable(si, staged.get(), fetched);
    }

    LOG_DBG("SDCF", "Advance table style %u: +%u from SD, total=%u/%u", si, fetched, advanceTableSize_[si],
            ADVANCE_CACHE_LIMIT);
  }

  return totalMissed;
}

template <typename Iter>
int SdCardFont::buildAdvanceTableRange(Iter begin, Iter end, const std::vector<EpdFontFamily::Style>* wordStyles,
                                       bool includeSpace, bool includeHyphen, uint8_t styleMask, const char* extraText,
                                       bool (*cpFilter)(uint32_t)) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // +2 reserved slots for space and hyphen injected after the main scan.
  static constexpr uint32_t MAX_UNIQUE_CODEPOINTS = 4096;
  // v164：常駐化（v55「一次配滿、就地重用」手法）。原本每次呼叫都 new/delete 16KB —— 
  // 實機 diag（v161/162 段 12 次 SDCFFAIL codepoint-buf）證明建置視窗裡這顆 16KB 連續塊
  // 常常賭輸（最緊時 defMax 只剩 3,060），輸了整頁 advance 表就退回逐字 SD 慢路徑。
  // 改成第一次成功配置後就留著（freeAll 釋放）：配置時機在開書早期、堆積寬鬆，
  // 之後建置視窗裡不再有這顆反覆出現的峰值。UI 備援字型不做版面量測，永遠不配。
  if (!cpScratch_) {
    cpScratch_ = new (std::nothrow) uint32_t[MAX_UNIQUE_CODEPOINTS + 2];
  }
  uint32_t* codepoints = cpScratch_;
  if (!codepoints) {
    // v150：附上【失敗當下】的堆積數字。三次實機（v141/v148/v149）都出現「量測點顯示
    // p2 有 26–80KB，274–450ms 後這個 16KB 配置卻失敗」—— 只有失敗現場的數字能分辨
    // 「瞬時被吃光」與「這個配置根本用不到 p2」兩個假說。
    {
      const unsigned dm = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
      const unsigned df = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate codepoint buffer (%u bytes) maxAlloc=%u free=%u",
              MAX_UNIQUE_CODEPOINTS * 4, dm, df);
      noteAllocFail("codepoint-buf", MAX_UNIQUE_CODEPOINTS * 4, dm, df);
    }
    return -1;
  }

  // v253：先掃描、再收集碼位 —— 掃描借用 cpScratch_ 當讀取緩衝，不另配記憶體。
  // 條件：這個字面舊路徑成功讀過 ≥128（內文）／256（其他）個範圍內字、表已配置（READY 之後 renderer 的 hasAdvanceTable 閘必過）、
  // 距上一次掃描 ≥2 秒（同一個建置步驟裡不背對背掃兩個字面）。一次呼叫最多掃一個。
  static_assert(CJK_SCAN_SCRATCH_SLOTS == MAX_UNIQUE_CODEPOINTS + 2, "scan buffer must match cpScratch_ size");
  // v255：只在呼叫端允許的時候掃（背景排版的 tick、txt 閱讀停留時的預取）。v254 實機：內文掃描 743ms 落在
  //   翻到新章的同步排版裡（使用者正在等那一頁），txt 則落在翻頁的排版裡。條件滿足但不允許 → 記一筆 deferred，
  //   這一段照舊逐字讀，留給下一個允許的時機。
  bool scanWindow = false;
  bool (*abortFn)(void*) = nullptr;
  void* abortCtx = nullptr;
  {
    const TaskHandle_t me = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&g_scanWindowMux);
    if (scanWindowDepth_ > 0 && scanWindowOwner_ == me) {
      scanWindow = true;
      abortFn = scanAbortHook_;
      abortCtx = scanAbortCtx_;
    }
    portEXIT_CRITICAL(&g_scanWindowMux);
  }
  if (!cjkScannedOnce_ || millis() - lastCjkScanEndMs_ >= CJK_SCAN_SPACING_MS) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present || !advanceTable_[si]) continue;
      const uint32_t trigger = si == resolveStyle(EpdFontFamily::REGULAR) ? CJK_SCAN_TRIGGER_BODY : CJK_SCAN_TRIGGER;
      if (cjk_[si].state != CJK_SCAN_NONE || cjk_[si].oldPathCjk < trigger) continue;
      if (!scanWindow || (abortFn && abortFn(abortCtx))) {
        ++advanceScanDeferred_;  // 不在允許的時機，或已經有按鍵／畫面在等
        break;
      }
      scanCjkAdvances(si, reinterpret_cast<uint8_t*>(codepoints), CJK_SCAN_IO_RECORDS, abortFn, abortCtx);
      break;
    }
  }

  // v254：有逐字字重（wordStyles）時，每個實際字面只收【自己那些字】的碼位 —— 一段裡幾個粗體字，
  //   不再讓整段每個字都去讀一次粗體字寬（直排原本更是一律四個字重全要）。
  //   空白、連字號、extraText 照舊給每個字面：空白用前一個字的字重量、連字號用該字的字重量。
  //   表裡少放哪些字只影響速度：所有讀字寬的地方查不到都會退回讀字形紀錄（v254 起空白寬度也是）。
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    uint8_t fetchMask;
    if (wordStyles) {
      if (!(styleMask & (1u << si))) continue;
      fetchMask = static_cast<uint8_t>(1u << si);
    } else {
      if (si > 0) break;  // 沒有逐字字重：一次收全部、照遮罩讀（原本的行為）
      fetchMask = styleMask;
    }
    uint32_t cpCount = 0;
    bool hitCap = false;
    size_t idx = 0;
    for (auto it = begin; it != end && !hitCap; ++it, ++idx) {
      if (wordStyles && resolveStyle(static_cast<uint8_t>((*wordStyles)[idx]) & 0x03) != si) continue;
      hitCap = collectUniqueCodepoints(asCStr(*it), codepoints, cpCount, MAX_UNIQUE_CODEPOINTS, cpFilter);
    }
    if (extraText && !hitCap) {
      hitCap = collectUniqueCodepoints(extraText, codepoints, cpCount, MAX_UNIQUE_CODEPOINTS, cpFilter);
    }

    if (includeSpace && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == ' '; }))
      codepoints[cpCount++] = ' ';
    if (includeHyphen && std::none_of(codepoints, codepoints + cpCount, [](uint32_t c) { return c == '-'; }))
      codepoints[cpCount++] = '-';

    if (hitCap) {
      LOG_ERR("SDCF", "buildAdvanceTable: unique codepoint cap (%u) hit, layout may be approximate",
              MAX_UNIQUE_CODEPOINTS);
    }
    std::sort(codepoints, codepoints + cpCount);
    totalMissed += fetchAdvancesForCodepoints(codepoints, cpCount, fetchMask);
  }
  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::buildAdvanceTable(const char* utf8Text, uint8_t styleMask, const char* extraText) {
  return buildAdvanceTableRange(&utf8Text, &utf8Text + 1, nullptr, false, false, styleMask, extraText);
}

int SdCardFont::buildAdvanceTable(const std::deque<std::string>& words, bool includeHyphen, uint8_t styleMask,
                                  const char* extraText) {
  return buildAdvanceTableRange(words.begin(), words.end(), nullptr, words.size() > 1, includeHyphen, styleMask,
                                extraText);
}

int SdCardFont::buildAdvanceTable(const std::deque<std::string>& words,
                                  const std::vector<EpdFontFamily::Style>& wordStyles, bool includeHyphen,
                                  const char* extraText, bool (*cpFilter)(uint32_t)) {
  if (wordStyles.size() != words.size()) {
    // 平行陣列對不上（不該發生）：退回不分字重、全部字重都準備（最保守，只是慢）。
    // v262（codex）：過濾要一起帶下去，否則直排在這條退路又會替每個漢字讀一次。
    return buildAdvanceTableRange(words.begin(), words.end(), nullptr, words.size() > 1, includeHyphen, 0x0F, extraText,
                                  cpFilter);
  }
  // 內文字面一律在內：首行縮排固定用內文的空白量（ParsedText::resolveFirstLineIndent），全粗體段落也一樣
  // （codex 複查：只準備粗體的話，每個全粗體段落都要為內文空白讀一次 SD）。
  uint8_t styleMask = static_cast<uint8_t>(1u << EpdFontFamily::REGULAR);
  for (const auto s : wordStyles) styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(s) & 0x03));
  // 空白一律準備（不看字數）：單字段落也會量縮排。表裡有沒有它只影響速度（查不到會讀字形）。
  return buildAdvanceTableRange(words.begin(), words.end(), &wordStyles, true, includeHyphen, styleMask, extraText,
                                cpFilter);
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  style &= (MAX_STYLES - 1);
  if (!styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return styles_[style & (MAX_STYLES - 1)].present; }

uint8_t SdCardFont::resolveStyle(uint8_t style) const {
  static const uint8_t kFallbacks[MAX_STYLES][MAX_STYLES] = {
      // REGULAR: REGULAR -> BOLD -> ITALIC -> BOLD_ITALIC
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC},
      // BOLD: BOLD -> REGULAR -> BOLD_ITALIC -> ITALIC
      {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::ITALIC},
      // ITALIC: ITALIC -> REGULAR -> BOLD_ITALIC -> BOLD
      {EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD},
      // BOLD_ITALIC: BOLD_ITALIC -> BOLD -> ITALIC -> REGULAR
      {EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR},
  };

  const uint8_t styleBits = style & (MAX_STYLES - 1);
  for (uint8_t candidate : kFallbacks[styleBits]) {
    if (styles_[candidate].present) return candidate;
  }
  return EpdFontFamily::REGULAR;
}

uint8_t SdCardFont::resolveStyleMask(uint8_t styleMask) const {
  uint8_t resolvedMask = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (styleMask & (1 << si)) {
      resolvedMask |= static_cast<uint8_t>(1u << resolveStyle(si));
    }
  }
  return resolvedMask;
}

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  const auto& s = self->styles_[styleIdx];
  if (!s.fullIntervals && !s.bmpIntervals) return nullptr;

  // Check overflow cache first (matching both codepoint and style)
  for (uint32_t i = 0; i < self->overflowCount_; i++) {
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx) {
      return &self->overflow_[i].glyph;
    }
  }

  // Look up global glyph index via full intervals
  int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails. Bookkeeping (count/next)
  // is deferred until after all I/O succeeds to avoid inconsistent state.
  uint32_t slot = self->overflowNext_;
  bool wasAtCapacity = (self->overflowCount_ == OVERFLOW_CAPACITY);

  // Read glyph metadata into temporary
  // v154：共用檔柄 —— miss 路徑原本每個字開一次檔（12–18ms），
  // 降級階梯丟出來的字全走這裡，這是 27 秒頁的第二半。
  struct AsdProf {  // v252：只在建置探測（asd 有計數）時計時；所有出口都計
    int64_t t0;
    bool on;
    explicit AsdProf(bool active) : t0(active ? esp_timer_get_time() : 0), on(active) {}
    ~AsdProf() {
      if (on) SdCardFont::advanceSdReadUs_ += static_cast<uint64_t>(esp_timer_get_time() - t0);
    }
  } asdProf(advanceSdProbeDepth_ > 0);
  if (advanceSdProbeDepth_ > 0) {
    ++advanceSdReadCount_;  // v192：計嘗試次數，讀失敗也算；繪製 miss／overflow 命中不算
  }

  const SharedFileLock fileLock(*self);  // v255
  if (!self->ensureFileOpen()) {
    LOG_ERR("SDCF", "Overflow: failed to open .cpfont");
    return nullptr;
  }

  EpdGlyph tempGlyph = {};
  uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  if (!self->sharedFile_.seekSet(glyphFileOff)) {
    LOG_ERR("SDCF", "Overflow: failed to seek to glyph for U+%04X style %u", codepoint, styleIdx);
    self->dropSharedFile();  // v255
    
    return nullptr;
  }
  if (self->sharedFile_.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    self->dropSharedFile();  // v255
    return nullptr;
  }

  // Read bitmap data into temporary (if any)
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength > 0) {
    // v167（crash_report166 定案）：事前檢查，不能只靠 nothrow —— libstdc++ 的 nothrow
    // 版是「呼叫丟例外版再 catch」，堆積低到連 130B 的例外物件都配不出來時，
    // __cxa_allocate_exception 直接 terminate（v93 實測教訓）。miss ring 在渲染最缺
    // 記憶體的時刻每個字都要進來一次，這裡是全機最高頻的輪盤。512B 餘裕給例外機制。
    if (heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) <
        static_cast<size_t>(tempGlyph.dataLength) + 512) {
      LOG_ERR("SDCF", "Overflow: heap floor, skip U+%04X (%u bytes)", codepoint, tempGlyph.dataLength);
      return nullptr;
    }
    tempBitmap = new (std::nothrow) uint8_t[tempGlyph.dataLength];
    if (!tempBitmap) {
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      return nullptr;
    }
    if (!self->sharedFile_.seekSet(s.bitmapFileOffset + tempGlyph.dataOffset)) {
      LOG_ERR("SDCF", "Overflow: failed to seek to bitmap for U+%04X", codepoint);
      self->dropSharedFile();  // v255
      delete[] tempBitmap;
      
      return nullptr;
    }
    if (self->sharedFile_.read(tempBitmap, tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      self->dropSharedFile();  // v255
      delete[] tempBitmap;
      return nullptr;
    }
  }

  // All reads succeeded — commit to slot and advance ring buffer
  if (wasAtCapacity) {
    delete[] self->overflow_[slot].bitmap;
  } else {
    self->overflowCount_++;
  }
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = tempBitmap;
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;

  LOG_DBG("SDCF", "Overflow: loaded U+%04X style %u on demand (slot %u/%u)", codepoint, styleIdx, slot,
          OVERFLOW_CAPACITY);

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) {
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }
