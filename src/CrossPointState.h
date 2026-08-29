#pragma once
#include <ArduinoJson.h>
#include <cstdio>
#include <DataDir.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

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
  bool showBootScreen = true;

  static const char* getFilePath() {
    // v36/v186：掛在開機解析出的資料目錄上；首用必在 DataDir::resolve() 之後（開機順序）。
    static char p[40] = "";
    if (!p[0]) snprintf(p, sizeof(p), "%s/state.json", DataDir::path());
    return p;
  }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
};

// Helper macro to access state
#define APP_STATE CrossPointState::getInstance()
