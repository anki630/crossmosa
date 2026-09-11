#pragma once
#include <GfxRenderer.h>

#include <cstddef>

#include "ScreenshotInfo.h"

class ScreenshotUtil {
 public:
  static void takeScreenshot(GfxRenderer& renderer);
  // ⚠️ `allowFallback` 是**遞迴閘**，呼叫端不要傳。建目錄失敗時本函式會改用根目錄的
  //    平坦檔名重試一次，而重試時必須關掉它 —— 否則失敗是持續性的（卡滿、唯讀、
  //    根目錄項數用盡）就會無限遞迴（複查抓到，六個 agent 從五個面向各自發現）。
  static bool saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height,
                                   bool allowFallback = true);

 private:
  static void buildFilename(const ScreenshotInfo& info, char* buf, size_t bufSize);
};
