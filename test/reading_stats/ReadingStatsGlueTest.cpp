// 閱讀統計・第一步（2026-10-07）：韌體的同一個 src/util/ReadingStats.cpp，NVS／時鐘／設定／log 換成 stubs/ 的替身。
// 每一條對應 codex 程式碼複查點出的整合不變量；把對應的那一段改壞，這裡要變紅。

#include <gtest/gtest.h>

#include <cstring>

#include "Arduino.h"
#include "CrossPointSettings.h"
#include "HalClock.h"
#include "hal/ClockCache.h"
#include "util/DiagLog.h"
#include "util/NvsStore.h"
#include "util/ReadingStats.h"
#include "util/ReadingStatsCore.h"

namespace {

const uint8_t kCid1[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const uint8_t kCid2[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
const std::string kBook = "/Books/浮生六記.epub";

uint32_t clockEpoch(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s) {
  return clockcache::epochOf(y, mo, d, h, mi, s);
}

class Glue : public ::testing::Test {
 protected:
  void SetUp() override {
    ReadingStats::resetForTest();
    NvsStore::store.clear();
    NvsStore::failPuts = 0;
    NvsStore::getErr = 0;
    NvsStore::puts = 0;
    DiagLog::last.clear();
    DiagLog::lines = 0;
    halClock = HalClock{};
    SETTINGS = FakeSettings{};
    fakers::nowMs = 1000;
  }

  // 讀一頁 secs 秒後翻到下一頁
  static void readPages(const std::string& path, uint32_t fromPage, int pages, uint32_t secs) {
    ReadingStats::observe(path, 0, fromPage, 0);
    for (int i = 1; i <= pages; ++i) {
      fakers::nowMs += secs * 1000u;
      ReadingStats::observe(path, 0, fromPage + i, 0);
    }
  }

  static readingstats::Snapshot stored() {
    readingstats::Snapshot s;
    const auto& v = NvsStore::store.at("rstat");
    EXPECT_EQ(readingstats::decode(v.data(), v.size(), s), readingstats::LoadResult::Compatible);
    return s;
  }

  static uint64_t expectedKey(const uint8_t* cid, const std::string& path) {
    return readingstats::fnv64(path.data(), path.size(), readingstats::fnv64(cid, 16));
  }
};

}  // namespace

TEST_F(Glue, FirstSaveWritesTheWholeSnapshot) {
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 3, 20);
  ReadingStats::endSession();
  ReadingStats::save("exit");
  const auto s = stored();
  EXPECT_EQ(s.totalSec, 60u);
  EXPECT_EQ(s.books[0].key, expectedKey(kCid1, kBook));  // 持久的身分契約：FNV64(路徑, 種子 FNV64(CID))
  EXPECT_EQ(s.books[0].seconds, 60u);
  EXPECT_EQ(s.books[0].navs, 3u);
  EXPECT_NE(DiagLog::last.find("RSTAT save why=exit dirty=1 ok=1"), std::string::npos) << DiagLog::last;
  EXPECT_NE(DiagLog::last.find("load=absent blocked=0"), std::string::npos) << DiagLog::last;
}

TEST_F(Glue, NothingToSaveWritesNothing) {
  ReadingStats::begin(kCid1);
  ReadingStats::save("sleep");  // 第一次：只印 load 證人
  EXPECT_EQ(NvsStore::puts, 0);
  const int before = DiagLog::lines;
  ReadingStats::save("final");
  EXPECT_EQ(NvsStore::puts, 0);
  EXPECT_EQ(DiagLog::lines, before);  // 沒變動、也已經報過 load → 一行都不印
}

TEST_F(Glue, ReloadContinuesFromTheStoredSnapshot) {
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 2, 30);
  ReadingStats::endSession();
  ReadingStats::save("exit");
  ReadingStats::resetForTest();  // 「重開機」
  ReadingStats::begin(kCid1);
  readPages(kBook, 2, 1, 15);
  ReadingStats::endSession();
  ReadingStats::save("exit");
  EXPECT_EQ(stored().books[0].seconds, 75u);
  EXPECT_NE(DiagLog::last.find("load=ok"), std::string::npos) << DiagLog::last;
}

