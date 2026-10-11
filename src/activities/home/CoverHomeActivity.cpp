#include "CoverHomeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "components/covers/CoverTile.h"
#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"
#include "util/DiagLog.h"
#include "util/Favorites.h"
#include "util/ReadingStats.h"
#include "util/RecentCoverLoader.h"

namespace {

// 版面（v367 像素級畫面 work/home-cover/covers/v2-home-*-192.png；寬高一律從螢幕算，X3 528×792／X4 480×800 都用同一套）
// v385（維護者：書架常用，要最短路徑）：由上而下＝書架列、正在閱讀卡、最近閱讀三本、工具列。
//   預設選取仍是正在閱讀卡 → 往上一下到書架、往下一到三下到最近閱讀。
constexpr int kSide = 20;
constexpr int kShelfRowH = 48;
constexpr int kShelfY = 52;
constexpr int kShelfGap = 12;  // 書架列到正在閱讀卡
constexpr int kCardY = kShelfY + kShelfRowH + kShelfGap;
constexpr int kCardH = 272;
constexpr int kCardCoverW = 160;
constexpr int kCardCoverH = 240;
constexpr int kRecentW = 128;
constexpr int kRecentH = 192;
constexpr int kRecentBarGap = 10;  // 封面到細進度條
constexpr int kRecentBarH = 6;
constexpr int kRecentTileH = kRecentH + kRecentBarGap + kRecentBarH;
constexpr int kRecentCount = 3;
constexpr int kToolRowH = 40;
constexpr int kToolsBottomGap = 16;  // 工具卡到按鍵提示
constexpr int kRadius = 16;
constexpr int kRingW = 3;             // 選取框粗細（v370：2→3，維護者「很難一眼看到選到哪一項」）
constexpr int kRingOut = 4 + kRingW;  // 外圈粗框離元素多遠（4 px 留白＋框）
constexpr unsigned long kLongPressMs = 1000;

void formatDuration(char* buf, const size_t n, const uint32_t seconds) {
  const uint32_t minutes = seconds / 60;
  if (minutes < 60) {
    snprintf(buf, n, tr(STR_DURATION_MINUTES), static_cast<unsigned>(minutes));
  } else {
    snprintf(buf, n, tr(STR_DURATION_HOURS_MINUTES), static_cast<unsigned>(minutes / 60),
             static_cast<unsigned>(minutes % 60));
  }
}

// 細進度條＋整數 %（10 號字）
void drawProgress(const GfxRenderer& r, const int x, const int y, const int w, const unsigned pct, const int barH) {
  char label[8];
  snprintf(label, sizeof(label), "%u%%", pct);
  const int lw = r.getTextWidth(UI_10_FONT_ID, label);
  const int barW = w - lw - 8;
  r.drawSmoothRoundedRect(x, y + 8, barW, barH, 1, barH / 2, CoverTile::kSmoothing, true);
  const int fillW = barW * static_cast<int>(pct > 100 ? 100 : pct) / 100;
  if (fillW > 0)
    r.fillSmoothRoundedRect(x, y + 8, std::max(barH, fillW), barH, barH / 2, CoverTile::kSmoothing, Color::Black);
  r.drawText(UI_10_FONT_ID, x + barW + 8, y - 1, label, true);
}

// 細進度條（最近閱讀封面下，不寫百分比：v367 減法）
void drawThinBar(const GfxRenderer& r, const int x, const int y, const int w, const unsigned pct) {
  r.drawSmoothRoundedRect(x, y, w, kRecentBarH, 1, kRecentBarH / 2, CoverTile::kSmoothing, true);
  const int fillW = w * static_cast<int>(pct > 100 ? 100 : pct) / 100;
  if (fillW > 0)
    r.fillSmoothRoundedRect(x, y, std::max(kRecentBarH, fillW), kRecentBarH, kRecentBarH / 2, CoverTile::kSmoothing,
                            Color::Black);
}

}  // namespace

std::vector<RecentBook> CoverHomeActivity::collectRecents() const {
  std::vector<RecentBook> out;
  out.reserve(1 + kRecentCount);
  for (const RecentBook& b : RECENT_BOOKS.getBooks()) {
    if (static_cast<int>(out.size()) >= 1 + kRecentCount) break;
    if (RecentBooksStore::isMissing(b)) continue;
    out.push_back(b);
  }
  return out;
}

void CoverHomeActivity::loadRecents() { recents_ = collectRecents(); }

