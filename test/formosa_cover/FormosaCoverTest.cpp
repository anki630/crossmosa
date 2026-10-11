// Formosa Cover（2026-10-07）：書架索引、書名排版、歐風底框資料。把對應的那一段改壞，這裡要變紅（交付前逐條注入過）。

#include <InflateReader.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "activities/home/ShelfIndex.h"
#include "components/covers/CoverFrames.h"
#include "components/covers/CoverTitleLayout.h"
#include "util/FavoriteSet.h"
#include "util/HoldRepeat.h"

// uzlib 的檢查碼只有 uzlib_uncompress_chksum 會用到（我們的一次解壓不呼叫）；裝置上由別的函式庫提供，測試給替身。
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t prev) { return prev; }
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t crc) { return crc; }

// ---------------- ShelfIndex ----------------

TEST(ShelfIndex, RecentsFirstThenByFileName) {
  ShelfIndex idx;
  ASSERT_TRUE(idx.begin(24 * 1024));
  idx.add("/books/c.epub");
  idx.add("/books/a.epub");
  idx.add("/Read/b.epub");
  idx.add("/books/z.txt");
  idx.sort({"/books/z.txt", "/books/c.epub"});  // 最近的在前
  ASSERT_EQ(idx.size(), 4u);
  EXPECT_STREQ(idx.path(0), "/books/z.txt");
  EXPECT_STREQ(idx.path(1), "/books/c.epub");
  EXPECT_STREQ(idx.path(2), "/books/a.epub");  // 其他照檔名（不看資料夾）
  EXPECT_STREQ(idx.path(3), "/Read/b.epub");
  EXPECT_TRUE(idx.isRecent(0, 2));
  EXPECT_TRUE(idx.isRecent(1, 2));
  EXPECT_FALSE(idx.isRecent(2, 2));
}

TEST(ShelfIndex, StopsAtTheBookCapAndSaysSo) {  // 記憶體不隨書量成長：超過上限就停
  ShelfIndex idx;
  ASSERT_TRUE(idx.begin(24 * 1024));
  size_t added = 0;
  for (int i = 0; i < 1000; ++i) {
    if (idx.add("/books/" + std::to_string(i) + ".epub")) ++added;
  }
  EXPECT_EQ(added, ShelfIndex::kMaxBooks);
  EXPECT_EQ(idx.size(), ShelfIndex::kMaxBooks);
  EXPECT_TRUE(idx.capped());
}

TEST(ShelfIndex, StopsAtTheArenaCapWithLongNames) {
  ShelfIndex idx;
  ASSERT_TRUE(idx.begin(4096));
  const std::string longName = "/books/" + std::string(200, 'x') + ".epub";
  size_t added = 0;
  while (idx.add(longName)) ++added;
  EXPECT_GT(added, 0u);
  EXPECT_LE(idx.arenaBytes(), idx.arenaCapacity());
  EXPECT_TRUE(idx.capped());
}

TEST(ShelfIndex, TinyBudgetFailsCleanlyInsteadOfCrashing) {  // 配不到 → 回 false，之後 add 不會寫進去
  ShelfIndex idx;
  EXPECT_FALSE(idx.begin(1000));  // 小於最小 2 KB
  EXPECT_FALSE(idx.add("/books/a.epub"));
  EXPECT_EQ(idx.size(), 0u);
}

TEST(ShelfIndex, ReleaseResetsEverything) {
  ShelfIndex idx;
  ASSERT_TRUE(idx.begin(8192));
  idx.add("/books/a.epub");
  idx.release();
  EXPECT_EQ(idx.size(), 0u);
  EXPECT_FALSE(idx.capped());
  EXPECT_EQ(idx.arenaCapacity(), 0u);
}

