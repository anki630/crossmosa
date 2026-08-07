#include "BleRemoteManager.h"

#include <HalGPIO.h>
#include <Logging.h>
#include <NimBLEDevice.h>

#include <cstdio>   // snprintf -- pairing-scan entry.addr built from raw bytes (M1)
#include <cstring>
#include <string>
#include <strings.h>  // strcasecmp -- compares the old vs. new peer address
                      // when a pairing succeeds (tick()'s pairSuccessPending_ handling, C1c);
                      // not pulled in transitively on this toolchain -- SmbFileHandlers.cpp
                      // needed this same include for the same reason.

#include "CrossPointSettings.h"
#include "util/DiagLog.h"
#include "util/HidUsageMap.h"

// HidUsageMap mirrors HalGPIO's button indices so it can stay Arduino-free for
// the host tests; drift here would silently remap every remote key.
static_assert(HalGPIO::BTN_BACK == 0 && HalGPIO::BTN_CONFIRM == 1 && HalGPIO::BTN_LEFT == 2 &&
                  HalGPIO::BTN_RIGHT == 3 && HalGPIO::BTN_UP == 4 && HalGPIO::BTN_DOWN == 5 &&
                  HalGPIO::BTN_POWER == 6,
              "HidUsageMap button indices no longer mirror HalGPIO::BTN_*");

namespace {
constexpr uint16_t kHidService = 0x1812;
constexpr uint16_t kReportChar = 0x2A4D;
constexpr uint16_t kReportMapChar = 0x2A4B;
constexpr uint16_t kReportRefDesc = 0x2908;
constexpr uint8_t kReportTypeInput = 1;
constexpr uint32_t kPairingScanMs = 15000;
constexpr uint32_t kConnectTimeoutMs = 15000;

// Guards latch_ + scanEntries_/scanCount_ between the NimBLE host task and the
// main task. Critical sections stay tiny (no allocation, no I/O inside).
portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;

// M1: hex-nibble decode, used only by parseAddrString below.
int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// M1: parses "aa:bb:cc:dd:ee:ff" (SETTINGS.bleRemotePeerAddr's format) into a
// 6-byte buffer in ble_addr_t::val[] order -- NOT the same order the string
// reads in. NimBLEAddress.cpp says it outright ("NimBLE address bytes are in
// INVERSE ORDER") and its own operator std::string() prints val[5]..val[0], so
// the first hex pair parsed ("aa") lands in out[5] and the last ("ff") in
// out[0]. Getting this backwards would make every reconnect-scan match fail
// silently (memcmp against the wrong byte order never matches a real device).
// Zeroes `out` and returns false on any malformed input; a zeroed buffer can
// only match a peer literally advertising 00:00:00:00:00:00, i.e. fails
// closed rather than matching garbage.
bool parseAddrString(const char* addr, uint8_t out[6]) {
  memset(out, 0, 6);
  if (addr == nullptr) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    const int hi = hexNibble(addr[i * 3]);
    const int lo = hexNibble(addr[i * 3 + 1]);
    if (hi < 0 || lo < 0) {
      memset(out, 0, 6);
      return false;
    }
    if (i < 5 && addr[i * 3 + 2] != ':') {
      memset(out, 0, 6);
      return false;
    }
    out[5 - i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}
}  // namespace

class BleRemoteScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override { BLE_REMOTE.handleScanResult(dev); }
  void onScanEnd(const NimBLEScanResults&, int reason) override {
    (void)reason;
    BLE_REMOTE.scanEnded_ = true;
  }
};

class BleRemoteClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override { BLE_REMOTE.connEstablished_ = true; }
  void onConnectFail(NimBLEClient*, int reason) override {
    BLE_REMOTE.cbError_ = reason;
    BLE_REMOTE.connFailed_ = true;
  }
  void onDisconnect(NimBLEClient*, int reason) override {
    BLE_REMOTE.cbError_ = reason;
    BLE_REMOTE.disconnected_ = true;
  }
  // I3: security-manager completion. Fix round 2 correction: this only ever
  // fires on SUCCESS. Verified against NimBLEClient.cpp's BLE_GAP_EVENT_ENC_CHANGE
  // handler -- it calls onAuthenticationComplete() exclusively from the
  // enc_change.status==0 branch; a status of BLE_ERR_PINKEY_MISSING instead
  // deletes the stored key and silently retries (or gives up after one retry),
  // and every other failure status skips the callback entirely. A real SMP
  // failure surfaces to us as a disconnect (onDisconnect, handled in tick())
  // or, if the peer never answers at all, the 12s authDeadlineMs_ timeout --
  // not as this callback with isEncrypted()==false. tick()'s `!authOk_` branch
  // is therefore a defensive backstop for "the event fired but somehow wasn't
  // actually encrypted", not the normal failure path. Pure flag writes here,
  // no I/O, same rule as every other callback in this file.
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    BLE_REMOTE.authOk_ = connInfo.isEncrypted();
    BLE_REMOTE.authDone_ = true;
  }
};

namespace {
BleRemoteScanCallbacks scanCallbacks;
BleRemoteClientCallbacks clientCallbacks;
}  // namespace

BleRemoteManager& BleRemoteManager::getInstance() {
  static BleRemoteManager instance;
  return instance;
}

bool BleRemoteManager::hasPairing() const { return SETTINGS.bleRemotePeerAddr[0] != '\0'; }

uint8_t BleRemoteManager::buttonHookThunk() {
  auto& self = BLE_REMOTE;
  const uint32_t now = millis();  // M8: keep the syscall out of the lock
  portENTER_CRITICAL(&bleMux);
  const uint8_t mask = self.latch_.poll(now);
  portEXIT_CRITICAL(&bleMux);
  return mask;
}

void BleRemoteManager::begin() {
  if (!hasPairing()) {
    return;  // feature off: no init, no hook, no radio
  }
  DiagLog::mem("ble-pre");
  if (!initStack()) {
    LOG_ERR("BLE", "NimBLE init failed");
    DiagLog::line("BLE init failed");
    state_ = State::Off;
    return;
  }
  DiagLog::mem("ble-post");
  state_ = State::WaitingForPeer;
  startReconnectScan(/*aggressive=*/true);  // v95: the user is likely pressing the remote right now
}

