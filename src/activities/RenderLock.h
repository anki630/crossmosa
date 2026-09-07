#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  static bool peek();
  // v200：本 task 是否【已經】持有這把鎖。renderingMutex 非遞迴，持鎖時再取一次＝永久死鎖，
  // 而註解攔不住未來新增的呼叫點 —— 讓它在 debug build 直接斷在現場。
  static bool heldByCurrentTask();
};