TEST(ShelfIndex, PinnedRecentsSurviveTheCapAndAreNotDuplicated) {
  // 最近閱讀的書在掃描順序的最後、而且掃描撞到上限：它還是要在書架上、排第一頁，而且只出現一次
  ShelfIndex idx;
  ASSERT_TRUE(idx.begin(24 * 1024));
  const std::string recent = "/zzz/最後才掃到.epub";
  ASSERT_TRUE(idx.addPinned(recent.c_str(), recent.size()));
  for (int i = 0; i < 1000; ++i) {
    if (!idx.add("/books/" + std::to_string(i) + ".epub")) break;
  }
  EXPECT_TRUE(idx.add(recent));  // 掃到同一本：不重複放（回 true，掃描繼續）
  EXPECT_TRUE(idx.capped());
  idx.sort({recent});
  EXPECT_STREQ(idx.path(0), recent.c_str());
  size_t copies = 0;
  for (size_t i = 0; i < idx.size(); ++i) copies += std::string(idx.path(i)) == recent ? 1 : 0;
  EXPECT_EQ(copies, 1u);
}

TEST(ShelfIndex, DuplicateOfPinnedIsSkippedBeforeTheCap) {
  ShelfIndex idx;
  ASSERT_TRUE(idx.begin(24 * 1024));
  ASSERT_TRUE(idx.addPinned("/books/a.epub", 13));
  EXPECT_TRUE(idx.add("/books/a.epub"));
  EXPECT_TRUE(idx.add("/books/b.epub"));
  EXPECT_TRUE(idx.addPinned("/books/a.epub", 13));  // 重複 pin 也不留兩份
  EXPECT_EQ(idx.size(), 2u);
  EXPECT_FALSE(idx.capped());
}

TEST(ShelfIndex, TitleFromPathDropsFolderAndExtension) {
  EXPECT_EQ(ShelfIndex::titleFromPath("/books/浮生六記.epub"), "浮生六記");
  EXPECT_EQ(ShelfIndex::titleFromPath("/a.b/c.d.txt"), "c.d");
  EXPECT_EQ(ShelfIndex::titleFromPath("/books/.hidden"), ".hidden");  // 開頭的點不是副檔名
  EXPECT_EQ(ShelfIndex::titleFromPath("noext"), "noext");
}

TEST(ShelfIndex, FavoritesViewKeepsOnlyFavoritesInTheSameOrder) {  // 「最愛」分頁：只留最愛，順序規則同「全部」
  ShelfIndex idx;
  ASSERT_TRUE(idx.begin(24 * 1024));
  for (const char* p : {"/books/c.epub", "/books/a.epub", "/books/b.epub", "/books/d.txt"}) idx.add(p);
  const std::vector<std::string> recents = {"/books/d.txt"};
  const std::vector<std::string> favs = {"/books/c.epub", "/books/d.txt", "/books/gone.epub"};
  const auto isFav = [&](const char* p) { return std::find(favs.begin(), favs.end(), std::string(p)) != favs.end(); };
  idx.sort(recents, isFav, false);
  ASSERT_EQ(idx.size(), 4u);
  EXPECT_EQ(idx.total(), 4u);
  EXPECT_STREQ(idx.path(0), "/books/d.txt");
  EXPECT_TRUE(idx.isFavorite(0));
  EXPECT_FALSE(idx.isFavorite(1));  // a.epub
  idx.sort(recents, isFav, true);
  ASSERT_EQ(idx.size(), 2u);  // 不在卡上的最愛（gone.epub）不出現
  EXPECT_EQ(idx.total(), 4u);
  EXPECT_STREQ(idx.path(0), "/books/d.txt");  // 最近閱讀仍在前
  EXPECT_STREQ(idx.path(1), "/books/c.epub");
  EXPECT_TRUE(idx.isFavorite(1));
  EXPECT_EQ(idx.favoriteCount(), 2u);  // 卡上找得到的最愛（gone.epub 不算）：決定要不要顯示「最愛」分頁
  idx.sort(recents, isFav, false);     // 切回「全部」：四本都回來
  EXPECT_EQ(idx.size(), 4u);
}

// ---------------- FavoriteSet（NVS blob "fav"）----------------

namespace {
using favorites::FavoriteSet;
using favorites::LoadResult;
using favorites::Toggle;
}  // namespace