bool CoverHomeActivity::zoneAvailable(const Zone z) const {
  switch (z) {
    case Zone::Card:
      return !recents_.empty();
    case Zone::Recent:
      return recents_.size() > 1;
    case Zone::Shelf:
    case Zone::Tools:
      return true;
  }
  return false;
}

int CoverHomeActivity::zoneSize(const Zone z) const {
  switch (z) {
    case Zone::Card:
    case Zone::Shelf:
      return 1;
    case Zone::Recent:
      return static_cast<int>(recents_.size()) - 1;
    case Zone::Tools:
      return toolCount();
  }
  return 1;
}

CoverHomeActivity::Tool CoverHomeActivity::toolAt(const int i) const {
  if (i == 0) return Tool::Transfer;
  if (hasOpds_ && i == 1) return Tool::Opds;
  return Tool::Settings;
}

void CoverHomeActivity::onEnter() {
  Activity::onEnter();
  backPressSeen_ = false;
  confirmPressSeen_ = false;
  longPressFired_ = false;
  hasOpds_ = OPDS_STORE.hasServers();
  loadRecents();
  favLoad_ = Favorites::load(favorites_);
  navNext_.reset();
  navPrev_.reset();
  switch (initialMenuItem_) {
    case HomeMenuItem::FILE_BROWSER:
    case HomeMenuItem::RECENTS:
      focus_ = {Zone::Shelf, 0};
      break;
    case HomeMenuItem::FILE_TRANSFER:
      focus_ = {Zone::Tools, 0};
      break;
    case HomeMenuItem::OPDS_BROWSER:
      focus_ = {Zone::Tools, hasOpds_ ? 1 : 0};
      break;
    case HomeMenuItem::SETTINGS_MENU:
      focus_ = {Zone::Tools, toolCount() - 1};
      break;
    default:
      focus_ = zoneAvailable(Zone::Card) ? Focus{Zone::Card, 0} : Focus{Zone::Shelf, 0};
      break;
  }
  drawn_ = false;
  thumbsChecked_ = false;
  requestUpdate();
}

void CoverHomeActivity::onExit() {
  Activity::onExit();
  std::vector<RecentBook>().swap(recents_);
  favorites_.clear();
}

void CoverHomeActivity::moveFocus(const Focus& f) {
  if (f == focus_) return;
  focus_ = f;
  partialOk_.store(true);
  requestUpdate();
}

namespace {
constexpr CoverHomeActivity::Zone kOrder[] = {CoverHomeActivity::Zone::Shelf, CoverHomeActivity::Zone::Card,
                                              CoverHomeActivity::Zone::Recent, CoverHomeActivity::Zone::Tools};
constexpr int kZones = 4;
int zoneIndex(const CoverHomeActivity::Zone z) {
  for (int i = 0; i < kZones; ++i)
    if (kOrder[i] == z) return i;
  return 0;
}
}  // namespace

void CoverHomeActivity::step(const int dir) {
  // 畫面由上而下：書架 → 卡 → 最近閱讀 → 工具；走到最後一格再往後＝回第一格（反過來也一樣）
  Focus f = focus_;
  if (dir > 0 && f.index + 1 < zoneSize(f.zone)) {
    moveFocus({f.zone, f.index + 1});
    return;
  }
  if (dir < 0 && f.index > 0) {
    moveFocus({f.zone, f.index - 1});
    return;
  }
  int zi = zoneIndex(f.zone);
  for (int k = 0; k < kZones; ++k) {
    zi = (zi + (dir > 0 ? 1 : kZones - 1)) % kZones;
    if (zoneAvailable(kOrder[zi])) {
      moveFocus({kOrder[zi], dir > 0 ? 0 : zoneSize(kOrder[zi]) - 1});
      return;
    }
  }
}

void CoverHomeActivity::jumpZone(const int dir) {
  int zi = zoneIndex(focus_.zone);
  for (int k = 0; k < kZones; ++k) {
    zi = (zi + (dir > 0 ? 1 : kZones - 1)) % kZones;
    if (zoneAvailable(kOrder[zi])) {
      moveFocus({kOrder[zi], 0});
      return;
    }
  }
}

