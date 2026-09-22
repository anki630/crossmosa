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
// v295：esp_rtc_get_time_us() —— RTC 計數器自晶片上電起算，用來量 millis() 開始【之前】
// 那一段（ROM ＋ 二階段 bootloader ＋ 映像驗證）。用 SDK 正式表頭，不手寫 extern "C"。
#include <esp_rtc_time.h>
// v296：淺睡眠 —— esp_light_sleep_start / 喚醒來源設定 / p2 的 52KB 畫面暫存 / 按鍵喚醒腳
#include <driver/gpio.h>
#include <driver/rtc_io.h>     // v324：電源鍵腳位（RTC 腳位）的重設
#include <soc/gpio_periph.h>   // v324：GPIO_PIN_MUX_REG[]
#include <soc/gpio_struct.h>   // v324：GPIO.pin[n]／func_out_sel_cfg[n]（per-pin 暫存器，不手算位址）
#include <soc/gpio_reg.h>
#include <soc/io_mux_reg.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>
#include <soc/soc_caps.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
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
#include "activities/boot_sleep/SleepActivity.h"  // v296：淺睡眠時直接畫桌布，不取代 activity
#include "activities/reader/ReaderUtils.h"  // v322：休眠提示提早到存 wake frame 之前，要套閱讀方向
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BenchFlags.h"
#include "util/BootRecovery.h"
#include "util/NvsStore.h"

#include <esp_timer.h>
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

// How the device is coming back to life, resolved once at boot. The silent
// flow suppresses the splash and leaves the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
// v330：上游的 QuickResume（第三種：SD 旗標 showBootScreen＋sleep_frame.bin，「待機畫面就是書頁」）整套移除 ——
//   淺睡眠（書頁留在 RAM）與 wake frame（冷開機回書）已涵蓋它的用途，而它是一個要使用者「選對設定」
//   才有的行為（設計原則 36）。
enum class BootResume : uint8_t {
  Splash,  // cold boot, flash, panic, or plain reboot
  Silent,  // heap-defrag ESP.restart() (RTC flag; lost on power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// v304：閒置計時器。原本是 loop() 裡的 static，**淺睡眠續讀時必須重置它**。
// ⛔ 實機 bug（diag302）：`millis()` 在淺睡眠期間照常前進，所以只要睡得比「自動休眠」
//    設定值久，一醒來閒置計時器就已經到期 —— log 上是
//    `LSLEEP resume slept=1502760`（睡了 25 分鐘）緊接著 117ms 後的 `SLEEP timeout=1`。
//    使用者的體感就是「按一下醒來，它又自己睡回去」，而且看到兩次休眠畫面。
//    （維護者自己就推出了這個因果：「淺睡眠超過自動睡眠的設定，我設定 9 分鐘」。）
static unsigned long g_lastActivityTime = 0;

// ─── v310：電源鍵的【唯一真相】 ─────────────────────────────────────────────
// 委員會鑑識（grok 讀 InputManager，codex 做結構審查，兩者收斂）：
//   InputManager 把乾淨的數位電源腳（GPIO3）和會毛的 ADC ladder 綁進【同一個 state word】、
//   同一個 5ms 整字去彈跳；X3 的 getState() 又是先做兩次 analogRead 才 digitalRead 電源腳，
//   SAR ADC 切換後緊接讀相鄰腳位會被汙染。主迴圈一圈 10–50ms > 5ms 去彈跳窗 →
//   **連續兩筆相同就提交**：漏讀兩筆 POWER=0 就提交「放開」，下一筆又提交「按下」→
//   新的 press edge → held 歸零。長按被拆成無限個 1–7ms 的短按。
//   實機證人：SLPGATE blocked held=1..7 own=0 raw=1 —— own=0 正是因為 v309 的自製計時器
//   踩在同一個會翻的 isPressed 上。raw 全是 1 是選擇偏差（只在剛提交按下的那一幀拍照）。
//   首頁特別嚴重：閒置 delay(50)＋CPU 10MHz → 50ms 一筆 → 每一對相同樣本都提交。
//   「續讀後 2 秒又睡」也是同一根因：waitForPowerRelease() 的第一個 update() 只重設視窗、
//   不提交，isPressed 仍 false → while 一次都不進，立刻返回。
//
//   → 這是 v291／v294／v309 同一個病的第四次：**用不屬於這顆鍵的時鐘去判斷它按了多久**。
//     修法：電源鍵只看原始電平（powerDownRaw() 已改成純 digitalRead），自己做去彈跳，
//     所有「按住多久」的判斷都問這裡。isPressed(BTN_POWER) 的翻轉從此與休眠路徑無關。
//   ⚠️ 只替換休眠相關的判定（loop 關卡、waitForPowerRelease）；短按動作與截圖組合鍵
//     仍走 isPressed —— 一次只動一個變數，那些若也受影響再談。
struct PowerKeyTracker {
  static constexpr unsigned long STABLE_MS = 30;  // 原始電平要穩定多久才承認改變（> 一圈 10ms）
  bool stable = false;            // 去彈跳後的「按著」
  bool pending = false;           // 有一個與 stable 不同的候選電平正在等待穩定
  bool pendingLevel = false;
  unsigned long pendingSince = 0;
  unsigned long downSince = 0;    // stable 變成按著的時刻（用候選開始的時刻，含去彈跳窗）

  // 每圈呼叫一次（loop 開頭 gpio.update() 之後）。只做一次 digitalRead，不碰 ADC。
  void poll(const unsigned long now) {
    const bool raw = gpio.powerDownRaw();
    if (raw == stable) {
      pending = false;
      return;
    }
    if (!pending || raw != pendingLevel) {
      pending = true;
      pendingLevel = raw;
      pendingSince = now;
      return;
    }
    if (now - pendingSince >= STABLE_MS) {
      stable = raw;
      pending = false;
      downSince = raw ? pendingSince : 0;
    }
  }
  unsigned long heldMs(const unsigned long now) const { return stable ? (now - downSince) : 0; }
};
static PowerKeyTracker g_powerKey;

// ⭐ v324：電源鍵腳位（X3＝GPIO3，是 ESP32-C3 的 RTC 腳位 0–5 之一）的暫存器快照與重設。
//   實機兩次「整段開機都讀不到電源鍵按下」（v317 diag-prev319：542 秒零 SLPGATE；v323 diag323-3：417 秒零 PWRKEY，
//   其他鍵都正常）都緊接在 WEB exit → silentRestart()（rst=3）之後，而 v317 那次是斷電式重開（rst=8）後才恢復。
//   RTC 域的 pad hold／喚醒設定／RTC mux 會撐過軟體重啟，只有斷電才清 —— 形狀吻合，但還沒有證據。
//   這裡做兩件事：① 開機時把腳位狀態記下來（DiagLog 起來之後印）；② 無條件把它重設成乾淨的數位輸入再記一次。
//   ② 對正常狀態是 no-op；若真是殘留設定，這一步就是修法，而 ① 會說出殘留的是哪一個位元。
struct PwrPadSnap {
  uint32_t iomux = 0, enable = 0, out = 0, outSel = 0, pin = 0, in = 0, padHold = 0, digHold = 0, gpioWake = 0, extWake = 0;
};
static int g_pwrPadPin = -1;
static PwrPadSnap g_pwrPadPre, g_pwrPadPost;
static constexpr int PWRPAD_NA = 9999;  // 這顆晶片沒有該 API（印成 na，不要印成 0＝成功）
static int g_pwrPadErr[8] = {PWRPAD_NA, PWRPAD_NA, PWRPAD_NA, PWRPAD_NA, PWRPAD_NA, PWRPAD_NA, PWRPAD_NA, PWRPAD_NA};
static PwrPadSnap snapPwrPad(const int pinNo) {
  PwrPadSnap s;
  if (pinNo < 0 || pinNo >= GPIO_NUM_MAX) return s;
  s.iomux = REG_READ(GPIO_PIN_MUX_REG[pinNo]);        // IO_MUX：FUN_IE(9) FUN_WPU(8) FUN_WPD(7) MCU_SEL(12–14) SLP_SEL(1)
  s.enable = (GPIO.enable.val >> pinNo) & 1u;         // GPIO 輸出致能（1 ＝ 腳位在驅動）
  s.out = (GPIO.out.val >> pinNo) & 1u;               // 輸出 latch
  s.outSel = GPIO.func_out_sel_cfg[pinNo].val;        // 輸出矩陣：OUT_SEL(0–7) INV(8) OEN_SEL(10) OEN_INV(11)——codex：周邊可繞過 enable 驅動 pad
  s.pin = GPIO.pin[pinNo].val;                        // GPIO_PINn：pad_driver(2) int_type(7–9) wakeup_en(10)
  s.in = (GPIO.in.val >> pinNo) & 1u;                 // 數位輸入電平（低有效：0 ＝ 按著）
  s.padHold = (REG_READ(RTC_CNTL_PAD_HOLD_REG) >> pinNo) & 1u;  // RTC 腳位（0–5）的 hold 位元
  s.digHold = REG_READ(RTC_CNTL_DIG_PAD_HOLD_REG);
  s.gpioWake = REG_READ(RTC_CNTL_GPIO_WAKEUP_REG);    // 深睡眠 GPIO 喚醒設定／狀態
  s.extWake = REG_READ(RTC_CNTL_EXT_WAKEUP_CONF_REG);
  return s;
}
static void logPwrPad(const char* tag, const int pinNo, const PwrPadSnap& s) {
  DiagLog::line("PWRPAD %s pin=%d iomux=%04lx oe=%lu out=%lu outsel=%04lx cfg=%08lx in=%lu hold=%lu dhold=%08lx gwake=%08lx ext=%08lx",
                tag, pinNo, static_cast<unsigned long>(s.iomux), static_cast<unsigned long>(s.enable),
                static_cast<unsigned long>(s.out), static_cast<unsigned long>(s.outSel), static_cast<unsigned long>(s.pin),
                static_cast<unsigned long>(s.in), static_cast<unsigned long>(s.padHold), static_cast<unsigned long>(s.digHold),
                static_cast<unsigned long>(s.gpioWake), static_cast<unsigned long>(s.extWake));
}
// 無條件把電源鍵腳位重設成乾淨的數位輸入（上拉／下拉依 profile）。每次開機做；正常狀態下每一步都是 no-op。
static void resetPwrPad(const int pinNo) {
  if (pinNo < 0 || pinNo >= GPIO_NUM_MAX) return;
  const auto g = static_cast<gpio_num_t>(pinNo);
  // ESP32-C3：SOC_RTCIO_PIN_COUNT=0 —— 沒有 RTC IO mux 可以 deinit；GPIO0–5 的「RTC 能力」只有 hold 與深睡眠喚醒，
  //   兩者都走數位 GPIO 驅動的 API（gpio_hold_dis／gpio_deep_sleep_wakeup_disable）。
  g_pwrPadErr[0] = gpio_hold_dis(g);
#if SOC_GPIO_SUPPORT_SLP_SWITCH
  g_pwrPadErr[1] = gpio_sleep_sel_dis(g);  // codex：SLP_SEL 不能靠 reset_pin 的副作用，明講
#endif
  g_pwrPadErr[2] = gpio_deep_sleep_wakeup_disable(g);
  g_pwrPadErr[3] = gpio_wakeup_disable(g);
#if SOC_RTCIO_PIN_COUNT > 0
  g_pwrPadErr[4] = rtc_gpio_is_valid_gpio(g) ? rtc_gpio_deinit(g) : ESP_OK;  // pad 從 RTC 功能交回數位 IO_MUX（C3 沒有）
#endif
  g_pwrPadErr[5] = gpio_reset_pin(g);  // IO_MUX 回 GPIO 功能、斷開矩陣訊號、清 hold
  // ⭐ v325（diag324 定案）：黑洞那幾次開機 `boot-pre cfg=0x2200`、正常 `0x0000` —— GPIO_PIN3 的 INT_TYPE=4（低電位觸發）
  //   ＋ INT_ENA（CPU 中斷致能）留著；其他欄位全同。來源是淺睡眠的 gpio_wakeup_enable(LOW_LEVEL)：disarm 只清
  //   wakeup 位元，中斷型別與致能留下，撐過 WEB exit 的軟體重啟。而 v324 的 gpio_reset_pin 也沒清它（boot-post 仍 0x2200）。
  //   這裡明講清掉；lightSleepCycle 的 disarm() 也同樣清（從源頭不留）。
  g_pwrPadErr[6] = gpio_set_intr_type(g, GPIO_INTR_DISABLE);
  g_pwrPadErr[7] = gpio_intr_disable(g);
  // ⚠️ 喚醒設定被清掉沒關係：淺睡眠每次入睡前 gpio_wakeup_enable()（lightSleepCycle），真關機每次
  //   armPowerButtonWakeup()（SDK deepSleepUntilPowerButton）—— 兩條路都是每次重掛，不依賴這裡的狀態（codex 第一輪點名，讀碼確認）。
  pinMode(pinNo, BoardConfig::ACTIVE.input.powerActiveHigh ? INPUT_PULLDOWN : INPUT_PULLUP);  // 與 InputManager::begin 同
}

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  logPwrPad("restart", g_pwrPadPin, snapPwrPad(g_pwrPadPin));  // v324：重啟前的電源鍵腳位狀態（WEB exit 走這裡；magic 已設好，best-effort）
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
  // v310：看【原始電平】等它穩定放開，不再問會翻轉的 isPressed（見 PowerKeyTracker 的說明）。
  //   舊版在淺睡眠續讀後會立刻返回：第一個 update() 看到 POWER 但 state != lastState →
  //   只重設去彈跳視窗、不提交 → isPressed 仍 false → while 一次都不進 → 2 秒後同一根手指
  //   被 loop() 當成新的長按 → 「睡眠 2 次」。SDK 自己的 waitForPowerButtonRelease() 也是
  //   直接 digitalRead 迴圈。
  unsigned long releasedSince = 0;
  for (;;) {
    gpio.update();
    const unsigned long now = millis();
    g_powerKey.poll(now);
    if (!gpio.powerDownRaw()) {
      if (releasedSince == 0) releasedSince = now;
      if (now - releasedSince >= 50) break;  // 穩定放開 50ms
    } else {
      releasedSince = 0;
    }
    delay(10);
  }
  // 把「放開」提交進 InputManager 的 currentState 而不產生 edge（同 setup() 尾段的作法：
  // 兩次 update() 間隔 > 5ms 去彈跳窗），loop() 的第一次 update() 才不會看到新的 press。
  gpio.update();
  delay(10);
  gpio.update();
}

