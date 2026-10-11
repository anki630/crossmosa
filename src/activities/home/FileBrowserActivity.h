#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/HoldRepeat.h"

class FileBrowserActivity final : public Activity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // Deletion
  bool removeDirFile(const std::string& fullPath);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;
  // v374：Formosa Cover 的瀏覽檔案＝書架的「資料夾」分頁 → 上方畫同一條分頁列。
  // v375（維護者：分頁列看起來像選到了、長按卻在換頁）：分頁列也能選，規則同書架 ——
  //   最上層按 ⑤＝選取跳到分頁列；分頁列上 ⑥＝下一個分頁、長按 ⑦⑧＝往左／往右換分頁（繞圈、每次按住只換一格）、
  //   短按 ⑧／⑦＝回到第一個／最後一個檔案、⑤＝回首頁。
  bool showShelfTabs() const;
  int tabBarSpace() const;
  void loopOnTabs();
  void switchShelfTab(int dir);
  int shelfTabAfter(int dir) const;
  bool onTabs_ = false;
  bool tabHoldUsed_ = false;
  bool tabConfirmSeen_ = false;
  uint32_t tabConfirmAt_ = 0;  // v379 換分頁量測
  bool tabBackSeen_ = false;
  HoldRepeat tabNext_;
  HoldRepeat tabPrev_;
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;

  Mode mode = Mode::Books;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books)
      : Activity("FileBrowser", renderer, mappedInput),
        mode(mode),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