bool BleRemoteManager::ensureStarted() {
  if (state_ == State::StoppedForWifi) {
    return false;
  }
  if (stackReady_) {
    return true;
  }
  DiagLog::mem("ble-pre");
  if (!initStack()) {
    LOG_ERR("BLE", "NimBLE init failed");
    DiagLog::line("BLE init failed");  // M12: parity with begin()
    return false;
  }
  DiagLog::mem("ble-post");
  return true;
}

bool BleRemoteManager::initStack() {
  if (stackReady_) {
    return true;
  }
  if (!NimBLEDevice::init("CrossMosa")) {
    return false;
  }
  NimBLEDevice::setSecurityAuth(true, false, false);  // bond, no MITM, no SC (Just Works)
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setMaxResults(0);  // results consumed in the callback; store nothing
  HalGPIO::setButtonHook(&BleRemoteManager::buttonHookThunk);
  stackReady_ = true;
  return true;
}

void BleRemoteManager::teardownStack(bool clearAll) {
  if (!stackReady_) {
    return;
  }
  NimBLEDevice::getScan()->stop();
  if (client_ != nullptr && client_->isConnected()) {
    // I5: deinit() while a disconnect is still in flight leaks the client's
    // connection slot -- eventually createClient() returns nullptr forever.
    // Bounded main-task wait for the link to actually drop first (<=600ms;
    // typical is instant). Shares this path with shutdownForSleep(), where a
    // worst-case 600ms stall at sleep entry is an acceptable trade for never
    // leaking a slot.
    client_->disconnect();
    const uint32_t deadline = millis() + 600;
    // Fix round 2: wraparound-safe idiom -- millis() < deadline breaks near the
    // uint32_t rollover (~49.7 days uptime), since the comparison stops being
    // monotonic right at the wrap. Signed subtraction wraps the same way on
    // both sides of the rollover, so the sign of the difference stays correct.
    while (!disconnected_ && client_->isConnected() &&
           static_cast<int32_t>(millis() - deadline) < 0) {
      delay(10);
    }
    disconnected_ = false;
  } else if (client_ != nullptr &&
             (state_ == State::Connecting || state_ == State::Pairing || securing_)) {
    // I5 residual (fix round 2): an ASYNC CONNECT in flight has no live ACL
    // link yet -- isConnected() reads false the whole time NimBLE's internal
    // m_connStatus is CONNECTING, not CONNECTED (confirmed by reading
    // NimBLEClient.cpp: it only flips to CONNECTED inside the
    // BLE_GAP_EVENT_CONNECT handler once the GAP procedure actually
    // succeeds) -- so the branch above never triggers for it, and deinit()
    // would tear down the host stack out from under a connect NimBLE itself
    // still considers pending, leaking the client's connection slot the same
    // way an in-flight disconnect does. cancelConnect() (ble_gap_conn_cancel())
    // aborts the GAP connect procedure; NimBLE reports the abort back through
    // the SAME BLE_GAP_EVENT_CONNECT path a real connect failure takes
    // (verified in NimBLEClient.cpp), which fires our onConnectFail callback
    // (connFailed_) -- watching disconnected_ too is defensive in case some
    // other path is taken. (securing_ is included per the review but is
    // provably redundant here: it can only be true once connEstablished_ has
    // already fired, and that only happens after m_connStatus is already
    // CONNECTED -- i.e. isConnected() would already be true and we'd have
    // taken the branch above instead. Left in for defensive completeness /
    // to match the same set of "abort mid-flight" states used elsewhere in
    // this file, not because it changes behavior.)
    client_->cancelConnect();
    const uint32_t deadline = millis() + 300;
    while (!connFailed_ && !disconnected_ && static_cast<int32_t>(millis() - deadline) < 0) {
      delay(10);
    }
    connFailed_ = false;
    disconnected_ = false;
  }
  NimBLEDevice::deinit(clearAll);
  client_ = nullptr;
  stackReady_ = false;
  reportCount_ = 0;
  scanMode_ = ScanMode::None;
  // I3 hygiene: teardownStack() can run mid-securing_ (e.g. the pairing
  // activity's onExit() calling stopIfUnpaired() while a connect is still in
  // flight). Left stale-true, a later re-init + connect whose own
  // secureConnection(async=true) call fails synchronously would see this
  // securing_ plus a long-expired authDeadlineMs_ and spuriously re-run the
  // auth-timeout branch on top of the failure branch's own handling in the
  // same tick() -- redundant, not corrupting, but not the intent either.
  securing_ = false;
  portENTER_CRITICAL(&bleMux);
  latch_.reset();
  portEXIT_CRITICAL(&bleMux);
  HalGPIO::setButtonHook(nullptr);
}

void BleRemoteManager::stopIfUnpaired() {
  if (!hasPairing()) {
    if (stackReady_ && state_ != State::StoppedForWifi) {
      teardownStack(true);
      state_ = State::Off;
    }
    return;
  }
  // I1: a pairing exists -- make sure something is actually trying to reach
  // it. Covers the pairing-UI exit path where a scan ran but the user backed
  // out without pairing (state() left in Scanning/ScanDone/PairFailed, which
  // tick() has no edge back to WaitingForPeer from).
  resumeReconnectIfPaired();
}

void BleRemoteManager::resumeReconnectIfPaired() {
  if (!stackReady_ || !hasPairing() || state_ == State::StoppedForWifi) {
    return;
  }
  // Fix round 2: already connected, or a connect/pairing is in flight -- don't
  // stomp the state machine; tick() owns those paths. The original
  // `client_ && client_->isConnected()` check missed Connecting/Pairing
  // entirely (isConnected() only reflects the ACL link, which isn't up yet
  // during either state). Reachable via the Task 6 contract: a background
  // reconnect completes its ACL link (state Connecting) right as the user
  // opens the pairing screen's status view, then backs out (Back ->
  // stopIfUnpaired() -> here) before GATT setup finishes -- without this
  // guard, state_ gets overwritten to WaitingForPeer and the connect that's
  // about to succeed has its connEstablished_ flag consumed-and-dropped by
  // tick() (state_ no longer Connecting/Pairing when it lands), leaving a live
  // but never-secured-or-subscribed ACL link: the remote looks connected at
  // the radio level but is silently dead until the link eventually drops.
  if ((client_ != nullptr && client_->isConnected()) || state_ == State::Connecting ||
      state_ == State::Pairing) {
    return;
  }
  NimBLEDevice::getScan()->stop();
  scanMode_ = ScanMode::None;
  state_ = State::WaitingForPeer;
  startReconnectScan(/*aggressive=*/true);  // v95: entering the pairing screen means the user is pressing keys
}

