#pragma once

#include <cstddef>

// v196：裝置序號（eFuse USER_DATA 32 bytes）→ 寫進呼叫端提供的緩衝，讀不到就寫空字串並回傳 false。
// v196（複查）：**刻意不回傳 std::string** —— 這個函式會在記憶體吃緊時被呼叫（設定頁、網頁 status），
// 而 -fno-exceptions 下配置失敗＝abort。緩衝至少 33 bytes。
// 網頁 status API 與設定頁共用；**不要把序號寫進 diag.log**。
bool deviceSerial(char* out, size_t outLen);

// v196：面板控制器名稱（與開機 Hardware detect 同一判斷）。
const char* displayControllerName();
