#include "DiagLog.h"
#include <BitmapHelpers.h>
#include <Breadcrumb.h>
#include <DataDir.h>
#include <strings.h>  // strcasecmp -- isDiagnosticPath()

#include "BootRecovery.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

// v186：DataDir 搬回來了（維護者 2026-08-28 重啟：原廠韌體本身是 CrossPoint 分支、也寫 /.crosspoint，
// 從原廠直接刷過來的人靠這一步保住進度）。資料目錄一律問 DataDir::path()；diagPath()/prevDiagPath()
// 與 isDiagnosticPath() 用同一個函式，兩邊天然一致。

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

// v186：BOOT 橫幅用的面板名。x3-8279 = 新批次 UC8279d；x3-8253 = 舊批次；x4。
const char* panelName() {
  if (!gpio.deviceIsX3()) return "x4";
  return gpio.displayIsUc8279() ? "x3-8279" : "x3-8253";
}

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

// v277：寫入路徑的輪替檢查與丟行統計。
//   ⚠️ 不要每一行都去 stat 檔案（每次 open/size 都是 SD I/O，12–18ms，見 CLAUDE.md A-4）——
//      用本地累計的**嘗試寫入**位元組數（含被上限擋掉的）估算，只有估到門檻才真的去看檔案。
//      「嘗試」而不是「寫入」是刻意的：撞上限之後每一行都被擋，靠它才逃得出被夾住的狀態。
//   ⚠️ 丟掉的行**必須說出來**：診斷機制自己也要有失敗回報，否則就是「看起來正常、後半段全沒了」。
size_t g_bytesSinceCheck = 0;
uint32_t g_droppedLines = 0;
uint32_t g_dropNoticePending = 0;
constexpr size_t WRITE_CHECK_INTERVAL = 8 * 1024;

// v249：append 的跨 task 鎖。每一行是 open(O_APPEND)／write／flush／close 四步，Storage 的鎖只包住每一步，
// 整段沒鎖 —— render task 與 main loop 同時各開一個把手追加時，兩邊各自以為的檔尾與檔案大小會互蓋：
// diag248.log 的 `IMGDEC … prog=0 fil` 被截在正好等於同時寫入的 IMGPAGE 那一行的長度、IMGPAGE 本身消失。
// 在 begin()（setup，單執行緒）建立；FreeRTOS mutex 有優先權繼承。Storage 不會回頭呼叫 DiagLog（HAL 不碰它），不會死結。
// 靜態配置（codex 複查）：diag 最有用的時候正是堆積吃緊的時候，不能因為配不到而悄悄退回無鎖。
StaticSemaphore_t g_appendMutexStorage;
SemaphoreHandle_t g_appendMutex = nullptr;

struct AppendLock {
  bool held = false;
  AppendLock() {
    if (g_appendMutex) held = xSemaphoreTake(g_appendMutex, portMAX_DELAY) == pdTRUE;
  }
  ~AppendLock() {
    if (held) xSemaphoreGive(g_appendMutex);
  }
};