TEST(FavoriteSet, ToggleAddsToFrontAndRemoves) {
  FavoriteSet f;
  EXPECT_EQ(f.toggle(11), Toggle::Added);
  EXPECT_EQ(f.toggle(22), Toggle::Added);
  ASSERT_EQ(f.size(), 2u);
  EXPECT_EQ(f.at(0), 22u);  // 最近加入的在前
  EXPECT_TRUE(f.contains(11));
  EXPECT_EQ(f.toggle(11), Toggle::Removed);
  EXPECT_FALSE(f.contains(11));
  EXPECT_EQ(f.size(), 1u);
  EXPECT_EQ(f.toggle(0), Toggle::Full);  // 0 不是合法身分
  EXPECT_EQ(f.size(), 1u);
}

TEST(FavoriteSet, FullRefusesWithoutChanging) {
  FavoriteSet f;
  for (uint64_t i = 1; i <= favorites::kMax; ++i) ASSERT_EQ(f.toggle(i), Toggle::Added);
  EXPECT_EQ(f.toggle(999), Toggle::Full);
  EXPECT_EQ(f.size(), favorites::kMax);
  EXPECT_FALSE(f.contains(999));
  EXPECT_EQ(f.toggle(1), Toggle::Removed);  // 滿了仍然可以移除
}

TEST(FavoriteSet, RenameKeepsPositionAndNeverDuplicates) {
  FavoriteSet f;
  f.toggle(5);
  f.toggle(7);  // [7, 5]
  EXPECT_TRUE(f.rename(7, 70));
  EXPECT_EQ(f.at(0), 70u);
  EXPECT_FALSE(f.rename(123, 4));
  EXPECT_TRUE(f.rename(70, 5));  // 新身分已在：只留一份
  EXPECT_EQ(f.size(), 1u);
  EXPECT_TRUE(f.remove(5));
  EXPECT_FALSE(f.remove(5));
}

TEST(FavoriteSet, RetainKeepsOrder) {
  FavoriteSet f;
  for (uint64_t i = 1; i <= 5; ++i) f.toggle(i);  // [5,4,3,2,1]
  EXPECT_TRUE(f.retain([](uint64_t id) { return id % 2 == 1; }));
  ASSERT_EQ(f.size(), 3u);
  EXPECT_EQ(f.at(0), 5u);
  EXPECT_EQ(f.at(2), 1u);
  EXPECT_FALSE(f.retain([](uint64_t) { return true; }));
}

TEST(FavoriteSet, EncodeDecodeRoundTripIncludingFull) {
  FavoriteSet f;
  for (uint64_t i = 1; i <= favorites::kMax; ++i) f.toggle(0x0123456789ABCDEFull ^ i);
  uint8_t buf[favorites::kMaxBlob];
  const size_t len = f.encode(buf);
  EXPECT_EQ(len, favorites::kMaxBlob);
  FavoriteSet g;
  ASSERT_EQ(g.decode(buf, len), LoadResult::Compatible);
  ASSERT_EQ(g.size(), f.size());
  for (size_t i = 0; i < f.size(); ++i) EXPECT_EQ(g.at(i), f.at(i));
  FavoriteSet e;
  EXPECT_EQ(e.decode(buf, e.encode(buf)), LoadResult::Compatible);  // 空清單
}

