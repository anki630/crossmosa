#include "BleRemotePairingActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "util/DiagLog.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
const char* signalBars(int8_t rssi) {
  if (rssi > -50) return "||||";
  if (rssi > -60) return " |||";
  if (rssi > -70) return "  ||";
  return "   |";
}
}  // namespace

void BleRemotePairingActivity::onEnter() {
  Activity::onEnter();
  // v85: force diagnostics for this screen the way file-transfer mode does
  // (v64 precedent). Without it the ONE path we most need evidence from is the
  // one path that writes nothing: the v84 reboot report came back with a log
  // containing zero BLE lines because the user's card had no /diag.on sentinel.
  diagWasForced = DiagLog::setForced(true, "blepair");
  DiagLog::line("BLE pairing screen entered");
  entryCount = 0;
  selectedIndex = 0;
  forgetPromptSelection = 0;
  lastSeenState = BLE_REMOTE.state();

  if (BLE_REMOTE.state() == BleRemoteManager::State::StoppedForWifi) {
    // Radio yielded to WiFi for the rest of this session; message-only screen.
    uiState = UiState::INIT_FAILED;
    requestUpdate();
    return;
  }
  if (!BLE_REMOTE.ensureStarted()) {
    uiState = UiState::INIT_FAILED;
    requestUpdate();
    return;
  }
  if (BLE_REMOTE.hasPairing()) {
    // Entry must make the「等待遙控器」promise true, not just the
    // return-to-status paths -- its internal guard makes this a no-op when a
    // connect is already in flight or connected.
    BLE_REMOTE.resumeReconnectIfPaired();
    uiState = UiState::PAIRED_STATUS;
    requestUpdate();
    return;
  }
  startScanUi();
}

void BleRemotePairingActivity::onExit() {
  Activity::onExit();
  DiagLog::line("BLE pairing screen exited state=%d", static_cast<int>(BLE_REMOTE.state()));
  // v92: dump the scan outcome here too, not only when a scan ends by itself.
  // The ScanDone-only version had the diagnostic backwards: a user who cannot
  // find their remote keeps hitting rescan, so no scan ever reaches its 15s end
  // and the ONE line that says what the radio actually heard is never written.
  // diag91.log is exactly that — four scans, four restarts, zero record.
  if (!loggedScanOutcome) {
    loggedScanOutcome = true;
    refreshScanEntries();
    // v98: zero-address counters printed HERE (main task). They used to be logged from
    // inside the scan callback on the NimBLE host task, which overflowed its 2,560-byte
    // stack and caused the v95/v96/v97 panics.
    DiagLog::line("BLE zero addr seen=%lu substituted=%lu", static_cast<unsigned long>(BLE_REMOTE.zeroAddrSeen()),
                  static_cast<unsigned long>(BLE_REMOTE.zeroAddrSubstituted()));
    DiagLog::line("BLE scan outcome at exit: adverts=%lu listed=%u reconnect_adverts=%lu",
                  static_cast<unsigned long>(BLE_REMOTE.advertsSeen()), static_cast<unsigned>(entryCount),
                  static_cast<unsigned long>(BLE_REMOTE.reconnectAdvertsSeen()));
    for (uint8_t i = 0; i < entryCount; i++) {
      DiagLog::line("BLE seen[%u] %s rssi=%d hid=%d appear=0x%04x name=%s", static_cast<unsigned>(i),
                    entries[i].addr, static_cast<int>(entries[i].rssi), entries[i].isHid ? 1 : 0,
                    static_cast<unsigned>(entries[i].appearance), entries[i].name);
    }
  }
  DiagLog::setForced(diagWasForced);
  BLE_REMOTE.stopScan();
  // Frees the whole stack (and its heap) when the user leaves without pairing.
  BLE_REMOTE.stopIfUnpaired();
}