TEST_F(Glue, NewerVersionIsNeverOverwritten) {  // 第三輪阻斷 3
  std::vector<uint8_t> newer(readingstats::kBlobSize, 0);
  newer[0] = 'R';
  newer[1] = 2;
  NvsStore::store["rstat"] = newer;
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 3, 20);
  ReadingStats::save("exit");
  EXPECT_EQ(NvsStore::store["rstat"], newer);
  EXPECT_EQ(NvsStore::puts, 0);
  EXPECT_NE(DiagLog::last.find("load=newer blocked=1"), std::string::npos) << DiagLog::last;
}

TEST_F(Glue, BlobLongerThanTheReadBufferIsTreatedAsNewer) {
  const std::vector<uint8_t> big(600, 0x5A);
  NvsStore::store["rstat"] = big;
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 1, 20);
  ReadingStats::save("exit");
  EXPECT_EQ(NvsStore::store["rstat"], big);
}

TEST_F(Glue, CorruptBlobIsReplaced) {
  NvsStore::store["rstat"] = std::vector<uint8_t>(10, 0x00);
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 1, 20);
  ReadingStats::save("exit");
  EXPECT_EQ(stored().totalSec, 20u);
  EXPECT_NE(DiagLog::last.find("load=corrupt blocked=0"), std::string::npos) << DiagLog::last;
}

TEST_F(Glue, FailedWriteRetriesTheSameSnapshotWithoutDoubleCounting) {
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 2, 20);
  ReadingStats::pause();
  NvsStore::failPuts = 1;
  ReadingStats::save("sleep");
  EXPECT_EQ(NvsStore::store.count("rstat"), 0u);
  EXPECT_NE(DiagLog::last.find("ok=0"), std::string::npos) << DiagLog::last;
  ReadingStats::pause();  // 淺睡失敗轉真關機：onExit 又停一次錶
  ReadingStats::save("exit");
  ReadingStats::save("final");
  EXPECT_EQ(stored().totalSec, 40u);
  EXPECT_EQ(NvsStore::puts, 2);  // 失敗那次＋成功那次；final 沒有變動就不寫
}

TEST_F(Glue, WithoutACardIdNoPerBookRecordIsKept) {
  ReadingStats::begin(nullptr);
  readPages(kBook, 0, 2, 20);
  ReadingStats::save("exit");
  const auto s = stored();
  EXPECT_EQ(s.totalSec, 40u);
  for (const auto& b : s.books) EXPECT_EQ(b.key, 0u);
  EXPECT_NE(DiagLog::last.find("cid=0"), std::string::npos) << DiagLog::last;
}

TEST_F(Glue, SamePathOnAnotherCardIsAnotherBook) {  // 第三輪阻斷 1
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 1, 20);
  ReadingStats::endSession();
  ReadingStats::save("exit");
  ReadingStats::resetForTest();
  ReadingStats::begin(kCid2);  // 換卡重開機
  readPages(kBook, 0, 1, 30);
  ReadingStats::endSession();
  ReadingStats::save("exit");
  const auto s = stored();
  int found = 0;
  for (const auto& b : s.books) {
    if (b.key == expectedKey(kCid1, kBook)) {
      EXPECT_EQ(b.seconds, 20u);
      ++found;
    }
    if (b.key == expectedKey(kCid2, kBook)) {
      EXPECT_EQ(b.seconds, 30u);
      ++found;
    }
  }
  EXPECT_EQ(found, 2);
}

