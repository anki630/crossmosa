#include "DiagLog.h"
#include <strings.h>  // strcasecmp -- isDiagnosticPath()

#include <Arduino.h>
#include <DataDir.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

namespace {
// 檔案上限:超過就不再追加(避免長期使用把 SD 寫滿;量測版本用不到這麼多)。
constexpr size_t MAX_DIAG_BYTES = 192 * 1024;

// v57:儀器總開關。false 時所有進入點在第一行就 return(見 DiagLog.h 的說明)。
bool enabled_ = false;

// v64:setForced() 的獨立旗標——刻意不去覆寫 enabled_,因為那會讓「哨兵檔本來就在」
// 的卡在離開檔案傳輸模式之後被靜靜關掉儀器(還原成 false),使用者從此拿不到閱讀端的
// log 卻完全看不出為什麼。兩個旗標分開,or 起來才是「現在要不要記」。
bool forced_ = false;
inline bool active() { return enabled_ || forced_; }

// v57:哨兵檔放【SD 根目錄】而不是資料目錄。
// 兩個理由:①資料目錄有 v36 的遷移邏輯——DataDir::resolve() 用一份哨兵清單
// (state.json / settings.json / …)判斷 /.crossmosa 是真資料目錄還是「失敗 rename 的殘骸」。
// 若使用者照指示在【尚未遷移】的卡上手建 /.crossmosa 只為了放 diag.on,那個目錄會被判成殘骸,
// 而 rmdir 在非空目錄上必定失敗 → 遷移被永久擋住。放根目錄就沒有這類互動。
// ②對使用者也簡單得多:直接丟在卡的最上層,不用進隱藏資料夾。
constexpr const char* SENTINEL_PATH = "/diag.on";

const char* diagPath() {
  static char p[64] = {0};
  if (!p[0]) snprintf(p, sizeof(p), "%s/diag.log", DataDir::path());
  return p;
}

// 輪替目的地。只保留一代——這是「不要無聲把卡寫滿」與「不要無聲丟掉上一次的證據」
// 之間的折衷,總量因此固定在 2 × MAX_DIAG_BYTES。
const char* prevDiagPath() {
  static char p[64] = {0};
  if (!p[0]) snprintf(p, sizeof(p), "%s/diag-prev.log", DataDir::path());
  return p;
}

// 一整個檔案傳輸 session 綽綽有餘的餘裕;逼近上限就輪替,而不是等真的撞到才發現。
constexpr size_t FORCED_ROTATE_HEADROOM = 48 * 1024;

void rotateIfNearCap() {
  if (!Storage.ready()) return;
  size_t size = 0;
  {
    // 這個 scope 是必要的:rename 之前一定要先關檔。
    HalFile f = Storage.open(diagPath(), O_RDONLY);
    if (!f) return;  // 還沒有檔,不用輪替
    size = f.size();
  }
  if (size + FORCED_ROTATE_HEADROOM < MAX_DIAG_BYTES) return;

  // 順序很重要:**先 rename、不要先刪上一代**。SdFat 的 rename 是 create-exclusive,
  // 所以目的地已存在時第一次會失敗——那時候才刪上一代、才重試。這個順序保證任何
  // 一步失敗都至少還留著【一代】證據;先刪再 rename 的話,rename 一失敗就兩代全沒。
  // 這是保存證據的程式碼,不該有把證據全毀的路徑。
  if (Storage.rename(diagPath(), prevDiagPath())) {
    LOG_INF("DIAG", "rotated %s at %u bytes", diagPath(), (unsigned)size);
    return;
  }
  Storage.remove(prevDiagPath());
  if (Storage.rename(diagPath(), prevDiagPath())) {
    LOG_INF("DIAG", "rotated %s at %u bytes (previous generation replaced)", diagPath(), (unsigned)size);
    return;
  }
  // 兩次都失敗 = 檔案系統真的有問題。**刻意不 truncate**:此時 diag.log 是僅存的
  // 一代,為了騰出空間寫「大概也寫不進去」的新紀錄而把它毀掉是錯的取捨。
  // append() 的上限檢查會讓機制停下來,但證據還在,而且這行 LOG_ERR 會進 RTC
  // ring buffer(當機也活得下來),所以不是無聲的。
  LOG_ERR("DIAG", "rotate FAILED, %s kept intact at %u bytes -- diagnostics will stop appending", diagPath(),
          (unsigned)size);
}

// 追加一行(自帶換行)。每次開關檔:量測點已節流,FAT 成本可接受,
// 且避免長期持有檔案控制代碼干擾其他 SD 存取。
void append(const char* text) {
  if (!Storage.ready()) return;

  const char* path = diagPath();
  HalFile f = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    // 新卡上資料目錄可能還不存在(DataDir::resolve 刻意不建,由各 store 首次存檔時建),
    // 而 SdFat 的 O_CREAT 不會建父目錄 → 開機基準線會靜默消失。建一次再試。
    Storage.mkdir(DataDir::path());
    f = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
    if (!f) {
      LOG_ERR("DIAG", "Cannot open %s", path);
      return;
    }
  }
  if (f.size() >= MAX_DIAG_BYTES) return;  // 上限保護(f 於 scope 結束自動關閉)
  f.write(reinterpret_cast<const uint8_t*>(text), strlen(text));
  f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  f.flush();
}
}  // namespace

