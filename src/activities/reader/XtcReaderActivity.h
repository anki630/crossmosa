/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <string>
#include <utility>

#include "EndOfBookOptions.h"
#include "activities/Activity.h"

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  // 閱讀統計（2026-10-07）：這一次 render 正在畫的頁（render 開頭捕捉）。狀態列與 renderPage 用它，不回讀 currentPage
  //   （主任務會不持鎖改 currentPage；codex 第一、三輪）。只在 render 任務、持 RenderLock 時讀寫。
  uint32_t drawnPage_ = 0;

  int pagesUntilFullRefresh = 0;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  // 回傳這一頁有沒有真的畫出來（配置／讀取失敗畫的是錯誤畫面 → false）。page＝render 開頭捕捉的頁碼。
  bool renderPage(uint32_t page);
  // Opens chapter selection when the book has chapters (short-press Confirm); no-op otherwise
  void openChapterSelection();
  void renderStatusBarOverlay(StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  void saveProgress(uint32_t page) const;
  void loadProgress();

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc,
                             int initialRefreshCountdown)
      : Activity("XtcReader", renderer, mappedInput),
        xtc(std::move(xtc)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = 1;
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
};
