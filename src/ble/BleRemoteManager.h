#pragma once

// v108: BLE is a LINK-TIME option, and it is OFF by default.
//
// The maintainer's call, after twenty versions of measurement: "閱讀體驗應該最優先要保護,這是基本
// 功能". The arithmetic never permitted both — linking NimBLE costs 28KB and its runtime a
// further 34-41KB, while one page of Chinese glyphs needs a single 35,620-byte block. With BLE
// resident the reader was left 5,000-10,000, and long chapters dropped ~52 glyphs per page.
//
// So the accessory yields, not the core function. Without -DCROSSMOSA_BLE the whole subsystem
// compiles to the no-op stub below, src/ble/ and the pairing activity are excluded from the
// build (build_src_filter), and NimBLE is not in lib_deps — the daily firmware is byte-for-byte
// unaffected. Every call site stays exactly as it is; that is the point of stubbing here rather
// than #ifdef-ing ten call sites.
//
// To build the remote firmware again: add -DCROSSMOSA_BLE to build_flags, put NimBLE-Arduino
// back in lib_deps, and drop the two exclusions from build_src_filter. See CLAUDE.md's v107 row
// for what that firmware costs.
#ifndef CROSSMOSA_BLE

#include <cstdint>

class GfxRenderer;

class BleRemoteManager {
 public:
  enum class State : uint8_t { Off, WaitingForPeer, Connecting, Connected, Scanning, ScanDone, Pairing, PairFailed, StoppedForWifi };

  static BleRemoteManager& getInstance() {
    static BleRemoteManager instance;
    return instance;
  }

  // Every method a no-op; hasPairing() false keeps callers on their "feature off" paths.
  void begin() {}
  void tick() {}
  void stopForWifi() {}
  void shutdownForSleep() {}
  void setReaderActive(bool) {}
  bool hasPairing() const { return false; }
  bool isStackActive() const { return false; }
  State state() const { return State::Off; }
};

#define BLE_REMOTE BleRemoteManager::getInstance()

#else  // CROSSMOSA_BLE


#include <cstddef>
#include <cstdint>

#include "util/BleButtonLatch.h"
#include "util/HidReportMap.h"

// Forward declarations keep NimBLE headers out of every file that includes us
// (main.cpp, activities, WiFi choke points).
class NimBLEClient;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;

// BLE HID central for page-turner remotes.
//
// Lifecycle: the feature is ON iff a pairing exists (SETTINGS.bleRemotePeerAddr
// non-empty; there is no separate enable toggle — see the design doc).
//   boot        begin(): init NimBLE only when paired, then low-duty passive
//               scan until the bonded remote advertises.
//   main loop   tick(): all deferred work — connect, blocking GATT setup,
//               reconnect scheduling, DiagLog lines. NimBLE callbacks (host
//               task) only set flags / feed the latch under bleMux.
//   pairing UI  ensureStarted()/startScan()/scanResults()/pairWith()/
//               consumePairSuccess()/forgetPairing()/stopIfUnpaired().
//   WiFi        stopForWifi(): yields the radio; terminal until reboot (every
//               WiFi path either reboots afterwards or leaves WiFi up — the
//               radio never comes back to BLE within a session).
//   deep sleep  shutdownForSleep() from enterDeepSleep().
class BleRemoteManager {
 public:
  enum class State : uint8_t {
    Off,             // stack not running (unpaired, or init failed)
    WaitingForPeer,  // passive scan for the bonded remote
    Connecting,      // reconnect connect/GATT setup in flight
    Connected,       // subscribed; buttons flowing
    Scanning,        // pairing-UI scan running
    ScanDone,        // pairing-UI scan finished; results ready
    Pairing,         // pairing-UI connect+bond+subscribe in flight
    PairFailed,      // last pairing attempt failed (lastError())
    StoppedForWifi,  // radio yielded to WiFi; terminal until reboot
  };

  struct ScanEntry {
    char name[24];
    char addr[18];  // "aa:bb:cc:dd:ee:ff" (lowercase)
    uint8_t addrType;
    int8_t rssi;
    bool isHid;           // v86: advertisement claimed HID (display hint only, never a gate)
    uint16_t appearance;  // v86: 0 when absent; logged so an unknown remote can be identified
  };
  static constexpr uint8_t kMaxScanEntries = 12;
  static constexpr uint8_t kMaxReports = 8;