TEST(FavoriteSet, DecodeTriStateAndLeavesSetUntouchedOnFailure) {
  FavoriteSet f;
  f.toggle(42);
  f.toggle(43);
  uint8_t buf[favorites::kMaxBlob];
  const size_t len = f.encode(buf);
  FavoriteSet g;
  g.toggle(9);
  uint8_t b[favorites::kMaxBlob];
  const auto with = [&](size_t at, uint8_t v) {
    std::copy(buf, buf + len, b);
    b[at] = v;
  };
  with(0, 'X');
  EXPECT_EQ(g.decode(b, len), LoadResult::Corrupt);  // 錯的 magic
  with(1, 2);
  EXPECT_EQ(g.decode(b, len), LoadResult::Newer);  // 較新版本：唯讀
  with(1, 0);
  EXPECT_EQ(g.decode(b, len), LoadResult::Corrupt);
  with(2, 3);
  EXPECT_EQ(g.decode(b, len), LoadResult::Corrupt);  // 本數跟長度對不上
  with(2, 101);
  EXPECT_EQ(g.decode(b, len), LoadResult::Corrupt);
  with(3, 1);
  EXPECT_EQ(g.decode(b, len), LoadResult::Corrupt);
  EXPECT_EQ(g.decode(buf, len - 1), LoadResult::Corrupt);  // 短讀
  EXPECT_EQ(g.decode(buf, 3), LoadResult::Corrupt);
  std::copy(buf, buf + len, b);
  std::fill(b + 4, b + 12, 0);  // 身分 0
  EXPECT_EQ(g.decode(b, len), LoadResult::Corrupt);
  std::copy(buf, buf + len, b);
  std::copy(b + 4, b + 12, b + 12);  // 兩筆一樣
  EXPECT_EQ(g.decode(b, len), LoadResult::Corrupt);
  ASSERT_EQ(g.size(), 1u);  // 失敗不動原本的
  EXPECT_EQ(g.at(0), 9u);
}

TEST(FavoriteSet, EveryCorruptionOfEveryByteDecodesSafely) {  // 故障注入：每個位元組換成每個值，不當機、不越界
  FavoriteSet f;
  for (uint64_t i = 1; i <= 3; ++i) f.toggle(i * 0x1111);
  uint8_t buf[favorites::kMaxBlob];
  const size_t len = f.encode(buf);
  for (size_t at = 0; at < len; ++at) {
    for (int v = 0; v < 256; ++v) {
      uint8_t b[favorites::kMaxBlob];
      std::copy(buf, buf + len, b);
      b[at] = static_cast<uint8_t>(v);
      FavoriteSet g;
      const LoadResult r = g.decode(b, len);
      if (r == LoadResult::Compatible) {
        EXPECT_LE(g.size(), favorites::kMax);
      } else {
        EXPECT_EQ(g.size(), 0u);
      }
    }
  }
}

// ---------------- HoldRepeat ----------------

namespace {
using A = HoldRepeat::Action;
}

TEST(HoldRepeat, ShortPressFiresOnReleaseOnly) {
  HoldRepeat h;
  EXPECT_EQ(h.update(true, false, true, 1000), A::None);
  EXPECT_EQ(h.update(false, false, true, 1200), A::None);
  EXPECT_EQ(h.update(false, true, false, 1300), A::Short);
  EXPECT_EQ(h.update(false, false, false, 1400), A::None);
}

TEST(HoldRepeat, LongPressRepeatsAndSwallowsTheRelease) {
  HoldRepeat h;
  h.update(true, false, true, 0);
  EXPECT_EQ(h.update(false, false, true, 499), A::None);
  EXPECT_EQ(h.update(false, false, true, 500), A::Long);
  EXPECT_EQ(h.update(false, false, true, 700), A::None);
  EXPECT_EQ(h.update(false, false, true, 1000), A::Long);  // 每 0.5 秒一次
  EXPECT_EQ(h.update(false, true, false, 1100), A::None);  // 放開不再多走一格
}

TEST(HoldRepeat, ShorterStartForTheTabBar) {  // v380：分頁列 0.3 秒；短按（0.1–0.15 秒）照樣是短按
  HoldRepeat h;
  h.update(true, false, true, 0, HoldRepeat::kTabStartMs);
  EXPECT_EQ(h.update(false, false, true, 299, HoldRepeat::kTabStartMs), A::None);
  EXPECT_EQ(h.update(false, false, true, 300, HoldRepeat::kTabStartMs), A::Long);
  EXPECT_EQ(h.update(false, true, false, 400, HoldRepeat::kTabStartMs), A::None);
  h.update(true, false, true, 1000, HoldRepeat::kTabStartMs);
  EXPECT_EQ(h.update(false, true, false, 1150, HoldRepeat::kTabStartMs), A::Short);
}

