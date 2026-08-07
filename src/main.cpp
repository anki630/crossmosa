#include <Arduino.h>
#include <DataDir.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>
#include <esp_system.h>  // v85: esp_reset_reason() for the BOOT diag line

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "ble/BleRemoteManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "util/ButtonNavigator.h"
#include "util/DiagLog.h"
#include "util/SdDateTime.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Fonts
//
// NOTE (繁中自訂版 / Traditional Chinese custom build):
// notoserif 家族已從 builtinFonts/all.h 移除，騰出的 ~794 KB flash 用來讓 UI 字型
// (ubuntu_10/12) 容納 5,413 個繁體漢字，讓檔名與 OPDS 書名能正常顯示中文。
// 下面的 notoserif* 物件名稱保留不動（設定邏輯、enum、switch 全部照舊），只是資料
// 來源改指向對應的 notosans_*，因此設定中的「Serif」會以黑體呈現。
// 內建閱讀字型本來就不含任何漢字，中文書仍需使用 SD 卡字型。
EpdFont notoserif14RegularFont(&notosans_14_regular);
EpdFont notoserif14BoldFont(&notosans_14_bold);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notosans_12_regular);
EpdFont notoserif12BoldFont(&notosans_12_bold);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont);
EpdFont notoserif16RegularFont(&notosans_16_regular);
EpdFont notoserif16BoldFont(&notosans_16_bold);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont);
EpdFont notoserif18RegularFont(&notosans_18_regular);
EpdFont notoserif18BoldFont(&notosans_18_bold);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

// UI_12_FONT_ID 現在承載 14px 字面（清單主文字/標題放大用）。變數名保留 ui12 以免動
// insertFont 與所有呼叫點；資料指向 ubuntu_14。10px（ui10）維持給狀態列/副標等窄處。
EpdFont ui12RegularFont(&ubuntu_14_regular);
EpdFont ui12BoldFont(&ubuntu_14_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// v85: names for esp_reset_reason(), for the BOOT line in diag.log. Only PANIC
// and CPU_LOCKUP produce /crash_report.txt, so every other value here is a
// reboot cause this device previously had no way to report.
static const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";  // ESP.restart(), e.g. our own silentRestart()
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default: return "OTHER";
  }
}

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