void BleRemotePairingActivity::startScanUi() {
  entryCount = 0;
  selectedIndex = 0;
  uiState = UiState::DEVICE_LIST;
  DiagLog::mem("ble-scan-pre");
  sawFirstResult = false;
  loggedScanOutcome = false;
  if (!BLE_REMOTE.startScan()) {
    // startScan() falls back to resuming the reconnect scan when a pairing
    // already exists (see its "an existing pairing must fall back to
    // reconnecting" comment in BleRemoteManager.cpp) -- BLE is still alive
    // in that case, so land on the status screen instead of lying with a
    // "Bluetooth init failed" message.
    uiState = BLE_REMOTE.hasPairing() ? UiState::PAIRED_STATUS : UiState::INIT_FAILED;
  }
  requestUpdate();
}

void BleRemotePairingActivity::refreshScanEntries() {
  const uint8_t n = BLE_REMOTE.scanResults(entries, BleRemoteManager::kMaxScanEntries);
  // v86: HID-claiming devices first, then by signal. The list is unfiltered
  // now (see BleRemoteManager::handleScanResult), so without this the remote
  // can sit below a dozen phones and earbuds. Insertion sort over <=12 items.
  for (uint8_t i = 1; i < n; i++) {
    const BleRemoteManager::ScanEntry key = entries[i];
    int8_t j = static_cast<int8_t>(i) - 1;
    while (j >= 0 && ((!entries[j].isHid && key.isHid) ||
                      (entries[j].isHid == key.isHid && entries[j].rssi < key.rssi))) {
      entries[j + 1] = entries[j];
      j--;
    }
    entries[j + 1] = key;
  }
  if (n > 0 && !sawFirstResult) {
    // The first advertisement we actually decode. If the device dies before
    // this line, it died with the radio scanning but nothing received; if it
    // dies after, the receive path was alive. v84 could not tell these apart.
    sawFirstResult = true;
    DiagLog::mem("ble-first-advert");
  }
  if (n != entryCount) {
    entryCount = n;
    if (selectedIndex >= entryCount && entryCount > 0) {
      selectedIndex = entryCount - 1;
    }
    requestUpdate();
  }
}

void BleRemotePairingActivity::persistPairing() {
  char addr[18];
  uint8_t type = 0;
  char name[24];
  if (!BLE_REMOTE.consumePairSuccess(addr, sizeof(addr), type, name, sizeof(name))) {
    return;
  }
  {
    RenderLock lock(*this);  // SD write shares the SPI bus with the display
    strlcpy(SETTINGS.bleRemotePeerAddr, addr, sizeof(SETTINGS.bleRemotePeerAddr));
    SETTINGS.bleRemotePeerAddrType = type;
    strlcpy(SETTINGS.bleRemotePeerName, name, sizeof(SETTINGS.bleRemotePeerName));
    SETTINGS.saveToFile();
  }
  uiState = UiState::PAIRED_STATUS;
  requestUpdate();
}

