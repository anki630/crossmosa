#pragma once
#include <ArduinoJson.h>
#include <cstdio>
#include <DataDir.h>
#include <PersistableStore.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace NvsStore {
struct StateBlob;
}

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  // v293：每次休眠 +1，同時寫進 APP_STATE 與 wake_frame 的檔頭。喚醒時兩者必須相等，
  //   才證明那張畫面與這次要還原的狀態**出自同一次休眠** —— 只比書路徑擋不掉
  //   「同一本書的舊頁」（codex 複查指出）。0 ＝ 沒有可用的畫面。
  uint32_t wakeFrameToken = 0;
  // v312：「自上次存檔以來曾經自願進過 enterDeepSleep()」的【歷史印記】。
  //   電池上冷開機與睡眠喚醒硬體分不出來（rst 都是 POWERON），所以只能靠自己記；
  //   用途：電源鍵喚醒回首頁時跳過開機 logo（~850ms 全刷）。
  //   ⚠️ 語意要說老實（codex 複查指出）：它**不是**「這次一定是睡眠喚醒」。第一次正常睡眠之後，
  //   之後任何電源鍵開機 —— 包括電池耗盡、硬體重置鍵、brownout 後的冷開機 —— 都會跳過 logo。
  //   這是產品取捨：那些情況面板上是最後一張畫面，首頁直接以清潔 GC 蓋上去同樣乾淨；
  //   當機（rst=4/6）與救援模式另外由 isRebootFromPanic／recoveryFirmwareMode 擋住照畫 logo。
  //   刻意不清除：清除要多一次 state.json 寫入（~60ms）換一個沒人在乎的 logo。
  bool deepSleepStamp = false;

  static const char* getFilePath() {
    // v36/v186：掛在開機解析出的資料目錄上；首用必在 DataDir::resolve() 之後（開機順序）。
    static char p[40] = "";
    if (!p[0]) snprintf(p, sizeof(p), "%s/state.json", DataDir::path());
    return p;
  }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // v332：三層儲存（v331 量過：NVS 一次 3–4ms、GC 33ms、零失敗；SD 的 state.json p90 1,130ms）。
  //   save()        熱路徑（休眠入口／醒來／開書／桌布 recent）：只寫 NVS。NVS 不可用／寫失敗／路徑放不進 blob
  //                 → 當場退回寫 state.json（v330 的行為）。
  //   saveDurable() 沒人等的時刻（離開書、淺睡眠入口桌布之後、真關機出口）：state.json ＋ NVS。
  //   load()        開機：先讀 state.json；NVS 那份【配對得上】（它記的 sdNonce ＝ state.json 裡的 nonce，
  //                 且路徑沒截斷）才用 NVS，否則 SD。
  //   規則（codex 否決了「NVS 有效就贏」）：每次寫 state.json 換一個隨機 nonce，NVS 記下它 —— nonce 對得上就證明
  //   SD 那份自我們上次寫之後沒被別人動過（NVS 寫失敗的退路、降版讀書、清快取、換卡都會動它），NVS 只在這時才比 SD 新。
  //   ⚠️ 換卡：另一張卡的 state.json nonce 對不上 → 用那張卡的（狀態跟著卡走，跟 v330 一樣）。
  bool save();
  bool saveDurable();
  bool load();
  uint8_t lastLoadSource() const { return loadSource_; }  // 0 none／1 nvs／2 sd（開機證人用）
  uint32_t sdNonce() const { return sdNonce_; }

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);

 private:
  // v332：不准直接叫基底的 saveToFile（會漏掉 NVS、又付一次 SD）——熱路徑用 save()，要 SD 用 saveDurable()。
  //   基底 loadFromFile() 內部的 resave 走的是基底自己的版本，不受影響。
  bool saveToFile() const = delete;
  // v332（codex 第二輪）：整個 save／saveDurable／load 用 saveMutex_ 序列化 —— 兩個呼叫不會「舊快照後寫」蓋掉新的；
  //   快照本身在 storeMutex 下一次取兩份（JSON＋blob 同一份欄位）。鎖序：saveMutex_ → storeMutex →（放開）→ NvsStore。
  mutable std::mutex saveMutex_;
  void snapshot(JsonDocument* doc, NvsStore::StateBlob* b, uint32_t nonce) const;
  bool writeSdLocked();  // 呼叫端持 saveMutex_：換新 nonce、寫 state.json，【只有寫成功才】換 sdNonce_
  uint8_t loadSource_ = 0;
  uint32_t sdNonce_ = 0;  // state.json 裡的 nonce（讀到的／上次寫的）；0＝沒有（v330 寫的檔、或還沒寫過）
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