  static BleRemoteManager& getInstance();

  void begin();
  bool ensureStarted();
  void stopIfUnpaired();
  // Resumes the reconnect scan when a pairing exists but nothing is currently
  // connected or trying to connect (e.g. the pairing UI ran a scan and the user
  // backed out without pairing, leaving state() in Scanning/ScanDone/PairFailed
  // with no path back to WaitingForPeer). No-op if unpaired, already connected,
  // mid-connect, or the stack isn't running. The pairing activity (Task 6) is
  // expected to call this on exit and whenever it returns to its status screen.
  void resumeReconnectIfPaired();
  void tick();
  void stopForWifi();

  // v96: tell the manager the reader is open/closed. Inside the reader the reconnect scan
  // runs continuously at high duty (that is when the remote gets pressed); elsewhere it
  // stays at the thin idle duty. Costs receive current while reading.
  void setReaderActive(bool active);
  void shutdownForSleep();

  bool startScan();
  void stopScan();
  uint8_t scanResults(ScanEntry* out, uint8_t cap);
  bool pairWith(const ScanEntry& entry);
  // True exactly once after a pairing completes; copies the peer identity out
  // so the caller (pairing activity) can persist it to settings.
  bool consumePairSuccess(char* addrOut, size_t addrCap, uint8_t& typeOut, char* nameOut, size_t nameCap);
  void forgetPairing();

  // v85: true whenever the NimBLE stack is initialised. main.cpp's idle
  // power-saving path must not drop the CPU to 10 MHz while this is true —
  // the BLE controller needs the higher clock to meet its radio deadlines.
  // CrumBLE (BluetoothHIDManager.cpp:119-125, "controller init hangs at the
  // low-power frequency") and CrossPet (main.cpp:657-661, "BLE needs >=80MHz")
  // both hit this independently. Prime suspect for the v84 scan reboot: the
  // scan is exactly when the user stops pressing buttons for 3+ seconds.
  bool isStackActive() const { return stackReady_; }

  // v86: every advertisement the pairing scan received, before any list
  // bookkeeping. Zero here with a running scan means the radio heard nothing
  // at all — which is a different problem from "heard things, none matched".
  uint32_t advertsSeen() const { return advertsSeen_; }

  State state() const { return state_; }
  bool hasPairing() const;
  int lastError() const { return lastError_; }

  // Registered with InputManager via HalGPIO::setButtonHook.
  static uint8_t buttonHookThunk();

 private:
  friend class BleRemoteScanCallbacks;
  friend class BleRemoteClientCallbacks;

  BleRemoteManager() = default;
  bool initStack();
  void teardownStack(bool clearAll);
  // aggressive: 80% duty for 5s (boot/wake/pairing screen -- the user is pressing
  // the remote now). Default is the thin 2.3% idle scan. See the .cpp for the
  // arithmetic that made the thin-only version miss more than half of all presses.
  bool readerActive_ = false;  // v96: reader open -> scan continuously (see .cpp)
  void startReconnectScan(bool aggressive = false);

  // v95: advertisements the reconnect scan decoded since boot. Written on the
  // NimBLE host task, read on the main task for the diagnostic line only -- a
  // torn read is a wrong log number, never wrong behaviour, so no lock.
  volatile uint32_t reconnectAdvertsSeen_ = 0;

  // v98: zero-address occurrences, counted on the host task and printed by the main task.
  // Never log from a scan callback -- see BleRemoteManager.cpp for the three panics that
  // cost.
  volatile uint32_t zeroAddrSeen_ = 0;
  volatile uint32_t zeroAddrSubstituted_ = 0;

 public:
  uint32_t reconnectAdvertsSeen() const { return reconnectAdvertsSeen_; }
  uint32_t zeroAddrSeen() const { return zeroAddrSeen_; }
  uint32_t zeroAddrSubstituted() const { return zeroAddrSubstituted_; }