// 追加一行(自帶換行)。每次開關檔:量測點已節流,FAT 成本可接受,
// 且避免長期持有檔案控制代碼干擾其他 SD 存取。
bool append(const char* text) {
  if (!Storage.ready()) return false;
  const AppendLock lock;  // v249

  // v277：累積到一定量才真的去看檔案大小（stat 是 SD I/O，不能每行做），
  //   逼近上限就地輪替 —— 把「撞上限之後無聲停寫」這條路整個拿掉。
  //   ⚠️ 必須在鎖【之內】：輪替會 rename，跟其他 task 的 append 同時發生就會寫進被改名的舊檔。
  g_bytesSinceCheck += strlen(text) + 1;
  if (g_bytesSinceCheck >= WRITE_CHECK_INTERVAL) {
    g_bytesSinceCheck = 0;
    rotateIfNearCap();
    if (g_droppedLines > 0) {
      // 輪替之後新檔有空間了 → 把「剛剛丟了幾行」帶過去（下面開檔後補印）。
      // ⚠️ 不能在這裡直接呼叫 line()／append()：這個函式已經持有 g_appendMutex，而它不是遞迴鎖。
      g_dropNoticePending += g_droppedLines;
      g_droppedLines = 0;
    }
  }

  const char* path = diagPath();
  HalFile f = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) {
    // 新卡上資料目錄可能還不存在(DataDir::resolve 刻意不建,由各 store 首次存檔時建),
    // 而 SdFat 的 O_CREAT 不會建父目錄 → 開機基準線會靜默消失。建一次再試。
    Storage.mkdir(DataDir::path());
    f = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
    if (!f) {
      LOG_ERR("DIAG", "Cannot open %s", path);
      return false;
    }
  }
  if (f.size() >= MAX_DIAG_BYTES) {
    // v277（複查第一輪）：**就地輪替並重試**，不要等下一次 8KB 檢查 ——
    //   否則「撞上限 → 再寫幾行 → 關機」這條路上，那幾行與計數全部只活在 RAM 裡，
    //   永遠不會有 DIAGDROP 說出來（最多可以無聲吃掉近 8KB 的尾巴）。
    f.close();          // ⚠️ rename 之前一定要先關檔
    rotateIfNearCap();  // 已經撞到上限 → 必定超過門檻 → 一定會嘗試輪替
    g_bytesSinceCheck = 0;
    f = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
    if (!f || f.size() >= MAX_DIAG_BYTES) {
      ++g_droppedLines;  // 輪替失敗（檔案系統有問題）才是真的丟掉
      return false;
    }
    if (g_droppedLines > 0) {
      g_dropNoticePending += g_droppedLines;
      g_droppedLines = 0;
    }
  }
  // v277：**寫入時也要檢查輪替**。原本 rotateIfNearCap() 只在 begin()／setForced() 跑，
  //   所以一次開機用久了就會撞到上面那行、然後【無聲停寫】——而 line() 的回傳值沒有人接。
  //   實測 share 裡 158 份 log 有 **17 份撞上限**（v163/164、v238、v250–258、v263/264、v267/268），
  //   也就是那幾輪分析用的是被截斷的 log，而我當時不知道。
  // v277：補印「剛剛丟了幾行」＋本文＋換行，**組成一筆、一次 write**（複查兩輪都打在這裡）。
  //   分開寫的話，任何一次短寫都會在檔案裡留下半截行，後面再接上別的東西 → 壞掉的紀錄。
  //   一次 write 之下，短寫仍可能截斷【這一筆】，但不可能把兩筆黏在一起，而且回傳 false 會被計數。
  //   ⚠️ 時間戳用**真正的 millis**，不是 0 —— 0 會被讀成時間倒退，甚至被誤認為開機邊界。
  // 緩衝區：通知 ≤48 ＋ 本文 ≤416（line() 的 buf 大小）＋ 換行 ＝ 465。
  // ⚠️ 堆疊預算：繪圖任務 8192 bytes，line() 的 384+416 加上這裡的 480 ＝ 約 1.3KB（16%）。
  //    要再加欄位就先算這筆帳，不要無限長大。
  char out[480];
  size_t len = strlen(text);
  if (len > sizeof(out) - 2) len = sizeof(out) - 2;  // 減法形式：任何輸入都不可能溢位（複查第四輪）
  size_t noticeLen = 0;
  const uint32_t noticeCount = g_dropNoticePending;
  if (noticeCount > 0 && len < sizeof(out) - 2) {
    const int n = snprintf(out, sizeof(out) - len - 2, "%lu DIAGDROP lost=%lu\n",
                           static_cast<unsigned long>(millis()), static_cast<unsigned long>(noticeCount));
    if (n > 0 && static_cast<size_t>(n) < sizeof(out) - len - 2) noticeLen = static_cast<size_t>(n);
  }
  memcpy(out + noticeLen, text, len);
  const size_t outLen = noticeLen + len + 1;
  out[outLen - 1] = '\n';
  const size_t written = f.write(reinterpret_cast<const uint8_t*>(out), outLen);
  const bool ok = written == outLen;
  // 通知那一段確定寫進去了就結清（用「寫了多少」判斷，不是用整筆成敗 —— 否則本文短寫會讓
  //   同一筆 DIAGDROP 再印一次）。期間若又有新的丟行，只扣掉這次帶走的量，不會被抹掉。
  if (noticeLen > 0 && written >= noticeLen) g_dropNoticePending -= noticeCount;
  if (!ok) {
    ++g_droppedLines;  // 短寫／寫失敗也是丟掉，要算進去
    // 半截行**封口**：補一個換行，下一筆才不會黏在它後面變成一行壞紀錄。
    // 盡力而為（卡真的滿了就補不上），但它把「污染下一行」這個後果擋掉。
    if (written > 0 && out[written - 1] != '\n') f.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  }
  f.flush();
  return ok;
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
  if (!g_appendMutex) g_appendMutex = xSemaphoreCreateMutexStatic(&g_appendMutexStorage);  // v249：見 append()
  // 必須在 Storage.begin() 成功且 DataDir::resolve() 之後呼叫(哨兵檔路徑依賴資料目錄)。
  if (!Storage.ready()) {
    enabled_ = false;
    return;
  }
  enabled_ = Storage.exists(SENTINEL_PATH);
  if (enabled_) LOG_INF("DIAG", "Instrumentation ON (%s present)", SENTINEL_PATH);

  // v165（使用者抓到的）：輪替原本只掛在 setForced()（網頁強制路徑）——哨兵路徑
  // 【永不輪替】，檔案撞 192KB 後每次開機都無聲拒寫（v163/v164 兩版的診斷因此全丟）。
  // 與 v151 的 CAPS 誤植同款：儀器要先證明自己會在【使用中的路徑】上執行（B-22）。
  // 必須在下面的 BOOT/CAPS 橫幅之前跑，橫幅才會落在新檔。
  rotateIfNearCap();

  // v151：版本橫幅【必須在 begin()】—— 沒有它，append-only 的 diag.log 無法按版本分段
  // （memory attribute-evidence-before-reasoning；v150 的 log 就是因此無法歸屬）。
  // v178：帶重置原因（esp_reset_reason；1=poweron 3=sw 4=panic 5=int_wdt 6=task_wdt 7=wdt 8=deepsleep
  //        9=brownout 12=jtag 15=cpu_lockup —— v185 更正：12 是 JTAG，cpu_lockup 是 15）
  // v186：帶面板控制器（雙面板 binary 的第一個證人——同一顆韌體在 UC8253 與 UC8279 上都要能跑）。
  line("BOOT version=%s rst=%d panel=%s", CROSSPOINT_VERSION, static_cast<int>(esp_reset_reason()), panelName());
  // v186：資料目錄的決定是這台機器上唯一的 SD 格式變更；LOG_* 在 X3 上等於丟掉，所以寫進 log。
  line("DATADIR active=%s outcome=%s", DataDir::path(), DataDir::outcomeName());
  // v151：CAPS 探測 —— v150 誤植進 setForced()（codex 警告過，我確認錯了），整版沒取到證。
  // boot 時 p2 是 pristine 的 ~115K：default_max 等於它 = p2 在 malloc 備援池裡；
  // 等於 p3 的 ~102K = plain new 用不到 p2，CLAUDE.md 的池備援模型要作廢。
  line("CAPS default_max=%u internal_max=%u default_free=%u",
       static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)),
       static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)));
  // v194：逃生口在 DiagLog 之前，麵包屑住 RTC。只在儀器開啟時讀。
  // peek → 寫 → 確認寫成功才清。寫失敗就把證據留在 RTC，下次再開 diag 還在。
  if (active()) {
    if (const char* why = boot_recovery::peekBootComboBreadcrumb()) {
      if (line("BOOTCOMBO why=%s", why)) {
        boot_recovery::consumeBootComboBreadcrumb();
      }
    }
    crumb("ALLOCFAIL", HalStorage::lastAllocFail, sizeof(HalStorage::lastAllocFail));
    crumb("ALLOCFAIL", ditherLastAllocFail, sizeof(ditherLastAllocFail));
  }
}

