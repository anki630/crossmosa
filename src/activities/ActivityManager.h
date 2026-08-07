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

  // v110: "a render has been requested but has not started yet". Set on every path that
  // notifies (or will notify) the render task; cleared by the render task itself before it
  // calls render(). A request arriving DURING a render therefore leaves it true, which is
  // exactly the signal the reader's glyph prefetch polls to abandon its work early.
  // This is a LATENCY HINT, not a correctness gate: the correctness gate is the per-field
  // WarmIdentity comparison at the next render, so missing the flag once only costs a few
  // extra milliseconds of prefetch. Single-core chip, one writer per direction (main task
  // sets, render task clears), no read-modify-write -- volatile bool is sufficient.
  volatile bool renderPending_ = false;

  // v110: raise renderPending_ unless the caller IS the render task. Called from RenderLock's
  // constructors BEFORE they block on the semaphore, which is the one chokepoint every
  // state-mutating / teardown path funnels through: pageTurn()'s spine cross, chapter skip,
  // footnote navigation, applyOrientation, activity push/pop/replace, the screenshot and
  // force-refresh paths in main.cpp. All of those take the lock FIRST and only call
  // requestUpdate() afterwards, so without this the hint would still be false while they sit
  // blocked behind an in-flight prefetch -- a ~250-330 ms input freeze exactly when the user
  // pressed a key. The render task must be excluded: its own acquisition in renderTaskLoop()
  // happens right after the unconditional clear, so raising it there would permanently disable
  // prefetching. A spurious raise (e.g. loop()'s build pump winning the lock during a render)
  // is harmless: the flag is cleared before every render, so it costs at most one skipped
  // speculative prefetch.
  void raiseRenderPendingIfNotRenderTask();

 public:
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
  void goToBrowser();
  void goToReader(std::string path);
  void goToSleep(bool fromTimeout = false);
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
  bool skipLoopDelay() const;
  ScreenshotInfo getScreenshotInfo() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // v110: true between "a render was requested" and "the render task picked it up".
  // See renderPending_ above -- a hint for aborting speculative work, never a correctness gate.
  bool isRenderPending() const { return renderPending_; }

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