 private:
  void beginConnect(const char* addr, uint8_t addrType, bool pairing);
  // If client_ holds a live ACL link, disconnect and bounded-wait (<=1s) for
  // onDisconnect to land before returning. Shared by startScan()/pairWith()
  // (C1 fix, 2026-08): connect() rejects when called on an already-connected
  // client, and its synchronous failure path sets internal state to
  // DISCONNECTED regardless of the real ACL state -- reusing client_ for a new
  // connect() without confirming the real disconnect first double-connects
  // over a live link.
  void disconnectAndWaitIfConnected();
  bool finishGattSetup();
  void handleScanResult(const NimBLEAdvertisedDevice* dev);
  void handleNotify(NimBLERemoteCharacteristic* chr, const uint8_t* data, size_t len);
  static void onNotifyStatic(NimBLERemoteCharacteristic* chr, uint8_t* data, size_t len, bool isNotify);

  enum class ScanMode : uint8_t { None, Reconnect, Pairing };

  struct ReportSlot {
    NimBLERemoteCharacteristic* chr = nullptr;
    uint8_t reportId = 0;
    uint8_t prevButtons = 0;
  };

  State state_ = State::Off;
  bool stackReady_ = false;
  NimBLEClient* client_ = nullptr;
  ScanMode scanMode_ = ScanMode::None;
  int lastError_ = 0;

  // Host-task -> tick() flags (single writer per flag, read-and-clear in tick).
  volatile bool connEstablished_ = false;
  volatile bool connFailed_ = false;
  volatile bool disconnected_ = false;
  volatile bool connectRequested_ = false;
  volatile bool scanEnded_ = false;
  volatile int cbError_ = 0;

  // I3 fix (2026-08): secureConnection() runs async so a stalling peer's SMP
  // timeout (up to BLE_SM_TIMEOUT, 30s) can't stall the main loop -- this
  // firmware has no task watchdog to save it from that. onAuthenticationComplete
  // (host task) writes authOk_/authDone_; tick() (main task) reads-and-clears,
  // same single-writer/single-reader convention as the flags above. securing_
  // and authDeadlineMs_ are main-task-only (tick() sets and reads both).
  volatile bool authDone_ = false;
  volatile bool authOk_ = false;
  bool securing_ = false;
  uint32_t authDeadlineMs_ = 0;

  // I2 fix (2026-08): retry deadline for a reconnect scan that failed to start
  // (or ended unexpectedly -- NimBLEScan::isScanning() going false while still
  // WaitingForPeer covers both). Main-task-only.
  uint32_t reconnectRetryAtMs_ = 0;

  // M1 fix (2026-08): SETTINGS.bleRemotePeerAddr parsed once per reconnect-scan
  // start into ble_addr_t val[] byte order (see parseAddrString() in the .cpp),
  // so onResult() (host task, once per advertisement) can memcmp raw bytes
  // instead of allocating a std::string every call. The match itself is
  // address-only, mirroring the address-only strcasecmp this replaces;
  // peerAddrType_ is snapshotted alongside for parity but not consulted by the
  // match -- tick()'s connectRequested_ handling reads
  // SETTINGS.bleRemotePeerAddrType directly when it actually connects.
  uint8_t peerAddrBytes_[6] = {};
  uint8_t peerAddrType_ = 0;

  // Pairing-in-flight peer identity (written on the main task in pairWith).
  char pendingAddr_[18] = "";
  uint8_t pendingAddrType_ = 0;
  char pendingName_[24] = "";
  bool pairSuccessPending_ = false;

  HidReportMap reportMap_;
  ReportSlot reports_[kMaxReports];
  // volatile (M2): orders slot publication (finishGattSetup(), main task) vs.
  // the host-task lookup in handleNotify() -- see the invariant comment above
  // finishGattSetup()'s loop in the .cpp (M3).
  volatile uint8_t reportCount_ = 0;

  BleButtonLatch latch_;                    // guarded by bleMux (see .cpp)
  ScanEntry scanEntries_[kMaxScanEntries];  // guarded by bleMux
  uint8_t scanCount_ = 0;                   // guarded by bleMux
  uint32_t notifyCount_ = 0;
  volatile uint32_t advertsSeen_ = 0;  // v86: written on the host task, read on the main task
  State lastLoggedState_ = State::Off;
};

#define BLE_REMOTE BleRemoteManager::getInstance()

#endif  // CROSSMOSA_BLE