void BleRemoteManager::setReaderActive(bool active) {
  // v96: called from EpubReaderActivity onEnter/onExit. Restarts the reconnect scan so the
  // new duty cycle takes effect immediately -- NimBLE applies interval/window at start().
  if (readerActive_ == active) {
    return;
  }
  readerActive_ = active;
  if (!stackReady_ || state_ != State::WaitingForPeer || scanMode_ != ScanMode::Reconnect) {
    return;  // nothing listening right now; whatever starts next picks up the flag
  }
  NimBLEDevice::getScan()->stop();
  startReconnectScan();
}

void BleRemoteManager::stopForWifi() {
  if (state_ == State::StoppedForWifi) {
    return;
  }
  if (stackReady_) {
    DiagLog::line("BLE stopForWifi");
    teardownStack(true);
  }
  state_ = State::StoppedForWifi;
}

void BleRemoteManager::shutdownForSleep() {
  if (!stackReady_) {
    return;
  }
  teardownStack(false);  // chip resets on wake; begin() re-inits from scratch
}

void BleRemoteManager::startReconnectScan(bool aggressive) {
  scanMode_ = ScanMode::Reconnect;
  // M1: parse once per scan start rather than per advertisement -- onResult()
  // runs on the host task for every packet in range and used to allocate a
  // std::string there via getAddress().toString() just to compare it.
  parseAddrString(SETTINGS.bleRemotePeerAddr, peerAddrBytes_);
  peerAddrType_ = SETTINGS.bleRemotePeerAddrType;
  NimBLEScan* scan = NimBLEDevice::getScan();
  // v99: active, because the zero-address fallback above matches on the advertised NAME and
  // some peripherals only carry the name in the scan response. Costs a SCAN_REQ per advert.
  scan->setActiveScan(true);
  // v95: two duty cycles, because 2.3% was too thin to catch this remote.
  //
  // A Kobo page-turner sleeps and only advertises for a second or two after a
  // button press -- roughly 20 packets at a 100ms advertising interval. Listening
  // 30ms out of every 1280ms means we hear 2.3% of them: an expected 0.46 hits
  // per button press, i.e. it is more likely than not that pressing a key does
  // nothing at all. That is exactly what the field report said ("並不是像畫面說的那樣,
  // 按一下遙控器的按鈕就配對好了").
  //
  // So: right after boot/wake, and whenever the user opens the pairing screen,
  // scan almost continuously for 5 seconds (80% duty) -- that is when the user
  // is actually pressing the remote. Then fall back to the thin duty cycle for
  // the idle case, where battery matters and there is no deadline. tick()'s
  // self-heal restarts the thin scan when the burst ends (it calls this with the
  // default argument).
  // v96: the 5-second burst was the right idea at the wrong moment. diag95 settled it:
  // the reconnect scan decoded 592 advertisements and matched none, while the pairing scan
  // saw the same remote at rssi=-51. A bonded BLE HID peripheral does not advertise while
  // idle -- it sleeps, and only advertises for a second or two after a key press. The burst
  // fires at boot, which is precisely when the user has NOT yet touched the remote.
  //
  // So the duty cycle now follows where the user is: inside the reader, listen continuously,
  // because that is both when a page-turn command matters and when the key gets pressed.
  // Everywhere else, stay thin. Costs receive current while reading; that is the trade.
  const bool highDuty = aggressive || readerActive_;
  scan->setInterval(highDuty ? 60 : 1280);
  scan->setWindow(highDuty ? 48 : 30);
  scan->setDuplicateFilter(false);  // the peer must re-trigger after every miss
  DiagLog::line("BLE reconnect scan start %s peer=%s adverts=%lu",
                readerActive_ ? "reader" : (aggressive ? "burst" : "idle"), SETTINGS.bleRemotePeerAddr,
                static_cast<unsigned long>(reconnectAdvertsSeen_));
  // I2(a): a failed start used to leave the machine silently dead in
  // WaitingForPeer forever. tick()'s periodic check below retries after
  // reconnectRetryAtMs_ and also self-heals a "forever" scan that ends
  // unexpectedly (NimBLEScan::isScanning() goes false either way).
  // v97: EVERY reconnect scan is now time-boxed, and that is a memory fix, not a policy one.
  //
  // NimBLEScan stores a heap-allocated NimBLEAdvertisedDevice for every distinct advertiser it
  // decodes, and the only thing that frees them is clearResults() -- which refuses to run while
  // a scan is active and is otherwise called only from start(). A scan started with duration 0
  // never ends, so that list never gets cleared: it grows for as long as the scan runs.
  //
  // We asked for that with setMaxResults(0). NimBLE's own doc comment says "0 == none (callbacks
  // only)", but the code reads `if (m_maxResults > 0 && ...) return 0;` -- with 0 the cap is
  // skipped entirely and every device is stored. Doc and implementation disagree, and the
  // implementation wins.
  //
  // Raising the cap instead is not an option: the store happens BEFORE onResult() fires, so a
  // capped list would silence the callback for every device past the cap -- and the one device
  // we need might be number 13. Cycling the scan is the fix that keeps every advertisement
  // visible while bounding the list to one cycle's worth. tick()'s existing self-heal restarts
  // it (reconnectRetryAtMs_ is long past by then), and start() clears the list on the way in.
  //
  // This is the most likely cause of the v95 and v96 crashes: both died at the pairing screen
  // immediately after ble-scan-pre -- i.e. while opening a SECOND scan on top of a list that had
  // been growing since boot -- with an empty panic reason and a heap-poison canary in the dump.
  const uint32_t durationMs = readerActive_ ? 20000 : (aggressive ? 5000 : 60000);
  if (!scan->start(durationMs, false, true)) {
    reconnectRetryAtMs_ = millis() + 2000;
  } else {
    // Refresh on the success path too: otherwise a stale value from an
    // earlier failed start lingers untouched, and the wraparound-safe
    // int32_t comparison in tick() would treat it as already-due again once
    // millis() wraps back around to it (~24.8 days of continuous uptime),
    // theoretically pausing the self-heal gate until then.
    reconnectRetryAtMs_ = millis() + 2000;
  }
}