namespace {
// 每個實體堆各自的統計。**不能用 caps 遮罩分辨各池**——複查從 ELF 解出 caps 表證實
// Pool A 的 all_caps(0x180e)是 Pool B(0x580e)的真子集,不存在能單獨選中 Pool A 的遮罩,
// 而 RTC 堆的 prio2 caps 也含 INTERNAL(讓「INTERNAL 減 RETENTION」系統性高估)。
// heap_caps_walk 給的是逐堆真實邊界,才能回答「卡住的是哪一池」。
struct PoolAcc {
  uintptr_t start;
  size_t free;
  size_t largest;
  size_t total;
};
struct WalkCtx {
  PoolAcc pools[6];
  int count;
};

bool walkCb(walker_heap_into_t heap, walker_block_info_t block, void* userData) {
  auto* ctx = static_cast<WalkCtx*>(userData);
  const auto start = static_cast<uintptr_t>(heap.start);
  int idx = -1;
  for (int i = 0; i < ctx->count; i++) {
    if (ctx->pools[i].start == start) idx = i;
  }
  if (idx < 0) {
    if (ctx->count >= 6) return true;
    idx = ctx->count++;
    ctx->pools[idx] = {start, 0, 0, static_cast<size_t>(heap.end - heap.start)};
  }
  if (!block.used) {
    ctx->pools[idx].free += block.size;
    if (block.size > ctx->pools[idx].largest) ctx->pools[idx].largest = block.size;
  }
  return true;  // 繼續走訪
}
}  // namespace

void DiagLog::begin() {
  // 必須在 Storage.begin() 成功且 DataDir::resolve() 之後呼叫(哨兵檔路徑依賴資料目錄)。
  if (!Storage.ready()) {
    enabled_ = false;
    return;
  }
  enabled_ = Storage.exists(SENTINEL_PATH);
  if (enabled_) LOG_INF("DIAG", "Instrumentation ON (%s present)", SENTINEL_PATH);
}

bool DiagLog::setForced(bool on, const char* reason) {
  const bool previous = forced_;
  forced_ = on;
  if (!on || previous) return previous;  // 只在 off->on 那一次做下面的事

  // v65:輪替【不再】以「使用者沒放哨兵檔」為條件。
  //
  // v64 的理由是「哨兵檔是使用者自己放的,他知道那個檔在長大」。那個假設在真實
  // 使用上不成立,而且代價是全部證據:使用者的卡上有 /diag.on,是 v59 那輪
  // 【我叫他放的】(為了量測跨頁字型重疊率),八個版本以來沒有理由再想起它。
  // 於是 diag.log 在 v62 期間長到 192 KB 上限就無聲停寫(append() 的第一行
  // 就 return),v63/v64 的每一行——包含 v64 特地為「第一次連線失敗」加的
  // forced=filetransfer 強制診斷、包含每一個 query_directory/query_info
  // unsupported——全部被丟掉。第一份實機 SMB log 回傳時,裡面連一個 SMB 字
  // 都沒有,而且結構上不可能有。
  //
  // 輪替本來就是「保留一代」而不是丟棄,所以對「使用者正在做長期量測」這個
  // 情境也不是損失:上一代還在 diag-prev.log。用一個會讓機制在最需要它的時候
  // 無聲死掉的條件去保護那個情境,是錯的取捨。
  rotateIfNearCap();
  if (!enabled_) {
    LOG_INF("DIAG", "Instrumentation FORCED ON (no %s needed)", SENTINEL_PATH);
  }
  // 刻意用跟 main.cpp 開機那行一模一樣的 `BOOT version=` 前綴:同一個 grep 就能把
  // append-only 檔案切成版本段落,不論那一段是開機記的還是 setForced 記的。
  line("BOOT version=%s forced=%s", CROSSPOINT_VERSION, reason != nullptr ? reason : "forced");
  return previous;
}