void CoverHomeActivity::activate() {
  switch (focus_.zone) {
    case Zone::Card:
      if (!recents_.empty()) activityManager.goToReader(recents_[0].path);
      return;
    case Zone::Recent:
      if (focus_.index + 1 < static_cast<int>(recents_.size()))
        activityManager.goToReader(recents_[focus_.index + 1].path);
      return;
    case Zone::Shelf:
      activityManager.goToBookshelf();
      return;
    case Zone::Tools:
      switch (toolAt(focus_.index)) {
        case Tool::Transfer:
          activityManager.goToFileTransfer();
          return;
        case Tool::Opds:
          activityManager.goToBrowser();
          return;
        case Tool::Settings:
          activityManager.goToSettings();
          return;
      }
  }
}

void CoverHomeActivity::openBookMenu(const int recentIndex) {
  if (recentIndex < 0 || recentIndex >= static_cast<int>(recents_.size())) return;
  const std::string path = recents_[recentIndex].path;
  const bool fav = favorites_.contains(Favorites::idOf(path));
  // 不能改的情況：較新版本的最愛（這次唯讀）、已滿 —— 選項寫原因，選了不做事
  const bool locked = !Favorites::writable(favLoad_);
  const bool full = !locked && !fav && favorites_.size() >= favorites::kMax;
  const char* options[] = {I18N.get(locked ? StrId::STR_FAVORITES_UNAVAILABLE
                                    : fav  ? StrId::STR_REMOVE_FROM_FAVORITES
                                    : full ? StrId::STR_FAVORITES_FULL
                                           : StrId::STR_ADD_TO_FAVORITES),
                           tr(STR_REMOVE_FROM_RECENTS_ACTION)};
  // 選單標題＝書名（截到螢幕寬的一半多一點，彈窗才不會撐滿）
  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, LyraTheme::displayTitleFor(recents_[recentIndex].title, path).c_str(),
                             renderer.getScreenWidth() * 2 / 3, EpdFontFamily::BOLD);
  partialOk_.store(false);  // 選單會蓋掉畫面：關掉之後整頁重畫
  backPressSeen_ = false;
  navNext_.reset();
  navPrev_.reset();
  menuPath_ = path;
  menuChoice_ = -1;
  menuCanToggle_ = !full && !locked;
  // 回呼只記下選了哪一項；真正的動作在 loop 裡、選單關掉之後
  menu_.show(title.c_str(), options, 2, 0, [this](const int idx) { menuChoice_ = idx; });
  requestUpdate();
}

// 書本選單的動作（codex 複查 v367 第三輪）：
//   ① 在同一段繪製鎖裡：選單關掉＋標記整頁重畫＋改最愛（NVS 寫入是毫秒級）—— render 插進來也只會看到一致的狀態整頁畫。
//   ② 從最近閱讀移除要寫 SD、重建清單要碰 SD（isMissing）：鎖外做完，鎖內只換掉成品。
void CoverHomeActivity::toggleFavoriteLocked() {
  const uint64_t id = Favorites::idOf(menuPath_);
  favorites_.toggle(id);
  if (!Favorites::save(favorites_, &saveInfo_)) favorites_.toggle(id);  // 存不進去：退回，畫面不假裝成功
}

void CoverHomeActivity::removeFromRecents() {
  if (!RECENT_BOOKS.removeByPath(menuPath_)) return;
  std::vector<RecentBook> fresh = collectRecents();
  RenderLock lock(*this);
  recents_.swap(fresh);
  if (!zoneAvailable(focus_.zone)) focus_ = zoneAvailable(Zone::Card) ? Focus{Zone::Card, 0} : Focus{Zone::Shelf, 0};
  if (focus_.zone == Zone::Recent) focus_.index = std::min(focus_.index, zoneSize(Zone::Recent) - 1);
  thumbsChecked_ = false;
  drawn_ = false;
}

