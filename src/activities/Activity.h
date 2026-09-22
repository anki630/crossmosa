#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }
  // v327：全域淺睡眠（黑名單制）—— 預設可以；WiFi／檔案傳輸／Calibre（WiFi 開著睡等於沒睡，退出時本來就重啟）、
  //   SD 韌體更新、當機、開機、桌布畫面回 false。淺睡眠只把 framebuffer 留在 RAM，跟畫面內容無關。
  virtual bool supportsLightSleep() const { return true; }
  // v329：把欠著沒寫的閱讀進度寫掉（閱讀器不再每頁寫 progress.bin）。休眠前由 enterDeepSleep 呼叫（持 RenderLock）。
  //   回 0＝沒有要寫的、1＝寫成功、-1＝寫失敗（呼叫端可重試一次並記錄）。
  virtual int flushProgress() { return 0; }
  // v332：淺睡眠入口桌布之後的 SD 檢查點（沒人等）：progress.bin 若過期就寫。回傳同 flushProgress。
  virtual int flushProgressDurable() { return 0; }
  // Returns true when the activity schedules its own forced refresh.
  virtual bool handleForcedRefresh() { return false; }
  virtual bool isHomeActivity() const { return false; }
  virtual bool handleHomeGesture() { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);

 protected:
  enum class ListTouchResult : uint8_t {
    None,      // touch did not hit the list
    Consumed,  // touchdown moved the highlight (repaint already requested)
    Activated  // tap landed on a row: selectedIndex is updated, caller activates it
  };

  // Shared touch handling for selectable list screens: touchdown highlights the
  // touched row, a tap selects and reports Activated. The caller supplies the
  // list band and runs its own activate action on Activated.
  ListTouchResult handleListTouch(int& selectedIndex, int itemCount, int listTop, int listHeight, bool hasSubtitle);
};
