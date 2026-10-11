#include "BookshelfActivity.h"

#include <DataDir.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "components/covers/CoverTile.h"
#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"
#include "util/DiagLog.h"
#include "util/Favorites.h"
#include "util/RecentCoverLoader.h"
#include "util/TabSwitchProbe.h"

namespace {

constexpr int kCoverW = 144;
constexpr int kCoverH = 216;
constexpr int kSideMargin = 20;
constexpr int kRingGap = 6;     // 選取框（2 px）與封面之間至少留這麼多，擦框才不會挖到封面（X4 寬 480 時要縮邊）
constexpr int kTilePadTop = 8;  // 選取框到封面
constexpr int kBarGap = 10;     // 封面到細進度條
constexpr int kBarH = 6;
constexpr int kTileH = kTilePadTop + kCoverH + kBarGap + kBarH + 8;
constexpr int kRowGap = 6;
constexpr int kGridGap = 10;                  // 分頁列到第一列
constexpr int kTitleLines = 2;                // 選到的書名最多幾行（v384）
constexpr unsigned long kLongPressMs = 1000;  // 同最近閱讀清單（RecentBooksActivity）
// 掃描的上限：書的數量與 arena 在 ShelfIndex；這裡限制「看過幾個項目」與資料夾深度，讓滿是圖片／字型的卡也有上限
constexpr uint32_t kMaxVisited = 3000;
// 資料夾深度上限（根目錄＝0）。深度優先：同時開著的資料夾最多 kMaxDepth＋1 個（每個一個檔柄，約百來 bytes）。
//   Calibre 傳書的「作者／書名／書.epub」放在 /books 底下是深度 3，留一倍餘裕。
constexpr int kMaxDepth = 6;
constexpr size_t kPathMax = 512;  // 完整路徑上限（位元組）；超過的那一項跳過並提示

int sideMargin(const int screenW) {
  return std::min(kSideMargin, (screenW - BookshelfActivity::kCols * (kCoverW + 2 * kRingGap + 2)) / 2);
}

bool isBookName(const char* name) {
  const std::string_view n{name};
  return FsHelpers::hasEpubExtension(n) || FsHelpers::hasXtcExtension(n) || FsHelpers::hasTxtExtension(n) ||
         FsHelpers::hasMarkdownExtension(n);
}

}  // namespace