// v293：**喚醒時先把睡前那一頁打出來**，讓使用者在約 1.6 秒看到書，而不是盯著桌布 2.5 秒。
//
// ⭐ 關鍵是「那張存下來的畫面【就是】最終畫面」—— 不是過場，所以不必多付一次刷新，
//    只是把同一次刷新提早；之後字型、開書、排版都在「已經正確的畫面」後面跑。
// ⭐ 也因此**不需要記方向**：framebuffer 存的是實體像素，橫直排早就烤進去了（教訓 15）。
//    （維護者指出的，我原本多慮。）
//
// ℹ️ 上游 QuickResume 的 `sleep_frame.bin` 是另一個檔（那條路是「待機畫面就是書頁」，這條是「待機畫面仍然是桌布，
//    只是醒來先還原書頁」；v330 起 QuickResume 整條路已移除，舊裝置上殘留的那個檔沒有讀者，歸 A5 的孤兒清理）。
//
// 穩健性（這條在開機路徑上，壞掉不能比現在差）：
//   ① 檔頭帶 magic／版本／緩衝大小／**休眠代號**／**內容校驗和** —— 任何一項對不上就放棄還原。
//   ② **一次性**：讀完就刪；而且**每次開機只要沒有用到它就刪掉**，
//      這樣「檔案存在」永遠等於「上一次休眠剛寫的」，不會還原到好幾次開機前的畫面。
//   ③ 直接讀進 framebuffer，不配置第二塊 52KB（這台機器只有一張）。
//   ④ 寫入失敗、短寫、檔案被截斷 —— 全部由 ① 的大小檢查吃掉，安靜地退回原本行為。
static const char* wakeFrameFile() {
  static char p[48] = "";
  if (!p[0]) snprintf(p, sizeof(p), "%s/wake_frame.bin", DataDir::path());
  return p;
}
// 「正在讀」的名字。⭐ **一次性資格在讀 payload【之前】就先用掉**（見 loadWakeFrameBuffer）。
static const char* wakeFrameUsingFile() {
  static char p[48] = "";
  if (!p[0]) snprintf(p, sizeof(p), "%s/wake_frame.using", DataDir::path());
  return p;
}
static const char* wakeFrameTmpFile() {
  static char p[48] = "";
  if (!p[0]) snprintf(p, sizeof(p), "%s/wake_frame.tmp", DataDir::path());
  return p;
}

struct WakeFrameHeader {
  char magic[4];      // 'C''M''W''F'
  uint8_t version;
  uint8_t pad[3];
  uint32_t token;     // 必須等於 APP_STATE.wakeFrameToken
  uint32_t bufSize;
  uint32_t payloadHash;  // fnv1a-1a over the framebuffer bytes
};
static constexpr uint8_t WAKE_FRAME_VERSION = 2;

static uint32_t wakeFrameHash(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;  // FNV-1a，與 WarmIdentity 同一套，不另外引進 CRC
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

// 休眠端：原子寫入（tmp → 檢查每一次寫入的位元組數 → rename）。
// ⚠️ 複查指出：忽略 write() 的回傳值時，「新檔頭 ＋ 只寫一半的 payload ＋ 舊檔剩下的尾巴」
//    會湊出讀得滿、檢查也過的**拼接畫面**。所以寫入端必須自己保證完整，不能靠讀取端的長度檢查。
static bool saveWakeFrameBuffer() {
  Storage.remove(wakeFrameFile());  // 先讓舊的失效：寧可沒有畫面，也不要舊畫面
  Storage.remove(wakeFrameTmpFile());
  const size_t bufSize = renderer.getBufferSize();
  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb || bufSize == 0) return false;

  WakeFrameHeader h{};
  h.magic[0] = 'C'; h.magic[1] = 'M'; h.magic[2] = 'W'; h.magic[3] = 'F';
  h.version = WAKE_FRAME_VERSION;
  h.token = APP_STATE.wakeFrameToken;
  h.bufSize = static_cast<uint32_t>(bufSize);
  h.payloadHash = wakeFrameHash(fb, bufSize);

  bool ok = false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("WFR", wakeFrameTmpFile(), file)) return false;
    const size_t w1 = file.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h));
    const size_t w2 = file.write(fb, bufSize);
    file.flush();
    ok = (w1 == sizeof(h) && w2 == bufSize);
  }  // 先關檔：SdFat 不可 rename 仍開啟的路徑
  if (!ok || !Storage.rename(wakeFrameTmpFile(), wakeFrameFile())) {
    Storage.remove(wakeFrameTmpFile());
    DiagLog::line("WAKEFRAME save-failed");
    return false;
  }
  return true;
}
// v326（帳本 A9）：帶計時證人的存檔。實機推估一次 0.6–1.3s（SD 寫入浮動大），以前沒有直接證人。
static bool saveWakeFrameTimed(const char* why) {
  const uint32_t t0 = millis();
  const bool ok = saveWakeFrameBuffer();
  DiagLog::line("WAKEFRAME save why=%s ok=%d ms=%lu", why, ok ? 1 : 0, static_cast<unsigned long>(millis() - t0));
  return ok;
}

// 喚醒端。回 true 表示 framebuffer 已填好且校驗通過。
//
// ⚠️⚠️ **一次性資格在讀 payload 之前就先用掉**（rename 成 .using）。
//   複查指出的最高風險：若「讀完才刪」而讀取途中當掉／看門狗重置，下次開機會走進
//   **完全相同的路徑** → 永久開機迴圈。這台機器沒有 USB 救磚，而且已經因為開機迴圈
//   報廢過一台，所以這條不能有例外。
//   rename 成功之後即使當場斷電，下次開機看到的是 `.using`（下面開頭就清掉），
//   而 `wake_frame.bin` 已經不存在 → **不會再試第二次**。
static bool loadWakeFrameBuffer(size_t* bytesReadOut = nullptr) {
  if (bytesReadOut) *bytesReadOut = 0;
  // 上一次沒讀完就掛掉的殘骸：清掉，而且這一輪不再嘗試。
  if (Storage.exists(wakeFrameUsingFile())) {
    // ⚠️ **順序不可對調**（codex 第二輪）：`.using` 是「這份內容已經試過了」的墓碑，
    //    要**最後**才刪。先刪墓碑再斷電的話，留下來的 `.bin` 下次又會重新取得資格。
    Storage.remove(wakeFrameFile());
    Storage.remove(wakeFrameUsingFile());
    DiagLog::line("WAKEFRAME abandoned-stale");
    return false;
  }
  if (!Storage.exists(wakeFrameFile())) return false;
  if (APP_STATE.wakeFrameToken == 0) {  // 沒有有效的休眠代號 → 不可能配對成功
    Storage.remove(wakeFrameFile());
    return false;
  }
  if (!Storage.rename(wakeFrameFile(), wakeFrameUsingFile())) {  // ← 資格在這裡用掉
    Storage.remove(wakeFrameFile());
    return false;
  }

  bool ok = false;
  {
    HalFile file;
    if (Storage.openFileForRead("WFR", wakeFrameUsingFile(), file)) {
      WakeFrameHeader h{};
      const size_t bufferSize = display.getBufferSize();
      if (file.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) == sizeof(h) && h.magic[0] == 'C' &&
          h.magic[1] == 'M' && h.magic[2] == 'W' && h.magic[3] == 'F' && h.version == WAKE_FRAME_VERSION &&
          h.bufSize == bufferSize && h.token == APP_STATE.wakeFrameToken) {
        const size_t got = file.read(display.getFrameBuffer(), bufferSize);
        if (bytesReadOut) *bytesReadOut = got;
        // ⚠️ 讀滿還不夠 —— 位元損壞的 52KB 照樣讀得滿。校驗和才擋得住。
        ok = (got == bufferSize) && (wakeFrameHash(display.getFrameBuffer(), bufferSize) == h.payloadHash);
      }
    }
  }
  Storage.remove(wakeFrameUsingFile());
  if (!ok) {
    // ⚠️ 讀到一半／校驗失敗時，framebuffer 已經被覆寫過。我們不會把它推上面板（下面的
    //    呼叫端只在 true 時 displayBuffer），而閱讀器之後也會整頁重畫 —— 但「後面一定會
    //    重畫」是一個控制流前提，不是保證。清成白的，把這一整類問題拿掉（codex 第二輪）。
    memset(display.getFrameBuffer(), 0xFF, display.getBufferSize());
  }
  return ok;
}

// Enter deep sleep mode
// ─── v296：淺睡眠（light sleep）─────────────────────────────────────────────
//
// 維護者 2026-09-19 定的情境：按電源鍵 → 看起來就是睡著了（桌布照常出現），但機器其實
// 還活著、閱讀器完整留在 RAM；30 分鐘內再按一下 → **秒開回到書上**；超過 30 分鐘沒人按
// → 在背景默默真關機（螢幕本來就是桌布，所以畫面完全沒變化）。
//
// ⭐ 為什麼這次「桌布」與「秒開」不再互斥：`goToSleep()` 會 replaceActivity 把閱讀器銷毀，
//    而桌布是 SleepActivity 畫的 —— 我一度以為兩者綁在一起。查證後 `SleepActivity` 的工作
//    **全在 onEnter() 裡做完、對「自己是不是 currentActivity」零依賴**（基底 onEnter 只有
//    一行 LOG_DBG），所以可以在堆疊上建一個、直接畫桌布，**不動 activity、不銷毀閱讀器**。
//
// 決策（維護者拍板）：
//   A. 書頁那 52KB 放 RAM（p2 實測有 115,616 連續可用）→ 貼回來是 memcpy，約 1ms
//   B. 30 分鐘先寫死，觀察過再決定要不要做成設定
//   C. **只有手動按電源鍵走 light sleep**；閒置自動休眠維持真關機 —— 風險小一半，
//      因為「放著不管」才是最常見、對電池影響最大的情境
//
// ⭐ v330（A4，設計原則 36）：**預設開、沒有設定項**。v296–v329 靠 SD 根目錄的 `/lightsleep.on` 哨兵開啟，
//    是為了在待機電流量出來之前把曝險關在自己這台；數字有了（5mA、30 分鐘上限、低電量政策）之後，
//    使用者不需要知道「淺睡眠」這個詞。除錯用的是【反向】哨兵 `/lightsleep.off`（放 SD 根目錄 → 退回
//    v295 的真關機），不寫進任何對外文件。
//
// ⚠️⚠️ 記憶體：桌布解碼要 RAM，而這次閱讀器【還活著】（以前是先銷毀才畫）。
//    這台 OOM 不會優雅失敗，是直接 abort() 重開機（硬限制第 2 條）→ 所以畫桌布前先
//    卸載 SD 內文字型（WiFi 那條路已經在用同一個機制），騰 30–90KB。
//    ⭐ 不影響秒開：貼回書頁靠的是存下來的畫面，**不需要字型**；字型等畫面出來之後再載回。
//
// ⚠️ v1 刻意【不】讓面板進深睡：那會多出「叫醒面板才能畫」的未知數。e-ink 不通電也留著畫面，
//    但控制器 idle 的耗電因此算在待機裡 —— 如果實測漏電偏高，這是第一個嫌犯。
static constexpr const char* LIGHT_SLEEP_SENTINEL = "/lightsleep.off";  // v330：反向哨兵 —— 存在＝關
// v328（設計原則 36）：電量低於這個值且沒接 USB 就不淺睡（5mA 在快沒電時不該花），直接真關機。使用者不必知道。
static constexpr unsigned LIGHT_SLEEP_MIN_SOC = 10;
static constexpr uint64_t LIGHT_SLEEP_WINDOW_US = 30ULL * 60ULL * 1000000ULL;  // 30 分鐘

static bool lightSleepEnabled() {
  static int8_t cached = -1;  // 只問 SD 一次
  if (cached < 0) cached = Storage.exists(LIGHT_SLEEP_SENTINEL) ? 0 : 1;  // v330：哨兵存在＝關閉
  return cached == 1;
}