void BleRemotePairingActivity::loop() {
  const auto bleState = BLE_REMOTE.state();
  if (bleState != lastSeenState) {
    lastSeenState = bleState;
    requestUpdate();  // live connection-state line on the status screen
  }

  if (uiState == UiState::INIT_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (uiState == UiState::PAIRING) {
    if (bleState == BleRemoteManager::State::Connected) {
      persistPairing();
    } else if (bleState == BleRemoteManager::State::PairFailed) {
      uiState = UiState::PAIR_FAILED;
      requestUpdate();
    }
    // Back is deliberately ignored: the attempt is bounded <= ~27 s worst
    // case (connect 15 s + SMP deadline 12 s, sequential -- see
    // kConnectTimeoutMs / authDeadlineMs_ in BleRemoteManager.cpp) and
    // cancelling mid-bond leaves half-written keys.
    return;
  }

  if (uiState == UiState::PAIR_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      startScanUi();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (BLE_REMOTE.hasPairing()) {
        uiState = UiState::PAIRED_STATUS;
        // Per controller contract update: resume the background reconnect scan
        // now that the user is back on the status screen, not only at exit.
        BLE_REMOTE.resumeReconnectIfPaired();
        requestUpdate();
      } else {
        finish();
      }
    }
    return;
  }

  if (uiState == UiState::PAIRED_STATUS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      startScanUi();  // pair a different remote (replaces the old pairing)
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      forgetPromptSelection = 0;
      uiState = UiState::FORGET_PROMPT;
      requestUpdate();
    }
    return;
  }

  if (uiState == UiState::FORGET_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (forgetPromptSelection > 0) {
        forgetPromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        BLE_REMOTE.forgetPairing();
        {
          RenderLock lock(*this);
          SETTINGS.bleRemotePeerAddr[0] = '\0';
          SETTINGS.bleRemotePeerName[0] = '\0';
          SETTINGS.bleRemotePeerAddrType = 0;
          SETTINGS.saveToFile();
        }
        startScanUi();
      } else {
        uiState = UiState::PAIRED_STATUS;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      uiState = UiState::PAIRED_STATUS;
      requestUpdate();
    }
    return;
  }

  // DEVICE_LIST
  const bool scanning = (bleState == BleRemoteManager::State::Scanning);
  if (scanning && millis() - lastListRefreshMs > 500) {
    lastListRefreshMs = millis();
    refreshScanEntries();  // list grows live while the scan runs
  } else if (bleState == BleRemoteManager::State::ScanDone) {
    refreshScanEntries();
    if (!loggedScanOutcome) {
      // v86: dump what the scan actually saw. This is the line that tells the
      // difference between "the radio heard nothing", "it heard plenty but the
      // remote was not among them" (→ the remote is not advertising over BLE,
      // e.g. it is in a Bluetooth Classic mode this chip cannot see) and "the
      // remote was right there" (→ our own handling is at fault). v85 could not
      // distinguish any of these.
      loggedScanOutcome = true;
      DiagLog::line("BLE scan done: adverts=%lu listed=%u", static_cast<unsigned long>(BLE_REMOTE.advertsSeen()),
                    static_cast<unsigned>(entryCount));
      for (uint8_t i = 0; i < entryCount; i++) {
        DiagLog::line("BLE seen[%u] %s rssi=%d hid=%d appear=0x%04x name=%s", static_cast<unsigned>(i),
                      entries[i].addr, static_cast<int>(entries[i].rssi), entries[i].isHid ? 1 : 0,
                      static_cast<unsigned>(entries[i].appearance), entries[i].name);
      }
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    BLE_REMOTE.stopScan();
    if (BLE_REMOTE.hasPairing()) {
      uiState = UiState::PAIRED_STATUS;
      // Per controller contract update: resume the background reconnect scan
      // now that the user is back on the status screen, not only at exit.
      BLE_REMOTE.resumeReconnectIfPaired();
      requestUpdate();
    } else {
      finish();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (entryCount > 0) {
      if (BLE_REMOTE.pairWith(entries[selectedIndex])) {
        uiState = UiState::PAIRING;
      } else {
        // Synchronous rejection (e.g. createClient() failed, or connect()
        // rejected because the client was still live) already leaves the
        // manager in PairFailed -- follow it instead of sitting on
        // DEVICE_LIST looking frozen.
        uiState = UiState::PAIR_FAILED;
      }
      requestUpdate();
    } else if (!scanning) {
      startScanUi();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    startScanUi();
    return;
  }
  buttonNavigator.onNext([this] {
    if (entryCount > 0) {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, entryCount);
      requestUpdate();
    }
  });
  buttonNavigator.onPrevious([this] {
    if (entryCount > 0) {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, entryCount);
      requestUpdate();
    }
  });
}

void BleRemotePairingActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  char countStr[24] = "";
  if (uiState == UiState::DEVICE_LIST) {
    snprintf(countStr, sizeof(countStr), "%u", static_cast<unsigned>(entryCount));
  }
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_BLE_REMOTE), countStr[0] ? countStr : nullptr);

  switch (uiState) {
    case UiState::INIT_FAILED: {
      renderCenteredMessage(&screen,
                            BLE_REMOTE.state() == BleRemoteManager::State::StoppedForWifi
                                ? tr(STR_BLE_STOPPED_WIFI)
                                : tr(STR_BLE_INIT_FAILED),
                            nullptr);
      // loop() handles both Back and Confirm here (either just exits) --
      // without this the screen has no visible way out.
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case UiState::DEVICE_LIST:
      renderDeviceList(&screen, &metrics);
      break;
    case UiState::PAIRING:
      // No hint bar here, deliberately: loop() ignores all input while a
      // pairing attempt is in flight, so drawing hints would promise
      // controls that don't exist.
      renderCenteredMessage(&screen, tr(STR_BLE_PAIRING), nullptr);
      break;
    case UiState::PAIR_FAILED: {
      renderCenteredMessage(&screen, tr(STR_BLE_PAIR_FAILED), tr(STR_PRESS_OK_SCAN));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BLE_RESCAN), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    case UiState::PAIRED_STATUS:
      renderPairedStatus(&screen, &metrics);
      break;
    case UiState::FORGET_PROMPT:
      renderForgetPrompt(&screen, &metrics);
      break;
  }
  renderer.displayBuffer();
}