void BleRemoteManager::disconnectAndWaitIfConnected() {
  if (client_ == nullptr || !client_->isConnected()) {
    return;
  }
  // C1: connect() rejects (BLE_HS_EREJECT) when called on an already-connected
  // client, and its synchronous failure path sets internal state to
  // DISCONNECTED regardless of the real ACL state -- so callers about to reuse
  // client_ for a fresh connect() (startScan() entering pairing mode,
  // pairWith() itself) must confirm the old link actually dropped first, not
  // just fire disconnect() and move on. Bounded main-task wait: both callers
  // are pairing-UI entry points reached only from user action, so a <=1s stall
  // here is acceptable (mirrors the I5/teardownStack bounded wait).
  client_->disconnect();
  const uint32_t deadline = millis() + 1000;
  // Fix round 2: wraparound-safe idiom, see teardownStack()'s comment.
  while (!disconnected_ && static_cast<int32_t>(millis() - deadline) < 0) {
    delay(10);
  }
  disconnected_ = false;
  securing_ = false;  // I3 hygiene: same reasoning as teardownStack() -- this
                      // can abort a connect that was mid-securing_
  reportCount_ = 0;
  portENTER_CRITICAL(&bleMux);
  latch_.reset();
  portEXIT_CRITICAL(&bleMux);
}

bool BleRemoteManager::startScan() {
  if (!ensureStarted()) {
    return false;
  }
  // C1(a) / M5: drop any live link (and the old remote's latched buttons)
  // before entering pairing mode -- see disconnectAndWaitIfConnected().
  disconnectAndWaitIfConnected();
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->stop();
  portENTER_CRITICAL(&bleMux);
  scanCount_ = 0;
  portEXIT_CRITICAL(&bleMux);
  scanMode_ = ScanMode::Pairing;
  advertsSeen_ = 0;
  scanEnded_ = false;
  scan->setActiveScan(true);  // scan responses carry the device names
  scan->setInterval(60);
  scan->setWindow(30);
  scan->setDuplicateFilter(1);
  state_ = State::Scanning;
  if (!scan->start(kPairingScanMs, false, true)) {
    // I2(b): only go all the way to Off when there's nothing to preserve --
    // an existing pairing must fall back to reconnecting, not go dark.
    if (hasPairing()) {
      resumeReconnectIfPaired();
    } else {
      state_ = State::Off;
    }
    return false;
  }
  return true;
}

void BleRemoteManager::stopScan() {
  if (!stackReady_) {
    return;
  }
  NimBLEDevice::getScan()->stop();
  scanMode_ = ScanMode::None;  // M4: stop late advertisements from mutating
                               // scanEntries_ after the UI has moved on
  if (state_ == State::Scanning) {
    state_ = State::ScanDone;
  }
}

uint8_t BleRemoteManager::scanResults(ScanEntry* out, uint8_t cap) {
  portENTER_CRITICAL(&bleMux);
  const uint8_t n = scanCount_ < cap ? scanCount_ : cap;
  for (uint8_t i = 0; i < n; i++) {
    out[i] = scanEntries_[i];
  }
  portEXIT_CRITICAL(&bleMux);
  return n;
}

