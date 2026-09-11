#pragma once

#include <cstring>

// ⚠️ 32 → 64（維護者 2026-09-11 回報「顯示不完整」）。上游假設這裡放的是註腳【標記】
//    （「1」「[12]」「*」），32 bytes 綽綽有餘。但目錄頁的超連結也走同一條路，
//    而那裡的連結文字是整個章節標題 —— 32 bytes 只有 10 個漢字。
//    64 bytes ＝ 21 個漢字，而註腳清單一列放得下約 24 個（10px UI 字型、寬 480px）。
//    代價：section 檔每條註腳 +32 B（每頁最多 16 條），建置期 pendingFootnotes
//    每條 288 → 320 B（封頂 32 條 ＝ 峰值 +1 KB）。
#define FOOTNOTE_NUMBER_LEN 64
#define FOOTNOTE_HREF_LEN 256
// 上游 #2722：Calibre 產的 EPUB 檔名長、又經 URL 編碼，"Author-Title_split_NNN.html#_ftnN"
// 常超過 96 字元 → 截斷後註腳點下去找不到。存在 section 檔裡 → 跟著 SECTION_FILE_VERSION 跳號。

struct FootnoteEntry {
  char number[FOOTNOTE_NUMBER_LEN];
  char href[FOOTNOTE_HREF_LEN];

  FootnoteEntry() {
    number[0] = '\0';
    href[0] = '\0';
  }
};