// 回傳 true ＝ 被電源鍵叫醒、畫面已貼回、閱讀器完好 → 呼叫端應直接 return。
// 回傳 false ＝ 沒睡成 或 計時器到了 → 呼叫端照原路真關機（螢幕已經是桌布）。
// v326（A9）：wakeFrameDeferred ＝ enterDeepSleep() 把 wake frame 的存檔延後到「真的要關機」的出口（本函式回 false 的每一條路）。
// v326：sleepLock 由 enterDeepSleep() 持有並傳入 —— 從提示、快照、wake frame 到這裡結束都是同一把鎖，
//   中間沒有空窗（codex：兩把鎖之間 render task 可能換頁，lowmem 那條提前寫檔更是沒鎖）。
static bool lightSleepCycle(bool wakeFrameDeferred, RenderLock& sleepLock) {  // v330：fromTimeout 參數拿掉（唯一讀者是 QuickResume 的桌布分支）
  const bool fromReader = APP_STATE.lastSleepFromReader;  // v327：全域淺睡眠 —— 字型與 em 探針只有閱讀器需要
  // ⭐ v306：記憶體見底時【不要】嘗試淺睡眠。畫桌布要解碼一張圖，而這台 OOM 不會優雅失敗，
  //    是直接 abort 重開機（硬限制第 2 條）。實機看過 WiFi 用完之後 `p3 f=24 max=12`、
  //    `p1 f=0` 的狀態 —— 那時候去做一件需要記憶體的事就是在賭。
  //    ⭐ v317：門檻 40KB → 20KB，並開始【量】畫桌布真正用掉多少。
  //      40KB 是 v306 拍的「留足餘裕」，實機擋掉約一半的嘗試（v300–v312 約 12/25，v312 那輪 5/5），
  //      被擋的值全落在 28.6–40.9KB —— 那是閱讀中 p2 被章節快取切剩的正常狀態，不是見底。
  //      讀碼盤點畫桌布的配置（SleepActivity.cpp／GfxRenderer::drawBitmap／Bitmap.cpp）：
  //        · BMP 兩個列緩衝 132＋528 B（malloc，失敗會優雅返回）· 抖色器 ~3KB（new nothrow）
  //        · ⚠️ 桌布清單 `std::vector<std::string> files`（沒有 reserve）：每張一個 24B 槽＋檔名字串，
  //          成長時新舊並存 —— 512 張以內瞬間最大連續需求約 18KB，這是唯一會 abort 的配置（-fno-exceptions）。
  //      20KB 蓋得住那個最壞情況，又低於所有被擋過的值。真正的需求由下面的 `LSLEEP mem` 證人量出來
  //      （minfree 是開機以來的歷史低點：畫桌布若創新低，差值就是這次的峰值用量）。
  const size_t lsLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t lsFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t lsMinFree = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (lsLargest < 20 * 1024) {
    DiagLog::line("LSLEEP skip why=lowmem largest=%u free=%u", static_cast<unsigned>(lsLargest),
                  static_cast<unsigned>(lsFree));
    if (wakeFrameDeferred) {
      saveWakeFrameTimed("lowmem");  // v326：framebuffer 還是書頁（鎖在手上），先寫再走真關機
      wakeFrameDeferred = false;     // 已處理
    }
    return false;
  }
  // ⭐⭐ v301：**完全不再快照畫面**。
  //    v296–v300 一路試「把 framebuffer 存起來、醒來貼回去」，實機接連給出三種壞畫面：
  //      v299 `inked=0/817` → 整張全白
  //      v300 使用者回報   → 「全部拉黑，在變成灰階之前的圖片」
  //    ⚠️ 後者點破了根因：抗鋸齒是**兩階段**的 ——
  //      第一階段 BW 基底幀留在 framebuffer；第二階段的灰階平面走
  //      copyGrayscaleLsb→DTM1 / copyGrayscaleMsb→DTM2，**住在面板控制器的 RAM 裡**。
  //    → **最終畫面從來就不在我們的 framebuffer**，所以「快照 framebuffer」對任何
  //      灰階／抗鋸齒內容都是結構上不可能正確的。白頁與黑圖是同一個根因的兩種面貌。
  //
  //    改成：醒來直接請閱讀器重畫。它完整留在 RAM，所以冷開機的大頭全部免付 ——
  //      bootloader 800ms、SD 初始化 750ms、字型載入 240ms、開書與排版，一樣都不用。
  //      剩下的只有繪製約 70ms ＋ 面板刷新約 715ms，而**那 715ms 貼圖方案也要付**。
  //    ⭐ 多約 70 毫秒，換掉一整類 bug，並刪掉快照／校驗／白紙啟發式三段程式碼。
  // ⚠️ 自我複查抓到的：原本的 WiFi／BLE 拆除排在 goToSleep() 【之後】，而這條路插在它【之前】
  //    → 不先關就等於帶著 WiFi 進淺睡眠，而那是耗電大戶（也可能干擾 light sleep 本身）。
  //    兩者都不是續讀需要的東西，關掉沒有副作用。
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  // ⚠️ BLE 目前是編譯期關閉的（v108），`shutdownForSleep()` 是空實作，所以這裡不呼叫它
  //    （BLE_REMOTE 的宣告也在本函式之後）。**哪天 BLE 重新啟用，這條路必須補上拆除**，
  //    否則會帶著 BLE controller 進淺睡眠 —— 那是 28KB 連結成本之外的執行期耗電。
  //    真關機那條路有 BLE_REMOTE.shutdownForSleep()，不受影響。

  // ⛔⛔ v305：**不要在這裡卸載 SD 字型。** v296 我加了 `unloadForLowMemory()` 想替桌布解碼
  //    騰 RAM，結果製造出一個更貴的 bug：
  //      LSLEEP resume → ADVRESET used=0 kept=0 → SCTLOAD reject=2 → 「建立索引中」
  //    根因在 `Section.cpp:293` 的快取比對：`renderer.probeEmFP(spec.fontId) != fileEmFP`。
  //    而 `probeEmFP()` **有兩條精度路徑** —— 字寬表熱的時候走 12.4 定點，
  //    表被重置就掉到「整數像素 × 16」，值不一樣 → 比對不符 → 整章重排。
  //    ⚠️ 這正是我自己在 v284/v285 寫下的警語（「probeEmFP 必須在每次 build 中凍結一次」），
  //      而卸載字型就是把它解凍的動作。使用者的症狀：翻過很多次的書，換章又要建索引。
  //
  //    ⭐ 而且那句話本來就是多餘的：v24 的 `MemoryRelief` 掛鉤由閱讀器 `onEnter` 裝上、
  //      淺睡眠期間閱讀器還活著所以仍然有效 —— **記憶體真的不夠時它自己會卸載再載回**。
  //      讓它在需要時才動手，比我在這裡無條件先砍一刀正確得多。

  // ⚠️ 複查抓到：SleepActivity::onEnter() 會動 renderer 的方向（applyOrientation → Portrait）。
  //    畫面本身是原始位元組、與方向無關，但**續讀之後的下一次繪製會用錯方向**（翻頁整頁歪掉）。
  //    存回來。
  const auto savedOrientation = renderer.getOrientation();

  // ⛔⛔ v304：**繪製鎖一路握到醒來。**（v323 起提前到這裡：快照、桌布都在鎖內做。）
  //    實機 bug：休眠中畫面會冒出「下一頁」的文字（英文、破折號尤其明顯）。
  //    根因 —— 淺睡眠的全部重點就是「閱讀器留在 RAM」，而閱讀器活著就表示 **render task
  //    也活著**，背景排版／預取完成後它照樣會把新的一頁畫到桌布上面。
  //    `deepSleepInProgress` 擋不住（它只擋 heap 重整重開機，不擋繪製）。
  //    ⭐ codex 複查第三次點名「暫停／排空 render task」—— 這次是真的咬到了。
  //    握著鎖的 render task 是阻塞在 mutex 上、不耗 CPU，正好也是我們要的。
  //    ⚠️ 續讀時必須在 requestUpdateAndWait()／requestUpdate() 之前 unlock()，否則自己鎖死自己。
  //    v323（codex 第三輪）：鎖要在【快照之前】取 —— 在途的繪製先做完，而且 8KB 分塊槽是 renderer 唯一的一組
  //    （抗鋸齒／截圖共用），render task 阻塞後才不會有第二個持有者。桌布路徑本身不用那個槽（讀碼確認）。
  //    v326：鎖改由 enterDeepSleep() 在畫提示之前就取好、傳進來（sleepLock 參數），這裡不再另取。

  // ⭐ v323（帳本 A8）：睡前那一頁用既有的 8KB 分塊留在 RAM（≈52KB），醒來判定完直接推回面板 ——
  //   第一眼不再等算圖（實機 diag319：prewarm 305＋bw 60），只剩「按滿門檻＋GC 轉場」。分塊配不到就走原路（重畫）。
  //   先放掉預取快取（14–35KB 的死重：醒來的背景重畫會重新預取）換空間；存完仍要維持既有的 20KB 最大塊
  //   不變量（codex 第三輪：12KB 證明不了桌布安全），不夠就放掉、走原路。分塊在 disarm() 統一還原＋釋放。
  //   codex 第二輪否決了「等按滿時預熱」—— 預熱不可中斷，門檻落在預熱裡就判不準；留住畫面不需要在等待裡
  //   做任何事，判定回到 v319 的形狀。
  bool frameKept = false;
  bool frameRestoredOk = false;
  bool pageStillValid = true;  // 進來時 framebuffer 是書頁（呼叫端保證；提示還原失敗時根本不會延後 wake frame）
  {
    size_t released = 0;
    if (auto* fcm = renderer.getFontCacheManager()) released = fcm->releaseRetainedCache();
    frameKept = renderer.storeBwBuffer();
    const size_t afterLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t afterFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);  // 與 largest 同一時間點
    // ⚠️ v327（diag326 定案）：門檻從 20KB 降到 12KB。v326 實機 11 次休眠有 9 次 `keep ok=0 largest=20468`——
    //   快照【存成功了】，只因存完的最大塊比 20,480 少 12 bytes 就被放掉，醒來走 1.95s 的舊路、還多付一次 nokeep 寫檔。
    //   那 9 次桌布畫完後 largest 都回到 69,620（快照還回去了），畫桌布本身的需求（快取命中：讀平面進 framebuffer、
    //   一個 HalFile、一條路徑字串；未命中：列緩衝 660B＋抖色 ~3KB）遠低於 12KB。20KB 是淺睡眠【入口】的不變量
    //   （上面 lsLargest 那關仍是 20KB），不是畫桌布的需求。
    constexpr size_t KEEP_MIN_LARGEST_AFTER = 12 * 1024;
    if (frameKept && afterLargest < KEEP_MIN_LARGEST_AFTER) {
      pageStillValid = renderer.restoreBwBuffer();  // codex：還原失敗＝framebuffer 不再是書頁，下面不可拿去存
      frameKept = false;
    }
    DiagLog::line("LSLEEP keep ok=%d released=%u largest=%u free=%u", frameKept ? 1 : 0,
                  static_cast<unsigned>(released), static_cast<unsigned>(afterLargest), static_cast<unsigned>(afterFree));
  }
  // v326（A9）：留不住書頁（kept=0）→ 桌布馬上要蓋掉 framebuffer，wake frame 只能現在寫（付 ~1.3s，跟以前一樣）。
  if (wakeFrameDeferred && !frameKept) {
    if (pageStillValid) saveWakeFrameTimed("nokeep");
    else DiagLog::line("WAKEFRAME save skipped why=nokeep norestore=1");
    wakeFrameDeferred = false;  // 已處理（codex 第二輪：之後的 powerOffExit 不再重複記「skipped」）
  }
  // 桌布峰值的基準改在快照之後取（否則 peak 會把 52KB 快照算進去）。
  const size_t keepFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const size_t keepMinFree = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  // 畫桌布 —— 堆疊上的一次性物件，不碰 activity 堆疊、不銷毀閱讀器。
  {
    SleepActivity sleepScreen(renderer, mappedInputManager);
    sleepScreen.onEnter();
  }
  {
    // v317 證人：畫桌布前後的堆積。peak = 畫桌布期間創下的新低相對於進場時的可用量
    //   （0 ＝ 沒創新低 → 需求不超過 pre_free − pre_minfree，只知道上界）。
    const size_t postMinFree = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // v323：基準在快照之後。kept=0 但曾配過又放掉快照時，歷史最低已被快照壓低 → peak 可能低報成 0（只在 kept=1 時可信）。
    const size_t peak = postMinFree < keepMinFree ? (keepFree - postMinFree) : 0;
    DiagLog::line("LSLEEP mem pre_largest=%u pre_free=%u pre_minfree=%u post_largest=%u post_free=%u post_minfree=%u peak=%u kept=%d",
                  static_cast<unsigned>(lsLargest), static_cast<unsigned>(lsFree), static_cast<unsigned>(lsMinFree),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(postMinFree), static_cast<unsigned>(peak), frameKept ? 1 : 0);
  }

  // v332：桌布已上面板、沒人在等 → 現在把 SD 的兩份保險補寫（progress.bin 若過期、state.json）。
  //   淺睡眠中斷電、或之後走另一個 OTA 槽（降版）時，SD 最多落後「這次醒來之後的翻頁」，不再是整段閱讀（codex）。
  //   代價：真正入睡晚 60ms–1.3s（卡片停頓）；這段裡按鍵要等它 —— 但比 v331 以前「桌布之前就等」好。
  //   仍在 sleepLock（RenderLock）內：進度寫入的不變量不變。
  {
    const uint32_t ckT0 = millis();
    const unsigned pfails = activityManager.flushProgressDurable();
    const bool sok = APP_STATE.saveDurable();
    DiagLog::line("LSLEEP sd-checkpoint ms=%lu pfail=%u state=%d", static_cast<unsigned long>(millis() - ckT0), pfails,
                  sok ? 1 : 0);
  }
  // （v304 的 RenderLock 自 v323 起提前到上面「留住書頁」之前取得 —— 快照與桌布都在鎖內做。）

  const gpio_num_t powerPin = static_cast<gpio_num_t>(BoardConfig::ACTIVE.input.power);
  // 每一條離開路徑都要把本次掛上的喚醒源撤掉（複查點名）：殘留的 30 分鐘計時器會
  // 影響接下來的真關機（插著 USB 時那是真的深度睡眠，會被它叫醒）。
  auto disarm = [&]() -> bool {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    gpio_wakeup_disable(powerPin);
    // ⭐ v325：gpio_wakeup_enable(LOW_LEVEL) 順便把 GPIO_PINn 的 INT_TYPE 設成低電位觸發並開了 CPU 中斷致能，
    //   gpio_wakeup_disable 只清 wakeup 位元 —— 留下的 0x2200 撐過軟體重啟，就是「WEB exit 之後電源鍵黑洞」的兇手
    //   （diag324：黑洞開機 boot-pre cfg=0x2200，正常 0x0000，其他欄位全同）。從源頭清掉。
    gpio_set_intr_type(powerPin, GPIO_INTR_DISABLE);
    gpio_intr_disable(powerPin);
    // v297：一定要放掉 GPIO13 的 hold —— 留著它會讓接下來的真關機失效
    //（startDeepSleep() 靠把同一支腳拉【低】來切電池，鎖在 HIGH 就切不掉 → 關不了機）。
    // ⛔⛔ v306：**同一個道理的另一半，v297 漏了。**
    //    `esp_sleep_pd_config(VDD_SDIO, ON)` 是【全域且持續生效】的睡眠設定，不是一次性的。
    //    設下去之後不還原 → 之後每一次真關機都會保住 flash／SPI 電源域，
    //    而 **GPIO13 就是 SPIWP、就在那個域裡** → 拉不低 → 電池切不斷 → **關不掉機**。
    //    使用者實機症狀：「按電源鍵沒反應，按再久也沒用，但翻得動頁、回得了首頁，
    //    最後只能按硬體重置鍵」——翻頁用既有緩衝區所以照常，關機要動電源路徑所以死。
    //    ⚠️ 而且只在「那次開機曾經淺睡過」之後發生，所以是間歇性的（＝他說的「某些情況」）。
    // v323（codex）：兩個還原呼叫要看結果 —— 它們失敗正是 v306「之後關不了機」的形狀。有限重試 3 次＋證人，
    //   並回傳成敗。⚠️ 沒有更安全的恢復路徑：這兩個 API 只會因參數無效而失敗（GPIO13 支援 hold、VDDSDIO 是
    //   合法電源域），所以這是防禦性檢查；真的失敗時繼續走原路仍是最好的選擇 —— 留在這裡等於機器卡死。
    esp_err_t eUnhold = ESP_FAIL;
    esp_err_t ePdAuto = ESP_FAIL;
    for (int i = 0; i < 3 && (eUnhold != ESP_OK || ePdAuto != ESP_OK); i++) {
      if (i) delay(5);
      eUnhold = gpio_hold_dis(GPIO_NUM_13);
      ePdAuto = esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_AUTO);
    }
    if (eUnhold != ESP_OK || ePdAuto != ESP_OK) {
      DiagLog::line("LSLEEP disarm-fail unhold=%d pd=%d", static_cast<int>(eUnhold), static_cast<int>(ePdAuto));
    }
    renderer.setOrientation(savedOrientation);
    // v323（帳本 A8）：睡前留在 RAM 的書頁還原回 framebuffer 並釋放分塊 —— 每一條離開路徑都經過這裡。
    if (frameKept) {
      frameRestoredOk = renderer.restoreBwBuffer();
      frameKept = false;
    }
    return eUnhold == ESP_OK && ePdAuto == ESP_OK;
  };
  // v326（A9）：真關機的出口 —— disarm（含把 RAM 快照還原進 framebuffer）之後，若 wake frame 延後了就在這裡寫
  //   （這一刻沒人在等）；還原失敗就沒有畫面可寫，冷開機走慢路（正確，只是慢）。
  auto powerOffExit = [&](const char* why) -> bool {
    disarm();
    if (wakeFrameDeferred) {
      if (frameRestoredOk) saveWakeFrameTimed(why);
      else DiagLog::line("WAKEFRAME save skipped why=%s norestore=1", why);
    }
    return false;
  };
  // ⚠️⚠️ 複查抓到，而且這條會讓功能【根本不會動】：現在電源鍵多半還按著，
  //    而我們要掛的是「低電平喚醒」→ 一進淺睡眠就被自己立刻叫醒。
  //    原本的 startDeepSleep() 也是先等放開才武裝喚醒源，同樣的理由。
  //    （v327 曾在這裡做「按住不放＝真關機」；v328 依設計原則 36 拿掉：使用者不需要知道休眠與關機的差別，
  //      關機由政策決定 —— 30 分鐘沒動、電量低、WiFi 開著。實機也證明手勢的觸發點落在人自然長按之後。）
  waitForPowerRelease();

  // 喚醒來源：電源鍵（低電平）＋ 30 分鐘計時器。回傳值都要檢查（複查點名）。
  const esp_err_t eGpio = gpio_wakeup_enable(
      powerPin, BoardConfig::ACTIVE.input.powerActiveHigh ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
  const esp_err_t eSrc = esp_sleep_enable_gpio_wakeup();
  const esp_err_t eTmr = esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_WINDOW_US);


  if (eGpio != ESP_OK || eSrc != ESP_OK || eTmr != ESP_OK) {
    DiagLog::line("LSLEEP skip why=arm gpio=%d src=%d tmr=%d", static_cast<int>(eGpio), static_cast<int>(eSrc),
                  static_cast<int>(eTmr));
    return powerOffExit("arm");
  }

  // ─── v297：對抗電池閂鎖 ───────────────────────────────────────────────
  // v296 實機判定：`LSLEEP enter` 之後【一行都沒有】，下一行就是 `BOOT rst=1`
  // （ESP_RST_POWERON ＝ 晶片整個斷電又上電，不是 panic／看門狗／brownout）。
  // 在電池上唯一能切斷電源的只有 GPIO13 的電池閂鎖 MOSFET，而上游 #1298 的逆向寫著
  // 「一旦進入睡眠，它會預設變成浮接」—— GPIO13 在 C3 上是 SPIWP（flash 腳位），
  // 睡眠關掉 flash 電源域 → 腳位放掉 → 閘極失去驅動 → 電池斷開。
  // ⭐ 所以「深度睡眠＝完全斷電」與「淺睡眠也斷電」是同一個原因。
  //
  // 兩個對策，一起上（各自都可能不夠）：
  //   ① 把 GPIO13 釘在 HIGH 並鎖住 —— 現行 startDeepSleep() 就是用同一組 API 鎖 LOW 來關機，
  //      方向相反而已。
  //   ② 保住 VDD_SDIO 電源域 —— 不讓 flash／SPI 腳位在睡眠時被關掉。
  // v323（codex）：依序做、前一步失敗就不做下一步（不把未知電平鎖住）；任一失敗 → 不睡、走真關機 ——
  //   沒鎖住的 GPIO13 在睡眠中會斷電池（v296 的死法）。
  const esp_err_t eDir = gpio_set_direction(GPIO_NUM_13, GPIO_MODE_OUTPUT);
  const esp_err_t eLvl = eDir == ESP_OK ? gpio_set_level(GPIO_NUM_13, 1) : ESP_FAIL;
  const esp_err_t eHold = eLvl == ESP_OK ? gpio_hold_en(GPIO_NUM_13) : ESP_FAIL;
  const esp_err_t ePd = eHold == ESP_OK ? esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON) : ESP_FAIL;
  if (ePd != ESP_OK) {
    DiagLog::line("LSLEEP skip why=hold dir=%d lvl=%d hold=%d pd=%d", static_cast<int>(eDir), static_cast<int>(eLvl),
                  static_cast<int>(eHold), static_cast<int>(ePd));
    return powerOffExit("holdfail");
  }

  // ⛔ v319：**1 秒試睡拿掉了。** 它是 v297 為了驗證「GPIO13 鎖住後淺睡眠不會斷電」加的，
  //    v297–v317 已經活過來上百次（probe-ok 從沒失敗過），該驗的早就驗完。而它製造了一個真缺陷
  //    （diag317-2，實機回報：「按休眠沒有辦法作用」）：使用者看到桌布就按電源鍵要喚醒，那一按落在試睡的
  //    1 秒窗裡 → 試睡被 GPIO 提早叫醒（`probe-ok slept=735／663`），程式不看是誰叫醒的、照樣往下 →
  //    這時鍵還按著，正式的 esp_light_sleep_start() 因為「喚醒源已觸發」回 ESP_ERR_INVALID_STATE(259)
  //    → `LSLEEP failed` → 落回真關機。使用者的體感：淺睡眠沒作用，每次都是冷開機。
  //    （v301／v309 那幾次 err=259 當時歸因成「周邊還握著電源鎖」—— 現在看是同一件事。）
  //    30 分鐘計時器在上面 eTmr 已經掛好，拿掉試睡之後不再有人覆寫它（v298 那個坑一併消失）。

  // v303：mV／mA 是待機耗電量測的主要依據 —— SOC 只有整數百分比，30 分鐘 5mA 只掉 0.17%。
  uint16_t mvIn = 0;
  int16_t maIn = 0;
  gpio.readBatteryVI(&mvIn, &maIn);
  // v305：進睡時的 em 值，醒來要拿來對照（見 resume 那行的說明）。
  const int32_t emIn = fromReader ? renderer.probeEmFP(SETTINGS.getReaderFontId()) : 0;
  DiagLog::line("LSLEEP enter win=%lu soc=%u mv=%u ma=%d hold=%d pd=%d rd=%d",
                static_cast<unsigned long>(LIGHT_SLEEP_WINDOW_US / 1000ULL),
                static_cast<unsigned>(powerManager.getBatteryPercentage()), static_cast<unsigned>(mvIn),
                static_cast<int>(maIn), static_cast<int>(eHold), static_cast<int>(ePd), fromReader ? 1 : 0);

  const uint32_t beforeMs = millis();
  // ⭐ v319：**進睡眠的那一瞬間電源鍵已經按著 ＝ 喚醒請求，不是失敗。**
  //    喚醒源是「電源腳低電位」；鍵按著時 esp_light_sleep_start() 會因為喚醒源已觸發而回 259。
  //    兩個入口：① 呼叫前鍵已按著（連睡都不用睡）② 呼叫後回 259 且鍵按著（v323 起只認 259，其他錯誤不遮蔽）。
  //    v302 的重試保留給「鍵沒按著卻被拒絕」那種真正的短暫狀態。
  // ⭐ v323：包成迴圈 —— **短按（沒按滿 getPowerButtonDuration）不再變真關機，改用剩餘時間續睡**（帳本 C-3）。
  //    每次入睡前重掛計時器：esp_sleep_enable_timer_wakeup 是「下次入睡起算」，剩餘時間在【即將入睡那一刻】
  //    從第一次入睡起算重算（codex：醒著處理的時間要扣掉；重試那次也一樣）。GPIO 喚醒源、GPIO13 hold、VDD_SDIO
  //    都還在，不必重做。上限 5 次短按或剩不到 5 秒 → 照舊真關機。
  esp_err_t eSleep = ESP_OK;
  uint8_t sleepRetries = 0;
  uint32_t sleptMs = 0;    // 從第一次入睡起算的經過時間（含各次短按醒著的處理）
  uint32_t heldMs = 0;
  uint32_t wakeAtMs = 0;
  uint32_t pressAtMs = 0;  // 放開等待裡看到「又按下」的時刻 → 下一輪的按滿計時從它起算（不吞掉那一按）
  uint8_t taps = 0;
  const uint32_t windowMs = static_cast<uint32_t>(LIGHT_SLEEP_WINDOW_US / 1000ULL);
  auto armTimer = [&]() -> bool {
    const uint32_t elapsed = millis() - beforeMs;
    const uint32_t remain = elapsed < windowMs ? windowMs - elapsed : 0;
    if (remain < 5000) return false;
    return esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(remain) * 1000ULL) == ESP_OK;
  };
  for (;;) {
    const bool repress = pressAtMs != 0;
    bool wokeAtEntry = repress || gpio.powerDownRaw();
    if (!wokeAtEntry) {
      if (!armTimer()) {
        DiagLog::line("LSLEEP timer-expired slept=%lu taps=%u", static_cast<unsigned long>(millis() - beforeMs),
                      static_cast<unsigned>(taps));
        return powerOffExit("expired");
      }
      eSleep = esp_light_sleep_start();
      if (eSleep != ESP_OK && !gpio.powerDownRaw()) {
        delay(50);
        if (!armTimer()) {
          DiagLog::line("LSLEEP timer-expired slept=%lu taps=%u retry=1", static_cast<unsigned long>(millis() - beforeMs),
                        static_cast<unsigned>(taps));
          return powerOffExit("expired-retry");
        }
        eSleep = esp_light_sleep_start();
        sleepRetries++;
      }
      if (eSleep == ESP_ERR_INVALID_STATE && gpio.powerDownRaw()) {
        wokeAtEntry = true;
      }
    }
    if (wokeAtEntry && !repress) {
      DiagLog::line("LSLEEP wake-at-entry err=%d retries=%u", static_cast<int>(eSleep),
                    static_cast<unsigned>(sleepRetries));
      eSleep = ESP_OK;
    }
    wakeAtMs = millis();
    sleptMs = wakeAtMs - beforeMs;
    // 沒真的睡著時 esp_sleep_get_wakeup_cause() 回的是上一次的原因，不能用 —— 直接當成按鍵。
    const esp_sleep_wakeup_cause_t cause = wokeAtEntry ? ESP_SLEEP_WAKEUP_GPIO : esp_sleep_get_wakeup_cause();

    // ⚠️ start 失敗時 wakeup cause 不是本次的（複查點名）→ 不可據以續讀。
    if (eSleep != ESP_OK) {
      DiagLog::line("LSLEEP failed err=%d retries=%u slept=%lu taps=%u", static_cast<int>(eSleep),
                    static_cast<unsigned>(sleepRetries), static_cast<unsigned long>(sleptMs),
                    static_cast<unsigned>(taps));
      return powerOffExit("failed");
    }

    // ⚠️ 計時器叫醒（或任何非按鍵的原因）→ 不續睡，交回去真關機。
    //    sleptMs 是「儀器自證」：若它遠小於 30 分鐘，代表 light sleep 根本沒睡著。
    if (cause != ESP_SLEEP_WAKEUP_GPIO) {
      // ⭐ 這一對 mv/ma 與上面 `LSLEEP enter` 的那一對，就是待機耗電的量測值。
      //    30 分鐘的電壓降遠比 1% 的 SOC 解析度靈敏。
      uint16_t mvOut = 0;
      int16_t maOut = 0;
      gpio.readBatteryVI(&mvOut, &maOut);
      DiagLog::line("LSLEEP timeout cause=%d slept=%lu soc=%u mv=%u ma=%d dmv=%d taps=%u", static_cast<int>(cause),
                    static_cast<unsigned long>(sleptMs), static_cast<unsigned>(powerManager.getBatteryPercentage()),
                    static_cast<unsigned>(mvOut), static_cast<int>(maOut),
                    static_cast<int>(static_cast<int32_t>(mvOut) - static_cast<int32_t>(mvIn)),
                    static_cast<unsigned>(taps));
      return powerOffExit("timeout");
    }

    // 電源鍵叫醒。此時按鍵立刻讀得到（沒有開機那段盲區），所以誤觸保護終於可以【真的】生效
    // 而且不必付任何等待 —— 這正是 v294/v295 在冷開機上做不到的事。
    // 判定形狀與 v319 相同：逐 10ms 看鍵，**到門檻時鍵仍按著**才算要醒。
    // 上一輪放開等待裡看到的「又按下」從它按下的時刻起算（codex 第二輪：那一按不能被吞掉）。
    const uint32_t holdStart = repress ? pressAtMs : millis();
    pressAtMs = 0;
    const uint32_t req = SETTINGS.getPowerButtonDuration();
    bool stillDown = gpio.powerDownRaw();
    while (stillDown && millis() - holdStart < req) {
      delay(10);
      stillDown = gpio.powerDownRaw();
    }
    heldMs = millis() - holdStart;
    if (stillDown && heldMs >= req) break;  // 按滿 → 真的要醒

    // 短按 → 續睡（v323，C-3）。先等它穩定放開（有上限 500ms）：接點彈跳會把低電位喚醒源立刻又觸發、
    // 一次放開就把 5 次額度燒光。三種結果：穩定放開 → 續睡；放開後又按下 → 記下時刻、下一輪不睡直接判定；
    // 逾時而鍵仍按著（半按著／接點壞）→ 同樣交給下一輪判定。
    taps++;
    {
      const uint32_t r0 = millis();
      uint32_t upSince = 0;
      for (;;) {
        const bool down = gpio.powerDownRaw();
        if (!down) {
          if (upSince == 0) {
            upSince = millis();
          } else if (millis() - upSince >= 50) {
            break;  // 穩定放開
          }
        } else if (upSince != 0) {
          pressAtMs = millis();  // 放開後又按下
          break;
        }
        if (millis() - r0 >= 500) {
          if (down) pressAtMs = millis();
          break;
        }
        delay(5);
      }
    }
    if (taps > 5) {
      DiagLog::line("LSLEEP tap-poweroff held=%lu req=%lu slept=%lu taps=%u", static_cast<unsigned long>(heldMs),
                    static_cast<unsigned long>(req), static_cast<unsigned long>(sleptMs), static_cast<unsigned>(taps));
      return powerOffExit("taps");
    }
    DiagLog::line("LSLEEP tap-resleep held=%lu req=%lu slept=%lu taps=%u repress=%d", static_cast<unsigned long>(heldMs),
                  static_cast<unsigned long>(req), static_cast<unsigned long>(sleptMs), static_cast<unsigned>(taps),
                  pressAtMs != 0 ? 1 : 0);
  }

  // ⭐ v323（帳本 A8）：睡前那一頁若還留在 RAM（frameKept），disarm() 會把它還原進 framebuffer ——
  //   直接推上面板就是第一眼，不等算圖（今天：prewarm 305＋bw 60 之後才推）。桌布之後舊平面無效，
  //   驅動自己走 GC（Uc8279Driver::displayStart 的 forceGc），與今天重畫的轉場同一種、同價（769ms）。
  //   ⚠️ 推面板要在放鎖之前（render task 一拿到鎖就可能畫）；背景重畫在放鎖之後才請 —— 時鐘／電量／
  //   抗鋸齒／圖片灰階由閱讀器自己補，舊平面已有效 → FAST 走 DU 差分，只動有變化的像素（v318 的同一種形狀）。
  const bool unarmOk = disarm();  // 含還原 renderer 方向；frameKept 時把書頁還原進 framebuffer（→ frameRestoredOk）
  const bool frameRestored = frameRestoredOk;
  if (fromReader) sdFontSystem.ensureLoaded(renderer);  // v301／v305：安全網（已載入時是 no-op）；v327：首頁醒來不載字型
  uint32_t paintMs = 0;
  uint8_t firstBank = 0;  // 第一眼那次推面板用的 bank —— 要在背景重畫之前讀（codex 第四輪）
  if (frameRestored) {
    // ⚠️⚠️ restoreBwBuffer() 內含 cleanupGrayscaleBuffers(fb)：把 DTM1／DTM2 都改成書頁並標記「舊平面有效」——
    //   但面板上此刻還是桌布。不明講 resync 的話，FAST 走 DU 差分「書頁→書頁」＝一個像素都不驅動，
    //   使用者會永遠停在桌布（codex 三輪都以為驅動會 forceGc；是讀 restoreBwBuffer 全文才看到的）。
    display.requestResync();  // 下一次刷新走 GC（與今天重畫的轉場同價 769ms），之後恢復差分
    const uint32_t p0 = millis();
    renderer.displayBuffer();
    paintMs = millis() - p0;
    firstBank = static_cast<uint8_t>(renderer.lastRefreshBank());
  }

  // ⚠️ v304：放開繪製鎖 —— 下面要請 render task 重畫，還握著就是自己鎖死自己。
  sleepLock.unlock();

  // ⭐ v304：**重置閒置計時器**。`millis()` 在淺睡眠期間照常前進，不重置的話，只要睡得比
  //    「自動休眠」設定值久，一醒來就立刻又睡回去（實機：睡 25 分鐘 → 117ms 後 SLEEP timeout=1）。
  g_lastActivityTime = millis();

  if (frameRestored) {
    activityManager.requestUpdate();  // 背景重畫（不等）
  } else {
    // 請閱讀器重畫。它完整留在 RAM，所以只要付「繪製 ＋ 面板刷新」。
    // ⚠️ 用 AndWait：不等的話會在桌布還在畫面上時就返回，而按鍵邊緣會對著一個看不見的頁面
    //    派送（開機路徑的 Silent 分支為了同一個理由也用它）。
    const uint32_t p0 = millis();
    activityManager.requestUpdateAndWait();
    paintMs = millis() - p0;
    firstBank = static_cast<uint8_t>(renderer.lastRefreshBank());
  }
  // ⭐ v323 證人（帳本 A1）：醒來（GPIO 喚醒返回）→ 第一個畫面上面板。first 從 wakeAtMs 起算
  //    ＝ hold（等按滿）＋ disarm ＋ paint（kept=1：推睡前那頁；kept=0：重畫＋推）。bank 1=GC／2=DU。
  //    pcfg：醒來 disarm 之後電源腳的 GPIO_PINn（v325 起應為 0；0x2200 ＝ 中斷殘留）。
  DiagLog::line("LSLEEP repaint first=%lu hold=%lu kept=%d paint=%lu bank=%u taps=%u unarm=%d stackfree=%u pcfg=%08lx",
                static_cast<unsigned long>(millis() - wakeAtMs), static_cast<unsigned long>(heldMs),
                frameRestored ? 1 : 0, static_cast<unsigned long>(paintMs),
                static_cast<unsigned>(firstBank), static_cast<unsigned>(taps), unarmOk ? 1 : 0,
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                static_cast<unsigned long>(GPIO.pin[BoardConfig::ACTIVE.input.power].val));

  uint16_t mvOut = 0;
  int16_t maOut = 0;
  gpio.readBatteryVI(&mvOut, &maOut);
  // ⭐ v305 證人：`em` 是 Section 快取比對用的那個值（Section.cpp:293 的 probeEmFP）。
  //    進睡與醒來必須【相同】—— 不同就會 SCTLOAD reject=2、整章重排（使用者看到「建立索引中」）。
  const NvsStore::Stats nvsSt = NvsStore::takeStats();  // v331：這個週期裡的 NVS 影子寫入（含入口 2 次 state）
  DiagLog::line("LSLEEP resume slept=%lu held=%lu retries=%u taps=%u soc=%u mv=%u ma=%d dmv=%d em=%ld/%ld nvsw=%lu nvsmax=%lu nvsfail=%lu nvserr=%d",
                static_cast<unsigned long>(sleptMs), static_cast<unsigned long>(heldMs),
                static_cast<unsigned>(sleepRetries), static_cast<unsigned>(taps), static_cast<unsigned>(powerManager.getBatteryPercentage()),
                static_cast<unsigned>(mvOut), static_cast<int>(maOut),
                static_cast<int>(static_cast<int32_t>(mvOut) - static_cast<int32_t>(mvIn)),
                static_cast<long>(emIn),
                static_cast<long>(fromReader ? renderer.probeEmFP(SETTINGS.getReaderFontId()) : 0), static_cast<unsigned long>(nvsSt.writes),
                static_cast<unsigned long>(nvsSt.usMax), static_cast<unsigned long>(nvsSt.fails), nvsSt.lastErr);

  // v293 的喚醒畫面是為「真關機後冷開機」準備的。既然這次是續睡回來、而且畫面已經重畫過，
  // 那份就過期了 —— 留著會在日後萬一當機重開時貼出一張舊頁（而且可能是那張黑的）。
  APP_STATE.wakeFrameToken = 0;
  Storage.remove(wakeFrameFile());
  APP_STATE.save();

  // 放開電源鍵再回去，避免 loop() 把同一次按壓當成新的長按。
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
  return true;
}