void DiagLog::mem(const char* tag) {
  if (!active()) return;  // heap_caps_walk 就是關掉之後省下的主要成本
  WalkCtx ctx{};
  heap_caps_walk(MALLOC_CAP_INTERNAL, walkCb, &ctx);

  // 一行含逐池 total/free/largest。largest 才是本專案真正的殺手指標
  // (歷史上的 OOM 與 OPDS「failed to fetch」都是連續區塊不足,不是總量不足)。
  char buf[320];  // 逐池欄位 ~48 字元/池 + 聚合欄位;預留到 5 池不截斷
  int n = snprintf(buf, sizeof(buf), "%lu MEM %-14s", static_cast<unsigned long>(millis()), tag);
  for (int i = 0; i < ctx.count && n > 0 && n < static_cast<int>(sizeof(buf)); i++) {
    n += snprintf(buf + n, sizeof(buf) - n, " p%d@%08x t=%u f=%u max=%u", i,
                  static_cast<unsigned>(ctx.pools[i].start), static_cast<unsigned>(ctx.pools[i].total),
                  static_cast<unsigned>(ctx.pools[i].free), static_cast<unsigned>(ctx.pools[i].largest));
  }
  // 聚合值一併留存,方便與歷史版本的 [MEM] log 對照(ESP.getHeapSize/getMaxAllocHeap 同源)
  if (n > 0 && n < static_cast<int>(sizeof(buf))) {
    snprintf(buf + n, sizeof(buf) - n, " | int_total=%u int_free=%u int_max=%u",
             static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  }
  append(buf);
  LOG_INF("DIAG", "%s", buf);
}

namespace {
constexpr int MAX_DUMP_BLOCKS = 16;
struct DumpCtx {
  uintptr_t poolStart;
  unsigned minBytes;
  uint32_t addr[MAX_DUMP_BLOCKS];
  uint32_t size[MAX_DUMP_BLOCKS];
  int count;
  int overflow;      // 超過 MAX_DUMP_BLOCKS 沒記到的筆數
  uint32_t usedSum;  // 該池已用總量(含小塊)
  uint32_t freeLargest;
  uint32_t freeLargestAddr;
};

bool dumpCb(walker_heap_into_t heap, walker_block_info_t block, void* userData) {
  auto* c = static_cast<DumpCtx*>(userData);
  if (static_cast<uintptr_t>(heap.start) != c->poolStart) return true;
  const auto addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block.ptr));
  if (block.used) {
    c->usedSum += block.size;
    if (block.size >= c->minBytes) {
      if (c->count < MAX_DUMP_BLOCKS) {
        c->addr[c->count] = addr;
        c->size[c->count] = static_cast<uint32_t>(block.size);
        c->count++;
      } else {
        c->overflow++;
      }
    }
  } else if (block.size > c->freeLargest) {
    c->freeLargest = static_cast<uint32_t>(block.size);
    c->freeLargestAddr = addr;
  }
  return true;
}
}  // namespace

void DiagLog::dumpPools(unsigned minBytes, const char* tag) {
  if (!active()) return;  // 這支要走兩趟 heap_caps_walk,關掉時絕對不能進來
  // 先用既有的 walk 取得各池邊界,再逐池走第二趟(walk 的 callback 拿不到「這是第幾池」,
  // 只拿得到 heap.start,所以兩趟比在單趟裡塞多池狀態單純且不會爆棧)。
  WalkCtx pools{};
  heap_caps_walk(MALLOC_CAP_INTERNAL, walkCb, &pools);

  for (int i = 0; i < pools.count; i++) {
    if (pools.pools[i].total <= 64 * 1024) continue;  // 只看能供應大配置的池
    DumpCtx c{};
    c.poolStart = pools.pools[i].start;
    c.minBytes = minBytes;
    heap_caps_walk(MALLOC_CAP_INTERNAL, dumpCb, &c);

    // 前綴約 89 字元 + 16 筆 × 每筆 " xxxxxxxx:nnnnn"(~15)= 329;288 會把尾端幾筆吃掉,
    // 而那正是高位址端的區塊——找「卡在池中間的是誰」最不能少的一段。
    char buf[416];
    int n = snprintf(buf, sizeof(buf), "%lu POOL %-10s p@%08x used=%u maxfree=%u@%08x big=%u+%u |",
                     static_cast<unsigned long>(millis()), tag, static_cast<unsigned>(c.poolStart),
                     static_cast<unsigned>(c.usedSum), static_cast<unsigned>(c.freeLargest),
                     static_cast<unsigned>(c.freeLargestAddr), static_cast<unsigned>(c.count),
                     static_cast<unsigned>(c.overflow));
    for (int b = 0; b < c.count && n > 0 && n < static_cast<int>(sizeof(buf)) - 20; b++) {
      n += snprintf(buf + n, sizeof(buf) - n, " %08x:%u", static_cast<unsigned>(c.addr[b]),
                    static_cast<unsigned>(c.size[b]));
    }
    append(buf);
    LOG_INF("DIAG", "%s", buf);
  }
}

void DiagLog::line(const char* fmt, ...) {
  if (!active()) return;
  // v58:放大到 384。加了 reuse/cum_reuse 之後最壞情況已達 252 字元,對 256 只剩 4 bytes——
  // 而新欄位都在【行尾】,截斷會剛好吃掉要量的東西(這正是原註解警告的情況再次發生),
  // 且 cum_reuse 會隨閱讀時間變長。
  char body[384];
  va_list args;
  va_start(args, fmt);
  vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);

  char buf[416];  // body + "<millis> " 前綴
  snprintf(buf, sizeof(buf), "%lu %s", static_cast<unsigned long>(millis()), body);
  append(buf);
  LOG_INF("DIAG", "%s", buf);
}

bool DiagLog::isDiagnosticPath(const char* path) {
  if (path == nullptr || path[0] == '\0') return false;
  // strcasecmp because FAT is case-insensitive: "/.crossmosa/DIAG.LOG" names
  // the same file. Exact match only -- see the header for why nothing looser.
  return strcasecmp(path, diagPath()) == 0 || strcasecmp(path, prevDiagPath()) == 0;
}