bool DiagLog::setForced(bool on, const char* reason) {
  const bool previous = forced_;
  forced_ = on;
  if (!on || previous) return previous;  // 只在 off->on 那一次做下面的事

  // v65:輪替【不再】以「使用者沒放哨兵檔」為條件。
  //
  // v64 的理由是「哨兵檔是使用者自己放的,他知道那個檔在長大」。那個假設在真實
  // 使用上不成立,而且代價是全部證據:實機的卡上有 /diag.on,是 v59 那輪
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

  line("BOOT version=%s forced=%s panel=%s", CROSSPOINT_VERSION, reason != nullptr ? reason : "forced", panelName());
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

  // v146：已用區塊的【大小直方圖】。
  // 為什麼需要它：位址列表上限只有 16 筆，而 p3 實測是 `big=16+103` —— 列出來的 16 筆
  // 全在池子最前段（偏移 944 起），那是【開機早期配的】，不是讀書累積的那 103 個。
  // 換句話說位址列表看得到的永遠是最早那批，看不到兇手。而列 119 個位址也讀不完。
  // 直方圖用固定的 8 個桶回答「有幾個、多大」，一行就夠，而且成本是常數。
  //   桶：<=64 / <=128 / <=256 / <=512 / <=1K / <=4K / <=16K / >16K
  uint16_t hist[8];
  uint32_t histBytes[8];
};