void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  // v185 證人：分清「睡著醒來回主畫面」與「重置回主畫面」——兩者在 log 上原本都只有一行 BOOT。
  const NvsStore::Stats nvsIn = NvsStore::takeStats();  // v331：上次醒來（或開機）之後的 NVS 影子寫入 —— 翻頁那些
  DiagLog::line("SLEEP timeout=%d mode=%d nvsw=%lu nvsmax=%lu nvsfail=%lu nvserr=%d", static_cast<int>(fromTimeout),
                static_cast<int>(WiFi.getMode()), static_cast<unsigned long>(nvsIn.writes),
                static_cast<unsigned long>(nvsIn.usMax), static_cast<unsigned long>(nvsIn.fails), nvsIn.lastErr);
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  // ⭐ v326（帳本 A9，維護者 2026-09-21 選定）：wake frame 只有【冷開機回書】會用到（`loadWakeFrameBuffer`），淺睡眠醒來
  //   根本不用它 —— 而手動休眠卻每次付 ~1.3s 寫檔（diag323：popup→狀態存檔 1,417ms）。實機頻率：手動休眠 197 次、
  //   冷開機用到 54 次。改成：**會走淺睡眠的休眠不在入口寫**，留到真的要關機的出口（30 分鐘超時／連點／睡不成）
  //   再從 RAM 快照寫（v323 起書頁本來就留在 RAM；那一刻沒人在等）。不走淺睡眠的（自動休眠超時、首頁、哨兵沒開）照舊。
  //   代價：淺睡眠中電池耗盡／按重置鍵 → 沒寫到 → 下次冷開機走慢路（4.8s），不會錯。
  // ⭐ v327（維護者 2026-09-22 拍板）：淺睡眠改【全域、黑名單制】（Activity::supportsLightSleep）。v303 限在閱讀器是因為
  //   當時待機電流沒量到；現在有了：5mA、30 分鐘上限 ＝ 每次最多 2.5mAh（≈0.17%），讀書 4 分鐘的電。
  //   wake frame 仍只給閱讀器（冷開機回書才用得到）。
  //   WiFi 開著就不睡（codex：黑名單之外的保險 —— lightSleepCycle 會把 WiFi 關掉，OPDS 之類醒來就壞）。
  //   v328：電量低（<LIGHT_SLEEP_MIN_SOC%）且沒接 USB 也不睡 —— 關機是政策，不是使用者的選擇（設計原則 36）。
  const unsigned socNow = powerManager.getBatteryPercentage();
  const bool onUsb = gpio.isUsbConnected();
  const bool batteryOkForLightSleep = socNow >= LIGHT_SLEEP_MIN_SOC || onUsb;
  const bool willTryLightSleep = !fromTimeout && activityManager.supportsLightSleep() &&
                                 WiFi.getMode() == WIFI_MODE_NULL && batteryOkForLightSleep && lightSleepEnabled();
  if (!batteryOkForLightSleep && !fromTimeout && activityManager.supportsLightSleep() && lightSleepEnabled()) {
    DiagLog::line("LSLEEP skip why=lowbat soc=%u usb=%d", socNow, onUsb ? 1 : 0);
  }
  const bool readerOnTop = activityManager.currentIsReaderActivity();  // wake frame 只給「閱讀器在最上層」
  bool wakeFrameDeferred = false;

  // v293：**必須在 goToSleep() 之前** —— 它會把待機畫面（桌布）畫進同一張 framebuffer。
  //   只在「從閱讀器休眠」時存 —— 不是從閱讀器睡的 → 醒來不會回書，存了也沒用。
  //   （v330 以前還排除 QuickResume 模式：那條路自己有 sleep_frame.bin；整條路已移除。）
  // ⭐ v322（v321 實機回報：「按完電源一下，才接著出現休眠中的訊息」）：「進入休眠」提示提早到
  //   存 wake frame 之前。原本它在 SleepActivity::onEnter() 才畫，前面排著 wake frame 存檔（≈0.4s）＋
  //   狀態存檔，使用者按了鍵卻沒有任何畫面回應。
  //   提示畫在 framebuffer 上會蓋掉書頁，而 wake frame 要存的正是書頁 → 先用既有的 8KB 分塊把
  //   framebuffer 收起來（storeBwBuffer，抗鋸齒與截圖走的同一個原語，不另配整塊 52KB），畫完提示還原，
  //   再存 frame。分塊配不到就照舊順序（提示晚約半秒），不是錯誤。
  //   方向：SleepActivity 會先套閱讀方向再畫；這裡閱讀器還活著、renderer 已是那個方向，
  //   但仍明寫一次並還原，不依賴「閱讀器一定有套過」這個假設。
  //   ⚠️ codex 第一輪（v322）：這一段到存完 wake frame 之前必須與 render task 互斥 —— 閱讀器還活著，
  //   背景排版／預取完成後它會把新頁畫進同一張 framebuffer（v304 淺睡眠就是被這個咬過）。
  //   `RenderLock` 會等在途的那一筆繪製結束再往下。v326 起這把鎖【一路握到淺睡眠結束】並傳進 lightSleepCycle()
  //   （它不再自己取鎖；renderingMutex 不可重入），真關機前才 unlock 再 goToSleep()。
  //   快照只有「回書」才需要（wake frame 存的是書頁）；從首頁等處休眠時 framebuffer 接下來就被桌布覆寫，
  //   直接畫提示、不付 52KB（codex 第一輪點名）。
  RenderLock fbLock;      // v326：一把鎖從提示、快照、wake frame 一路握到淺睡眠結束（傳進 lightSleepCycle）
  bool pageValid = true;  // framebuffer 此刻是不是書頁（提示還原失敗就不是 → 不存 wake frame、也不延後）
  {
    bool early = false;
    // v327（codex）：會淺睡眠的畫面【都】要在提示前保住 framebuffer —— 否則留在 RAM 的快照帶著提示，醒來第一眼就是它。
    const bool needSnapshot = APP_STATE.lastSleepFromReader || willTryLightSleep;
    if (!needSnapshot || renderer.storeBwBuffer()) {
      const auto savedOrientation = renderer.getOrientation();
      if (APP_STATE.lastSleepFromReader) ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
      GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));  // 同步：displayBuffer() 送完並等 busy 才返回
      renderer.setOrientation(savedOrientation);
      if (needSnapshot) pageValid = renderer.restoreBwBuffer();  // codex：回傳值決定書頁還在不在
      early = true;
    }
    SleepActivity::skipEnteringPopup = early;
    DiagLog::line("SLEEP popup early=%d", early ? 1 : 0);
    // v329：閱讀器欠著的進度先寫掉（它不再每頁寫；淺睡眠期間可能斷電）。在提示之後、wake frame 之前，鎖在手上。
    //   失敗重試一次（codex：不能默默睡掉），還是失敗就記下來 —— 沒有更好的處置。
    {
      unsigned fails = activityManager.flushProgress();
      if (fails) {
        delay(50);
        fails = activityManager.flushProgress();
        DiagLog::line("PROGRESS flush retry fail=%u", fails);
      }
    }
  if (APP_STATE.lastSleepFromReader && readerOnTop && pageValid) {
    // ⭐ 代號先 +1 再存，而且 **APP_STATE 緊接著就會被寫出去**（下面那行），
    //   所以畫面檔與狀態檔帶的是同一個值。喚醒時兩者不相等就不還原 ——
    //   這才擋得掉「同一本書的舊頁」（只比書路徑擋不掉，codex 複查指出）。
    if (++APP_STATE.wakeFrameToken == 0) APP_STATE.wakeFrameToken = 1;  // 0 保留給「沒有畫面」
    if (willTryLightSleep) {
      Storage.remove(wakeFrameFile());  // 舊的先失效（代號已換、對不上，但不留殘骸）；寫檔留到真關機出口
      wakeFrameDeferred = true;
    } else {
      saveWakeFrameTimed("entry");
    }
  } else {
    // 沒存新的就讓舊的失效 —— 「檔案存在且代號相符」必須永遠等於「上一次休眠剛寫的」。
    APP_STATE.wakeFrameToken = 0;
    Storage.remove(wakeFrameFile());
    if (!pageValid) DiagLog::line("WAKEFRAME skip why=norestore");
    else if (APP_STATE.lastSleepFromReader && !readerOnTop)
      DiagLog::line("WAKEFRAME skip why=overlay");  // 閱讀器上疊著選單／註腳：快照是那個畫面，不當書頁存
  }
  }

  // v312：印記 ——「這次關機是 enterDeepSleep() 自願做的」。下次電源鍵喚醒若回首頁，
  //   憑它跳過開機 logo（見 Splash 路徑的說明）。寫進下面那次【既有的】saveToFile()，零額外 SD 寫入。
  //   淺睡眠續讀（沒真的關機）時它仍留 true —— 無害：沒有開機就用不到它。
  APP_STATE.deepSleepStamp = true;
  APP_STATE.save();

  // ⭐ v296：淺睡眠插在這裡 —— **必須在 goToSleep() 之前**，因為那一行會銷毀閱讀器，
  //    而「閱讀器留在 RAM」正是秒開的全部理由。決策 C：只有手動按電源鍵才走這條。
  //    回 true ＝ 已經被按鍵叫醒並貼回原畫面 → 直接返回，loop() 帶著完好的閱讀器繼續。
  //    回 false ＝ 沒睡成／計時器到了／誤觸 → 往下走原本的真關機路徑（螢幕已是桌布）。
  // ⭐ v303：**只有從閱讀器休眠才走淺睡眠**（維護者 2026-09-20 拍板）。
  //    v302 以前哪個畫面都走 —— 判斷式根本沒檢查。但淺睡眠真正值錢的地方是「省掉開書與排版」，
  //    首頁本來就重畫得很快，秒開省不到多少，卻讓**待機耗電的曝險加倍**。
  //    在待機電流還沒量出來之前，把它收斂到唯一划算的情境：曝險減半，而且對照實驗更乾淨。
  //    ⚠️ 這不違反「一致性靠補上、不靠刪掉」—— 那條講的是【功能缺漏】，
  //      這裡是【值不值得付電】的取捨，量到數字之後隨時可以放寬。
  if (willTryLightSleep && pageValid) {  // v326：同一個判斷用兩次（codex：lightSleepEnabled 不重算）
    if (lightSleepCycle(wakeFrameDeferred, fbLock)) {
      return;
    }
  }

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  fbLock.unlock();  // v326：goToSleep() 會銷毀閱讀器並重畫，不能還握著繪製鎖
  activityManager.goToSleep();

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");
  // v332：真關機出口＝「沒人等」的時刻 —— state.json 在這裡補寫（入口那幾次 save() 只寫了 NVS）。
  //   閱讀器的 progress.bin 已由 goToSleep() 觸發的 onExit 寫掉（ActivityManager 持 RenderLock）。
  //   ⚠️ 在 display.deepSleep() 之後：面板已睡，這次 SD 寫入（偶爾 1 秒）不會晚到任何畫面。
  APP_STATE.saveDurable();
  // v331（codex 第二輪）：真關機前把這一段的 NVS 統計印掉 —— 入口那幾次 state 寫入只有淺睡眠醒來那條路
  //   （LSLEEP resume）看得到；真關機的路上 RAM 會沒掉。這裡沒人在等（面板已睡），一次 SD append 無妨。
  {
    const NvsStore::Stats nvsEnd = NvsStore::takeStats();
    DiagLog::line("SLEEP nvs-final nvsw=%lu nvsmax=%lu nvsfail=%lu nvserr=%d", static_cast<unsigned long>(nvsEnd.writes),
                  static_cast<unsigned long>(nvsEnd.usMax), static_cast<unsigned long>(nvsEnd.fails), nvsEnd.lastErr);
  }

  powerManager.startDeepSleep(gpio);
}

