#pragma once

#include <cstring>

#define FOOTNOTE_NUMBER_LEN 32
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