TEST_F(Glue, RenameBookMovesTheTotalAndSavesAtOnce) {  // 第三輪阻斷 2
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 2, 20);
  ReadingStats::endSession();
  const std::string moved = "/Read/浮生六記.epub";
  const int putsBefore = NvsStore::puts;
  ReadingStats::renameBook(kBook, moved);
  EXPECT_EQ(NvsStore::puts, putsBefore + 1);  // 立刻存，不等下一個持久點
  const auto s = stored();
  EXPECT_EQ(s.books[0].key, expectedKey(kCid1, moved));
  EXPECT_EQ(s.books[0].seconds, 40u);
}

TEST_F(Glue, DailyBucketUsesTheTrustedClockAndTheTimeZone) {
  halClock.valid = true;
  // 2026-10-07 20:00 UTC ＝ 台北 10-08 04:00
  halClock.epoch = clockEpoch(2026, 10, 7, 20, 0, 0);
  halClock.publishedAtMs = fakers::nowMs;
  SETTINGS.clockUtcOffsetQ = 48 + 32;  // UTC+8
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 2, 30);
  ReadingStats::save("exit");
  const auto s = stored();
  const uint32_t taipeiDay = (halClock.epoch + 60 + 8 * 3600) / 86400;  // 結算在發布後 60 秒
  EXPECT_EQ(s.lastDay, taipeiDay);
  EXPECT_EQ(s.days[taipeiDay % readingstats::kDays], 60u);
  EXPECT_NE(DiagLog::last.find("clock=1"), std::string::npos) << DiagLog::last;
}

TEST_F(Glue, NoTrustedClockMeansNoDailyBucket) {  // X4
  halClock.available = false;
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 2, 30);
  ReadingStats::save("exit");
  const auto s = stored();
  EXPECT_EQ(s.daysValid, 0u);
  EXPECT_EQ(s.totalSec, 60u);
  ReadingStats::refreshClock();
  EXPECT_EQ(halClock.polls, 0);  // 沒有 RTC 就不去讀
}

TEST_F(Glue, CorruptTimeZoneValueIsClamped) {
  halClock.valid = true;
  halClock.epoch = clockEpoch(2026, 10, 7, 12, 0, 0);
  halClock.publishedAtMs = fakers::nowMs;
  SETTINGS.clockUtcOffsetQ = 250;  // 壞掉的設定值 → 夾到 104（UTC+14）
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 1, 30);
  ReadingStats::save("exit");
  EXPECT_EQ(stored().lastDay, (halClock.epoch + 14 * 3600) / 86400);
}

TEST_F(Glue, StaleClockIsNotUsedForTheDailyBucket) {  // 主迴圈太久沒刷新時鐘 → 不歸日
  halClock.valid = true;
  halClock.epoch = clockEpoch(2026, 10, 7, 12, 0, 0);
  halClock.publishedAtMs = fakers::nowMs;
  ReadingStats::begin(kCid1);
  fakers::nowMs += 11u * 60u * 1000u;  // 超過 10 分鐘
  readPages(kBook, 0, 1, 30);
  ReadingStats::save("exit");
  EXPECT_EQ(stored().daysValid, 0u);
  EXPECT_EQ(stored().totalSec, 30u);
}

TEST_F(Glue, UnknownReadErrorBlocksWrites) {  // 不知道裡面是什麼 → 保守不寫
  NvsStore::getErr = 0x1101;                  // ESP_ERR_NVS_NOT_INITIALIZED 之類
  ReadingStats::begin(kCid1);
  readPages(kBook, 0, 1, 30);
  ReadingStats::save("exit");
  EXPECT_EQ(NvsStore::puts, 0);
  EXPECT_NE(DiagLog::last.find("blocked=1"), std::string::npos) << DiagLog::last;
}

TEST_F(Glue, NamespaceNotCreatedYetIsAbsentNotBlocked) {  // 從沒寫過任何東西：NOT_FOUND ＝ 沒有，可以寫
  NvsStore::getErr = ESP_ERR_NVS_NOT_FOUND;
  ReadingStats::begin(kCid1);
  NvsStore::getErr = 0;
  readPages(kBook, 0, 1, 30);
  ReadingStats::save("exit");
  EXPECT_EQ(stored().totalSec, 30u);
}