// v311：loadReaderFont=false 時只探索 SD 字型、註冊解析器，不載入內文字型（首頁醒來用不到，
//   實機 FONTLOAD why=boot 每次 267ms）。之後進閱讀器由 ReaderActivity::onEnter 的 ensureLoaded()
//   載入（FONTLOAD why=ensure）—— 那個安全網本來就在。
void setupDisplayAndFonts(bool seamless = false, bool loadReaderFont = true) {
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

  // Discover and (if this boot is going to the reader) load SD card fonts
  sdFontSystem.begin(renderer, loadReaderFont);

  LOG_DBG("MAIN", "Fonts setup");
}

// v196（複查）：開機證人**不可以**在這一段寫 SD。DiagLog::line() 每行都會開檔／寫入／flush／關檔，
// 而這幾個點全都排在 verifyPowerButtonWakeup() 與 recovery 按鍵取樣【之前】——慢卡上多幾次同步寫入
// 就會把電源鍵的驗證往後推，讓「按了醒不來」變得更嚴重，也可能讓 UP+POWER 的 SD 更新入口失效。
// 這裡只記時間戳（純 RAM，零 I/O），等過了那一段再一行印出來。
static uint32_t g_wakeT[6] = {0, 0, 0, 0, 0, 0};

// v197：19% 的開機會在 BENCH 之後、RESUME 之前無聲地睡回去（v196 soak：64 次裡 12 次）。
// 那個窗口裡【恰好】有兩個 startDeepSleep()，而兩條路以前都不寫任何東西 —— log 上分不出是哪一個。
// ⚠️ 證人只能放在「已經決定要睡」之後：在 verifyPowerButtonWakeup() 【之前】寫 SD，
//    等於把電源鍵驗證往後推，會讓正在診斷的問題變嚴重（同 v196 複查抓到的形狀）。
//    成功路徑的旗標一律先進 RAM，跟著既有的 WAKE steps 一起印。
static uint8_t g_wakeReason = 255;
static uint8_t g_wakeUsb = 0;
static HalGPIO::PowerVerifyDiag g_pwrDiag;
// v311 settle 證人（純 RAM，跟 WAKE steps 一起在路由之後印）：顯示初始化吃掉多少 settle 窗、
// 初始化之後補了幾筆 gpio.update()、UP 鍵最終判定。沒有這三個數字，救援組合鍵的判定
// 「成功」與「只是沿用了舊狀態」分不出來（codex 指出）。
static unsigned long g_settleInitMs = 0;
static uint8_t g_settleTailSamples = 0;
static uint8_t g_settleUpAt = 0;  // v314：UP 在尾段第幾筆樣本首次讀到（0=沒讀到）
static uint8_t g_settleUp = 0;

