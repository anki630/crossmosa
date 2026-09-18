#pragma once

// 直排的 em 量測。**單獨一個檔**，因為它需要 `GfxRenderer`，而 `VerticalText.h`
// 必須維持在桌面編得動 —— `tools/vertical-oracle/cross_check` 直接編那個檔，
// 拿韌體的碼位表跟 Python 預言機逐項比對，那是直排三張表唯一的自動守門員。

#include <GfxRenderer.h>

#include <cstdint>

#include "VerticalText.h"

namespace vtext {

// ⭐⭐ **量一個全形字的寬度（em）—— 直排的一切都建立在這個數字上。**
//
// ⚠️⚠️ **不要直接用 `getCodepointAdvanceFP`。** 那個 API 只對【SD 卡字型、而且 advance
//    表已建立】有效，其餘一律回 `kAdvanceUnavailable`（-1），它的標頭自己寫著這條但書。
//    後果不是「量不準」而是**整章變空白**：em 變成 -0.0625 →
//    `consumeAllAndBail("no-em")` → 整段文字被吃掉，而且那個空白版面會被當成
//    **有效快取寫進 SD 卡**（重刷韌體也清不掉）。
//    ⚠️ 觸發條件是【預設狀態】：全新安裝沒有選 SD 字型（`sdFontFamilyName` 是空字串）
//      → 閱讀字型是內建的 → 打開任何 rtl 的書就整本空白。
//      實機從未遇到，只是因為維護者一直有選 SD 字型（複查抓到）。
//
// → 備援鏈：SD 的逐碼位快路徑 → `getTextAdvanceX`（**會走 fontMap，內建字型有效**）
//   → U+4E00「一」（任何中文字型都有）。回傳 12.4 定點；<= 16 表示量不到。
// v284：本體已移進 GfxRenderer（`probeEmFP`），因為**橫排的行距也要用同一個字身框**——
// 兩軸各留一份實作遲早會漂移。這裡保留薄包裝，讓既有呼叫點與桌面的 vertical-oracle 不必改。
inline int32_t probeEmFP(const GfxRenderer& renderer, const int fontId) { return renderer.probeEmFP(fontId); }

}  // namespace vtext