TEST(HoldRepeat, ReleaseOfAKeyPressedElsewhereIsIgnored) {  // 帶著按住的鍵進畫面／從彈窗回來
  HoldRepeat h;
  EXPECT_EQ(h.update(false, false, true, 0), A::None);
  EXPECT_EQ(h.update(false, false, true, 900), A::None);   // 沒看過按下：不會長按
  EXPECT_EQ(h.update(false, true, false, 1000), A::None);  // 也不會短按
  h.update(true, false, true, 2000);
  h.reset();  // 彈窗打開
  EXPECT_EQ(h.update(false, true, false, 2100), A::None);
}

TEST(HoldRepeat, OwnTimerNotTheGlobalOne) {  // 先按住別的鍵很久，再按這顆：照這顆自己的按下時間算
  HoldRepeat h;
  EXPECT_EQ(h.update(true, false, true, 5000), A::None);
  EXPECT_EQ(h.update(false, false, true, 5100), A::None);
  EXPECT_EQ(h.update(false, true, false, 5150), A::Short);
}

TEST(HoldRepeat, MissedReleaseEdgeForgetsThePress) {
  HoldRepeat h;
  h.update(true, false, true, 0);
  EXPECT_EQ(h.update(false, false, false, 100), A::None);  // 電平已放開但沒看到邊緣
  EXPECT_EQ(h.update(false, true, false, 200), A::None);
}

// ---------------- CoverTitleLayout ----------------

namespace {
// 等寬假字型：大字一個字 29 px、小字 21 px（跟 UI 字型的中文字寬一樣），行高 34／24
int fakeWidth(const std::string& s, bool big) {
  int n = 0;
  for (const auto& c : covertitle::splitChars(s)) n += (c == "…") ? 1 : 1;
  return n * (big ? 29 : 21);
}
int fakeLineH(bool big) { return big ? 34 : 24; }
}  // namespace

TEST(CoverTitleLayout, BalancedLinesNoOrphan) {  // 「聊齋誌異」→ 2＋2，不是 3＋1
  const auto lay = covertitle::fit("聊齋誌異", 100, 200, 0, fakeWidth, fakeLineH);
  EXPECT_TRUE(lay.big);
  ASSERT_EQ(lay.lines.size(), 2u);
  EXPECT_EQ(lay.lines[0], "聊齋");
  EXPECT_EQ(lay.lines[1], "誌異");
}

TEST(CoverTitleLayout, SevenCharsOnThreeLinesIsThreeTwoTwo) {
  const auto lay = covertitle::fit("三寶太監西洋記", 90, 300, 0, fakeWidth, fakeLineH);
  EXPECT_TRUE(lay.big);
  ASSERT_EQ(lay.lines.size(), 3u);
  EXPECT_EQ(lay.lines[0], "三寶太");
  EXPECT_EQ(lay.lines[1], "監西");
  EXPECT_EQ(lay.lines[2], "洋記");
}

TEST(CoverTitleLayout, LatinWordsAreNeverSplitWhenTheyFit) {  // v370 實機截圖：「Sample B / ook 0176」
  // 假字型每個字元一樣寬（大 29、小 21）；寬 130：小字一行 6 字元
  const auto lay = covertitle::fit("Sample Book 0176", 130, 200, 0, fakeWidth, fakeLineH);
  ASSERT_EQ(lay.lines.size(), 3u);
  EXPECT_FALSE(lay.big);
  EXPECT_EQ(lay.lines[0], "Sample");
  EXPECT_EQ(lay.lines[1], "Book");
  EXPECT_EQ(lay.lines[2], "0176");
}

TEST(CoverTitleLayout, MixedChineseAndLatinBreaksAtBoundaries) {
  const auto lay = covertitle::fit("範例書坊 Books", 29 * 5, 200, 0, fakeWidth, fakeLineH);
  ASSERT_EQ(lay.lines.size(), 2u);
  EXPECT_EQ(lay.lines[0], "範例書坊");  // 空白不畫在行頭行尾
  EXPECT_EQ(lay.lines[1], "Books");
}