void BookshelfActivity::scan() {
  const uint32_t t0 = millis();
  scanTruncated_ = false;
  // 記憶體：arena 最多 24 KB，而且不超過「目前最大連續塊 − 16 KB」（留給畫封面時解縮圖、底框）
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
  const size_t budget = largest > 16 * 1024 ? largest - 16 * 1024 : 0;
  if (!index_.begin(budget)) {
    DiagLog::line("SHELFSCAN alloc-fail largest=%u", static_cast<unsigned>(largest));
    scanTruncated_ = true;
    return;
  }
  // 最近閱讀先放：書多到撞上限時，最近讀的書也一定在書架上（recents_ 已濾掉檔案不在的）
  for (const RecentBook& b : recents_) {
    if (!index_.addPinned(b.path.c_str(), b.path.size())) scanTruncated_ = true;
  }

  // 深度優先走訪（codex 複查三輪後改寫）：迴圈裡不配置任何字串或容器 —— 路徑放在固定緩衝區，
  //   每一層一個開著的資料夾檔柄（HalFile 的配置本身是 nothrow，配不到就是空檔柄）。
  //   -fno-exceptions 下 vector／string 配不到＝abort，所以這裡完全不用它們（硬限制第 2 條）。
  char path[kPathMax];
  size_t lens[kMaxDepth + 1];
  HalFile stack[kMaxDepth + 1];
  uint32_t visited = 0;
  uint32_t dirs = 1;
  int top = 0;
  path[0] = '\0';
  lens[0] = 0;
  stack[0] = Storage.open("/");
  if (!stack[0] || !stack[0].isDirectory()) {
    scanTruncated_ = true;
    top = -1;
  }
  bool full = false;
  while (top >= 0 && !full) {
    HalFile entry = stack[top].openNextFile();
    if (!entry) {  // 這一層看完了：回上一層
      // 空檔柄也可能是檔柄配置失敗（HAL 另記 ALLOCFAIL）：那不是看完了，要提示
      if (entry.allocFailed()) scanTruncated_ = true;
      stack[top].close();
      --top;
      continue;
    }
    if (++visited > kMaxVisited) {  // 還有第 3001 項：提早結束（剛好 3000 項看完不算）
      scanTruncated_ = true;
      break;
    }
    // 檔名直接寫進路徑緩衝區（不另外放 name：長的 UTF-8 檔名可以到 765 bytes）。放不下 getName 回 0
    char* const name = path + lens[top] + 1;
    const size_t room = sizeof(path) - lens[top] - 1;
    const size_t nameLen = room > 1 ? entry.getName(name, room) : 0;
    if (nameLen == 0) {  // 路徑太長：跳過、提示
      scanTruncated_ = true;
      continue;
    }
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) continue;
    const bool isDir = entry.isDirectory();
    if (!isDir && !isBookName(name)) continue;
    const size_t len = lens[top] + 1 + nameLen;
    path[lens[top]] = '/';
    if (isDir) {
      if (top + 1 > kMaxDepth) {  // 太深的資料夾不掃：裡面可能有書，所以提示
        scanTruncated_ = true;
        continue;
      }
      ++top;
      ++dirs;
      stack[top] = std::move(entry);
      lens[top] = len;
    } else if (!index_.add(path, len)) {
      full = true;  // 書數或 arena 到上限（index_.capped()）
    }
  }
  for (; top >= 0; --top) stack[top].close();
  std::vector<std::string> recentPaths;
  recentPaths.reserve(recents_.size());
  std::transform(recents_.begin(), recents_.end(), std::back_inserter(recentPaths),
                 [](const RecentBook& b) { return b.path; });
  index_.sort(recentPaths);
  scanMs_ = millis() - t0;
  scanVisited_ = visited;
  DiagLog::line("SHELFSCAN books=%u visited=%u dirs=%u ms=%lu capped=%d trunc=%d arena=%u/%u largest=%u",
                static_cast<unsigned>(index_.total()), static_cast<unsigned>(visited), static_cast<unsigned>(dirs),
                static_cast<unsigned long>(scanMs_), index_.capped() ? 1 : 0, scanTruncated_ ? 1 : 0,
                static_cast<unsigned>(index_.arenaBytes()), static_cast<unsigned>(index_.arenaCapacity()),
                static_cast<unsigned>(largest));
}

void BookshelfActivity::onEnter() {
  Activity::onEnter();
  backPressSeen_ = false;
  confirmPressSeen_ = false;
  longPressFired_ = false;
  recents_.clear();
  const auto& stored = RECENT_BOOKS.getBooks();
  std::copy_if(stored.begin(), stored.end(), std::back_inserter(recents_),
               [](const RecentBook& b) { return !RecentBooksStore::isMissing(b); });
  favLoad_ = Favorites::load(favorites_);
  navNext_.reset();
  navPrev_.reset();
  scan();
  placeInitialSelection();
  drawnPage_ = -1;
  TabSwitchProbe::ready();  // 從「資料夾」換回來：掃卡、排書都做完了
  requestUpdate();
}

namespace {
// 上一次從書架開的那本書在哪個分頁（RAM 就好：重開機之後從首頁進來，本來就停在分頁列）
std::string lastOpenedPath;
int lastOpenedTab = BookshelfActivity::kTabAll;
}  // namespace

// 進書架停在哪（v373，維護者 2026-10-08）：
//   從首頁、資料夾進來＝停在分頁列（書架的最上層；⑥／長按就能換分頁，第一本只差一下 ⑧）。
//   從閱讀器回來（長按 ⑤）＝停在剛讀的那本，分頁照開書時那個；不是從書架開的書：是最愛就在「最愛」、否則「全部」。
//   分頁的規則照 v369：卡上找得到最愛才有「最愛」分頁、有就預設在它。
void BookshelfActivity::placeInitialSelection() {
  tab_ = kTabAll;
  resort();
  hasFavTab_ = index_.favoriteCount() > 0;
  int wantTab = hasFavTab_ ? kTabFav : kTabAll;
  if (initialTab_ == kTabAll || (initialTab_ == kTabFav && hasFavTab_))
    wantTab = initialTab_;  // v375：從資料夾分頁換過來
  if (!returnPath_.empty()) {
    if (returnPath_ == lastOpenedPath) {
      wantTab = lastOpenedTab;
    } else {
      wantTab = favorites_.contains(Favorites::idOf(returnPath_.c_str())) ? kTabFav : kTabAll;
    }
    if (wantTab == kTabFav && !hasFavTab_) wantTab = kTabAll;
  }
  if (wantTab != tab_) {
    tab_ = wantTab;
    resort();
  }
  selected_ = kOnTabs;
  shownPage_.store(0);
  if (returnPath_.empty()) return;
  const int n = static_cast<int>(index_.size());
  for (int i = 0; i < n; ++i) {
    if (returnPath_ == index_.path(static_cast<size_t>(i))) {
      selected_ = i;
      shownPage_.store(i / kPerPage);
      return;
    }
  }
}