void BleRemoteManager::handleScanResult(const NimBLEAdvertisedDevice* dev) {
  if (scanMode_ == ScanMode::Reconnect) {
    if (state_ != State::WaitingForPeer) {
      return;
    }
    // M1: byte compare against the bytes parsed once in startReconnectScan()
    // -- was a std::string allocation (toString()) on every advertisement in
    // range, on the host task.
    reconnectAdvertsSeen_++;  // v95: "did the radio hear anything at all" (host task, plain counter)
    const uint8_t* rval = dev->getAddress().getVal();
    if (memcmp(rval, peerAddrBytes_, 6) == 0) {
      connectRequested_ = true;
      return;
    }
    // v99: the same zero-address quirk the pairing path already works around. diag98 is
    // conclusive -- "zero addr seen=1 substituted=1" (so the remote DOES come through with
    // 00:00:00:00:00:00) next to "reconnect_adverts=897" with zero matches, while manual
    // scan-and-pair succeeds because THAT path substitutes the stored address. The reconnect
    // path was still doing a raw compare against the real address, which a zero address can
    // never satisfy: that, and not the duty cycle, is why auto-reconnect has never once
    // worked. Fall back to matching the advertised NAME when the address is zeroed.
    //
    // getName() allocates a std::string, and this runs on the NimBLE host task for every
    // advertisement in range -- which is exactly what M1 above removed from this path. So it
    // is reached ONLY in the all-zero case (once per session in practice), never on the hot
    // path.
    if (rval[0] == 0 && rval[1] == 0 && rval[2] == 0 && rval[3] == 0 && rval[4] == 0 && rval[5] == 0 &&
        SETTINGS.bleRemotePeerName[0] != '\0') {
      zeroAddrSeen_++;
      const std::string advName = dev->getName();
      if (!advName.empty() && strncmp(advName.c_str(), SETTINGS.bleRemotePeerName,
                                      sizeof(SETTINGS.bleRemotePeerName) - 1) == 0) {
        zeroAddrSubstituted_++;
        connectRequested_ = true;  // beginConnect() uses SETTINGS.bleRemotePeerAddr, not this packet
      }
    }
    return;
  }
  if (scanMode_ != ScanMode::Pairing) {
    return;
  }
  // v86: NO filtering. v84/v85 only accepted devices whose ADVERTISEMENT
  // declared the HID service (0x1812) or a HID-class appearance — but that
  // declaration is optional, and plenty of BLE HID peripherals expose the HID
  // service only after you connect. A real page-turner (8BitDo Micro) was
  // invisible on v85 with a scan that ran for 90 s. Every device is listed
  // now; the HID hint is kept as a display marker and a sort key, never as a
  // gate. (advertsSeen_ counts them all, filtered or not, so a log with
  // adverts>0 and an empty list can never again be confused with a dead radio.)
  advertsSeen_++;
  const bool hidAppearance = dev->haveAppearance() && (dev->getAppearance() >> 6) == 0x000F;
  const bool looksLikeHid = dev->isAdvertisingService(NimBLEUUID(kHidService)) || hidAppearance;
  ScanEntry e = {};
  e.isHid = looksLikeHid;
  e.appearance = dev->haveAppearance() ? dev->getAppearance() : 0;
  // M1: build from raw bytes instead of toString() (which heap-allocates a
  // std::string that may exceed this libstdc++'s SSO buffer for a 17-char MAC
  // string). Reversed: ble_addr_t val[] is stored inverse of the printed
  // string -- see parseAddrString's comment above.
  const uint8_t* val = dev->getAddress().getVal();
  snprintf(e.addr, sizeof(e.addr), "%02x:%02x:%02x:%02x:%02x:%02x", val[5], val[4], val[3], val[2],
           val[1], val[0]);
  e.addrType = dev->getAddress().getType();

  e.rssi = static_cast<int8_t>(dev->getRSSI());
  const std::string name = dev->getName();  // pairing scan only, low rate -- fine
  strlcpy(e.name, name.empty() ? e.addr : name.c_str(), sizeof(e.name));

  // v95/v98: NimBLE sometimes hands us an all-zero address for a peer we are already
  // bonded to (diag94: the same Kobo listed as a4:3c:d7:94:89:5d in one session and
  // 00:00:00:00:00:00 in the next, name and HID flag intact). A zero address matches no
  // stored pairing, so pairWith()'s stale-bond drop never fires and beginConnect() dials an
  // address nothing answers -- that was the whole "must forget before re-pairing" complaint.
  // Substituting the stored address makes "select it, press pair" work by itself; gated on
  // the NAME matching so it can never redirect a pairing attempt at some other device.
  //
  // ⚠️ v98: this used to also call DiagLog::line() here. That was the bug behind the v95,
  // v96 and v97 panics -- three identical crashes with an empty panic reason whose stack
  // dumps carried _svfprintf_r, this function's format string, and printf's hex table.
  // DiagLog::line writes to SD and needs a 416-byte buffer plus vfprintf's frame; this runs
  // on the NimBLE HOST TASK, whose stack is 2,560 bytes (the 0xA5-filled block in every one
  // of those dumps). It overflowed. The rule this file states everywhere -- "Pure flag
  // writes here, no I/O, same rule as every other callback in this file" -- is exactly the
  // rule I broke. Counters only now; the main task prints them.
  if (val[0] == 0 && val[1] == 0 && val[2] == 0 && val[3] == 0 && val[4] == 0 && val[5] == 0) {
    zeroAddrSeen_++;
    if (SETTINGS.bleRemotePeerAddr[0] != '\0' && SETTINGS.bleRemotePeerName[0] != '\0' &&
        strncmp(e.name, SETTINGS.bleRemotePeerName, sizeof(e.name)) == 0) {
      strlcpy(e.addr, SETTINGS.bleRemotePeerAddr, sizeof(e.addr));
      e.addrType = SETTINGS.bleRemotePeerAddrType;
      zeroAddrSubstituted_++;
    }
  }

  portENTER_CRITICAL(&bleMux);
  for (uint8_t i = 0; i < scanCount_; i++) {
    if (strcmp(scanEntries_[i].addr, e.addr) == 0) {
      // Refresh: an active-scan response may add the name after the first hit.
      if (e.name[0] != '\0' && strcmp(e.name, e.addr) != 0) {
        scanEntries_[i] = e;
      } else {
        scanEntries_[i].rssi = e.rssi;
      }
      portEXIT_CRITICAL(&bleMux);
      return;
    }
  }
  if (scanCount_ < kMaxScanEntries) {
    scanEntries_[scanCount_++] = e;
  } else if (e.isHid) {
    // List full and a HID device just showed up: evict the weakest non-HID
    // entry rather than dropping the one the user actually came here for.
    // Without this, a dozen phones/earbuds in range would starve the remote
    // out of the list — the exact failure the removed filter used to prevent.
    uint8_t worst = kMaxScanEntries;
    for (uint8_t i = 0; i < scanCount_; i++) {
      if (!scanEntries_[i].isHid && (worst == kMaxScanEntries || scanEntries_[i].rssi < scanEntries_[worst].rssi)) {
        worst = i;
      }
    }
    if (worst < kMaxScanEntries) {
      scanEntries_[worst] = e;
    }
  }
  portEXIT_CRITICAL(&bleMux);
}

bool BleRemoteManager::pairWith(const ScanEntry& entry) {
  if (!stackReady_ || state_ == State::StoppedForWifi) {
    return false;
  }
  NimBLEDevice::getScan()->stop();
  scanMode_ = ScanMode::None;
  // C1: do NOT delete the old bond here -- for a DIFFERENT remote. If
  // beginConnect() below fails (out of range, or connect() rejects because the
  // client is still live) the OLD remote must remain pairable after reboot --
  // deleting its bond up front and then failing to pair the new one would brick
  // it. The old bond is only dropped on confirmed pairing SUCCESS, in tick()'s
  // pairSuccessPending_ handling, and only when the new address actually differs
  // from the old one.
  //
  // ⚠️ The parenthetical that used to end this comment -- "re-pairing the same
  // remote just overwrites its NVS bond" -- was WRONG, and cost a hardware
  // debugging round. It does not overwrite: a live bond makes BLE encrypt rather
  // than re-pair. The same-address case is handled explicitly below.
  //
  // startScan() (the normal pairing-UI entry point) already drops a live link
  // before the user ever reaches this call, but guard it again here too --
  // NimBLEClient::connect() rejects (BLE_HS_EREJECT) when called on an
  // already-connected client, and its failure path desyncs isConnected() from
  // the real ACL state (see disconnectAndWaitIfConnected()'s comment).
  disconnectAndWaitIfConnected();

  // v92: re-pairing the SAME remote must drop its bond FIRST.
  //
  // C1 above is right that a bond must survive a failed attempt — but only when
  // the failure could strand a *different*, still-good pairing. Re-pairing the
  // address we are already bonded to is the opposite case, and leaving the bond
  // in place there is a deadlock, observed on hardware (2026-08-05):
  //
  //   The remote had been paired to something else in the meantime, so it no
  //   longer held our key. Our NVS bond survived (a firmware flash does not
  //   touch NVS). From then on BLE never re-pairs — seeing a bond, it tries to
  //   ENCRYPT with the stored LTK, the remote rejects it, and both auto-reconnect
  //   and "select it in the list and pair" fail identically, every time. The one
  //   line that clears the stale bond lives in tick()'s pairSuccessPending_
  //   handler, i.e. behind the success that the stale bond is preventing.
  //
  // The user escaped it by hand with Settings → forget → pair again, which is
  // exactly this deleteBond() with extra steps. Doing it here makes the obvious
  // action — pick the remote, press pair — work on its own.
  //
  // Narrow on purpose: gated on the address matching, so the different-remote
  // case keeps C1's behaviour byte for byte. Nothing is lost when this fires —
  // the bond it deletes is the one the user is asking us to replace, and if the
  // attempt then fails they are no worse off than the deadlock they were in.
  if (hasPairing() && strcasecmp(entry.addr, SETTINGS.bleRemotePeerAddr) == 0) {
    DiagLog::line("BLE dropping stale bond for %s before re-pair", SETTINGS.bleRemotePeerAddr);
    NimBLEDevice::deleteBond(NimBLEAddress(std::string(SETTINGS.bleRemotePeerAddr), SETTINGS.bleRemotePeerAddrType));
  }

  strlcpy(pendingAddr_, entry.addr, sizeof(pendingAddr_));
  pendingAddrType_ = entry.addrType;
  strlcpy(pendingName_, entry.name, sizeof(pendingName_));
  beginConnect(entry.addr, entry.addrType, /*pairing=*/true);
  return state_ == State::Pairing;
}