// v198：**只加量測，不改判定。** v197 證明夭折全是「問的那一刻按鍵沒被按著」
// （11/11 outcome=1 waited=1000），但沒告訴我們按壓是【多早】結束的。
// ⚠️ 兩個離散快照【定不出放開的邊緣】（複查指出，成立）。它們回答的是一個更小、
//    但足以決定修法的問題：**開機途中的這兩個時刻，按鍵還在不在？**
//      每個取樣點都 0 → 按壓在第一個取樣點之前就結束了 → 那個時刻之後判定救不了
//      某個取樣點是 1 → 按壓至少撐到那裡 → 在那裡判定就能修好這一類
//    沒有這個分辨，任何修法都是猜的 —— 對抗式複查正是這樣打掉 v198 的第一版：
// 用離散取樣點去推「按住多久」，既可能被一個毛刺誤判成按滿（接受集合變寬、
// 新增口袋誤觸開機），也可能整個錯過真正的放開窗口。
//
// 所以這一版只做一件事：在開機途中的幾個既有位置記下電源鍵的瞬時原始電平與時刻，
// 事後一起印出來。**判定邏輯一個字都沒動。**
// ⚠️ 取樣全部排在 boot_recovery::checkBootCombo()【之後】—— 逃生口沒動。
// ⚠️ 只寫 RAM、無配置、無 I/O、無延遲;不參與任何分支。
static constexpr uint8_t PWR_TRACE_MAX = 6;
static uint32_t g_pwrTraceMs[PWR_TRACE_MAX] = {0};
static uint8_t g_pwrTraceDown[PWR_TRACE_MAX] = {0};
static uint8_t g_pwrTraceN = 0;
static const char* powerTraceStr() {
  static char buf[80];
  int off = 0;
  for (uint8_t i = 0; i < g_pwrTraceN && off < static_cast<int>(sizeof(buf)) - 1; ++i) {
    const int w = snprintf(buf + off, sizeof(buf) - off, "%s%lu:%u", i ? " " : "",
                           static_cast<unsigned long>(g_pwrTraceMs[i]),
                           static_cast<unsigned>(g_pwrTraceDown[i]));
    if (w <= 0) break;
    off += w;
  }
  buf[off < 0 ? 0 : (off < static_cast<int>(sizeof(buf)) ? off : static_cast<int>(sizeof(buf)) - 1)] = '\0';
  return buf;
}

static void tracePower() {
  if (g_pwrTraceN >= PWR_TRACE_MAX) return;
  // 先讀再記時刻:getState() 內含兩次 analogRead，先記 millis() 的話那個時刻是下界、
  // 不是取樣發生的時刻（複查指出）。
  const bool down = gpio.powerDownRaw();
  g_pwrTraceDown[g_pwrTraceN] = down ? 1 : 0;
  g_pwrTraceMs[g_pwrTraceN] = millis();
  ++g_pwrTraceN;
}

// v294：把「按住多久」從【問的那一瞬間】換成【證據】。
//
// 缺陷（帳本 2026-09-02 歸因，v197/v198/v199 三代證人）：verifyPowerButtonWakeup() 裡
// calibratedDuration = required − millis()，而它在開機約 744ms 才被呼叫 → 恆飽和成 1，
// 於是「按住 ≥400ms」這條政策整個消失，只剩「你問的那一瞬間按鍵還在不在」。
// 那個瞬間由【開機速度】決定（實測 600–1,600ms），使用者感知不到 → 只能按到兩秒保險。
// 實測夭折率 19–28%（v196 乾淨 soak 19%；全 log 256 次 WAKE abort）。
//
// 修法：電源鍵按下去【才是】機器上電的原因，所以 t≈0 時它必然被按著。於是
// 「按住了多久」＝「最後一次看到它被按著的時刻」，而既有的 tracePower() 取樣點正好給了這個。
// 這才是真的在量按住時間，不是量「開機比你的手快還是慢」。
//
// ⚠️⚠️ 單調安全性（codex 對抗式複查第一輪判「不可放行」後改成現在這個形狀）：
//    我原本的寫法是「證據足夠就【略過】 verifyPowerButtonWakeup()」，並主張那是純加法。
//    **那個證明是錯的** —— verify 不是純函式，它會呼叫 inputMgr.update()、推進去彈跳狀態、
//    更新 held-time 與按鍵邊緣。略過它＝改變下游看到的輸入狀態，不是「逐位元組等價」。
//    現在改成：**舊檢查永遠先跑**（副作用與時序完全不變），early evidence 只用來
//    【覆蓋它的 false】。這樣接受集合嚴格單調遞增，結構上不可能拒絕今天會過的按法。
//    代價：被救回的那次會多花最多 1 秒（verify 等滿才回 false）——
//    但今天那一次是【整個失敗】、使用者要再按一輪，所以仍然是淨賺。
//
// ⚠️ 證據必須是【連續前綴】，不是取最大值（複查 E 點）：若第一個取樣讀到 0、後面才出現 1，
//    那可能是「放開又再按」（上游 TODO 明載 double-tap 能開機），不是一次連續長按。
//    取最大值會把它誤判成按滿。實機 184 次夭折驗證：兩種規則救回數相同（皆 92），
//    且「0 之後出現 1」出現 0 次 —— 採嚴格規則零代價。
static uint32_t powerHeldEvidenceMs() {
  uint32_t latest = 0;
  for (uint8_t i = 0; i < g_pwrTraceN; ++i) {
    if (!g_pwrTraceDown[i]) break;  // 連續性一斷就停：之後的 1 不能證明是同一次按壓
    latest = g_pwrTraceMs[i];
  }
  return latest;
}

// v294：早期證據的毫秒數（0 = 取樣從沒看到按著）與「這次是不是靠證據救回來的」。
static uint32_t g_pwrEarlyMs = 0;
static uint8_t g_pwrRescued = 0;