void BookshelfActivity::onExit() {
  Activity::onExit();
  index_.release();  // 進閱讀器前把 arena 還回去
  std::vector<RecentBook>().swap(recents_);
  favorites_.clear();
}

void BookshelfActivity::resort() {
  std::vector<std::string> recentPaths;
  recentPaths.reserve(recents_.size());
  std::transform(recents_.begin(), recents_.end(), std::back_inserter(recentPaths),
                 [](const RecentBook& b) { return b.path; });
  index_.sort(recentPaths, [this](const char* p) { return favorites_.contains(Favorites::idOf(p)); }, tab_ == kTabFav);
  // 加了第一本最愛 → 出現「最愛」分頁；在「最愛」分頁裡移掉最後一本 →
  // 分頁先留著（不要在使用者眼前消失），下次進書架才收
  hasFavTab_ = hasFavTab_ || tab_ == kTabFav || index_.favoriteCount() > 0;
}

int BookshelfActivity::pageCount() const {
  const int n = static_cast<int>(index_.size());
  return n == 0 ? 1 : (n + kPerPage - 1) / kPerPage;
}

const RecentBook* BookshelfActivity::recentFor(const char* path) const {
  const auto it =
      std::find_if(recents_.begin(), recents_.end(), [path](const RecentBook& b) { return b.path == path; });
  return it == recents_.end() ? nullptr : &*it;
}

std::string BookshelfActivity::titleOf(const int index) const {
  const char* p = index_.path(static_cast<size_t>(index));
  const RecentBook* r = recentFor(p);
  return r && !r->title.empty() ? r->title : ShelfIndex::titleFromPath(p);
}

std::string BookshelfActivity::thumbPathFor(const char* path, const RecentBook* recent) const {
  if (recent && !recent->coverBmpPath.empty()) return UITheme::getCoverThumbPath(recent->coverBmpPath, kCoverH);
  // 開過、但已經不在最近閱讀的書：快取目錄裡可能還有這個高度的縮圖（快取目錄＝路徑雜湊，同 Epub／Xtc 建構子）
  const std::string_view n{path};
  const char* prefix = FsHelpers::hasEpubExtension(n) ? "/epub_" : FsHelpers::hasXtcExtension(n) ? "/xtc_" : nullptr;
  if (!prefix) return {};
  std::string p = std::string(DataDir::path()) + prefix + std::to_string(std::hash<std::string>{}(std::string(path))) +
                  "/thumb_" + std::to_string(kCoverH) + ".bmp";
  return Storage.exists(p.c_str()) ? p : std::string();
}

int BookshelfActivity::gridTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + kGridGap;
}

int BookshelfActivity::titleLineY() const { return gridTop() + kRows * kTileH + (kRows - 1) * kRowGap + 4; }

void BookshelfActivity::tileRect(const int slot, int& x, int& y, int& w, int& h) const {
  const int side = sideMargin(renderer.getScreenWidth());
  const int col = (renderer.getScreenWidth() - 2 * side) / kCols;
  x = side + col * (slot % kCols) + 1;
  y = gridTop() + (slot / kCols) * (kTileH + kRowGap);
  w = col - 2;
  h = kTileH;
}

void BookshelfActivity::moveSelection(const int newIndex) {
  const int n = static_cast<int>(index_.size());
  if (newIndex < kOnTabs || newIndex >= n || newIndex == selected_) return;
  // 同一頁內移動（含分頁列 ↔ 這一頁的書）才能局部重畫。選取在分頁列時，畫面停在原本那一頁（shownPage_）
  const int oldPage = selected_ == kOnTabs ? shownPage_.load() : selected_ / kPerPage;
  const int newPage = newIndex == kOnTabs ? oldPage : newIndex / kPerPage;
  shownPage_.store(newPage);
  if (newIndex != kOnTabs) tabsByBack_ = false;
  selected_ = newIndex;
  partialOk_.store(oldPage == newPage);
  requestUpdate();
}

