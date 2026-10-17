#pragma once

// 巨集而不是 constexpr 陣列：要跟 CROSSPOINT_VERSION 在編譯期接成一個字串常數。
// 每一版韌體的代號（介紹一樣台灣的東西），跟開機／休眠 logo 的黑熊造型成套，
// 同一版的 Beta 與正式版用同一個代號。接在版號後面顯示：開機畫面、設定頁標題。
// 字要在內建 UI 字型裡（fonts-src/charsets/charset-ui-v4.txt），否則會變方塊。
#define RELEASE_CODENAME "珍奶"
