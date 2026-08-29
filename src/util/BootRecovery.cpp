#include "BootRecovery.h"

#include <Arduino.h>
#include <InputManager.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "network/OtaBootSwitch.h"

namespace boot_recovery {
namespace {

// Back + Up。實體位置經持機確認（見專案的按鍵位置表）：
//   BTN_BACK = 正面下緣【最左邊】那顆「返回」（ADC 梯 1 / GPIO1）
//   BTN_UP   = 【左側邊】那顆「上一頁」（ADC 梯 2 / GPIO2）—— 既有救援模式按的也是它
// ⚠️ BTN_UP 不是 UI 上「讓選取往上移」的鍵。寫任何按鍵指示之前先查那份位置表；
//    從程式碼或想像推位置已經害人白驗三輪。
//
// 這兩顆坐在【不同的】ADC ladder 上（Back=索引 0、Up=索引 4），
// 那是這種階梯唯一能同時回報的雙鍵組合 —— 共用同一支腳的組合（例如 Back+Right）
// 會塌成單一讀值。
//
// 走 InputManager 而不是自己寫 ADC 門檻，是為了沿用板子校準過的範圍。
// 不需要先跑 BoardConfig::selectDevice()：XTEINK_X3 與 XTEINK_X4 的 buttons 設定
// 逐位元組相同（兩者都是 {0,1,2,3,4,5,3,false}，2026-08-24 核對），
// 所以雙機種 binary 開機時預設生效的 X4 profile 也讀得對。這才讓它能當 setup() 第一行。
// 偵測窗口：16 次輪詢 × 6ms ≈ 96ms。**這個值是對的，不要再去加大它。**
//
// ⚠️⚠️ 【為什麼不可以把窗口拉長 —— 這是一個實際發生過的 blocker，2026-08-25】
//   main.cpp 的 verifyPowerButtonWakeup() 失敗會直接 startDeepSleep()，
//   而它問的【不是】「電源鍵按了多久」，是 HalGPIO.cpp 的【此刻電源鍵還按著嗎】：
//   等最多 1 秒，還沒按著就 return false。
//   （shortPwrBtn 預設 IGNORE 不是 SLEEP，所以那條 shortPressAllowed 快速路徑
//     【不會】生效，慢路徑一定走。）
//   -> 在本函式多停留 N 毫秒，就是把那個檢查往後推 N 毫秒。停留數秒的後果是
//      【機器自己進深度睡眠】，看起來就像「按了沒反應」。
//   -> 同一條路也擋著既有的 UP＋POWER SD 救援模式（它在那個檢查【之後】才判定），
//      **而那是這台唯一的刷機與救磚途徑。**
//
// ⚠️ 【曾經改錯又改回來，別再走一次】v136 一度把窗口拉到 1.2–3 秒，理由是使用者
//   回報「時間抓不準、要試很多次」。那個診斷是**從症狀反推**的，而且是錯的：
//   照「開機前就按住」的程序，96ms 在第 6 個樣本就會成立，結構上不可能是失敗原因。
//   真正有效的是**操作指示**（電源鍵也要按著、三顆一起放開）。
//   單變數實測（2026-08-25，實機）：**在【未改程式】的 v135 上用新指示，也是 1 次就中。**
//   -> 窗口回到 96ms。memory `user-skill-is-a-hidden-variable`。
//
// ⚠️ 使用者在這個階段是【全盲】的 —— 面板要到 setupDisplayAndFonts() 才亮，而本函式
//    在它之前一百多行。**任何以「畫面亮起」為信號的操作指示都是不可執行的。**
constexpr int kSettleSamples = 16;   // 去彈跳暖機期間最多輪詢幾次（約 96ms）
constexpr int kConfirmSamples = 5;   // 需要連續幾次成立（約 30 ms）
constexpr int kPollDelayMs = 6;
static_assert(kSettleSamples > kConfirmSamples, "去彈跳會吃掉第一次取樣，窗口必須有餘裕");

bool comboHeld(InputManager& input) {
  int consecutive = 0;
  for (int i = 0; i < kSettleSamples; ++i) {
    input.update();  // 套用去彈跳；currentState 會落後最初一兩次輪詢
    const bool held = input.isPressed(InputManager::BTN_BACK) && input.isPressed(InputManager::BTN_UP);
    consecutive = held ? consecutive + 1 : 0;
    if (consecutive >= kConfirmSamples) return true;
    delay(kPollDelayMs);
  }
  return false;
}
// 分割區開頭是不是一個看起來合理的 app 映像（magic 0xE9）。
// 排除被抹除（0xFF）或空的槽。
//
// ⚠️ 刻意【不用】esp_image_verify：這顆晶片上的 ESP-IDF 會用假的 efuse-blk-rev 錯誤
// 拒絕我們 patch 過的 firmware.bin（官方網頁 flasher 證明那些映像其實開得起來）。
// SD 韌體更新路徑基於同樣理由繞過它 —— 見 network/OtaBootSwitch.h。
bool hasApp(const esp_partition_t* p) {
  if (!p) return false;
  uint8_t magic = 0;
  return esp_partition_read(p, 0, &magic, sizeof(magic)) == ESP_OK && magic == 0xE9;
}

// 確認組合鍵之後、切換之前，等兩顆鍵【放開】。
//
// ⚠️ 這是一次性閘門，不是禮貌 —— 少了它這個機制會退化成擲硬幣。
// 上游把目標【寫死】成 ota_0，那是個不動點：切過去之後 running == hatch，
// 上游的 `running->address == hatch->address` 就成立並 return，所以「按一次最多切一次」。
// 我們把目標換成「另一個槽」之後，那一行在兩槽表下【恆為假】—— 防迴圈性質沒了。
// 兩槽都帶 hook 時，按住不放就是 A→B→A→B 無限來回，每圈約一秒、面板還沒起來、
// 零回饋，落點由你何時放手的奇偶決定；而且會撤銷一次已經成功的救援。
//
// 等放開就恢復「一次組合鍵 = 最多切一次」。
// 卡鍵或一直按著 -> 逾時放棄、照常開機（fail-closed，比切過去更安全）。
// ⚠️ 維持 5 秒，【不要放寬】。使用者指示是「按住約 2 秒後放開」（v134 實機驗證過的程序），
//    5 秒對它綽綽有餘。放寬只會拉長卡鍵時的無回饋停留，而那段停留正好壓在
//    verifyPowerButtonWakeup() 前面（見 comboHeld 上方的 blocker 說明）。
//    ⛔ 不得把指示改成「按住到畫面亮起」—— 面板在本函式執行期間【不可能】亮。
constexpr unsigned long kReleaseTimeoutMs = 5000;
constexpr int kReleasePollMs = 20;

bool waitForRelease(InputManager& input) {
  // millis() - start（不是 millis() < start + timeout）：後者在 millis() 接近 UINT32_MAX 時
  // 會回繞成「已逾時」。開機當下不可能發生，但與 main.cpp:344 的寫法保持一致。
  const unsigned long start = millis();
  while (millis() - start < kReleaseTimeoutMs) {
    input.update();
    if (!input.isPressed(InputManager::BTN_BACK) && !input.isPressed(InputManager::BTN_UP)) return true;
    delay(kReleasePollMs);
  }
  return false;  // 卡鍵：不切換
}

}  // namespace

// v194：麵包屑只寫 RTC slow memory。此時 DiagLog／SD／序列埠都還沒起來，
// 寫檔或 LOG_* 都會改時序或丟掉。兩個 store、零 delay。
constexpr uint32_t kBreadcrumbMagic = 0xB007C0B0;
RTC_NOINIT_ATTR uint32_t bootComboMagic;
RTC_NOINIT_ATTR uint32_t bootComboWhy;

enum BootComboWhy : uint32_t {
  WhyNoCombo = 1,
  WhyNoPrev = 2,
  WhySameSlot = 3,
  WhyNoApp = 4,
  WhyHeldStuck = 5,
  WhySwitchOk = 6,
  WhySwitchFail = 7,
};

void crumb(uint32_t why) {
  // 未讀走的「有意義」紀錄不要被下一輪正常開機的 nocombo 蓋掉
  // （尤其 switch-ok 之後會重開機，checkBootCombo 會先跑一次 nocombo）。
  if (why == WhyNoCombo && bootComboMagic == kBreadcrumbMagic && bootComboWhy != WhyNoCombo) {
    return;
  }
  // v194：先寫 why、再寫 magic。中途重置最多是「沒有紀錄」，不會變成「magic 有效但 why 是舊值」。
  bootComboWhy = why;
  bootComboMagic = kBreadcrumbMagic;
}

const char* peekBootComboBreadcrumb() {
  if (bootComboMagic != kBreadcrumbMagic) return nullptr;
  switch (bootComboWhy) {
    case WhyNoCombo:
      return "nocombo";
    case WhyNoPrev:
      return "noprev";
    case WhySameSlot:
      return "sameslot";
    case WhyNoApp:
      return "noapp";
    case WhyHeldStuck:
      return "heldstuck";
    case WhySwitchOk:
      // v194：switchok 在切槽之後由另一個槽的韌體開機。那份若不是 v194+ 就不會讀它；
      // 期間斷電 RTC_NOINIT 也會掉。只有之後又回到 v194+ 的槽、且期間沒有斷電時才讀得到，
      // 不是可靠證人。
      return "switchok";
    case WhySwitchFail:
      return "switchfail";
    default:
      return "unknown";
  }
}

void consumeBootComboBreadcrumb() {
  // v194：只在 DiagLog 確認寫進 diag.log 成功之後才清。寫失敗就把證據留在 RTC。
  bootComboMagic = 0;
}

void checkBootCombo() {
  // 沒按組合鍵就什麼都不做 —— 這是第一道、也是最重要的一道閘門。
  // 正常開機時本函式的成本 = 一次 InputManager::begin() 加最多 16 次 ADC 輪詢。
  InputManager input;
  input.begin();
  if (!comboHeld(input)) {
    crumb(WhyNoCombo);
    return;
  }

  // 另一個 app 槽。SD 韌體更新（FirmwareFlasher）寫的也是這一個，
  // 所以它裝的必定是【上一版】。
  const esp_partition_t* prev = esp_ota_get_next_update_partition(nullptr);
  if (!prev) {
    crumb(WhyNoPrev);
    return;
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running && running->address == prev->address) {
    crumb(WhySameSlot);
    return;  // 只有一個槽：無處可退
  }
  if (!hasApp(prev)) {
    crumb(WhyNoApp);
    return;  // 那個槽沒有可開機的映像
  }

  // 一次性閘門：等兩鍵放開，否則會 A→B→A 來回切（見 waitForRelease）。
  if (!waitForRelease(input)) {
    crumb(WhyHeldStuck);
    return;
  }

  // 複用 SD 韌體更新每次都在跑的同一支函式，不是新程式碼。
  if (ota_boot::switchTo(prev)) {
    crumb(WhySwitchOk);  // v194：見 peek 的 switchok 註解——不是可靠證人，但仍保留這一筆。
    delay(50);
    esp_restart();
  }
  // switchTo 失敗就照常開機 —— fail-closed。
  crumb(WhySwitchFail);
}

}  // namespace boot_recovery
