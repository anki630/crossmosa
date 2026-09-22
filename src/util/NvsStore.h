#pragma once
#include <cstddef>
#include <cstdint>

// 三層儲存（RAM／晶片 NVS／SD）的晶片層。
//
// v331 先【量】：state 與閱讀位置照舊寫 SD，只多寫一份影子到 NVS 並計時 —— 實機 215 次：p50 3.1ms、p90 4.1ms、
//   max 34.5ms（頁面 GC，每 26–63 次寫入一次）、零失敗、真關機後讀回與 SD 一致、開機讀回 0.9ms。
// v332 起【主檔】：休眠／翻頁／喚醒路徑上的持久化全走這裡（APP_STATE、閱讀位置），SD 只在「沒人等」的時刻寫
//   （離開書、淺睡眠入口桌布之後的檢查點、真關機出口）。
//
// ⭐ 誰贏（codex 第一輪否決了「NVS 有效就贏」：NVS 寫失敗後的 SD 退路、降版後讀書、清快取、換卡、v331 的影子，
//   都能讓 SD 比 NVS 新）—— 規則改成【配對】：NVS 那格記著「它對應的 SD 那份」的指紋；開機／開書時指紋對得上
//   （＝SD 那份自我們上次寫之後沒被別人動過）NVS 才贏，否則 SD 贏。
//   - APP_STATE：state.json 帶一個隨機 nonce（每次寫 SD 換新），StateBlob 記 sdNonce；v330 重寫 JSON 會把 nonce 丟掉 → 對不上。
//   - 閱讀位置：ProgBlob 記 progress.bin 的內容指紋與長度（開書時本來就讀進 RAM，零成本），txt 另記檔案大小。
//   代價：升級後第一次 saveDurable（離開書／睡一次）之前，NVS-only 的更新在開機時不被採信（一次性、有界）。
//
// 為什麼：diag327–330 的 `PERSISTW state.json` 536 次寫入 p90 1,130ms、23% 超過 500ms（SD 卡 FTL）；每次休眠入口
//   桌布前寫 2 次 state.json ＋ 1 次進度、醒來再 1 次；每 10 頁一次進度 —— 全部是使用者感覺得到的停頓。
//
// NVS 事實（partitions.csv、Arduino 核心 esp32-hal-misc.c）：分割區 0x5000＝20KB＝5 頁，GC 留 1 頁 → ≈504 entry；
//   WiFi（nvs.net80211）與 HalGPIO（`cphw`）已在用。核心開機 `nvs_flash_init()` 遇 NO_FREE_PAGES／NEW_VERSION 會整區
//   擦除重建。耐久（v331 實測換算）：重度讀者 ≈3,500 次擦除/實體頁/年，NOR flash 10 萬次 ≈ 28 年 —— 家留在這裡，
//   不搬 spiffs 窗（帳本有算術與重啟條件）。
//
// ⚠️ 安全邊界：這個模組【不寫 SD、不記 log】—— 錯誤與慢寫只進 Stats，由既有的 SLEEP／LSLEEP resume 證人行印。
//   失敗只回 false；不 abort、不重試、不在 ISR 呼叫。呼叫端的規則：NVS 不可用／寫失敗 → 退回寫 SD（v330 的行為；
//   閱讀器用 v329 的節奏：第一次失敗立刻、之後每 10 次一次，不讓壞掉的 NVS 變成每頁一次 SD 停頓）。
namespace NvsStore {

// APP_STATE 的 NVS 格式（212 bytes）。v2（v332）：加 sdNonce；v1（v331 影子）一律不採信。
struct StateBlob {
  uint8_t magic;    // 'S'
  uint8_t version;  // 2
  uint8_t recentPos;
  uint8_t recentFill;
  uint32_t seq;  // 配號順序（不是落地順序）；只給證人對照
  uint32_t wakeFrameToken;
  uint8_t readerLoadCount;
  uint8_t deepSleepStamp;
  uint8_t lastSleepFromReader;
  uint8_t pathTrunc;  // openEpubPath 放不進 path[] 時為 1 → 這份不完整：save() 會連 SD 一起寫、load() 選 SD
  uint16_t recent[16];
  char path[160];  // openEpubPath（實機 path-len 84；UTF-8 中文路徑 3 bytes/字）；最後一個 byte 永遠 0
  uint32_t sdNonce;  // 這份 NVS 對應的 state.json 的 nonce（0＝還沒配對過 → 不採信）
};
static_assert(sizeof(StateBlob) == 4 + 4 + 4 + 4 + 32 + 160 + 4, "StateBlob layout drifted");

constexpr uint8_t PROG_HAS_OFFSET = 1;  // ProgBlob::flags：offset 有效（v1 用 0 當「沒有」，會把章首當真 offset；codex）

// 閱讀位置（單一槽＝最後一本書）：每一次真的翻頁寫一次。v2（v332）：flags、sdHash／sdLen、identity；v1 不採信。
//   開書時 sdLen==0（沒有 progress.bin）一律不採信 —— 第一次翻頁會先寫一次 progress.bin 當錨（codex 第三輪）。
struct ProgBlob {
  uint8_t magic;    // 'P'
  uint8_t version;  // 2
  uint8_t kind;     // 1=epub 2=txt
  uint8_t flags;    // PROG_HAS_OFFSET
  uint32_t bookHash;  // fnv1a(書的路徑)|1，開書時算一次
  uint32_t seq;
  uint16_t spine;
  uint16_t page;      // 寫入端夾在 0xFFFE 以內；0xFFFF 留給導覽用的哨兵，讀到就不採信
  uint16_t pageCount;
  uint16_t sdLen;     // 對應的 progress.bin 長度（0＝沒有檔）
  uint32_t offset;    // epub：頁首可見文字 offset（flags 說有效才算）；txt：pageStartOffset
  uint32_t sdHash;    // 對應的 progress.bin 內容指紋（fnv1aBytes；0 長度時＝basis）
  uint32_t identity;  // 同名不同書的保險：txt＝檔案大小；epub＝fnv(書名) ^ spine 數×黃金常數（codex 第二輪：
                      //   progress.bin 內容一樣（都在第 0 頁）時光靠指紋分不出「換了一本」）
};
static_assert(sizeof(ProgBlob) == 32, "ProgBlob layout drifted");

struct Stats {
  uint32_t writes;   // 進到 nvs_set_blob 的次數（開不了 namespace 的那些只算 fails）
  uint32_t usTotal;  // 累計微秒（含等鎖）；每次 takeStats 歸零
  uint32_t usMax;    // 單次最大微秒（含等鎖＋set＋commit）
  uint32_t fails;    // 開不了 namespace ＋ set/commit 失敗
  int lastErr;       // 最後一次失敗的 esp_err_t（0＝沒有）
  int openErr;       // namespace 開啟失敗的 esp_err_t（0＝開了或還沒開）
};

// 寫一個 blob 並 commit（惰性開啟 read-write namespace，只做一次；失敗後這一段開機不再試）。
//   usOut ＝ 從進入本函式到 commit 回來的微秒（含等 gMutex）—— 呼叫端的尾段真的等了這麼久。
bool putBlob(const char* key, const void* data, size_t len, uint32_t* usOut = nullptr, int* errOut = nullptr);
// 讀 blob：用【唯讀】handle 開、讀、關 —— 不會建立 namespace、不寫 flash。
//   lenInOut 進＝緩衝大小、出＝實際長度；不存在或長度不合回 false。
bool getBlob(const char* key, void* data, size_t* lenInOut, int* errOut = nullptr);

// 型別化的入口。read* 驗長度／magic／版本（v1 一律拒絕），不合就回 false（呼叫端走 SD）。
bool putState(const StateBlob& b, uint32_t* usOut = nullptr, int* errOut = nullptr);
bool readState(StateBlob* out, int* errOut = nullptr);
bool putProg(const ProgBlob& b, uint32_t* usOut = nullptr, int* errOut = nullptr);
bool readProg(ProgBlob* out, int* errOut = nullptr);

// 取走並歸零累計統計。印在 SLEEP（休眠入口：涵蓋上次醒來之後的翻頁寫入）、LSLEEP resume（入口的 state 寫入）
//   與 SLEEP nvs-final（真關機前）。
Stats takeStats();
// 分割區用量（nvs_get_stats）：看 GC 週期用。
bool usage(uint32_t* used, uint32_t* freeEntries, uint32_t* available, uint32_t* total, uint32_t* namespaces);
// 序號（state 與 prog 共用一條，單調遞增）。⚠️ 是【配號】順序：兩個 task 各自配到號之後才寫，落地順序可能相反。
uint32_t nextSeq();

constexpr uint32_t FNV1A_BASIS = 2166136261u;

inline uint32_t fnv1aBytes(const void* data, const size_t len, const uint32_t seed = FNV1A_BASIS) {
  uint32_t h = seed;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

inline uint32_t fnv1a(const char* s) {
  uint32_t h = FNV1A_BASIS;
  for (; s && *s; ++s) {
    h ^= static_cast<uint8_t>(*s);
    h *= 16777619u;
  }
  return h;
}

}  // namespace NvsStore