TEST(CoverTitleLayout, OverlongWordFallsBackToCharacters) {
  const auto lay = covertitle::fit("Supercalifragilistic", 21 * 7, 200, 0, fakeWidth, fakeLineH);
  ASSERT_FALSE(lay.lines.empty());
  for (const auto& l : lay.lines) EXPECT_LE(fakeWidth(l, lay.big), 21 * 7);
}

TEST(CoverTitleLayout, ShortTitleOnOneLine) {
  const auto lay = covertitle::fit("史記", 100, 200, 0, fakeWidth, fakeLineH);
  EXPECT_TRUE(lay.big);
  ASSERT_EQ(lay.lines.size(), 1u);
}

TEST(CoverTitleLayout, FallsBackToSmallFontWhenTooTall) {  // 看高度不只看寬度
  // 大字 3 行要 96 px（＋作者 28），放不下 100；小字 2 行 44＋28 放得下
  const auto lay = covertitle::fit("三寶太監西洋記", 90, 100, 28, fakeWidth, fakeLineH);
  EXPECT_FALSE(lay.big);
  for (const auto& l : lay.lines) EXPECT_LE(fakeWidth(l, false), 90);
}

TEST(CoverTitleLayout, TruncatesWithEllipsisWhenNothingFits) {
  const auto lay = covertitle::fit("三寶太監西洋記通俗演義後傳續集", 60, 50, 0, fakeWidth, fakeLineH);
  EXPECT_FALSE(lay.big);
  ASSERT_FALSE(lay.lines.empty());
  EXPECT_LE(static_cast<int>(lay.lines.size()) * (24 - 2), 50);
  const std::string& last = lay.lines.back();
  EXPECT_EQ(last.substr(last.size() - 3), "…");
  for (const auto& l : lay.lines) EXPECT_LE(fakeWidth(l, false), 60);
}

// v378 複查：英文在空白換行是正常的換行，不是斷字 —— 書名完整、高度夠，就要放作者
TEST(CoverTitleLayout, AuthorStaysWhenAnEnglishTitleBreaksAtSpaces) {
  bool showAuthor = false;
  // 等寬假字：6 字寬 126 px；三行小字 66＋作者 28 放得下 200
  const auto lay = covertitle::fitWithAuthor("Sample Book 0166", 130, 200, 28, fakeWidth, fakeLineH, showAuthor);
  EXPECT_TRUE(lay.complete);
  EXPECT_GE(lay.lines.size(), 2u);
  EXPECT_TRUE(showAuthor);
}

// 截斷時，只有拿掉作者真的露出更多字才拿掉；兩邊都截在第三行就留作者
TEST(CoverTitleLayout, AuthorStaysWhenDroppingItWouldNotShowMoreTitle) {
  bool showAuthor = false;
  const auto lay =
      covertitle::fitWithAuthor("三寶太監西洋記通俗演義後傳續集", 93, 139, 28, fakeWidth, fakeLineH, showAuthor);
  EXPECT_FALSE(lay.complete);
  EXPECT_TRUE(showAuthor);
}

// 帶作者會截斷、不帶作者放得完：拿掉作者（書名優先）
TEST(CoverTitleLayout, AuthorGoesWhenItWouldCostTheTitle) {
  bool showAuthor = true;
  // 寬而矮：一行大字 32＋作者 28 > 50 → 帶作者只能小字一行（截斷）；不帶作者小字兩行放得完
  const auto lay = covertitle::fitWithAuthor("三寶太監西洋記", 90 + 21 * 2, 50, 28, fakeWidth, fakeLineH, showAuthor);
  EXPECT_TRUE(lay.complete);
  EXPECT_FALSE(showAuthor);
}

