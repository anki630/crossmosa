#pragma once

// v249：跨 task 的一行麵包屑（lib 記、src 讀 —— lib 不能依賴 src 的 DiagLog）。
//
// 寫入者多半是 render task（HalStorage 的可能是任何 task），讀取者是各 activity 的 loop（印進 diag.log）。
// 原本寫入者直接 snprintf 進共用緩衝、讀取者看 buf[0] != '\0' 就讀 —— 跨 task 的普通 char 讀寫是資料競爭，
// 讀取者可能讀到寫一半、或還接著上一行尾巴的內容。「本地組好、最後才寫 [0]」也不夠：那不是同步點，
// 編譯器可以重排寫入（codex 複查 v249）。
//
// 所以寫入與讀走都在同一個臨界區內做：只複製（格式化在外面），最長 300 bytes，單核上是關中斷幾微秒。
// 語意不變：先到先得（還沒被讀走就丟掉新的一行）；讀走＝清空、交回給寫入者。不可在 ISR 呼叫。
#include <freertos/FreeRTOS.h>

#include <cstddef>
#include <cstring>

inline portMUX_TYPE g_breadcrumbMux = portMUX_INITIALIZER_UNLOCKED;

// 還有一行沒被讀走？（寫入者用來省掉格式化；結果只是提示，publish 內會再確認一次。）
inline bool breadcrumbPending(const char* buf) {
  portENTER_CRITICAL(&g_breadcrumbMux);
  const bool pending = buf[0] != '\0';
  portEXIT_CRITICAL(&g_breadcrumbMux);
  return pending;
}

// 放出一行（超過 cap−1 截斷）。緩衝裡還有沒被讀走的就丟掉這一行。
inline void breadcrumbPublish(char* buf, const size_t cap, const char* line) {
  if (cap == 0 || line == nullptr || line[0] == '\0') return;
  const size_t len = strnlen(line, cap - 1);
  portENTER_CRITICAL(&g_breadcrumbMux);
  if (buf[0] == '\0') {
    memcpy(buf, line, len);
    buf[len] = '\0';
  }
  portEXIT_CRITICAL(&g_breadcrumbMux);
}

// 讀走一行：有就複製到 out（超過 outCap−1 截斷）並清空緩衝，回 true。
inline bool breadcrumbTake(char* buf, const size_t cap, char* out, const size_t outCap) {
  if (cap == 0 || outCap == 0) return false;
  bool got = false;
  portENTER_CRITICAL(&g_breadcrumbMux);
  if (buf[0] != '\0') {
    size_t len = strnlen(buf, cap);
    if (len > outCap - 1) len = outCap - 1;
    memcpy(out, buf, len);
    out[len] = '\0';
    buf[0] = '\0';
    got = true;
  }
  portEXIT_CRITICAL(&g_breadcrumbMux);
  return got;
}