void BleRemoteManager::beginConnect(const char* addr, uint8_t addrType, bool pairing) {
  if (client_ == nullptr) {
    client_ = NimBLEDevice::createClient();
    if (client_ == nullptr) {
      lastError_ = -1;
      if (pairing) {
        state_ = State::PairFailed;
      } else {
        // Off would be unrecoverable here: tick() early-returns whenever
        // state_ == Off, so the I2 self-heal gate below (which only fires
        // from WaitingForPeer AND scanMode_ == Reconnect) would never get
        // another look at this. Land in WaitingForPeer with the retry gate
        // armed and scanMode_ set to Reconnect instead -- do NOT call
        // startReconnectScan() here (recursion); tick()'s periodic check
        // genuinely fires on its own from this state (or, sooner, the next
        // resumeReconnectIfPaired()) and restarts the scan.
        state_ = State::WaitingForPeer;
        reconnectRetryAtMs_ = millis() + 2000;
        scanMode_ = ScanMode::Reconnect;  // the self-heal gate in tick() requires this;
                                           // no scan is running so no callback can fire meanwhile
      }
      return;
    }
    client_->setClientCallbacks(&clientCallbacks, false);
    client_->setConnectTimeout(kConnectTimeoutMs);
  }
  connEstablished_ = false;
  connFailed_ = false;
  disconnected_ = false;
  notifyCount_ = 0;  // M6: tally starts fresh for this connection attempt
  state_ = pairing ? State::Pairing : State::Connecting;
  // Async connect: the result arrives via onConnect/onConnectFail and is
  // consumed in tick() — nothing here may block the main loop.
  if (!client_->connect(NimBLEAddress(std::string(addr), addrType), true, /*asyncConnect=*/true,
                        /*exchangeMTU=*/false)) {
    lastError_ = -2;
    if (pairing) {
      state_ = State::PairFailed;
    } else {
      state_ = State::WaitingForPeer;
      startReconnectScan();
    }
  }
}

bool BleRemoteManager::finishGattSetup() {
  // M3: reportCount_ must land at 0 BEFORE reportMap_.parse() touches the
  // decode table below, and each reports_[i] slot must be fully written
  // BEFORE reportCount_ is incremented past it (M2 made it volatile so the
  // compiler can't defer that write past the slot writes). handleNotify() runs
  // on the host task and can fire mid-setup on a reconnect -- a bonded peer
  // may resume sending notifications on its own remembered CCCD state before
  // our subscribe() call below runs again. It looks a characteristic up by
  // scanning reports_[0..reportCount_) and only calls reportMap_.decode() on a
  // hit; as long as reportCount_==0 while parse() is rewriting the table, and
  // a slot only becomes visible via reportCount_ once it's complete, that
  // lookup can never observe a half-built reportMap_ or a half-written slot.
  // Moving either line opens that race.
  reportCount_ = 0;
  // I3: secureConnection() used to run here, synchronously. It's gone -- tick()
  // now drives it asynchronously before calling this function at all, because
  // the sync call blocks up to BLE_SM_TIMEOUT (30s) if the peer stalls SMP,
  // and this firmware has no task watchdog to save the main loop from that.
  if (client_ == nullptr || !client_->isConnected()) {
    lastError_ = -3;
    return false;
  }
  NimBLERemoteService* svc = client_->getService(NimBLEUUID(kHidService));
  if (svc == nullptr) {
    // v87: "-5" alone cannot distinguish "this remote speaks HID but we asked
    // wrong" from "this device simply has no HID-over-GATT profile". A real
    // 8BitDo Micro (v86, diag86) connected and encrypted fine, then failed
    // here — so dump what it DOES expose. Runs on the main task inside tick(),
    // where blocking GATT is allowed; getServices(true) is bounded by the
    // supervision timeout and the discovery has already happened by now.
    lastError_ = -5;
    const auto& services = client_->getServices(true);
    DiagLog::line("BLE no HID service; device exposes %u services", static_cast<unsigned>(services.size()));
    uint8_t logged = 0;
    for (const auto* s : services) {
      if (logged++ >= 12) {  // bounded: a chatty device must not flood diag.log
        DiagLog::line("BLE   ... (more services not logged)");
        break;
      }
      DiagLog::line("BLE   service %s", s->getUUID().toString().c_str());
    }
    return false;
  }
  NimBLERemoteCharacteristic* mapChr = svc->getCharacteristic(NimBLEUUID(kReportMapChar));
  if (mapChr == nullptr) {
    lastError_ = -6;
    return false;
  }
  NimBLEAttValue mapVal = mapChr->readValue();
  if (mapVal.size() == 0 || !reportMap_.parse(mapVal.data(), mapVal.size())) {
    lastError_ = -7;
    return false;
  }
  for (auto* chr : svc->getCharacteristics(true)) {
    if (reportCount_ >= kMaxReports) {
      break;
    }
    if (chr->getUUID() != NimBLEUUID(kReportChar) || !chr->canNotify()) {
      continue;
    }
    uint8_t reportId = 0;
    bool haveReportRef = false;
    if (auto* refDesc = chr->getDescriptor(NimBLEUUID(kReportRefDesc))) {
      NimBLEAttValue ref = refDesc->readValue();
      if (ref.size() >= 2) {
        if (ref[1] != kReportTypeInput) {
          continue;  // output/feature report — not ours
        }
        reportId = ref[0];
        haveReportRef = true;
      }
    }
    // M10: a missing/short Report Reference descriptor on a multi-report-ID
    // device silently decodes every notification against the wrong report
    // (id=0) -- the device would look connected-but-dead with nothing in the
    // log to explain why.
    if (!haveReportRef && reportMap_.hasReportIds()) {
      DiagLog::line("BLE report-ref missing; id=0 fallback");
    }
    if (!chr->subscribe(true, &BleRemoteManager::onNotifyStatic)) {
      continue;
    }
    reports_[reportCount_].chr = chr;
    reports_[reportCount_].reportId = reportId;
    reports_[reportCount_].prevButtons = 0;
    // M2: `reportCount_++` on a volatile is deprecated in C++20 (-Wvolatile;
    // ambiguous whether it's one or two volatile accesses) -- explicit
    // read-then-store is the unambiguous, non-deprecated equivalent, and
    // still publishes the slot (writes above) before the count that makes it
    // visible (see the invariant comment at the top of this function).
    reportCount_ = static_cast<uint8_t>(reportCount_ + 1);
  }
  if (reportCount_ == 0) {
    lastError_ = -8;
    return false;
  }
  return true;
}