void BookshelfActivity::step(const int dir) {
  const int n = static_cast<int>(index_.size());
  // v368：用 ⑤ 跳上分頁列的，⑧／⑦ 回到畫面上這一頁的第一本／最後一本（看了分頁不換，就回原處）。
  //   走到底繞上來的（最後一本 ⑧、第一本 ⑦）照舊繞圈：⑧ 到第一本、⑦ 到最後一本。
  if (selected_ == kOnTabs && n > 0 && tabsByBack_) {
    tabsByBack_ = false;
    const int first = std::min(shownPage_.load(), pageCount() - 1) * kPerPage;
    moveSelection(dir > 0 ? first : std::min(first + kPerPage, n) - 1);
    return;
  }
  // 位置：分頁列（kOnTabs）、第 0 本 … 第 n−1 本，共 n＋1 格；走到底繞回頭
  const int positions = n + 1;
  const int cur = selected_ + 1;  // 0＝分頁列
  const int next = (cur + (dir > 0 ? 1 : positions - 1)) % positions;
  moveSelection(next - 1);
}

void BookshelfActivity::jumpPage(const int dir) {
  const int n = static_cast<int>(index_.size());
  if (n == 0) return;
  const int page = selected_ == kOnTabs ? shownPage_.load() : selected_ / kPerPage;
  const int target = std::max(0, std::min(pageCount() - 1, page + dir));  // 到頭停住（長按不繞圈，同 v31）
  moveSelection(target * kPerPage);
}

// 分頁順序（v369）：最愛｜全部｜資料夾（沒有最愛時：全部｜資料夾）。⑥ 往右一格；「資料夾」＝離開到瀏覽檔案，
//   從那裡在根目錄按 ⑤ 回到書架時重新套「有最愛就停在最愛」—— 所以從「全部」回「最愛」也只要 ⑥ ⑤。
void BookshelfActivity::nextTab() { switchTab(+1); }

// 換分頁（v372）：dir＝+1 往右、−1
// 往左，繞圈。順序＝畫面上的分頁（有最愛時「最愛｜全部｜資料夾」，沒有時「全部｜資料夾」）；
//   換到「資料夾」＝打開瀏覽檔案（同 ⑥）。分頁列上 ⑥＝往右一格；長按 ⑧④／⑦①＝往右／往左一格（維護者 2026-10-08）。
void BookshelfActivity::switchTab(const int dir) {
  constexpr int kTabFolders = 2;
  int order[3];
  int n = 0;
  if (hasFavTab_) order[n++] = kTabFav;
  order[n++] = kTabAll;
  order[n++] = kTabFolders;
  int cur = 0;
  while (cur < n && order[cur] != tab_) ++cur;
  const int target = order[((cur + dir) % n + n) % n];
  if (target == kTabFolders) {
    sHasFavTab = hasFavTab_;               // 瀏覽檔案上方的分頁列要跟書架一樣
    sFolderOnTabs = true;                  // 換分頁＝選取留在分頁列（v376）
    activityManager.goToFileBrowser("/");  // 「資料夾」＝原本的瀏覽檔案（看圖、刪檔）
    return;
  }
  if (target == tab_) return;
  RenderLock lock(*this);  // 重排 index_ 的順序：render 任務可能正在讀
  tab_ = target;
  resort();
  selected_ = kOnTabs;
  shownPage_.store(0);  // 換了分頁：從第一頁開始
  tabsByBack_ = false;
  drawnPage_ = -1;
  TabSwitchProbe::ready();
  requestUpdate();
}

// 長按：選取在分頁列＝換分頁（每按住一次只換一格，免得連發衝過頭）；在書上＝翻頁（照舊，每 0.5 秒一頁）
void BookshelfActivity::longOnTabs(const int dir) {
  if (selected_ != kOnTabs) {
    jumpPage(dir);
    return;
  }
  if (tabHoldUsed_) return;
  tabHoldUsed_ = true;
  TabSwitchProbe::begin("hold", "shelf", (dir > 0 ? navNext_ : navPrev_).downAt());
  switchTab(dir);
}