// 書名前後與中間多餘的空白不吃 48 字上限、也不會讓完整的書名被補上「…」
TEST(CoverTitleLayout, SpacesAreNormalizedBeforeTheLengthCap) {
  const auto a = covertitle::fit(std::string(60, ' ') + "史記" + "  ", 200, 200, 0, fakeWidth, fakeLineH);
  ASSERT_EQ(a.lines.size(), 1u);
  EXPECT_EQ(a.lines[0], "史記");
  EXPECT_TRUE(a.complete);
  const auto b = covertitle::fit("   ", 200, 200, 0, fakeWidth, fakeLineH);
  EXPECT_TRUE(b.lines.empty());
  std::string wide;  // 全形空白與不斷行空白也一樣（第二輪複查）
  for (int i = 0; i < 48; ++i) wide += "\xE3\x80\x80";
  const auto c = covertitle::fit(wide + "史記\xC2\xA0", 200, 200, 0, fakeWidth, fakeLineH);
  ASSERT_EQ(c.lines.size(), 1u);
  EXPECT_EQ(c.lines[0], "史記");
  EXPECT_TRUE(c.complete);
}

TEST(CoverTitleLayout, SplitCharsAlwaysAdvancesOnBadBytes) {  // 壞位元組一個算一個字元（不卡住）
  const std::string bad = std::string("\xE4\xB8", 2) + std::string(1, '\0') + "a";
  const auto chars = covertitle::splitChars(bad);
  EXPECT_EQ(chars.size(), 4u);
}

TEST(CoverTitleLayout, OverlongTitleIsClippedAndEndsWithEllipsis) {  // 書名長度有上限：只拆前 kMaxTitleChars 個字
  std::string longTitle;
  for (int i = 0; i < 500; ++i) longTitle += "字";
  EXPECT_EQ(covertitle::splitChars(longTitle).size(), covertitle::kMaxTitleChars);
  // 寬到放得下 48 字也一樣要「…」（後面還有字沒排）
  const auto lay = covertitle::fit(longTitle, 29 * 20, 400, 0, fakeWidth, fakeLineH);
  ASSERT_FALSE(lay.lines.empty());
  const std::string& last = lay.lines.back();
  EXPECT_EQ(last.substr(last.size() - 3), "…");
}

// ---------------- CoverFrames ----------------

TEST(CoverFrames, EveryFrameInflatesToItsSizeAndHasAWhiteTitleBox) {
  for (int i = 0; i < coverframes::kStyles * coverframes::kSizes; ++i) {
    const coverframes::Frame& f = coverframes::kFrames[i];
    const size_t stride = (f.width + 7u) / 8u;
    std::vector<uint8_t> buf(stride * f.height);
    InflateReader r;
    r.init(false);
    r.setSource(f.deflated, f.deflatedLen);
    ASSERT_TRUE(r.read(buf.data(), buf.size())) << "frame " << i;
    r.deinit();
    const auto black = [&](int x, int y) { return (buf[y * stride + x / 8] & (0x80 >> (x % 8))) != 0; };
    int blacks = 0;
    for (int y = 0; y < f.height; ++y)
      for (int x = 0; x < f.width; ++x) blacks += black(x, y) ? 1 : 0;
    EXPECT_GT(blacks, f.width * f.height / 20) << "frame " << i << " 幾乎全白：底框沒畫出來";
    // v370：四周 4 px 留白（圓角遮罩只切到白邊，花紋的直角框線不會被切出缺口）
    for (int y = 0; y < f.height; ++y)
      for (int x = 0; x < f.width; ++x)
        if (x < 4 || y < 4 || x >= f.width - 4 || y >= f.height - 4)
          ASSERT_FALSE(black(x, y)) << "frame " << i << " 邊緣 4 px 內有黑點 (" << x << "," << y << ")";
    ASSERT_LT(f.textX + f.textW, f.width);
    ASSERT_LT(f.textY + f.textH, f.height);
    for (int y = f.textY + 1; y < f.textY + f.textH; ++y)
      for (int x = f.textX + 1; x < f.textX + f.textW; ++x)
        ASSERT_FALSE(black(x, y)) << "frame " << i << " 書名區有黑點 (" << x << "," << y << ")";
  }
}