void CoverHomeActivity::loop() {
  using B = MappedInputManager::Button;
  if (menu_.isActive()) {
    bool removeRecent = false;
    bool saved = false;
    {
      RenderLock lock(*this);
      menu_.handleInput(mappedInput, [this] { requestUpdate(); });
      if (!menu_.isActive()) {  // 選了或取消：整頁重畫（選單蓋掉的地方）＋做動作
        drawn_ = false;
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

  if (longPressFired_) {
    if (!mappedInput.isPressed(B::Confirm)) longPressFired_ = false;
    return;
  }
  const bool onBook = focus_.zone == Zone::Card || focus_.zone == Zone::Recent;
  const int bookIndex = focus_.zone == Zone::Card ? 0 : focus_.index + 1;
  if (onBook && confirmPressSeen_ && mappedInput.isPressed(B::Confirm) && millis() - confirmDownAt_ >= kLongPressMs) {
    longPressFired_ = true;
    confirmPressSeen_ = false;
    openBookMenu(bookIndex);
    return;
  }
  if (mappedInput.wasReleased(B::Confirm) && confirmPressSeen_) {
    confirmPressSeen_ = false;
    // 迴圈剛好在 1 秒那一刻被刷新卡住、恢復時已經放開：依按住時間判斷，不要誤開書
    if (onBook && millis() - confirmDownAt_ >= kLongPressMs) {
      openBookMenu(bookIndex);
      return;
    }
    activate();
    return;
  }
  // ⑤：繼續閱讀最近那本（所有主題一致）；backPressSeen 擋掉「從別的畫面按返回回來」那次放開
  if (mappedInput.wasReleased(B::Back) && backPressSeen_ && !recents_.empty()) {
    activityManager.goToReader(recents_[0].path);
    return;
  }

  // ⑦⑧／①④：短按一格（放開時）、長按換區（按住 0.5 秒起，每 0.5 秒一區）—— 同設定頁的長按換分類
  const uint32_t now = millis();
  switch (navNext_.update(mappedInput.wasPressed(B::NavNext), mappedInput.wasReleased(B::NavNext),
                          mappedInput.isPressed(B::NavNext), now)) {
    case HoldRepeat::Action::Short:
      step(+1);
      break;
    case HoldRepeat::Action::Long:
      jumpZone(+1);
      break;
    case HoldRepeat::Action::None:
      break;
  }
  switch (navPrev_.update(mappedInput.wasPressed(B::NavPrevious), mappedInput.wasReleased(B::NavPrevious),
                          mappedInput.isPressed(B::NavPrevious), now)) {
    case HoldRepeat::Action::Short:
      step(-1);
      break;
    case HoldRepeat::Action::Long:
      jumpZone(-1);
      break;
    case HoldRepeat::Action::None:
      break;
  }
}

int CoverHomeActivity::toolsTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - metrics.buttonHintsHeight - kToolsBottomGap - kToolRowH;
}

int CoverHomeActivity::recentTop() const {
  const int bandTop = kCardY + kCardH;
  return bandTop + (toolsTop() - bandTop - kRecentTileH) / 2;
}

void CoverHomeActivity::focusRect(const Focus& f, int& x, int& y, int& w, int& h) const {
  const int W = renderer.getScreenWidth();
  const int col = (W - 2 * kSide) / kRecentCount;
  switch (f.zone) {
    case Zone::Card:
      x = kSide, y = kCardY, w = W - 2 * kSide, h = kCardH;
      return;
    case Zone::Recent: {
      // 外圈粗框（v370）：離封面＋進度條 4 px 留白、3 px 框
      const int cx = kSide + col * f.index + (col - kRecentW) / 2;
      x = cx - kRingOut, y = recentTop() - kRingOut, w = kRecentW + 2 * kRingOut, h = kRecentTileH + 2 * kRingOut;
      return;
    }
    case Zone::Shelf:
      x = kSide, y = kShelfY, w = W - 2 * kSide, h = kShelfRowH;
      return;
    case Zone::Tools: {
      const int segW = (W - 2 * kSide) / toolCount();
      x = kSide + segW * f.index, y = toolsTop(), w = segW, h = kToolRowH;
      return;
    }
  }
}

// 選取的畫法（v370，一眼看得出來）：
//   卡片＝平常不畫框，選到才有 3 px 框（畫面上唯一的粗框就是選取）；最近閱讀＝封面外圈 3 px 框（離封面 4 px）；
//   書架列與工具格＝反白（黑底白字，同分頁列選中那一格）。on＝false：畫回沒選到的樣子（局部重畫擦掉舊的）。
void CoverHomeActivity::drawFocus(const Focus& f, const bool on) const {
  int x, y, w, h;
  focusRect(f, x, y, w, h);
  switch (f.zone) {
    case Zone::Card:
      renderer.drawSmoothRoundedRect(x, y, w, h, kRingW, kRadius, CoverTile::kSmoothing, on);
      return;
    case Zone::Recent:
      renderer.drawSmoothRoundedRect(x, y, w, h, kRingW, 18, CoverTile::kSmoothing, on);
      return;
    case Zone::Shelf:
      drawShelfRow(on);
      return;
    case Zone::Tools:
      drawToolCell(f.index, on);
      return;
  }
}

void CoverHomeActivity::drawShelfRow(const bool inverted) const {
  int sx, sy, sw, sh;
  focusRect({Zone::Shelf, 0}, sx, sy, sw, sh);
  // 外框不動：只畫內縮 4 px 的那一塊
  renderer.fillSmoothRoundedRect(sx + 4, sy + 4, sw - 8, kShelfRowH - 8, 12, CoverTile::kSmoothing,
                                 inverted ? Color::Black : Color::White);
  // 書架圖示：主題只有 32 px 的（v368 實機截圖：要 24 px 拿到空的 → 字前面一塊空白）
  constexpr int kShelfIcon = 32;
  int textX = sx + 20;
  if (const uint8_t* icon = LyraTheme::iconForName(Library, kShelfIcon)) {
    renderer.drawIcon(icon, sx + 16, sy + (kShelfRowH - kShelfIcon) / 2, kShelfIcon, !inverted);
    textX = sx + 16 + kShelfIcon + 10;
  }
  renderer.drawText(UI_12_FONT_ID, textX, sy + (kShelfRowH - renderer.getLineHeight(UI_12_FONT_ID)) / 2,
                    tr(STR_BOOKSHELF), !inverted, EpdFontFamily::BOLD);
}

void CoverHomeActivity::drawToolCell(const int i, const bool inverted) const {
  int tx, ty, tw, th;
  focusRect({Zone::Tools, i}, tx, ty, tw, th);
  renderer.fillSmoothRoundedRect(tx + 4, ty + 4, tw - 8, th - 8, 10, CoverTile::kSmoothing,
                                 inverted ? Color::Black : Color::White);
  const Tool tool = toolAt(i);
  const char* label = tool == Tool::Transfer ? tr(STR_FILE_TRANSFER)
                      : tool == Tool::Opds   ? tr(STR_OPDS_BROWSER)
                                             : tr(STR_SETTINGS_TITLE);
  // 工具格只寫字、置中（v368 減法）
  const int lw = renderer.getTextWidth(UI_10_FONT_ID, label);
  renderer.drawText(UI_10_FONT_ID, tx + (tw - lw) / 2, ty + (th - renderer.getLineHeight(UI_10_FONT_ID)) / 2, label,
                    !inverted);
}

void CoverHomeActivity::render(RenderLock&&) {
  // 書本選單：蓋在目前畫面上（framebuffer 裡就是首頁）
  if (menu_.isActive()) {
    partialOk_.store(false);
    menu_.processRender(renderer, mappedInput);
    return;
  }
  // 主任務隨時可能改 focus_：先消化 partialOk_（moveFocus 先寫 focus_ 再設它），再只讀一次 focus_（教訓 B-24）。
  //   drawnFocus_ 記的是實際畫上去的那個；畫面被別人蓋過（螢幕世代變了）就整頁重畫。
  const bool partial = partialOk_.exchange(false);
  const Focus focus = focus_;
  if (partial && drawn_ && drawnEpoch_ == activityManager.screenEpoch() && !(drawnFocus_ == focus)) {
    drawFocus(drawnFocus_, /*on=*/false);
    drawFocus(focus, /*on=*/true);
    drawnFocus_ = focus;
    renderer.displayBuffer();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int W = renderer.getScreenWidth();
  CoverTile::beginStats(renderer);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, W, metrics.homeTopPadding}, nullptr);

  // ① 正在閱讀卡（位置本身就說了「正在閱讀」，不另寫標籤；v370 起平常不畫框 —— 框只代表「選到了」）
  const int textX = kSide + 16 + kCardCoverW + 20;
  const int textW = W - kSide - 16 - textX;
  if (!recents_.empty()) {
    const RecentBook& cur = recents_[0];
    const std::string title = LyraTheme::displayTitleFor(cur.title, cur.path);
    CoverTile::draw(
        renderer, kSide + 16, kCardY + 16, kCardCoverW, kCardCoverH,
        cur.coverBmpPath.empty() ? std::string() : UITheme::getCoverThumbPath(cur.coverBmpPath, kCardCoverH), title,
        cur.author, favorites_.contains(Favorites::idOf(cur.path)));
    int ty = kCardY + 18;
    for (const std::string& line : renderer.wrappedText(UI_12_FONT_ID, title.c_str(), textW, 2, EpdFontFamily::BOLD)) {
      renderer.drawText(UI_12_FONT_ID, textX, ty, line.c_str(), true, EpdFontFamily::BOLD);
      ty += renderer.getLineHeight(UI_12_FONT_ID);
    }
    if (!cur.author.empty()) {
      const std::string a = renderer.truncatedText(UI_10_FONT_ID, cur.author.c_str(), textW);
      renderer.drawText(UI_10_FONT_ID, textX, ty + 2, a.c_str(), true);
    }
    // 閱讀統計（v365 起在記）：今天（X3 有可信時鐘才有）、這本；貼在進度條上方
    const int progressY = kCardY + kCardH - 50;
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    char dur[32];
    char line[64];
    uint32_t todaySec = 0;
    const bool hasToday = ReadingStats::todaySeconds(todaySec);
    const uint32_t bookSec = ReadingStats::bookSeconds(cur.path);
    int sy = progressY - (hasToday ? 1 : 0) * lineH - (bookSec > 0 ? 1 : 0) * lineH - 8;
    if (hasToday) {
      formatDuration(dur, sizeof(dur), todaySec);
      snprintf(line, sizeof(line), "%s　%s", tr(STR_STATS_TODAY), dur);
      renderer.drawText(UI_10_FONT_ID, textX, sy, line, true);
      sy += lineH;
    }
    if (bookSec > 0) {
      formatDuration(dur, sizeof(dur), bookSec);
      snprintf(line, sizeof(line), "%s　%s", tr(STR_STATS_THIS_BOOK), dur);
      renderer.drawText(UI_10_FONT_ID, textX, sy, line, true);
    }
    drawProgress(renderer, textX, progressY, textW, cur.progressPercent, 10);
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, kCardY + kCardH / 2 - 20, tr(STR_NO_RECENT_YET), true);
  }

  // ② 最近閱讀（封面＋細進度條；書名在封面上，不另寫）
  if (recents_.size() > 1) {
    const int col = (W - 2 * kSide) / kRecentCount;
    const int top = recentTop();
    for (int k = 0; k + 1 < static_cast<int>(recents_.size()); ++k) {
      const RecentBook& b = recents_[k + 1];
      const std::string title = LyraTheme::displayTitleFor(b.title, b.path);
      const int cx = kSide + col * k + (col - kRecentW) / 2;
      CoverTile::draw(renderer, cx, top, kRecentW, kRecentH,
                      b.coverBmpPath.empty() ? std::string() : UITheme::getCoverThumbPath(b.coverBmpPath, kRecentH),
                      title, b.author, favorites_.contains(Favorites::idOf(b.path)));
      drawThinBar(renderer, cx, top + kRecentH + kRecentBarGap, kRecentW, b.progressPercent);
    }
  }

  // ③ 書架列（頂端，正在閱讀卡上面）、工具列（貼在按鍵提示上方）：各自一張卡（v385 起分開）
  renderer.drawSmoothRoundedRect(kSide, kShelfY, W - 2 * kSide, kShelfRowH, 1, kRadius, CoverTile::kSmoothing, true);
  drawShelfRow(false);
  renderer.drawSmoothRoundedRect(kSide, toolsTop(), W - 2 * kSide, kToolRowH, 1, kRadius, CoverTile::kSmoothing, true);
  for (int i = 0; i < toolCount(); ++i) {
    int tx, ty, tw, th;
    focusRect({Zone::Tools, i}, tx, ty, tw, th);
    if (i > 0) renderer.drawLine(tx, ty, tx, ty + th - 1, true);
    drawToolCell(i, false);
  }

  drawFocus(focus, /*on=*/true);
  drawnFocus_ = focus;
  drawn_ = true;
  drawnEpoch_ = activityManager.screenEpoch();

  const auto labels = mappedInput.mapLabels(recents_.empty() ? "" : tr(STR_RESUME), tr(STR_OPEN), "‹", "›");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  CoverTile::logStats(renderer, "home", 0, 0);
  renderer.displayBuffer();

  // 畫面先上，再補產縮圖（正在閱讀 240、最近閱讀 192）；產了就要求整頁重畫。每次進首頁檢查一次。
  if (!thumbsChecked_) {
    thumbsChecked_ = true;
    const auto changed = [this] {
      drawn_ = false;
      requestUpdate();
    };
    if (!recents_.empty()) {
      std::vector<RecentBook> card(recents_.begin(), recents_.begin() + 1);
      RecentCoverLoader::ensureThumbs(renderer, card, kCardCoverH, changed);
    }
    if (recents_.size() > 1) {
      std::vector<RecentBook> rest(recents_.begin() + 1, recents_.end());
      RecentCoverLoader::ensureThumbs(renderer, rest, kRecentH, changed);
    }
  }
}