void BleRemoteManager::onNotifyStatic(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len,
                                      bool isNotify) {
  (void)isNotify;
  BLE_REMOTE.handleNotify(chr, data, len);
}

void BleRemoteManager::handleNotify(NimBLERemoteCharacteristic* chr, const uint8_t* data, size_t len) {
  // M3: this lookup only ever sees a slot once finishGattSetup() (main task)
  // has published it by incrementing reportCount_ past it, and reportMap_ is
  // only ever decoded against once parse() has fully completed (reportCount_
  // is 0 for the whole duration of parse()) -- see the invariant comment above
  // finishGattSetup()'s reportCount_ = 0 line. Runs on the host task; do not
  // reorder either side of that invariant.
  uint8_t idx = kMaxReports;
  for (uint8_t i = 0; i < reportCount_; i++) {
    if (reports_[i].chr == chr) {
      idx = i;
      break;
    }
  }
  if (idx == kMaxReports) {
    return;
  }
  HidReportMap::Usage usages[HidReportMap::kMaxActiveUsages];
  const uint8_t n =
      reportMap_.decode(reports_[idx].reportId, data, len, usages, HidReportMap::kMaxActiveUsages);
  uint8_t newMask = 0;
  for (uint8_t i = 0; i < n; i++) {
    const int btn = hidUsageToButtonIndex(usages[i].page, usages[i].usage);
    if (btn >= 0 && btn < static_cast<int>(BleButtonLatch::kButtonCount) && btn != HalGPIO::BTN_POWER) {
      newMask |= static_cast<uint8_t>(1u << btn);
    }
  }
  const uint8_t prev = reports_[idx].prevButtons;
  reports_[idx].prevButtons = newMask;
  const uint8_t pressed = newMask & static_cast<uint8_t>(~prev);
  const uint8_t released = prev & static_cast<uint8_t>(~newMask);
  if (pressed == 0 && released == 0) {
    return;
  }
  portENTER_CRITICAL(&bleMux);
  for (uint8_t b = 0; b < BleButtonLatch::kButtonCount; b++) {
    if (pressed & (1u << b)) {
      latch_.pressEvent(b);
    }
    if (released & (1u << b)) {
      latch_.releaseEvent(b);
    }
  }
  portEXIT_CRITICAL(&bleMux);
  notifyCount_++;
}

bool BleRemoteManager::consumePairSuccess(char* addrOut, size_t addrCap, uint8_t& typeOut, char* nameOut,
                                          size_t nameCap) {
  if (!pairSuccessPending_) {
    return false;
  }
  pairSuccessPending_ = false;
  strlcpy(addrOut, pendingAddr_, addrCap);
  typeOut = pendingAddrType_;
  strlcpy(nameOut, pendingName_, nameCap);
  return true;
}

void BleRemoteManager::forgetPairing() {
  // M9: caller contract is "only call this when the stack is started" (the
  // pairing UI only offers "forget" once ensureStarted()/startScan() have
  // already run). If that contract is violated -- called while !stackReady_ --
  // this returns without touching the NVS bond, so it stays orphaned
  // (unreachable, since SETTINGS.bleRemotePeerAddr is about to be cleared by
  // the caller regardless) rather than silently no-op'd as "forgotten".
  if (!stackReady_) {
    return;
  }
  NimBLEDevice::getScan()->stop();
  scanMode_ = ScanMode::None;
  if (client_ != nullptr && client_->isConnected()) {
    client_->disconnect();
  }
  if (hasPairing()) {
    NimBLEDevice::deleteBond(
        NimBLEAddress(std::string(SETTINGS.bleRemotePeerAddr), SETTINGS.bleRemotePeerAddrType));
  }
  reportCount_ = 0;
  portENTER_CRITICAL(&bleMux);
  latch_.reset();
  portEXIT_CRITICAL(&bleMux);
  state_ = State::Off;  // caller clears settings and, on exit, stopIfUnpaired()
}

