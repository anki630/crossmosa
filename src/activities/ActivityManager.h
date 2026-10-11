#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "util/ScreenshotInfo.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration

enum class HomeMenuItem { NONE, FILE_BROWSER, RECENTS, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU };

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  unsigned long readerTransitionStartMs_ = 0;  // v280，見 goToReader
  enum class PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = PendingAction::None;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  std::atomic<bool> requestedUpdate{false};
  std::atomic<uint32_t> screenEpoch_{0};

 public:
  // v110/v161：投機工作（字型預取）的中止提示，不是正確性閘門。新樹以 requestedUpdate
  // 映射舊樹的 renderPending_：主任務 requestUpdate() 設起、render task 取件時清掉 ——
  // 預取跑在 render 尾端，期間有新請求進來即為 true。
  bool isRenderPending() const { return requestedUpdate.load(); }
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer();
  void goToSettings();
  void goToFileBrowser(std::string path = {});
  void goToRecentBooks();
  // 2026-10-07：Formosa Cover
  // 的書架（卡上所有的書，封面）。returnPath＝剛讀完的那本（從閱讀器回來停在它上面）；空＝停在分頁列
  // initialTab（v375）：從「資料夾」分頁換過來＝停在那個分頁的分頁列（BookshelfActivity::kTabAll／kTabFav；−1＝照預設）
  void goToBookshelf(std::string returnPath = {}, int initialTab = -1);
  // 閱讀器的「回書庫」：Formosa Cover 沒有瀏覽檔案，書庫就是書架；其他主題進瀏覽檔案（path＝要停在哪個檔／資料夾）
  void goToLibrary(std::string path = {});
  void goToBrowser();
  // v280：goToReader 被呼叫的時刻（喚醒路徑分項計時）。
  // ⚠️ **取走即清空**（複查抓到）：不清的話，之後任何一次沒有經過 goToReader 的 `onEnter`
  //    （從註腳／選單回到閱讀器）都會拿到上一次的時刻，印出一個看起來合理、實際上毫無意義的
  //    `trans=`。清掉之後那些情況會印 -1，一眼就知道「這次不是從那條路進來的」。
  [[nodiscard]] unsigned long takeReaderTransitionStartMs() {
    const unsigned long v = readerTransitionStartMs_;
    readerTransitionStartMs_ = 0;
    return v;
  }

  void goToReader(std::string path, bool allowFastInitialRefresh = false);
  void goToSleep();
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE);

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  bool isReaderActivity() const;
  bool supportsLightSleep() const;       // v327：目前 activity 可不可以淺睡眠（黑名單制）
  bool currentIsReaderActivity() const;  // v327：閱讀器在最上層（wake frame 只在這時寫）
  unsigned flushProgress();              // v329：current ＋ 堆疊全部 flushProgress()，回【失敗】了幾個
  unsigned flushProgressDurable();       // v332：同上，但走 SD 檢查點
  bool handleForcedRefresh();
  bool skipLoopDelay() const;
  unsigned renderStackHighWater() const;  // v361：繪製任務開機以來的堆疊最低餘裕（bytes），SLEEP 行的 rstk=
  ScreenshotInfo getScreenshotInfo() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();
  // 螢幕世代（2026-10-07，Formosa Cover 的局部重畫用）：有別人蓋過畫面（子畫面進出、淺睡眠醒來沒還原畫面）就加一。
  //   只擦舊框、畫新框的 render 要先確認世代沒變；變了就整頁重畫。
  uint32_t screenEpoch() const { return screenEpoch_.load(); }
  void invalidateScreen() { screenEpoch_.fetch_add(1); }
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