// v295：量 millis() 開始【之前】那一段 —— ROM ＋ 二階段 bootloader ＋ 6.4MB 映像的 SHA256 驗證。
// 它從來沒被量過，而 2026-09-19 由實機碼表反推約 600ms：**比 setup() 裡所有能動的部分
// 加起來還大**（delay(250) 已否決移除、逃生口 96ms 不可縮、硬體 init 約 150ms）。
// 要不要碰它是另一個層級的決定（跳過驗證＝拿掉「映像壞掉自動退回另一槽」那張網，而這台沒有
// USB 救磚），但**先量到才談得下去**。
//
// 機制：RTC 計數器在晶片上電就開始跑。這台在電池上「深度睡眠」＝整顆斷電（含 RTC 域），
// 所以電源鍵喚醒時它從 0 起算 → `RTC 毫秒 − millis()` ＝ millis() 起算前花掉的時間。
// ⚠️ 只對 rst=1（POWERON）成立；`ESP.restart()` 不重置 RTC 域，rst=3 時這個值是
//    「自上次真正上電以來」，不是 bootloader 時間 —— 判讀時務必配 rst= 看。
// 兩個時鐘同步前進，所以在哪裡取樣都得到同一個差值；故意放在逃生口【之後】，不碰那一段。
// ⚠️ 複查要求：不要手寫 extern "C" 宣告（ABI 無法自證）→ 用 SDK 正式表頭。
// ⚠️ 算術全程 uint64，只在輸出時截斷：rst=3 時 RTC 可能累積很久，先截成 uint32 再相減
//    會在任一時鐘回繞時給出錯值（複查指出，成立）。
static uint32_t g_preAppMs = 0;

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

  // v295：bootloader 時間（見上方 g_preAppMs 的完整說明）。一次 RTC 讀取。
  {
    const uint64_t rtcMs = esp_rtc_get_time_us() / 1000ULL;  // 全程 64-bit，最後才截斷
    const uint64_t nowMs = static_cast<uint64_t>(millis());
    g_preAppMs = (rtcMs > nowMs) ? static_cast<uint32_t>(rtcMs - nowMs) : 0u;
  }

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

  // ⭐ v324：電源鍵腳位的快照 → gpio.begin()（InputManager 會 pinMode）→ 無條件重設 → 再快照。見 snapPwrPad 的說明。
  //   begin() 之前 BoardConfig 還沒選定，先照 X3 的 GPIO3 拍；begin() 之後若 ACTIVE.input.power 不是 3 就重拍。
  g_pwrPadPre = snapPwrPad(3);
  gpio.begin();
  g_pwrPadPin = BoardConfig::ACTIVE.input.power;
  if (g_pwrPadPin != 3) g_pwrPadPre = snapPwrPad(g_pwrPadPin);
  resetPwrPad(g_pwrPadPin);
  g_pwrPadPost = snapPwrPad(g_pwrPadPin);
  // v199 取樣 1：`gpio.begin()` 的最後一行就是 `inputMgr.begin()`，所以它一返回就讀得到按鍵——
  // 這是【逃生口之後】最早的可能時刻。v198 把取樣放在下面三個 I2C 裝置之後，量到約 500ms，
  // 而四筆軌跡裡有兩筆是「500ms 就已放開」——看不到就修不了。這一版要問的是：
  // 那 500ms 有多少是這三個 I2C 花掉的？若能提早兩三百毫秒，可救回的短按就變多。
  tracePower();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  tracePower();  // v199 取樣 2：＝ v198 的取樣 1 位置（約 500ms），保留以便直接對照

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
  // v324 證人：電源鍵腳位在 gpio.begin() 之前／重設之後的狀態（rst=3 的軟體重啟會把 RTC 域的殘留帶進來，這裡看得到）。
  logPwrPad("boot-pre", g_pwrPadPin, g_pwrPadPre);
  logPwrPad("boot-post", g_pwrPadPin, g_pwrPadPost);
  {
    char e[8][8];
    for (int i = 0; i < 8; i++) {
      if (g_pwrPadErr[i] == PWRPAD_NA) snprintf(e[i], sizeof(e[i]), "na");
      else snprintf(e[i], sizeof(e[i]), "%d", g_pwrPadErr[i]);
    }
    DiagLog::line("PWRPAD reset hold=%s slpsel=%s dsw=%s wake=%s deinit=%s rst=%s intr=%s ena=%s", e[0], e[1], e[2],
                  e[3], e[4], e[5], e[6], e[7]);
  }
  // v283：JSON 存檔的分段計時；lib/Serialization 看不到 DiagLog（同 vertDiagHook 的理由）。
  //   裝在 DiagLog::begin() 之後、任何 loadFromFile() 之前 —— 載入時若觸發改版重存也要量到。
  PersistableStoreBase::diagHook = [](const char* line) { DiagLog::line("%s", line); };
  DiagLog::mem("boot");
  BenchFlags::load();  // v185 bench 哨兵（同樣只在這裡讀一次 SD）

  // v196：BENCH→RESUME 黑盒補證人（純觀測，不改順序／行為）。
  HalSystem::checkPanic();
  tracePower();  // v199 取樣 3（＝ v198 的取樣 2）
  g_wakeT[0] = millis();

  SETTINGS.loadFromFile();
  // v187：粗體閱讀是 ParsedText 的全域旗標，開機就跟設定對齊（否則從設定頁進文字設定的預覽會用錯字重）。
  ParsedText::setBoldBodyText(SETTINGS.boldBodyText != 0);
  // 直排診斷：lib/Epub 看不到 DiagLog，所以在這裡接上（見 ParsedText.h 的註解）。
  ParsedText::vertDiagHook = [](const char* line) { DiagLog::line("%s", line); };
  g_wakeT[1] = millis();
  APP_STATE.load();  // v332：NVS 有效就用 NVS（0.9ms），否則 state.json
  g_wakeT[2] = millis();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  g_wakeT[3] = millis();

  bool wakeUsb = false;
  const auto wakeupReason = gpio.getWakeupReason(&wakeUsb);
  // v197：純 RAM，零 I/O。usb 取自 getWakeupReason() 內部那次 I2C 的結果，不重讀。
  g_wakeReason = static_cast<uint8_t>(wakeupReason);
  g_wakeUsb = wakeUsb ? 1 : 0;
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton: {
      LOG_DBG("MAIN", "Verifying power button press duration");
      // v294（見 powerHeldEvidenceMs() 上方的完整理由與複查結論）：
      // ⭐ 舊檢查【永遠先跑】，副作用與時序一個字都沒變；early evidence 只覆蓋它的 false。
      //    接受集合因此嚴格單調遞增 —— 結構上不可能拒絕今天會通過的按法。
      const uint16_t requiredMs = SETTINGS.getPowerButtonDuration();  // 一次讀值，避免兩處不一致
      // v295：證據先算（只讀既有的取樣陣列，零 I/O），再交給 verify 當「等待上限」的依據。
      // 它【不改判定】，只讓「手指已經放開」那種情況不必空等 1 秒。見 HalGPIO.cpp 的註解。
      g_pwrEarlyMs = powerHeldEvidenceMs();
      const bool earlyAccept = g_pwrEarlyMs >= requiredMs;
      // ⭐ v315（實機回報：「按到機器亮起來就該開，不用數秒」）：**電池供電下，app 能跑到這裡本身就是證據。**
      //    電池模式的電源是按鍵在供，直到 ~100ms 前的 holdPowerRails() 把 GPIO13 閂住；bootloader 約 800ms
      //    → 手指若在 ~0.9 秒前放開，晶片根本不會開機、也不會有任何 log。所以走到這一行 ＝ 已按 ≥ ~0.9s
      //    ＝ 政策 400ms 的兩倍。舊 verify 卻在 ~1.3s 才問「此刻還按著嗎」，沒看到就自己關機 ——
      //    v314 實機 11/32 夭折全是 `early=0`：使用者按夠了，是我們把機器關掉。
      //    插著電（usb=1）時晶片不靠按鍵供電、短碰也會醒 → 維持原判定。verify 照跑（副作用與哨兵不變，
      //    v294 的教訓），只是等待上限跟 early 一樣壓到 60ms、結果不再能導致關機。
      //    codex 複查（v315）：`!wakeUsb` 只代表「電量計此刻沒讀到充電電流」，插著電但電池已滿時也會是 0；
      //    那時晶片靠 USB 供電、真正在深睡（醒來 rst=DEEPSLEEP），短碰一下也會走到這裡 → 加一個條件：
      //    只有 **POWERON 重置** 才算「按鍵供電撐到閂鎖」。電池模式的每次喚醒都是 POWERON（真深睡只在 USB 上發生）。
      const bool latchedAccept = !wakeUsb && (esp_reset_reason() == ESP_RST_POWERON);
      const bool legacyAccept = gpio.verifyPowerButtonWakeup(
          requiredMs, SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP, &g_pwrDiag,
          earlyAccept || latchedAccept);
      g_pwrRescued = (!legacyAccept && earlyAccept) ? 1 : 0;
      // ⚠️ 不覆寫 g_pwrDiag.outcome —— verify 每次都跑過，那個哨兵維持原義（HalGPIO.h:121
      //    記著 v191 的撞號教訓）。「靠證據救回來」另外用 rescue= 表示。
      if (!legacyAccept && !earlyAccept && latchedAccept) {
        // v315 證人：只靠「電池閂鎖」這條救回來的開機 —— 以前這一筆會是 WAKE abort。
        DiagLog::line("WAKE accept why=latched usb=%u outcome=%u waited=%u early=%lu t=%lu trace=%s",
                      static_cast<unsigned>(g_wakeUsb), static_cast<unsigned>(g_pwrDiag.outcome),
                      static_cast<unsigned>(g_pwrDiag.waitedMs), static_cast<unsigned long>(g_pwrEarlyMs),
                      static_cast<unsigned long>(millis()), powerTraceStr());
      }
      if (!legacyAccept && !earlyAccept && !latchedAccept) {
        // 判定已經結束，這時候寫 SD 不會再影響它。outcome:1=等不到 isPressed 2=握持不足
        DiagLog::line(
            "WAKE abort why=verify usb=%u outcome=%u waited=%u held=%u req=%u cal=%u early=%lu t=%lu trace=%s",
            static_cast<unsigned>(g_wakeUsb), static_cast<unsigned>(g_pwrDiag.outcome),
            static_cast<unsigned>(g_pwrDiag.waitedMs), static_cast<unsigned>(g_pwrDiag.heldMs),
            static_cast<unsigned>(g_pwrDiag.requiredMs), static_cast<unsigned>(g_pwrDiag.calibratedMs),
            static_cast<unsigned long>(g_pwrEarlyMs), static_cast<unsigned long>(millis()), powerTraceStr());
        powerManager.startDeepSleep(gpio);
      }
      break;
    }
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      // v197：這條就是「充電時按電源鍵被判成是 USB 叫醒的」的嫌犯。
      DiagLog::line("WAKE abort why=usbpower usb=%u t=%lu", static_cast<unsigned>(g_wakeUsb),
                    static_cast<unsigned long>(millis()));
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
  // ⭐ v311：這裡只【記時刻】，不再空等 500ms。等待與 UP 鍵判定搬到 setupDisplayAndFonts() 之後
  //   （見下方 v311 區塊）—— 顯示初始化約 270ms 本來就要做，拿它填掉大半個 settle 窗，
  //   實機每次電源鍵開機省約 270ms（v310 log：wakeup→settle 恆為 506ms，純等待）。
  //   ⚠️ 語意不變：自 wakeup 檢查起仍保證 ≥500ms、期間仍持續餵 gpio.update()，
  //     UP+POWER 救援組合鍵照樣在路由前判定。
  //   ⚠️ v295 說這個迴圈「承重」（餵去彈跳給 waitForPowerRelease）—— v310 起 waitForPowerRelease()
  //     改看原始電平並自己提交，那個依賴已經消失；剩下的需求只有救援鍵的 isPressed(UP)，
  //     而它在搬過去之後仍有 ≥ 200ms、≥ 20 次 update() 可以穩定（去彈跳只要連續兩筆）。
  unsigned long settleStart = 0;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) settleStart = millis();
  g_wakeT[5] = millis();

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot ? BootResume::Silent : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  bool logoSkippedForHome = false;  // v312：WAKE splash 證人用（skipped=2）
  // v311：「會不會回閱讀器」拆成兩段。這裡是【暫定】版（不含救援鍵——它要等 settle 之後才可靠），
  //   只拿來決定顯示初始化時要不要順便載 SD 內文字型：首頁根本用不到它（v310 log：FONTLOAD
  //   why=boot 在首頁醒來也花 267ms）。萬一暫定為真、最後卻進救援模式，只是多載了一次字型，無害。
  //   正式的 willResumeToReader 在 settle 之後算（見下方 v311 區塊）。
  const bool shouldPreloadReaderFont = !HalSystem::isRebootFromPanic() && !APP_STATE.openEpubPath.empty() &&
                                 APP_STATE.lastSleepFromReader && APP_STATE.readerActivityLoadCount == 0;
  // v185 證人：0=Splash 1=Silent（2=QuickResume 已於 v330 移除）；配上 BOOT 行的 rst= 就能分類每次開機。
  // 前綴刻意不用 BOOT —— 那是 diag.log 的版本分段記號，多一行就把每段切成兩半。
  DiagLog::line("RESUME kind=%d target=%u", static_cast<int>(resume), static_cast<unsigned>(snapshotTarget));

  // v193：醒來路徑證人。完全沒有新的 BOOT 行＝面板凍住、韌體還活著。
  const bool seamlessDisplay = resume != BootResume::Splash;
  DiagLog::mem("wake-disp");
  DiagLog::line("WAKE disp-begin seamless=%d font=%d", seamlessDisplay ? 1 : 0, shouldPreloadReaderFont ? 1 : 0);
  setupDisplayAndFonts(seamlessDisplay, /*loadReaderFont=*/shouldPreloadReaderFont);
  // v185 bench：驅動選好之後才套灰階推力候選；文字 AA 深灰旋鈕落在 renderer。
  if (BenchFlags::grayVariant != 0) display.setGrayscaleVariant(BenchFlags::grayVariant);
  renderer.setTextAaDarkOnly(BenchFlags::aaDark);

  // ⭐ v311：救援組合鍵的 settle 窗搬到這裡 —— 顯示初始化（約 270ms）已經填掉大半，
  //   只補到「自 wakeup 檢查起滿 500ms」為止，期間照樣每 10ms 餵一次 gpio.update()。
  //   語意與 v310 以前完全相同（同樣的等待總長、同樣在路由前判定 UP+POWER），差別只是
  //   那段時間裡做了有用的事。實機每次電源鍵開機預期省約 270ms。
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // ⛔ codex 複查（v311 第一輪）抓到的洞：顯示初始化若碰上慢卡、字型多、或本來就要載字型而
    //    超過 500ms，下面的補等迴圈會【一次都不跑】→ isPressed(UP) 只剩初始化前的舊狀態可看
    //    → 唯一的 SD 救援入口可能漏判。去彈跳要連續兩筆才提交，所以硬性保證：
    //    滿 500ms【且】初始化之後至少 5 筆新樣本（≥50ms）。兩個條件都要成立才算 settle 完。
    // ⭐ v314：地板 500 → 250ms。500 是上游的註解「isPressed 要半秒才穩」；v197 實機 31 次開機量到
    //    按著的鍵【一個輪詢週期（10ms）】就測到（waited=10–11 全部），半秒是猜的。真正保護救援鍵的是
    //    下面「初始化之後至少 5 筆新樣本」的硬保證（去彈跳連續兩筆即提交），不是時間地板。
    //    250 留著當「顯示初始化異常快」時的最低取樣時間；實測初始化 230 → 整段約 280ms（v313 是 518）。
    //    只改這一個常數；救援組合鍵的判定位置、樣本保證、證人都不動。
    //    codex 複查（v314）：唯一的風險是「面板初始化剛結束那幾十毫秒 ADC 讀值還沒回穩」——所以尾段
    //    最少樣本 5 → 10（≥100ms），並記 UP 在第幾筆被看到（upat=）：救援鍵測試若印 upat=1，餘裕足；
    //    若接近 10，就把 SETTLE_MIN_TAIL 調回去。
    constexpr unsigned long SETTLE_FLOOR_MS = 250;
    constexpr uint8_t SETTLE_MIN_TAIL = 10;
    g_settleInitMs = millis() - settleStart;  // 顯示初始化實際吃掉多少 settle 窗（證人）
    uint8_t tailSamples = 0;
    while (millis() - settleStart < SETTLE_FLOOR_MS || tailSamples < SETTLE_MIN_TAIL) {
      gpio.update();
      delay(10);
      if (tailSamples < 255) ++tailSamples;
      if (g_settleUpAt == 0 && gpio.isPressed(HalGPIO::BTN_UP)) g_settleUpAt = tailSamples;  // 證人
    }
    g_settleTailSamples = tailSamples;
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
    g_settleUp = recoveryFirmwareMode ? 1 : 0;
  }
  // v196：喚醒回閱讀器時可跳過開機動畫（省一次完整面板刷新）。條件只用此刻已確定的欄位；
  // 刻意不含 Back 鍵——該鍵狀態此時不一定可靠，少判只會落到主畫面，仍會設定 activity。
  // v311：正式版（含救援鍵）在 settle 之後才算；顯示初始化用的是上面的 shouldPreloadReaderFont。
  const bool willResumeToReader = !recoveryFirmwareMode && shouldPreloadReaderFont;

  // v331：v320.1 的 `wallcache.build` 哨兵已拿掉（A5 刻意不做）：桌布第一次被選到時本來就會自己建快取、而且
  //   先上面板才寫（SleepActivity::renderBitmapSleepScreen），原則 36 已滿足；哨兵每次開機還付一次 exists()。
  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
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
      if (!willResumeToReader) {
        // ⭐ v312：從睡眠醒來、要去首頁 → 不畫開機 logo（維護者拍板）。
        //   桌布本來就還在面板上（e-ink 斷電不掉畫面），logo 是多付一次完整刷新（v310 log ~850ms），
        //   而 begin(seamless=false) 已 requestResync → 首頁那次繪製本來就會被升級成清潔刷新，
        //   從桌布直接切到首頁一樣乾淨，不必碰面板 API。只在四個條件同時成立時跳過：
        //   - APP_STATE.deepSleepStamp：上次是 enterDeepSleep() 自願關的（電池上冷開機與喚醒 rst 都是 1，靠自己記）
        //   - 電源鍵喚醒（rst=3 的 Silent 路徑不會到這裡；刷完機亦然）
        //   - 非當機重開、非救援模式 —— 「機器明顯重開了一次」是誠實的回饋，那兩種照畫
        const bool skipLogo = APP_STATE.deepSleepStamp && wakeupReason == HalGPIO::WakeupReason::PowerButton &&
                              !HalSystem::isRebootFromPanic() && !recoveryFirmwareMode;
        if (skipLogo) {
          logoSkippedForHome = true;
          // ⛔ 沒有這一行等於白做（codex 複查指出）：驅動在 begin() 掛了「前兩次繪製強制 GC」的計數，
          //    有 logo 時由 logo＋首頁吃掉；沒 logo 時第二次會落到首頁之後的第一次操作 —— 那 ~770ms
          //    只是延後不是省掉。這裡把計數歸零、保留 begin() 已請求的一次性 resync：
          //    首頁那次仍是乾淨的 GC（從桌布切過去不留鬼影），之後回到正常刷新。
          display.defuseInitialFullSyncsKeepResync();
        } else {
          activityManager.goToBoot();
        }
        Storage.remove(wakeFrameFile());  // 這次用不到 → 清掉，不留給下次
        break;
      }
      // v293：跳過開機動畫的那條路上，先把睡前那一頁打回面板。
      //   ⭐ 這【不是】過場：它就是接下來要顯示的同一張畫面，所以不多付刷新，只是提早。
      //   之後的字型載入、開書、排版、預熱都在「畫面已經對了」的背後跑。
      //   ⚠️ 用清潔刷新（begin() 對電源鍵喚醒已 requestResync）—— 桌布是全色調畫作，
      //      差分刷新會把畫的鬼影透到書頁上。
      {
        size_t frameBytes = 0;
        const bool frameOk = loadWakeFrameBuffer(&frameBytes);
        DiagLog::line("WAKEFRAME ok=%d bytes=%u", frameOk ? 1 : 0, static_cast<unsigned>(frameBytes));
        if (frameOk) {
          // ⚠️ 照抄 QuickResume 同一個動作用的檔位（HALF），不要自己挑 FULL ——
          //   FULL 是 124 幀的最慢波形，而 `begin()` 對電源鍵喚醒已經 requestResync()，
          //   驅動會把這一次升級成清潔刷新，乾淨度不是由這個參數決定的。
          // ⭐ v318（實機回報：「回書之後那次刷新只有時間變了，能不能不要全刷？」）：
          //   下面的 allowFastInitialReaderRefresh 從 v293 起就請求 FAST，但【從沒生效】——驅動開機掛的
          //   「前兩次繪製強制 GC」預算，wake frame 吃一次、閱讀器那次剛好是第二次，被硬升級成全刷
          //   （實機一直是 display=748ms bank=1；bank 證人 v312 才有，之前看不見）。
          //   這裡先把預算歸零（defuseInitialFulls：舊平面仍視為無效 → 這次照樣先填白再 GC，乾淨度不變），
          //   閱讀器的重畫就成為對這張畫面的差分（DU）：只驅動有變的像素（狀態列的時間／電量），其餘不閃。
          //   只在 frameOk 時做：讀不到 frame 而退回 logo 的路，保留上游「logo＋下一畫面各一次 GC」的設計。
          display.defuseInitialFullSyncsKeepResync();
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
          // 面板上現在就是那一頁 → 閱讀器稍後那次繪製與它幾乎相同，可以降成 FAST
          //（443ms 而不是 815ms）。QuickResume 用的是同一個旗標，不是新機制。
          allowFastInitialReaderRefresh = true;
          DiagLog::line("WAKEFRAME painted");
        }
      }
      break;
  }

  // v196：證人——兩種情況都印，對照 willResumeToReader 與實際有無畫 logo。
  // v312：logo 0=畫了 1=回書跳過（v196） 2=回首頁跳過（v312 印記） 3=不適用（Silent 本來就不畫）
  //   ⚠️ 印記 clean=1 只代表「自上次存檔以來曾自願睡過」，不能證明這次是睡眠喚醒（硬體分不出來，
  //   codex 指出）—— 所以同列 reason／resume／panic／recov，才能在 log 上把每種情況分開。
  DiagLog::line("WAKE splash logo=%d stamp=%d reason=%u resume=%d panic=%d recov=%d",
                resume != BootResume::Splash ? 3 : (willResumeToReader ? 1 : (logoSkippedForHome ? 2 : 0)),
                APP_STATE.deepSleepStamp ? 1 : 0, static_cast<unsigned>(wakeupReason), static_cast<int>(resume),
                HalSystem::isRebootFromPanic() ? 1 : 0, recoveryFirmwareMode ? 1 : 0);
  // v196（複查）：開機前段的時間戳集中在這裡印 —— 此時電源鍵驗證與 recovery 判定都已經過去。
  // v197：reason 0=PowerButton 1=AfterFlash 2=AfterUSBPower 3=Other
  //       verify 255=沒執行（reason 不是 PowerButton）0=通過 1=等不到 2=握持不足 3=快速路徑
  //       v294：early=連續前綴裡最後一次看到按鍵還按著的時刻（ms），0=取樣從沒看到按著；
  //             rescue=1 表示舊檢查判失敗、而這次是靠 early 證據救回來的（＝這一版的價值）
  DiagLog::line(
      "WAKE why reason=%u usb=%u verify=%u waited=%u held=%u req=%u cal=%u early=%lu rescue=%u trace=%s",
                static_cast<unsigned>(g_wakeReason), static_cast<unsigned>(g_wakeUsb),
                static_cast<unsigned>(g_pwrDiag.outcome), static_cast<unsigned>(g_pwrDiag.waitedMs),
                static_cast<unsigned>(g_pwrDiag.heldMs), static_cast<unsigned>(g_pwrDiag.requiredMs),
                static_cast<unsigned>(g_pwrDiag.calibratedMs), static_cast<unsigned long>(g_pwrEarlyMs),
                static_cast<unsigned>(g_pwrRescued), powerTraceStr());
  // v295：preapp = millis() 起算【之前】花掉的毫秒（ROM＋bootloader＋映像驗證）。
  //       ⚠️ 只對 rst=1（POWERON）成立，rst=3 是「自上次真正上電以來」，判讀要配 BOOT 的 rst=。
  DiagLog::line("WAKE preapp=%lu rst=%d", static_cast<unsigned long>(g_preAppMs),
                static_cast<int>(esp_reset_reason()));
  // v311：settle 窗被顯示初始化吃掉多少（init）、之後補了幾筆樣本（tail，硬保證 ≥5）、UP 最終判定。
  DiagLog::line("WAKE settle init=%lu tail=%u up=%u upat=%u", static_cast<unsigned long>(g_settleInitMs),
                static_cast<unsigned>(g_settleTailSamples), static_cast<unsigned>(g_settleUp),
                static_cast<unsigned>(g_settleUpAt));
  DiagLog::line("WAKE steps panic=%lu settings=%lu appstate=%lu stores=%lu wakeup=%lu settle=%lu",
                static_cast<unsigned long>(g_wakeT[0]), static_cast<unsigned long>(g_wakeT[1]),
                static_cast<unsigned long>(g_wakeT[2]), static_cast<unsigned long>(g_wakeT[3]),
                static_cast<unsigned long>(g_wakeT[4]), static_cast<unsigned long>(g_wakeT[5]));
  // v332：開機證人 —— state 從哪來（1 nvs／2 sd／0 都失敗）、NVS 裡的閱讀位置、分割區用量、讀回花的時間。
  //   救援模式不跑（不在救援畫面前面多放 SD append）。
  if (!recoveryFirmwareMode) {
    const int64_t nvsT0 = esp_timer_get_time();
    NvsStore::ProgBlob pb{};
    int perr = 0;
    const bool pok = NvsStore::readProg(&pb, &perr);
    uint32_t used = 0, freeE = 0, avail = 0, total = 0, ns = 0;
    const bool uok = NvsStore::usage(&used, &freeE, &avail, &total, &ns);
    const uint32_t nvsUs = static_cast<uint32_t>(esp_timer_get_time() - nvsT0);
    DiagLog::line("STATE src=%u nonce=%lu token=%lu stamp=%d rd=%d loads=%u pathlen=%u | prog ok=%d err=%d kind=%u seq=%lu spine=%u "
                  "page=%u/%u off=%lu | usage ok=%d used=%lu free=%lu avail=%lu total=%lu ns=%lu | us=%lu",
                  static_cast<unsigned>(APP_STATE.lastLoadSource()), static_cast<unsigned long>(APP_STATE.sdNonce()),
                  static_cast<unsigned long>(APP_STATE.wakeFrameToken),
                  APP_STATE.deepSleepStamp ? 1 : 0, APP_STATE.lastSleepFromReader ? 1 : 0,
                  static_cast<unsigned>(APP_STATE.readerActivityLoadCount),
                  static_cast<unsigned>(APP_STATE.openEpubPath.size()), pok ? 1 : 0, perr,
                  static_cast<unsigned>(pb.kind), static_cast<unsigned long>(pb.seq), static_cast<unsigned>(pb.spine),
                  static_cast<unsigned>(pb.page), static_cast<unsigned>(pb.pageCount), static_cast<unsigned long>(pb.offset),
                  uok ? 1 : 0, static_cast<unsigned long>(used), static_cast<unsigned long>(freeE),
                  static_cast<unsigned long>(avail), static_cast<unsigned long>(total), static_cast<unsigned long>(ns),
                  static_cast<unsigned long>(nvsUs));
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
    // 防呆的存檔：讓下一次開機知道「上次有試著開書」。v282 量到它在喚醒路徑上要 ~1.15 秒
    //   （不是 v280 報的 350ms —— 那是另一本書的幸運視窗），而內容只有一百多個位元組。
    //   v283 起這條路上的分段成本由 PERSISTW 那行 log 給出。
    APP_STATE.save();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  // ⭐ v316：首頁醒來的第一次繪製要在 waitForPowerRelease() 【之前】完成。首頁的 render 由
  //   ActivityManager::loop() 觸發，而 loop() 要等 setup() 結束才跑 —— setup() 的最後一步卻是
  //   等你放開電源鍵。結果：按著不放，畫面就停在桌布；一放開首頁才出來（v315 實機：home@5,706／4,287
  //   vs 正常 2,650）。讀書醒來沒有這個問題，因為 wake frame 是在 setup() 裡同步貼上去的。
  //   做法與 Silent 路徑相同（requestUpdateAndWait 已在那條路實機跑過幾百次）。
  //   ⛔ codex 複查（v316）：replaceActivity() 只在【沒有現行 activity】時同步 onEnter，否則延到 loop()。
  //      有畫 logo 的開機（Boot activity 在場）goHome() 會被延後，這裡等到的不是首頁 → 條件收緊為
  //      logoSkippedForHome（那條路 currentActivity 必為空，goHome 同步進場，等到的一定是首頁的第一次繪製）。
  //      其他路（有 logo、救援、當機、回書）維持原本的 loop() 觸發。
  const bool paintBeforeRelease = resume == BootResume::Silent || logoSkippedForHome;
  if (paintBeforeRelease) {
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
  // v304：閒置計時器從「離開 setup()」起算 —— 與它還是 loop() 裡的 static 時等價
  //（那時它在第一次進 loop() 才初始化）。少了這行會從 millis()=0 起算，差幾秒，但沒理由不對齊。
  g_lastActivityTime = millis();
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
  // v304：從 loop() 的 static 提到檔案範圍 —— 淺睡眠續讀時必須重置它（見 g_lastActivityTime）。
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    g_lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // v318 證人：電源鍵在 loop() 眼中到底長什麼樣。v317 實機（diag317）：297 秒的開機段裡使用者按電源鍵
  //   要休眠，log 上連一行 SLPGATE 都沒有 —— 下面的關卡根本沒看到按鍵。關卡之前只有兩條路會讓它看不到：
  //   ① 原始電平沒讀到按下（raw=0）② 被截圖組合鍵那段提早 return 吃掉（combo>0，DOWN 被誤讀）。
  //   放在所有 early return 之前；只在「有看到東西」時每 3 秒印一行，平常零輸出。
  {
    static uint16_t pkRaw = 0, pkIp = 0, pkCombo = 0;
    static unsigned long pkLast = 0;
    const bool pkIpNow = gpio.isPressed(HalGPIO::BTN_POWER);
    if (gpio.powerDownRaw()) pkRaw++;
    if (pkIpNow) pkIp++;
    if (pkIpNow && gpio.isPressed(HalGPIO::BTN_DOWN)) pkCombo++;
    const unsigned long pkNow = millis();
    // v324：這次開機 5 分鐘內電源鍵從沒被看到過 → 把腳位狀態記一次（黑洞發生在開機之後也抓得到）。
    static bool pkSeenEver = false, pkAlerted = false;
    if (pkRaw || pkIp) pkSeenEver = true;
    if (!pkSeenEver && !pkAlerted && pkNow > 300000UL) {
      pkAlerted = true;
      logPwrPad("5min-unseen", g_pwrPadPin, snapPwrPad(g_pwrPadPin));
    }
    if ((pkRaw || pkIp) && pkNow - pkLast > 3000) {
      DiagLog::line("PWRKEY raw=%u ip=%u combo=%u stable=%d allow=%d", static_cast<unsigned>(pkRaw),
                    static_cast<unsigned>(pkIp), static_cast<unsigned>(pkCombo), g_powerKey.stable ? 1 : 0,
                    pkNow >= allowSleepAt ? 1 : 0);
      pkRaw = pkIp = pkCombo = 0;
      pkLast = pkNow;
    }
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
  if (sleepTimeoutMs > 0 && millis() - g_lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // ⭐⭐ v308 證人：「按電源鍵沒反應」實機重現過兩次，而 log 上**連一行 SLEEP 都沒有** ——
  //    代表按下去根本沒進到 enterDeepSleep()，卡在下面這道關卡，但看不出卡在哪一項。
  //    這裡只在「電源鍵確實被按著、卻沒能休眠」時記一行（正常使用不會觸發），
  //    而且每 2 秒最多一行，不會洗版。四個欄位剛好對應四個可能的阻擋原因。
  // ⭐⭐ v309：**自己記電源鍵按下的時刻。**
  //   v308 的證人把「按電源鍵沒反應」釘死了：
  //     SLPGATE blocked allow=1 held=6 req=400 downcombo=0 raw=1
  //   `raw=1`（原始電平確實按著）、`downcombo=0`、`allow=1`，**唯一擋住的是 held**——
  //   `getPowerButtonHeldTime()` 永遠只有 1–6 毫秒，每一圈都被重置，累積不到 400。
  //   ⭐ 這跟 v291／v294 是同一類的病：**用一個不屬於這顆鍵的計時基準判斷它按了多久**
  //     （memory `x3-opds-nav-gotchas` 記過：正解是記該鍵自己的按下時間戳）。
  //   `isPressed(BTN_POWER)` 本身是可靠的（證人的外層條件就是它），所以用它自己計時，
  //   **零額外成本**（不多讀一次 ADC）。
  //   ⚠️ 純加法：`|| ownHeld > …`，只可能接受更多，不可能拒絕今天會過的按法。
  // v310：電源鍵長按只問 g_powerKey（原始電平＋自己的去彈跳）。v309 的 pwrDownSince 踩在
  //   會翻轉的 isPressed 上（實機 own=0），已由 PowerKeyTracker 取代。
  const unsigned long nowMs = millis();
  g_powerKey.poll(nowMs);
  const bool pwrDownNow = g_powerKey.stable;
  const unsigned long ownHeld = g_powerKey.heldMs(nowMs);

  {
    static unsigned long lastSleepGateLog = 0;
    if (pwrDownNow && nowMs - lastSleepGateLog > 2000) {
      const unsigned long held = gpio.getPowerButtonHeldTime();
      const bool allowed = nowMs >= allowSleepAt;
      const bool downCombo = gpio.isPressed(HalGPIO::BTN_DOWN);
      if (!allowed || (held <= SETTINGS.getPowerButtonDuration() && ownHeld <= SETTINGS.getPowerButtonDuration()) ||
          downCombo) {
        lastSleepGateLog = nowMs;
        // held=InputManager 的（預期仍會歸零）／own=追蹤器的（預期會累積）／ip=isPressed 此刻
        DiagLog::line("SLPGATE blocked allow=%d held=%lu own=%lu req=%u downcombo=%d raw=%d ip=%d",
                      allowed ? 1 : 0, static_cast<unsigned long>(held), static_cast<unsigned long>(ownHeld),
                      static_cast<unsigned>(SETTINGS.getPowerButtonDuration()), downCombo ? 1 : 0,
                      gpio.powerDownRaw() ? 1 : 0, gpio.isPressed(HalGPIO::BTN_POWER) ? 1 : 0);
      }
    }
  }

  // 保留與 InputManager 計時的 OR：純加法，不可能拒絕今天會過的按法。它翻轉時永遠到不了 400，
  // 不翻轉時與追蹤器一致 —— 留著沒有代價，拿掉是下一步的清理，不在這一版。
  if (nowMs >= allowSleepAt && pwrDownNow &&
      (ownHeld > SETTINGS.getPowerButtonDuration() ||
       gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration())) {
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
    if (millis() - g_lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
