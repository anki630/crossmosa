#pragma once

#include <stdint.h>

// v185 bench 旗標 —— SD 卡【根目錄】的空檔，開機時讀一次（同 /diag.on 的慣例：根目錄才
// 傳得進去，資料目錄 /.crossmosa 被 ProtectedPath 擋住；名字要 8.3 安全）。全部預設關閉，
// 沒放檔就是正式行為。每一個只切一個變數，一輪只開一個，才歸因得出來。
//
//   /gray.1 … /gray.4   UC8279 灰階推力候選（見 Uc8279X3Luts.h 的 kUc8279X3_XtfAaVariants）
//                       無檔 = 0 = 原廠對映修正版（v185 正式行為）
//   /scrub.on           閱讀器週期清殘影改走 Mid scrub（v55 的 HALF_REFRESH_SCRUB，不做 GC 閃黑）
//   /wall4.on           待機壁紙改走 XTH4 絕對四階波形（不先畫 B/W 底）
//   /aadark.on          文字 AA 的淺灰像素降為深灰（字不隨淺灰變淡）
//
// 開機會記一行 `BENCH gray=… scrub=… wall4=… aadark=…` 進 diag.log，log 才分得出哪個
// 候選在場。候選定案後這個檔整個拔掉（bench 完就不該留旋鈕）。
namespace BenchFlags {
extern uint8_t grayVariant;
extern bool scrub;
extern bool wall4;
extern bool aaDark;

// Storage.begin() 成功、DiagLog::begin() 之後呼叫一次。
void load();
}  // namespace BenchFlags