void BookshelfActivity::openBookMenu(const int idx) {
  if (idx < 0 || idx >= static_cast<int>(index_.size())) return;
  const std::string path = index_.path(static_cast<size_t>(idx));
  const bool isRecent = recentFor(path.c_str()) != nullptr;
  const bool fav = favorites_.contains(Favorites::idOf(path));
  // 不能改的情況：較新版本的最愛（這次唯讀）、已滿 —— 選項寫原因，選了不做事
  const bool locked = !Favorites::writable(favLoad_);
  const bool full = !locked && !fav && favorites_.size() >= favorites::kMax;
  const char* options[] = {I18N.get(locked ? StrId::STR_FAVORITES_UNAVAILABLE
                                    : fav  ? StrId::STR_REMOVE_FROM_FAVORITES
                                    : full ? StrId::STR_FAVORITES_FULL
                                           : StrId::STR_ADD_TO_FAVORITES),
                           tr(STR_REMOVE_FROM_RECENTS_ACTION)};
  const std::string title = renderer.truncatedText(UI_12_FONT_ID, titleOf(idx).c_str(),
                                                   renderer.getScreenWidth() * 2 / 3, EpdFontFamily::BOLD);
  partialOk_.store(false);  // 選單會蓋掉畫面：關掉之後整頁重畫
  backPressSeen_ = false;
  navNext_.reset();
  navPrev_.reset();
  menuPath_ = path;
  menuChoice_ = -1;
  menuCanToggle_ = !full && !locked;
  // 回呼只記下選了哪一項；真正的動作在 loop 裡、選單關掉之後
  menu_.show(title.c_str(), options, isRecent ? 2 : 1, 0, [this](const int i) { menuChoice_ = i; });
  requestUpdate();
}

// 書本選單的動作（codex 複查 v367 第三輪）：
//   ① 在同一段繪製鎖裡：選單關掉＋標記整頁重畫＋改最愛（NVS 寫入是毫秒級）＋重排 —— render
//   插進來也只會看到一致的狀態整頁畫。 ② 從最近閱讀移除要寫 SD、重建清單要碰
//   SD（isMissing）：鎖外做完，鎖內只換掉成品並重排。
void BookshelfActivity::toggleFavoriteLocked() {
  const uint64_t id = Favorites::idOf(menuPath_);
  favorites_.toggle(id);
  if (!Favorites::save(favorites_, &saveInfo_)) {  // 存不進去：退回，畫面不假裝成功
    favorites_.toggle(id);
    return;
  }
  resortLocked();
}

void BookshelfActivity::resortLocked() {
  resort();  // 「最愛」分頁裡取消最愛的那本消失；移除最近閱讀的那本回到照檔名的位置
  const int n = static_cast<int>(index_.size());
  if (selected_ >= n) selected_ = n > 0 ? n - 1 : kOnTabs;
  if (selected_ != kOnTabs) shownPage_.store(selected_ / kPerPage);
  drawnPage_ = -1;
}

void BookshelfActivity::removeFromRecents() {
  if (!RECENT_BOOKS.removeByPath(menuPath_)) return;
  std::vector<RecentBook> fresh;
  const auto& stored = RECENT_BOOKS.getBooks();
  std::copy_if(stored.begin(), stored.end(), std::back_inserter(fresh),
               [](const RecentBook& b) { return !RecentBooksStore::isMissing(b); });
  RenderLock lock(*this);
  recents_.swap(fresh);
  resortLocked();
}