// v36: hangs off the boot-resolved data dir; built on first use — always
// after DataDir::resolve() (first caller is enterDeepSleep / quick-resume
// boot, both well past Storage.begin()).
static const char* sleepFrameFile() {
  static char p[40] = "";
  if (!p[0]) snprintf(p, sizeof(p), "%s/sleep_frame.bin", DataDir::path());
  return p;
}

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", sleepFrameFile(), file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", sleepFrameFile(), file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(sleepFrameFile());
    return false;
  }
  Storage.remove(sleepFrameFile());
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  BLE_REMOTE.shutdownForSleep();

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif

  HalSystem::begin();

  // v85: reserve the framebuffer BEFORE anything else allocates. See
  // HalDisplay::reserveFrameBufferEarly() for the full reasoning — in short,
  // linking NimBLE shrinks the prio-0 pool by 28,256 B, and by the time
  // setupDisplayAndFonts() runs there is no longer a 52,272-byte hole there,
  // so the framebuffer spills into the one pool WiFi/OPDS/SMB depend on for
  // large contiguous blocks. Allocation only; no SPI or panel traffic yet, and
  // begin() below still works because the SDK guards its alloc on a null
  // pointer. Failure is not fatal here: begin() will simply allocate later,
  // exactly as it did before this line existed.
  if (!display.reserveFrameBufferEarly()) {
    LOG_ERR("MAIN", "Early framebuffer reservation failed; falling back to allocation in begin()");
  }

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    // SD 掛時 SETTINGS 讀不到 → 語言 fallback EN(實機顯示英文);重點是畫面可按鍵脫困
    activityManager.goToFullScreenMessage(tr(STR_SD_CARD_ERROR), EpdFontFamily::BOLD);
    return;
  }

  // One-shot /.crosspoint -> /.crossmosa migration. Must run before ANY
  // store/settings load or SD path is built (they all hang off
  // DataDir::path()).
  DataDir::resolve();

  // v77: register SdFat's date/time callback if this boot already knows the
  // date. ESP.restart() preserves the wall clock (the boot-time offset lives in
  // RTC scratch), and silentRestart() is a common path in this firmware, so a
  // synced session can survive into the next boot -- checking here rather than
  // only in loop() means files created during setup() get real dates too.
  //
  // It also warms up newlib's lazy time-lock creation off the storage mutex.
  SdDateTime::maybeRegister();

  // v57:儀器總開關(預設關;SD 資料目錄放 diag.on 才啟用)。必須在 DataDir::resolve() 之後,
  // 且在任何 DiagLog:: 呼叫之前——最早的呼叫點是下方的 mem("pre-route")。
  DiagLog::begin();

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      if (!gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP)) {
        powerManager.startDeepSleep(gpio);
      }
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossMosa version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  setupDisplayAndFonts(resume != BootResume::Splash);

  // I4 fix (2026-08): BLE page-turner remote, no-op unless a remote is paired
  // (settings). Moved here (was right after the wakeup-reason switch) so the
  // 52,272B framebuffer and font caches -- the largest single allocations in
  // the system -- land BEFORE NimBLE's own pools do; otherwise NimBLE's pools
  // sit ahead of them in the heap and shrink the largest contiguous block
  // available for the framebuffer (hard-limit-6). False wakes still never
  // reach this line: every wakeup-reason branch that deep-sleeps
  // (powerManager.startDeepSleep(gpio)) does so inside the switch above, well
  // before this point, and never returns. All three boot-presentation paths
  // (Splash / Silent / QuickResume, decided just above) still run through
  // this same line before they diverge in the switch below -- deep-sleep wake
  // is a full chip reset regardless of which path was taken.
  BLE_REMOTE.begin();

  // v53 量測:乾淨基準線——字型/顯示已就緒,但 activity 尚未路由(還沒載入 EPUB/section)。
  // 這個點在所有開機路徑上都可互相比較(下方的 boot-reader/boot-home 則不可)。
  DiagLog::mem("pre-route");

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon.
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      // v56:開機動畫已經畫完(吃掉 SDK 預備的兩次強制全同步的第一次)。取消第二次,
      // 改讓下一次 FAST 繪製走 scrub——省下約 2.3 秒,又不會留下熊 logo 的殘影。
      // 放在 goToBoot() 之後:這條路徑上 currentActivity 必為 nullptr(唯一更早的
      // goToFullScreenMessage 後面直接 return),所以 replaceActivity 會【立即】執行
      // onEnter,回來時動畫已上畫面。即使哪天這個前提改變也不會出事:skip 與 scrub 是
      // 同一次呼叫內原子完成的,下一次繪製一定是全像素驅動,不會出現無基準的差分。
      display.skipInitialResyncAndScrubNext();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;

  // v53 量測(暫時)。可證偽的預測——逐池 total 相加應約 274,000-277,000(int_total 同源);
  // 若印出約 200,000,2026-07-26 研究報告的整個記憶體模型作廢,記憶體類提案全部撤回。
  // 註:本取樣點在 activity 路由【之後】,走續讀路徑時 EPUB/section/SD 字型已載入,
  // 與 Home 路徑不可直接相比——故一併記下實際落點(下方 tag)。
  // v85: the reset reason is the only way this device can tell a panic from a
  // brownout from a watchdog. /crash_report.txt only covers ESP_RST_PANIC and
  // ESP_RST_CPU_LOCKUP (HalSystem.cpp:143-146), so "reboot with no crash report"
  // has always been ambiguous here — the v84 BLE scan reboot is exactly that
  // case. One line, no cost, permanently useful on a device with no serial port.
  DiagLog::line("BOOT version=%s resume=%d reader=%d reset=%d(%s)", CROSSPOINT_VERSION, static_cast<int>(resume),
                activityManager.isReaderActivity() ? 1 : 0, static_cast<int>(esp_reset_reason()),
                resetReasonName(esp_reset_reason()));
  DiagLog::mem(activityManager.isReaderActivity() ? "boot-reader" : "boot-home");
}

void loop() {
  // v77: SNTP lands ASYNCHRONOUSLY, seconds after the connect path has already
  // returned, so a one-shot check at connect time would miss it. One bool test
  // once it has succeeded; at most one time() per second before that. Here
  // rather than in the SMB activity so OPDS and Calibre downloads benefit too.
  SdDateTime::maybeRegister();

  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());
  BLE_REMOTE.tick();

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS && !BLE_REMOTE.isStackActive()) {
      // If we've been inactive for a while, increase the delay to save power.
      // v85: never while the BLE stack is up — LOW_POWER_FREQ is 10 MHz and the
      // BLE controller cannot meet its radio deadlines there. This is the prime
      // suspect for the v84 "reboots while scanning" report: scanning is exactly
      // when the user stops pressing buttons, so the 3 s idle timer fires with
      // the radio running. Nothing else in this firmware ever ran a real-time
      // subsystem under the idle clock (the web server forces full speed via
      // skipLoopDelay above), which is why this never bit us before.
      // Cost, stated plainly: BLE on = no CPU power saving = shorter battery.
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // v85: the guard above only stops us LOWERING the clock. If power saving
      // was already engaged when the BLE stack came up, it would simply stay
      // engaged — so raise it back explicitly. setPowerSaving() short-circuits
      // when the state already matches (HalPowerManager.cpp:42-53), and it is
      // the same call the WiFi/webserver path makes every loop, so this costs
      // nothing when BLE is off.
      if (BLE_REMOTE.isStackActive()) {
        powerManager.setPowerSaving(false);
      }
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