// v146：把區塊大小對到直方圖的桶。
inline int histBucket(size_t sz) {
  if (sz <= 64) return 0;
  if (sz <= 128) return 1;
  if (sz <= 256) return 2;
  if (sz <= 512) return 3;
  if (sz <= 1024) return 4;
  if (sz <= 4096) return 5;
  if (sz <= 16384) return 6;
  return 7;
}

bool dumpCb(walker_heap_into_t heap, walker_block_info_t block, void* userData) {
  auto* c = static_cast<DumpCtx*>(userData);
  if (static_cast<uintptr_t>(heap.start) != c->poolStart) return true;
  const auto addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(block.ptr));
  if (block.used) {
    c->usedSum += block.size;
    const int b = histBucket(block.size);
    if (c->hist[b] < 0xFFFF) c->hist[b]++;
    c->histBytes[b] += static_cast<uint32_t>(block.size);
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

    // v146：直方圖獨立一行。位址列表答不出「那 103 個沒列出來的是什麼大小」，這行可以。
    // 格式：桶上限:個數/總位元組。看的是【哪一桶的個數在讀書之後暴增】。
    static constexpr unsigned kBucketMax[8] = {64, 128, 256, 512, 1024, 4096, 16384, 0};
    char hbuf[240];
    int hn = snprintf(hbuf, sizeof(hbuf), "%lu HIST %-10s p@%08x |", static_cast<unsigned long>(millis()), tag,
                      static_cast<unsigned>(c.poolStart));
    for (int b = 0; b < 8 && hn > 0 && hn < static_cast<int>(sizeof(hbuf)) - 24; b++) {
      if (c.hist[b] == 0) continue;
      if (kBucketMax[b] == 0) {
        hn += snprintf(hbuf + hn, sizeof(hbuf) - hn, " >16k:%u/%u", static_cast<unsigned>(c.hist[b]),
                       static_cast<unsigned>(c.histBytes[b]));
      } else {
        hn += snprintf(hbuf + hn, sizeof(hbuf) - hn, " %u:%u/%u", kBucketMax[b], static_cast<unsigned>(c.hist[b]),
                       static_cast<unsigned>(c.histBytes[b]));
      }
    }
    append(hbuf);
    LOG_INF("DIAG", "%s", hbuf);
  }
}

bool DiagLog::line(const char* fmt, ...) {
  if (!active()) return false;
  // v58:放大到 384。加了 reuse/cum_reuse 之後最壞情況已達 252 字元,對 256 只剩 4 bytes——
  // 而新欄位都在【行尾】,截斷會剛好吃掉要量的東西(這正是原註解警告的情況再次發生),
  // 且 cum_reuse 會隨閱讀時間變長。
  // v277：原本是 body[384] ＋ buf[416] 兩個緩衝區（800 bytes）。append() 為了「一次寫入」
  //   另外用了 480 —— 而這條路是從排版／繪圖深處呼叫的。**合併成一個**，總量從 800 變 480，
  //   淨增只有 160 bytes（繪圖任務堆疊 8192，見 ActivityManager.cpp）。輸出格式一個位元組沒變。
  char buf[480];
  const int pre = snprintf(buf, sizeof(buf), "%lu ", static_cast<unsigned long>(millis()));
  if (pre > 0 && static_cast<size_t>(pre) < sizeof(buf)) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + pre, sizeof(buf) - static_cast<size_t>(pre), fmt, args);
    va_end(args);
  }
  const bool ok = append(buf);
  LOG_INF("DIAG", "%s", buf);
  return ok;
}

void DiagLog::crumb(const char* prefix, char* buf, const size_t cap) {
  char out[320];  // 最大的麵包屑是 ImageBlock::lastDecodeWitness[300]
  if (breadcrumbTake(buf, cap, out, sizeof(out))) line("%s %s", prefix, out);
}

bool DiagLog::isDiagnosticPath(const char* path) {
  if (path == nullptr || path[0] == '\0') return false;
  // strcasecmp because FAT is case-insensitive: "/.crossmosa/DIAG.LOG" names
  // the same file. Exact match only -- see the header for why nothing looser.
  return strcasecmp(path, diagPath()) == 0 || strcasecmp(path, prevDiagPath()) == 0;
}
