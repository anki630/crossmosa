#pragma once
#include <Epub.h>

#include <memory>

#include "../../BookmarkEntry.h"
#include "../Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

// v290：**格式無關的書籤清單**（原名 EpubReaderBookmarksActivity）。
// txt 也要書籤，而這 265 行裡只有四處碰得到 `epub`（章節標題與 spine 驗證）——
// 複製一份比把那四處改成可選貴得多。
// ⚠️ `epub` 可以是 **nullptr**（txt 就是），所有用到它的地方都必須先判斷。
class ReaderBookmarksActivity final : public Activity {
  std::shared_ptr<Epub> epub;  // txt 傳 nullptr
  std::string epubPath;        // 書的路徑（兩種格式都用它定位書籤檔）
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  std::vector<BookmarkEntry> bookmarks;
  bool confirmingDelete = false;
  OptionPopup confirmPopup;

 public:
  explicit ReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::shared_ptr<Epub>& epub, const std::string& epubPath)
      : Activity("EpubReaderBookmarks", renderer, mappedInput), epub(epub), epubPath(epubPath) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Calculate the vertical space to reserve for button hints based on orientation
  int getGutterBottom(const GfxRenderer& renderer);

  // Calculate the height available for the bookmark list based on orientation
  int getListHeight(const GfxRenderer& renderer);

  // Delete the currently selected bookmark and persist the list
  void deleteSelectedBookmark();
};
