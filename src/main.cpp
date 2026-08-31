#include "util/DiagLog.h"
#include <Arduino.h>
#include <BoardConfig.h>
#include <DataDir.h>
#include <Epub.h>
#include <Epub/ParsedText.h>
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

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "util/BenchFlags.h"
#include "util/BootRecovery.h"
#include "util/ButtonNavigator.h"
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
// CrossMosa（繁體中文自訂版）：
// notoserif 全家族已從 builtinFonts/all.h 移除，騰出的 flash 讓 UI 字型容納繁體漢字。
// 下面的 notoserif* 物件【名稱保留不動】（設定邏輯、enum、switch 全部照舊），只是資料
// 來源改指向對應的 notosans_*，因此設定裡的「Serif」會以黑體呈現。
// 斜體也一併移除：SD 中文字型只有正體與粗體，斜體本來就會 fallback。
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

// CrossMosa：物件名 ui12* 保留（UI_12_FONT_ID 與所有呼叫端不動），資料指向 14px。
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

// v36/v186: hangs off the boot-resolved data dir; built on first use — always
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

// v193：bytesReadOut 只給證人用，成功／失敗語意不變。
static bool loadSleepFrameBuffer(size_t* bytesReadOut = nullptr) {
  if (bytesReadOut) *bytesReadOut = 0;
  HalFile file;
  if (!Storage.openFileForRead("SLP", sleepFrameFile(), file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  if (bytesReadOut) *bytesReadOut = bytesRead;
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
  // v185 證人：分清「睡著醒來回主畫面」與「重置回主畫面」——兩者在 log 上原本都只有一行 BOOT。
  DiagLog::line("SLEEP timeout=%d mode=%d", static_cast<int>(fromTimeout), static_cast<int>(WiFi.getMode()));
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

// v196（複查）：開機證人**不可以**在這一段寫 SD。DiagLog::line() 每行都會開檔／寫入／flush／關檔，
// 而這幾個點全都排在 verifyPowerButtonWakeup() 與 recovery 按鍵取樣【之前】——慢卡上多幾次同步寫入
// 就會把電源鍵的驗證往後推，讓「按了醒不來」變得更嚴重，也可能讓 UP+POWER 的 SD 更新入口失效。
// 這裡只記時間戳（純 RAM，零 I/O），等過了那一段再一行印出來。
static uint32_t g_wakeT[6] = {0, 0, 0, 0, 0, 0};

void setup() {
  BoardConfig::holdPowerRails();

  // ⛔ 緊接在 holdPowerRails() 之後，這是逃生口。見 util/BootRecovery.h。
  // 這台沒有 USB 資料線，bootloader 也讀不到按鍵 —— 按住 Back+Up 退回上一版這件事，
  // 只能由真的開起來的這份韌體自己完成。2026-08-17 就是因為沒有它而永久失去一台機器
  // （v131 死在下面 setupDisplayAndFonts()，比這裡晚 100 行以上）。
  // 沒按組合鍵時它立即返回、不寫任何東西。
  //
  // ⚠️ 為什麼【不】放在 holdPowerRails() 之前：本檢查要輪詢 ADC ladder（約 96ms，
  //    固定約 96ms，不隨按鍵狀態變動），而 holdPowerRails() 是拉住電源閂鎖的
  //    （X4 profile 用 GPIO13 當 latch0，而雙機種 binary 開機時 ACTIVE 就是 X4）。
  //    在它之前插入延遲，最壞情況是放開電源鍵就斷電。
  //    不要為了「更早一點」把它移回去。
  //
  // ⚠️⚠️ 這裡【不是】零成本，原本的註解寫錯了。下面第 316 行的 verifyPowerButtonWakeup()
  //    失敗會直接 startDeepSleep()，而它問的是「此刻電源鍵還按著嗎」（HalGPIO.cpp:217-231）。
  //    在這裡多停留 N 毫秒，就是把那個檢查往後推 N 毫秒。checkBootCombo() 為此把延長
  //    正因如此，checkBootCombo() 的窗口【不可以加大】—— 動它之前先讀 BootRecovery.cpp。
  boot_recovery::checkBootCombo();

  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();

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

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? (gpio.displayIsUc8279() ? "X3 (UC8279)" : "X3 (UC8253)") : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  // v53/v57：診斷 log。**預設關閉**，靠 SD【根目錄】的空檔 `/diag.on` 開啟（放了要重開機）。
  // 判定只在這裡做一次，之後不再碰 SD；關閉時 mem()/line()/dumpPools() 全在第一行就 return，
  // 特別是【不做 heap_caps_walk】—— 那才是關掉之後省下的主要成本。
  //
  // ⚠️ 哨兵必須放【根目錄】，不可放資料目錄（/.crossmosa）—— 那裡已被 ProtectedPath 擋住，
  //    網頁與 WebDAV 都傳不進去，使用者只能拔卡。（v186 起兩個理由都活著：DataDir::resolve() 會把
  //    尚未遷移的卡上「手建的 /.crossmosa」當成失敗 rename 的殘骸處理；放根目錄就沒有這類互動。）
  //    ⚠️ 名字不可改成 `.diag.on`（會撞 8.3 別名假設）。
  //
  // 必須在 Storage.begin() 成功之後呼叫。
  // v36/v186：一次性 /.crosspoint → /.crossmosa 遷移。必須在【任何】store／設定載入或 SD 路徑
  // 組出來之前（它們全掛在 DataDir::path() 上），也在 DiagLog::begin() 之前（log 住在資料目錄）。
  // 原廠韌體就是 CrossPoint 的分支、也寫 /.crosspoint —— 從原廠直接刷過來的人，進度／書籤／WiFi
  // 就是靠這一步搬過來的。FAT 目錄改名是 metadata-only；epub 快取 hash 不含目錄名，零重排。
  DataDir::resolve();

  DiagLog::begin();
  DiagLog::mem("boot");
  BenchFlags::load();  // v185 bench 哨兵（同樣只在這裡讀一次 SD）

  // v196：BENCH→RESUME 黑盒補證人（純觀測，不改順序／行為）。
  HalSystem::checkPanic();
  g_wakeT[0] = millis();

  SETTINGS.loadFromFile();
  // v187：粗體閱讀是 ParsedText 的全域旗標，開機就跟設定對齊（否則從設定頁進文字設定的預覽會用錯字重）。
  ParsedText::setBoldBodyText(SETTINGS.boldBodyText != 0);
  g_wakeT[1] = millis();
  APP_STATE.loadFromFile();
  g_wakeT[2] = millis();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  g_wakeT[3] = millis();

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
  g_wakeT[4] = millis();

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
  g_wakeT[5] = millis();

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  // v196：喚醒回閱讀器時可跳過開機動畫（省一次完整面板刷新）。條件只用此刻已確定的欄位；
  // 刻意不含 Back 鍵——該鍵狀態此時不一定可靠，少判只會落到主畫面，仍會設定 activity。
  const bool willResumeToReader = !recoveryFirmwareMode && !HalSystem::isRebootFromPanic() &&
                                  !APP_STATE.openEpubPath.empty() && APP_STATE.lastSleepFromReader &&
                                  APP_STATE.readerActivityLoadCount == 0;
  // v185 證人：0=Splash 1=Silent 2=QuickResume；配上 BOOT 行的 rst= 就能分類每次開機。
  // 前綴刻意不用 BOOT —— 那是 diag.log 的版本分段記號，多一行就把每段切成兩半。
  DiagLog::line("RESUME kind=%d target=%u", static_cast<int>(resume), static_cast<unsigned>(snapshotTarget));

  // v193：醒來路徑證人。完全沒有新的 BOOT 行＝面板凍住、韌體還活著。
  const bool seamlessDisplay = resume != BootResume::Splash;
  DiagLog::mem("wake-disp");
  DiagLog::line("WAKE disp-begin seamless=%d", seamlessDisplay ? 1 : 0);
  setupDisplayAndFonts(seamlessDisplay);
  // v185 bench：驅動選好之後才套灰階推力候選；文字 AA 深灰旋鈕落在 renderer。
  if (BenchFlags::grayVariant != 0) display.setGrayscaleVariant(BenchFlags::grayVariant);
  renderer.setTextAaDarkOnly(BenchFlags::aaDark);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume: {
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      size_t frameBytes = 0;
      const bool frameOk = loadSleepFrameBuffer(&frameBytes);
      // v193：frame 讀完立刻記，ok=0 也要有行，否則卡住時分不出是沒檔還是讀到一半。
      DiagLog::line("WAKE frame ok=%d bytes=%u", frameOk ? 1 : 0, static_cast<unsigned>(frameBytes));
      if (frameOk) {
        const bool useDifferentialRefresh = gpio.deviceIsX3();
        if (useDifferentialRefresh) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline before replacing the moon with the loading icon.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }

        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        if (useDifferentialRefresh) {
          renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
          allowFastInitialReaderRefresh = true;
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }
        DiagLog::line("WAKE painted");  // v193：畫面已推上面板
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    }
    case BootResume::Splash:
      // v196：willResumeToReader 時跳過 goToBoot（開機 logo 的完整刷新）。
      // 安全性：switch 之後的路由是完整 if/else if/else，每一條都會 replaceActivity／goXxx：
      //   1) recoveryFirmwareMode → replaceActivity(SdFirmwareUpdate)
      //   2) isRebootFromPanic() → goToCrashReport()
      //   3) Silent + reader target + 有路徑 → goToReader()
      //   4) Silent（其餘）→ goHome()
      //   5) 無書／非閱讀器休眠／Back／loadCount>0 → goHome()
      //   6) else → goToReader()
      // willResumeToReader 為真時 (1)(2) 已排除；Splash 非 Silent 故 (3)(4) 不成立；
      // 落到 (5) 或 (6) 仍一定設定 activity（Back 少判只是進主畫面）。
      if (!willResumeToReader) activityManager.goToBoot();
      break;
  }

  // v196：證人——兩種情況都印，對照 willResumeToReader 與實際有無畫 logo。
  DiagLog::line("WAKE splash skipped=%d", (resume == BootResume::Splash && willResumeToReader) ? 1 : 0);
  // v196（複查）：開機前段的時間戳集中在這裡印 —— 此時電源鍵驗證與 recovery 判定都已經過去。
  DiagLog::line("WAKE steps panic=%lu settings=%lu appstate=%lu stores=%lu wakeup=%lu settle=%lu",
                static_cast<unsigned long>(g_wakeT[0]), static_cast<unsigned long>(g_wakeT[1]),
                static_cast<unsigned long>(g_wakeT[2]), static_cast<unsigned long>(g_wakeT[3]),
                static_cast<unsigned long>(g_wakeT[4]), static_cast<unsigned long>(g_wakeT[5]));

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    DiagLog::mem("wake-reader");  // v193：進閱讀器前的堆積；不印路徑本身（書名隱私）
    DiagLog::line("WAKE toreader path-len=%u", static_cast<unsigned>(APP_STATE.openEpubPath.size()));
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
    DiagLog::mem("wake-reader");  // v193：進閱讀器前的堆積；不印路徑本身（書名隱私）
    DiagLog::line("WAKE toreader path-len=%u", static_cast<unsigned>(path.size()));
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
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
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

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
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity() ||
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
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
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
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