void BleRemoteManager::tick() {
  if (state_ == State::Off || state_ == State::StoppedForWifi || !stackReady_) {
    return;
  }

  if (disconnected_) {
    disconnected_ = false;
    securing_ = false;  // abandon any in-flight security phase (defensive; see I3)
    portENTER_CRITICAL(&bleMux);
    latch_.reset();
    portEXIT_CRITICAL(&bleMux);
    reportCount_ = 0;
    if (state_ == State::Connected || state_ == State::Connecting) {
      // M6: notifyCount_ tallies this connection's button events; log it here
      // (its only consumer) then let the next beginConnect() reset it to 0.
      DiagLog::line("BLE disconnected err=%d notifies=%lu", cbError_,
                    static_cast<unsigned long>(notifyCount_));
      if (hasPairing()) {
        state_ = State::WaitingForPeer;
        startReconnectScan();
      } else {
        state_ = State::Off;
      }
    } else if (state_ == State::Pairing) {
      lastError_ = cbError_;
      state_ = State::PairFailed;
    }
  }

  if (connectRequested_) {
    connectRequested_ = false;
    if (state_ == State::WaitingForPeer) {
      NimBLEDevice::getScan()->stop();
      scanMode_ = ScanMode::None;
      beginConnect(SETTINGS.bleRemotePeerAddr, SETTINGS.bleRemotePeerAddrType, /*pairing=*/false);
    }
  }

  if (connFailed_) {
    connFailed_ = false;
    securing_ = false;  // defensive; connFailed_ always precedes the security phase
    lastError_ = cbError_;
    if (state_ == State::Connecting) {
      DiagLog::line("BLE reconnect failed err=%d", lastError_);
      state_ = State::WaitingForPeer;
      startReconnectScan();
    } else if (state_ == State::Pairing) {
      state_ = State::PairFailed;
    }
  }

  if (connEstablished_) {
    connEstablished_ = false;
    if (state_ == State::Connecting || state_ == State::Pairing) {
      // I3: security first, asynchronously. secureConnection(sync) can block
      // up to BLE_SM_TIMEOUT (30s) if the peer stalls SMP, and this firmware
      // has no task watchdog to save the main loop from that -- the rest of
      // GATT setup (service/report-map/subscribe, still synchronous, below)
      // only runs once authDone_ lands with authOk_==true. Each of those ops
      // is bounded by the connection's 2.56s supervision timeout, so staying
      // synchronous there is fine -- the one-time stall per (re)connect is
      // acceptable and documented.
      authDone_ = false;
      authOk_ = false;
      if (client_ == nullptr || !client_->secureConnection(/*async=*/true)) {
        lastError_ = -4;
        DiagLog::line("BLE secureConnection start failed err=%d", lastError_);
        if (client_ != nullptr && client_->isConnected()) {
          client_->disconnect();
        }
        if (state_ == State::Pairing) {
          state_ = State::PairFailed;
        } else {
          state_ = State::WaitingForPeer;
          startReconnectScan();
        }
      } else {
        securing_ = true;
        authDeadlineMs_ = millis() + 12000;
      }
    }
  }

  // I3: outcome of the async security phase kicked off above -- evaluated
  // every tick() (not nested under connEstablished_), since authDone_ almost
  // always lands on a LATER call than the one that started it.
  if (securing_ && authDone_) {
    securing_ = false;
    const bool wasPairing = (state_ == State::Pairing);
    if (authOk_ && finishGattSetup()) {
      DiagLog::line("BLE connected reports=%u", reportCount_);
      if (wasPairing) {
        // C1(c): only now, on confirmed pairing SUCCESS, drop the OLD bond --
        // and only when the new peer actually differs from it (re-pairing the
        // same remote just overwrites its NVS bond in place). Dropping it
        // earlier (e.g. up front in pairWith(), the pre-fix behavior) bricks
        // the old remote if the new one then fails to pair.
        if (hasPairing() && strcasecmp(SETTINGS.bleRemotePeerAddr, pendingAddr_) != 0) {
          NimBLEDevice::deleteBond(
              NimBLEAddress(std::string(SETTINGS.bleRemotePeerAddr), SETTINGS.bleRemotePeerAddrType));
        }
        pairSuccessPending_ = true;
      }
      state_ = State::Connected;
    } else {
      // Defensive backstop, not the normal failure path -- see the comment on
      // onAuthenticationComplete()'s definition. A real SMP failure normally
      // never reaches here at all; it surfaces as a disconnect instead.
      if (!authOk_) {
        lastError_ = -9;
        DiagLog::line("BLE authentication failed");
      } else {
        DiagLog::line("BLE GATT setup failed err=%d", lastError_);
      }
      if (client_ != nullptr && client_->isConnected()) {
        client_->disconnect();
      }
      if (wasPairing) {
        state_ = State::PairFailed;
      } else {
        state_ = State::WaitingForPeer;
        startReconnectScan();
      }
    }
    // Fix round 2: wraparound-safe idiom below (int32_t subtraction, see
    // teardownStack()'s comment for why) instead of millis() > authDeadlineMs_.
  } else if (securing_ && static_cast<int32_t>(millis() - authDeadlineMs_) > 0) {
    // I3: the peer never answered SMP. Give up well before BLE_SM_TIMEOUT
    // (30s) would, rather than risk stalling indefinitely with no watchdog.
    securing_ = false;
    lastError_ = -9;
    DiagLog::line("BLE auth timeout");
    const bool wasPairing = (state_ == State::Pairing);
    if (client_ != nullptr && client_->isConnected()) {
      client_->disconnect();
    }
    if (wasPairing) {
      state_ = State::PairFailed;
    } else {
      state_ = State::WaitingForPeer;
      startReconnectScan();
    }
  }

  if (scanEnded_) {
    scanEnded_ = false;
    if (state_ == State::Scanning) {
      state_ = State::ScanDone;
    }
    // I2(a): the WaitingForPeer+Reconnect restart that used to live here is
    // now handled by the periodic self-heal check below, which additionally
    // covers a scan that failed to start in the first place (scanEnded_ never
    // fires for that case) -- so it's redundant here and has been dropped.
  }

  // I2(a): self-heal a reconnect scan that failed to start (startReconnectScan()
  // sets reconnectRetryAtMs_ on failure) or that ended unexpectedly for any
  // other reason (isScanning() goes false either way). Every WaitingForPeer
  // transition in this file calls startReconnectScan() in the same breath, so
  // isScanning() is back to true immediately after a successful (re)start --
  // this only ever fires when there is genuinely nothing running. Fix round 2:
  // wraparound-safe idiom (int32_t subtraction) instead of
  // millis() >= reconnectRetryAtMs_, see teardownStack()'s comment for why.
  if (state_ == State::WaitingForPeer && scanMode_ == ScanMode::Reconnect &&
      !NimBLEDevice::getScan()->isScanning() &&
      static_cast<int32_t>(millis() - reconnectRetryAtMs_) >= 0) {
    startReconnectScan();
  }

  if (state_ != lastLoggedState_) {
    LOG_INF("BLE", "state %d -> %d", static_cast<int>(lastLoggedState_), static_cast<int>(state_));
    lastLoggedState_ = state_;
  }
}