void BookshelfActivity::loop() {
  using B = MappedInputManager::Button;
  if (menu_.isActive()) {
    bool removeRecent = false;
    bool saved = false;
    {
      RenderLock lock(*this);
      menu_.handleInput(mappedInput, [this] { requestUpdate(); });
      if (!menu_.isActive()) {  // 選了或取消：整頁重畫（選單蓋掉的地方）＋做動作
        drawnPage_ = -1;
        const int choice = menuChoice_;
        menuChoice_ = -1;
        saved = choice == 0 && menuCanToggle_;
        if (saved) toggleFavoriteLocked();
        removeRecent = choice == 1;
        navNext_.reset();
        navPrev_.reset();
        requestUpdate();
      }
    }
    if (saved) Favorites::logSave(saveInfo_);  // DiagLog 會寫 SD：放在鎖外
    if (removeRecent) {
      removeFromRecents();
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(B::Confirm)) {
    confirmPressSeen_ = true;
    confirmDownAt_ = millis();  // getHeldTime() 是全域的（任何鍵），長按要看確認鍵自己按了多久
  }
  if (mappedInput.wasPressed(B::Back)) backPressSeen_ = true;

  // 長按確認：時間到就觸發（不等放開）；之後吞掉輸入直到放開，免得放開又開書（同 RecentBooksActivity）
  if (longPressFired_) {
    if (!mappedInput.isPressed(B::Confirm)) longPressFired_ = false;
    return;
  }
  const bool onBook = selected_ != kOnTabs && selected_ < static_cast<int>(index_.size());
  if (onBook && confirmPressSeen_ && mappedInput.isPressed(B::Confirm) && millis() - confirmDownAt_ >= kLongPressMs) {
    longPressFired_ = true;
    confirmPressSeen_ = false;
    openBookMenu(selected_);
    return;
  }
  if (mappedInput.wasReleased(B::Confirm) && confirmPressSeen_) {
    confirmPressSeen_ = false;
    if (!onBook) {
      TabSwitchProbe::begin("ok", "shelf", confirmDownAt_);
      nextTab();
      return;
    }
    // 迴圈剛好在 1 秒那一刻被刷新卡住、恢復時已經放開：依按住時間判斷，不要誤開書
    if (millis() - confirmDownAt_ >= kLongPressMs) {
      openBookMenu(selected_);
      return;
    }
    lastOpenedPath = index_.path(static_cast<size_t>(selected_));
    lastOpenedTab = tab_;
    activityManager.goToReader(lastOpenedPath);
    return;
  }
  // ⑤（v368，維護者：在書上要換分頁得一路走回最上面）：在書上＝先跳到分頁列（畫面停在這一頁）；在分頁列＝回首頁
  if (mappedInput.wasReleased(B::Back) && backPressSeen_) {
    backPressSeen_ = false;
    if (selected_ != kOnTabs && index_.size() > 0) {
      moveSelection(kOnTabs);
      tabsByBack_ = true;
    } else {
      activityManager.goHome(HomeMenuItem::FILE_BROWSER);
    }
    return;
  }
  // ⑦⑧／①④：短按一本（放開時）、長按翻頁（按住 0.5 秒起，每 0.5 秒一頁；分頁列上 0.3 秒換分頁）——
  // 計時是每組鍵自己的（HoldRepeat）
  const uint32_t now = millis();
  if (!mappedInput.isPressed(B::NavNext) && !mappedInput.isPressed(B::NavPrevious)) tabHoldUsed_ = false;
  // 分頁列上長按＝換分頁，門檻短一點（v380）；書上長按＝翻頁，照舊 0.5 秒
  const uint32_t holdMs = selected_ == kOnTabs ? HoldRepeat::kTabStartMs : HoldRepeat::kStartMs;
  switch (navNext_.update(mappedInput.wasPressed(B::NavNext), mappedInput.wasReleased(B::NavNext),
                          mappedInput.isPressed(B::NavNext), now, holdMs)) {
    case HoldRepeat::Action::Short:
      step(+1);
      break;
    case HoldRepeat::Action::Long:
      longOnTabs(+1);
      break;
    case HoldRepeat::Action::None:
      break;
  }
  switch (navPrev_.update(mappedInput.wasPressed(B::NavPrevious), mappedInput.wasReleased(B::NavPrevious),
                          mappedInput.isPressed(B::NavPrevious), now, holdMs)) {
    case HoldRepeat::Action::Short:
      step(-1);
      break;
    case HoldRepeat::Action::Long:
      longOnTabs(-1);
      break;
    case HoldRepeat::Action::None:
      break;
  }
}

void BookshelfActivity::drawTabs(const bool focused) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect r{0, metrics.topPadding + metrics.headerHeight, renderer.getScreenWidth(), metrics.tabBarHeight};
  renderer.fillRect(r.x, r.y, r.width, r.height, false);
  std::vector<TabInfo> tabs;
  tabs.reserve(3);
  if (hasFavTab_) tabs.push_back({tr(STR_TAB_FAVORITES), tab_ == kTabFav});
  tabs.push_back({tr(STR_TAB_ALL), tab_ == kTabAll});
  tabs.push_back({tr(STR_TAB_FOLDERS), false});
  GUI.drawTabBar(renderer, r, tabs, focused);
}

