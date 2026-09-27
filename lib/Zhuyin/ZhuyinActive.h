#pragma once
// 閱讀字級那一個引擎（P2 設計第 4 節；codex 修訂 1、12；codex 複查 ③ F3、F5）。
//
// 登記的是一組「已驗證的配對」：引擎資料、登記當時的資料狀態序號、擁有它的那個字型（fontId —— 字形與資料在同一個
// cpfont 檔裡，配對在載入時驗過：檔頭的資料集 ID、PUA 數、抽樣；zy_verify_pack 是出貨閘門）。
// 世代號：每次登記或拿掉都加一。帶替換清單的行記住建立時（排版或反序列化）的世代；
//   - 序列化前：引擎可用、世代相同，才寫清單（否則寫成沒有清單的行）；
//   - 繪製前：再加上「交給字型的 fontId 就是登記的那個」—— 不符就畫原字（破音字不標）。
// 「引擎可用」＝ 有資料、已通過自我測試、而且資料的狀態序號跟登記時相同（資料就地換過就不算，連重新載入成功也一樣）。
// ⚠️ 執行緒：寫入（登記、拿掉）只在沒有任何繪製、排版、序列化、反序列化進行時（韌體：RenderLock 底下）；
//    讀取在那些工作裡面。這裡不另外上鎖。SwapStats 在併發時可能少算（只是診斷）。

#include <cstdint>

#include "ZhuyinSwaps.h"

namespace zhuyin {

class ZhuyinData;
class ZhuyinEngine;

struct ActiveEngine {
  ZhuyinData* data = nullptr;
  ZhuyinEngine* engine = nullptr;  // 擁有 data 的引擎（排版端要它的 session 暫存與一行清單的緩衝）；測試直接登記資料時為空
  uint32_t generation = 0;
  uint32_t dataSerial = 0;  // 登記時的 data->stateSerial()
  int fontId = 0;           // 擁有這個引擎的閱讀字型
};

ActiveEngine activeEngine();
// 低階：登記（data 為 nullptr ＝ 拿掉）。韌體用 EngineRegistration，不直接呼叫。
void setActiveEngine(ZhuyinData* data, int fontId = 0, ZhuyinEngine* engine = nullptr);
bool engineUsable(const ActiveEngine& e);
// 這個引擎、這個位置的清單的綁定（e 必須可用）
SwapBinding bindingOf(const ActiveEngine& e, const PagePlace& place = {});

// RAII：跟引擎資料放在同一個物件裡、宣告在資料【之後】（比資料先解構）→ 資料放掉之前一定先從登記處拿掉，不會留下懸空指標。
class EngineRegistration {
 public:
  EngineRegistration() = default;
  ~EngineRegistration() { release(); }
  EngineRegistration(const EngineRegistration&) = delete;
  EngineRegistration& operator=(const EngineRegistration&) = delete;
  void attach(ZhuyinData* data, int fontId, ZhuyinEngine* engine = nullptr);  // 登記（自己之前登記的會先拿掉）
  // 登記處上的還是【這一次】登記（世代沒變）→ 拿掉（世代加一）。只比資料指標不夠：
  // 同一份資料被別的登記物件重新登記過，舊的解構時不能把新的拿掉（codex 複查 ③ 第二輪 F5）。
  void release();

 private:
  ZhuyinData* data_ = nullptr;
  uint32_t generation_ = 0;
};

// 證人（閱讀器讀走寫進 diag；lib 不能呼叫 DiagLog）。只增不減，讀的人自己算差。
struct SwapStats {
  uint32_t built = 0;          // 排版時掛上清單的行
  uint32_t loaded = 0;         // 反序列化時驗過、掛上的行
  uint32_t dropStale = 0;      // 建構時沒有可用的引擎，或世代／字型不是登記的那個
  uint32_t dropNoEngine = 0;   // 反序列化時沒有可用的引擎
  uint32_t dropBinding = 0;    // 反序列化時那一行的綁定（資料集、語意版號、字型）跟目前的不同
  uint32_t dropCheck = 0;      // 結構或歸屬不過
  uint32_t dropSize = 0;       // arena＋清單超過 kMaxLineArenaBytes，或筆數超過上限
  uint32_t listOom = 0;        // arena＋清單配不到 → 退回只配本體、這一行不換（在配置失敗的當下記，建構與反序列化一樣）
  uint32_t renderGated = 0;    // 繪製時閘門關著 → 整行畫原字；一頁畫 16 趟，每趟都算
  uint32_t renderSkipped = 0;  // 繪製時某個字詞換不了（放不下暫存、前提不成立）→ 那個字詞畫原字
  // 排版端（ParsedText）：
  uint32_t paragraphs = 0;     // 開了注音的段落
  uint32_t degradedEvents = 0; // 段落因資源或 I/O 停止標注（配不到 session、暫存被拿走、讀卡失敗、引擎被換掉）→ 章節要寫「沒注音」
  uint32_t contentStops = 0;   // 段落因內容停止標注（雙向重排、核對不符）→ 不算降級
  uint32_t bidiStops = 0;      // 其中因為雙向重排（那一行是視覺順序）
  uint32_t lineOverflow = 0;   // 一行的替換超過緩衝 → 那一行不換（照樣取出讀音保持對齊）
  uint32_t heldLines = 0;      // 還沒定案、留到下一批的行（證人：分批的延遲有沒有在作用）
  uint32_t queueMax = 0;       // session 佇列最深的一次（離容量多遠；不是累計 —— 閱讀器每次建置開始時歸零）
  uint32_t stackStops = 0;     // 堆疊不夠跑解析器 → 那一段停止標注（記成降級；不是讓堆疊爆掉）
  uint32_t annotatedParagraphs = 0;  // v342：屬於預先標注章節（<head> 有 bpmfvs 標記）的段落
  uint32_t selectors = 0;            // v342：排版前拿掉的變體選擇符號（預先標注的讀音寫在這裡；任何字型都算）
};
SwapStats& swapStats();

// 堆疊守衛（codex 整合複查 A5）：載入時的門檻量的是【載入的那個任務】，而解析器之後在繪製任務（前景排版）與主任務
// （背景建置）裡跑，深度不同 → 每次要進解析器之前（session 的 addWord／finish）現場問一次「這個任務還剩多少」。
// 韌體登記一個實作（FreeRTOS 的 pxTaskGetStackStart）；沒登記（電腦端）＝ 永遠夠。
using StackGuardFn = bool (*)();
void setStackGuard(StackGuardFn fn);
bool resolverStackOk();

}  // namespace zhuyin