void BleRemotePairingActivity::renderCenteredMessage(const Rect* screen, const char* line1,
                                                     const char* line2) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height) / 2;
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, line1);
  if (line2 != nullptr) {
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + height + 10, line2);
  }
}

void BleRemotePairingActivity::renderDeviceList(const Rect* screen, const ThemeMetrics* metrics) const {
  const bool scanning = (BLE_REMOTE.state() == BleRemoteManager::State::Scanning);
  if (entryCount == 0) {
    renderCenteredMessage(screen, scanning ? tr(STR_BLE_SCANNING) : tr(STR_BLE_NO_DEVICES),
                          scanning ? nullptr : tr(STR_PRESS_OK_SCAN));
  } else {
    const int contentTop = screen->y + metrics->topPadding + metrics->headerHeight + metrics->verticalSpacing;
    const int contentHeight = screen->height - contentTop - metrics->verticalSpacing * 2;
    GUI.drawList(
        renderer, Rect{screen->x, contentTop, screen->width, contentHeight}, static_cast<int>(entryCount),
        static_cast<int>(selectedIndex),
        [this](int index) { return std::string(entries[index].name); }, nullptr, nullptr,
        [this](int index) {
          // v86: the HID marker is a hint, not a promise — an unmarked device
          // can still be a working remote (many never advertise the service).
          return std::string(entries[index].isHid ? "HID " : "") + signalBars(entries[index].rssi);
        });
    if (scanning) {
      GUI.drawHelpText(
          renderer,
          Rect{screen->x, screen->y + screen->height - metrics->contentSidePadding - 15, screen->width, 20},
          tr(STR_BLE_SCANNING));
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BLE_PAIR), "", tr(STR_BLE_RESCAN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleRemotePairingActivity::renderPairedStatus(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const auto top = screen->y + (screen->height - lineHeight * 3) / 2;
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, tr(STR_BLE_PAIRED_TO));
  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top + lineHeight, SETTINGS.bleRemotePeerName,
                            true, EpdFontFamily::BOLD);
  const char* status;
  switch (BLE_REMOTE.state()) {
    case BleRemoteManager::State::Connected:
      status = tr(STR_BLE_CONNECTED);
      break;
    case BleRemoteManager::State::StoppedForWifi:
      status = tr(STR_BLE_STOPPED_WIFI);
      break;
    default:
      status = tr(STR_BLE_WAITING);
      break;
  }
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + lineHeight * 2 + 8, status);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BLE_RESCAN), tr(STR_BLE_FORGET), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleRemotePairingActivity::renderForgetPrompt(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const auto top = screen->y + (screen->height - lineHeight * 3) / 2;
  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top, tr(STR_BLE_FORGET_CONFIRM), true,
                            EpdFontFamily::BOLD);
  // Two-option prompt in the WifiSelection FORGET_PROMPT idiom.
  const char* options[2] = {tr(STR_NO), tr(STR_YES)};
  for (int i = 0; i < 2; i++) {
    const bool selected = (forgetPromptSelection == i);
    char line[40];
    snprintf(line, sizeof(line), "%s %s", selected ? ">" : " ", options[i]);
    UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top + lineHeight * (i + 1) + 8, line);
  }
  // Mirrors WifiSelectionActivity::renderForgetPrompt's hint shape: Left/Right
  // move the cursor between options (loop() handles both), Select executes it.
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