void BookshelfActivity::drawTitleLine(const int sel) const {
  const int W = renderer.getScreenWidth();
  const int y = titleLineY();
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);
  renderer.fillRect(0, y, W, kTitleLines * lineH, false);
  if (sel == kOnTabs || sel >= static_cast<int>(index_.size())) return;
  const char* p = index_.path(static_cast<size_t>(sel));
  const RecentBook* r = recentFor(p);
  // 書名優先：書名＋作者一行放得下才加作者；放不下就只放書名（v368 實機截圖：整行截斷會留下「書名　…」）。
  // v384（維護者：書名長會被截斷）：書名最多兩行，各行置中；第二行還放不下才加省略號。
  //   區塊固定留兩行高（頁碼不會跟著選到的書上下跳），一行的書名貼在封面下方。
  const std::string title = titleOf(sel);
  std::vector<std::string> lines;
  if (r && !r->author.empty()) {
    const std::string withAuthor = title + "　" + r->author;
    if (renderer.getTextWidth(UI_12_FONT_ID, withAuthor.c_str(), EpdFontFamily::BOLD) <= W - 40) {
      lines.push_back(withAuthor);
    }
  }
  if (lines.empty())
    lines = renderer.wrappedText(UI_12_FONT_ID, title.c_str(), W - 40, kTitleLines, EpdFontFamily::BOLD);
  int ty = y;
  for (const std::string& t : lines) {
    renderer.drawText(UI_12_FONT_ID, (W - renderer.getTextWidth(UI_12_FONT_ID, t.c_str(), EpdFontFamily::BOLD)) / 2, ty,
                      t.c_str(), true, EpdFontFamily::BOLD);
    ty += lineH;
  }
}