TEST(CoverFrames, ThreeSizesPerStyleInTheDocumentedOrder) {
  EXPECT_EQ(coverframes::kStyles,
            17);  // v373：歐風三款＋十款（北歐四、原住民、貓、柴犬、勇者魔龍、公主、纏繞畫）＋公有領域四款
  const int sizes[3][2] = {{160, 240}, {144, 216}, {128, 192}};
  for (int s = 0; s < coverframes::kStyles; ++s)
    for (int z = 0; z < coverframes::kSizes; ++z) {
      EXPECT_EQ(coverframes::kFrames[s * coverframes::kSizes + z].width, sizes[z][0]);
      EXPECT_EQ(coverframes::kFrames[s * coverframes::kSizes + z].height, sizes[z][1]);
    }
}

// ---------------- 書名排版 × 底框 × 真字型（v378） ----------------
// 上面的排版測試用假字寬；這裡用裝置上真的 UI 字面（14／10 號粗體），照 FallbackCover::draw 的算法（左右各留
// 4、上下各留 3、 作者一行＝間隔 6＋10 號行高−2）對每一款每一個尺寸排一次。v373–v377
// 書架的花卉款與最近閱讀的古典款書名區太窄， 「Sample」10 號粗也要 74 px →
// 從字中間斷成「Sampl／e」（維護者實機截圖）；假字寬的測試全綠。
#include "builtinFonts/ubuntu_10_bold.h"
#include "builtinFonts/ubuntu_10_regular.h"
#include "builtinFonts/ubuntu_14_bold.h"
#include "builtinFonts/ubuntu_14_regular.h"
#include "lib/EpdFont/EpdFont.h"

TEST(CoverFrames, AnOrdinaryEnglishTitleIsNeverSplitMidWordInAnyFrame) {
  const EpdFont big(&ubuntu_14_bold);
  const EpdFont small(&ubuntu_10_bold);
  const auto width = [&](const std::string& s, const bool b) {
    int w = 0, h = 0;
    (b ? big : small).getTextDimensions(s.c_str(), &w, &h);
    return w;
  };
  // GfxRenderer::getLineHeight 讀的是正體字面的 advanceY
  const auto lineH = [](const bool b) { return b ? ubuntu_14_regular.advanceY : ubuntu_10_regular.advanceY; };
  const int authorH = 6 + ubuntu_10_regular.advanceY - 2;
  const std::string title = "Sample Book 0166";
  for (int i = 0; i < coverframes::kStyles * coverframes::kSizes; ++i) {
    const coverframes::Frame& f = coverframes::kFrames[i];
    for (const int a : {0, authorH}) {
      bool showAuthor = false;
      const auto lay = covertitle::fitWithAuthor(title, f.textW - 8, f.textH - 6, a, width, lineH, showAuthor);
      std::string joined;
      for (const auto& l : lay.lines) joined += (joined.empty() ? "" : " ") + l;
      // 放得下就全部放；放不下（最近閱讀尺寸的書名區只有兩行高）只能在單字之間截斷：「Sample／Book…」，不能「Sampl／e」
      const std::string ell = "\xE2\x80\xA6";
      if (joined.size() >= ell.size() && joined.compare(joined.size() - ell.size(), ell.size(), ell) == 0) {
        const std::string kept = joined.substr(0, joined.size() - ell.size());
        EXPECT_EQ(title.compare(0, kept.size(), kept), 0) << "frame " << i << ": " << joined;
        EXPECT_TRUE(kept.size() == title.size() || title[kept.size()] == ' ') << "frame " << i << ": " << joined;
      } else {
        EXPECT_EQ(joined, title) << "frame " << i << " (" << f.width << "x" << f.height << ") author=" << a;
      }
      EXPECT_FALSE(showAuthor && !lay.complete) << "frame " << i << "：書名優先，截斷時不放作者";
      // 反方向（v378 複查）：花卉 144（第 7 格）三行小字加作者 94 px 放得下 139 —— 正常的英文換行不能讓作者消失
      if (i == 7 && a > 0) EXPECT_TRUE(showAuthor) << "frame 7";
    }
  }
}
