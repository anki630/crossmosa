#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "ble/BleRemoteManager.h"
#include "util/ButtonNavigator.h"

struct Rect;
struct ThemeMetrics;

// Scan / pair / status / unpair UI for the BLE page-turner remote. Modeled on
// WifiSelectionActivity: async scan polled from loop(), per-state early-return
// input blocks, full-screen transient states, GUI.drawList device list.
class BleRemotePairingActivity final : public Activity {
  enum class UiState : uint8_t {
    INIT_FAILED,    // NimBLE would not start; message + exit only
    DEVICE_LIST,    // scanning and/or listing found devices
    PAIRING,        // connect + bond + subscribe in flight
    PAIRED_STATUS,  // paired: name + live connection state
    PAIR_FAILED,    // last attempt failed
    FORGET_PROMPT,  // Yes/No confirmation
  };

  ButtonNavigator buttonNavigator;
  UiState uiState = UiState::DEVICE_LIST;
  BleRemoteManager::ScanEntry entries[BleRemoteManager::kMaxScanEntries] = {};
  uint8_t entryCount = 0;
  size_t selectedIndex = 0;
  int forgetPromptSelection = 0;  // 0 = No, 1 = Yes
  BleRemoteManager::State lastSeenState = BleRemoteManager::State::Off;
  unsigned long lastListRefreshMs = 0;
  bool diagWasForced = false;  // v85: previous DiagLog forced state, restored on exit
  bool sawFirstResult = false;
  bool loggedScanOutcome = false;  // v86: one scan-outcome dump per scan  // v85: one-shot probe on the first decoded advertisement

  void refreshScanEntries();
  void startScanUi();
  void persistPairing();
  void renderDeviceList(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderCenteredMessage(const Rect* screen, const char* line1, const char* line2) const;
  void renderPairedStatus(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderForgetPrompt(const Rect* screen, const ThemeMetrics* metrics) const;

 public:
  explicit BleRemotePairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleRemotePairing", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