void BookshelfActivity::drawHints(const bool onTabs) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int H = renderer.getScreenHeight();
  renderer.fillRect(0, H - metrics.buttonHintsHeight, renderer.getScreenWidth(), metrics.buttonHintsHeight, false);
  // 分頁列上 ⑥＝換到下一個分頁（同設定頁：確認鍵標的是下一個分類的名字）
  const char* confirm = !onTabs ? tr(STR_OPEN) : tab_ == kTabFav ? tr(STR_TAB_ALL) : tr(STR_TAB_FOLDERS);
  const auto labels = mappedInput.mapLabels(onTabs ? tr(STR_HOME) : tr(STR_SHELF_TABS), confirm, "‹", "›");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BookshelfActivity::render(RenderLock&&) {
  // 書本選單：蓋在目前畫面上（framebuffer 裡就是書架）
  if (menu_.isActive()) {
    partialOk_.store(false);
    menu_.processRender(renderer, mappedInput);
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();
  const int n = static_cast<int>(index_.size());
  const bool partial = partialOk_.exchange(false);  // 先消化（moveSelection 先寫 selected_ 再設它），再讀 selected_
  const int sel = n == 0 ? kOnTabs : std::max(kOnTabs, std::min(selected_, n - 1));
  const int page = sel == kOnTabs ? std::max(0, std::min(shownPage_.load(), pageCount() - 1)) : sel / kPerPage;

  // 局部重畫：同一頁、只移動選取、畫面沒被別人蓋過 → 擦舊框、畫新框、換書名行（分頁列進出時重畫分頁列與按鍵提示）
  if (partial && drawnPage_ == page && drawnSel_ != sel && drawnEpoch_ == activityManager.screenEpoch()) {
    int x, y, w, h;
    if (drawnSel_ == kOnTabs) {
      drawTabs(false);
      drawHints(false);
    } else {
      tileRect(drawnSel_ % kPerPage, x, y, w, h);
      CoverTile::drawRing(renderer, x, y, w, h, /*erase=*/true);
    }
    if (sel == kOnTabs) {
      drawTabs(true);
      drawHints(true);
    } else {
      tileRect(sel % kPerPage, x, y, w, h);
      CoverTile::drawRing(renderer, x, y, w, h, /*erase=*/false);
    }
    drawTitleLine(sel);
    drawnSel_ = sel;
    renderer.displayBuffer();
    return;
  }

  TabSwitchProbe::drawStart();
  CoverTile::beginStats(renderer);
  uint32_t lookupUs = 0;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageW, metrics.headerHeight}, tr(STR_BOOKSHELF), nullptr);
  drawTabs(sel == kOnTabs);

  std::vector<RecentBook> pageRecents;  // 這一頁的最近閱讀書：縮圖要這個高度（216）的，第一次顯示時補產
  if (n == 0) {
    const int y = gridTop() + 2 * kTileH / 3;
    if (tab_ == kTabFav) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_FAVORITES_EMPTY), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, y + 44, tr(STR_FAVORITES_HOWTO), true);
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_SHELF_EMPTY), true);
      // 掃描提早結束（記憶體不足、資料夾太深）：一本都沒有時也要說，不然看起來像卡上真的沒書
      if (scanTruncated_) renderer.drawCenteredText(UI_10_FONT_ID, y + 44, tr(STR_SHELF_SCAN_INCOMPLETE), true);
    }
  } else {
    for (int slot = 0; slot < kPerPage; ++slot) {
      const int i = page * kPerPage + slot;
      if (i >= n) break;
      const uint32_t lookupT0 = micros();
      const char* p = index_.path(static_cast<size_t>(i));
      const RecentBook* r = recentFor(p);
      const std::string thumb = thumbPathFor(p, r);
      const std::string title = titleOf(i);
      lookupUs += micros() - lookupT0;
      int x, y, w, h;
      tileRect(slot, x, y, w, h);
      const int cx = x + (w - kCoverW) / 2;
      const int cy = y + kTilePadTop;
      CoverTile::draw(renderer, cx, cy, kCoverW, kCoverH, thumb, title, r ? r->author : std::string(),
                      index_.isFavorite(static_cast<size_t>(i)));
      if (r && !r->coverBmpPath.empty()) pageRecents.push_back(*r);
      // 進度（只有最近閱讀知道）：細條，不寫百分比
      if (r) {
        const int by = cy + kCoverH + kBarGap;
        const int pct = std::min<int>(r->progressPercent, 100);
        renderer.drawSmoothRoundedRect(cx, by, kCoverW, kBarH, 1, kBarH / 2, CoverTile::kSmoothing, true);
        const int fillW = kCoverW * pct / 100;
        if (fillW > 0)
          renderer.fillSmoothRoundedRect(cx, by, std::max(kBarH, fillW), kBarH, kBarH / 2, CoverTile::kSmoothing,
                                         Color::Black);
      }
    }
    if (sel != kOnTabs) {
      int x, y, w, h;
      tileRect(sel % kPerPage, x, y, w, h);
      CoverTile::drawRing(renderer, x, y, w, h, /*erase=*/false);
    }
    drawTitleLine(sel);
    // 頁碼（＋超過上限的提示）
    char pg[64];
    if (index_.capped() || scanTruncated_) {
      snprintf(pg, sizeof(pg), tr(STR_SHELF_PAGE_CAPPED_FORMAT), page + 1, pageCount(),
               static_cast<unsigned>(index_.total()));
    } else {
      snprintf(pg, sizeof(pg), "%d／%d", page + 1, pageCount());
    }
    renderer.drawCenteredText(UI_10_FONT_ID, titleLineY() + kTitleLines * renderer.getLineHeight(UI_12_FONT_ID) + 2, pg,
                              true);
  }
  drawHints(sel == kOnTabs);
  drawnSel_ = sel;
  drawnPage_ = page;
  drawnEpoch_ = activityManager.screenEpoch();
  CoverTile::logStats(renderer, "shelf", page, lookupUs);
  TabSwitchProbe::beforeDisplay();
  renderer.displayBuffer();
  const bool tabFrame = TabSwitchProbe::afterDisplay("shelf", tab_, renderer.lastRefreshBank());
  // 補產縮圖（畫面已經先上了；產完要求整頁重畫）
  if (!pageRecents.empty()) {
    const uint32_t thumbT0 = millis();
    bool thumbRedraw = false;
    RecentCoverLoader::ensureThumbs(renderer, pageRecents, kCoverH, [this, &thumbRedraw] {
      thumbRedraw = true;
      drawnPage_ = -1;
      requestUpdate();
    });
    if (tabFrame) TabSwitchProbe::thumbs(millis() - thumbT0, thumbRedraw);
  } else if (tabFrame) {
    TabSwitchProbe::thumbs(0, false);
  }
}
