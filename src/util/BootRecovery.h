#pragma once

// CrossMosa — 開機逃生口（Back + Up）
//
// 這台 X3 的 USB【只有充電、沒有資料線】：不能接電腦、沒有 esptool、沒有網頁 flasher。
// 而第二階段 bootloader 讀不到按鍵，它只會照 otadata 開機。所以「按住組合鍵退回上一版」
// 這件事，只能由【真的開起來的那份韌體】自己完成 —— 這就是那個檢查。
//
// 2026-08-17 我們因此永久失去一台機器：v131 在 setupDisplayAndFonts() 崩潰（堆積毀損 →
// 看門狗迴圈）。當時 SD 卡盲按救援試過、失敗 —— 因為 recoveryFirmwareMode 只在
// main.cpp:325 被 latch，真正跳進 SdFirmwareUpdateActivity 是在 391，而崩潰點在 353。
// 組合鍵有被記下來，程式卻撐不到用它的地方。
//
// 本檢查放在 setup() 的【第一行】，比那個崩潰點早 106 行。
//
// 與上游 SDK 的差異（freeink-sdk/libs/hardware/RecoveryBoot/，我們沒有連結它）：
//   上游把 ota_0 【寫死】成救援槽。但我們的 SD 韌體更新走 esp_ota_get_next_update_partition()，
//   兩槽嚴格交替 —— 跑在 ota_0 時上游那版就是 no-op，等於只有一半的時候有效。
//   我們改成跳到【另一個槽】＝【上一版】，兩個方向都有效。
//
// ⚠️ 【不要】為了「設錨點」而把同一版連刷兩次。兩槽是嚴格交替的，非執行槽永遠裝著
//    「你發起刷機時正在跑的那一版」—— 也就是說交替本身就已經給你「上一版」了，
//    連刷兩次在下游一格都改變不了。它唯一的效果是把兩槽變成同一版，
//    而那會讓逃生口變成 no-op（跳過去也是同一版），等於把異質退路換掉。
//
// ⚠️ 它擋不了什麼：死在本函式【之前】（ROM / SDK 早期初始化）、bootloader 或分割表壞掉、
//    兩個槽都是壞的。映像本身損毀不用擔心 —— bootloader 會自己退回另一槽。
//
// ⚠️ 它是由【正在崩潰的那份韌體】讀的，不是退路那份。所以要逃離某一版，
//    那一版自己就得帶著這段程式碼。v134 是第一個帶的。

namespace boot_recovery {

// 讀 Back + Up。按住就把 otadata 指向另一個 app 槽並重開機（不返回）。
// 其餘所有情況立即返回，不做任何事、不寫任何東西。
// 安全性：無條件呼叫、盡早呼叫都可以。
void checkBootCombo();

// v194：只看、不清。無可讀資料（冷開垃圾、或已經讀過）回 nullptr。只觀測，不影響逃生口行為。
const char* peekBootComboBreadcrumb();
// v194：DiagLog 確認寫進 diag.log 成功之後才呼叫。寫失敗就把證據留在 RTC。
void consumeBootComboBreadcrumb();

}  // namespace boot_recovery
