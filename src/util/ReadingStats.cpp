#include "util/ReadingStats.h"

#include <Arduino.h>
#include <HalClock.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <nvs.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "util/DiagLog.h"
#include "util/NvsStore.h"
#include "util/ReadingStatsCore.h"

namespace ReadingStats {
namespace {

constexpr char KEY[] = "rstat";
// 比這個長的 blob 一定不是我們這一版寫的 → 直接當「比我新」（不讀進來、不覆寫）
constexpr size_t kMaxReadLen = 512;
// 時鐘發布值多舊就不拿來歸日（主迴圈每分鐘刷新一次；醒來刷新一次）
constexpr uint32_t kClockMaxAgeMs = 10u * 60u * 1000u;

portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
readingstats::Recorder gRec;  // 只在 gMux 內碰
uint64_t gCidHash = 0;
bool gCidOk = false;
bool gWriteBlocked = false;  // NVS 裡是比這一版新的格式：這一段開機只在 RAM 記，絕不覆寫
readingstats::LoadResult gLoad = readingstats::LoadResult::Absent;
bool gLoadReported = false;

// ⚠️ 持久的身分契約（rstat v1 起，改了每本紀錄就全部對不上）：
//   key = FNV-1a64(路徑的位元組，種子 = FNV-1a64(CID 16 bytes))；路徑照閱讀器拿到的原樣（不正規化）。
//   讀不到 CID 時種子是 FNV basis，key 只用來分辨「換書了沒」，不會寫進每本累計（perBook=false）。
uint64_t bookKey(const std::string& path) {
  const uint64_t k = readingstats::fnv64(path.data(), path.size(), gCidHash);
  return k == 0 ? 1 : k;  // 0 是空格
}

readingstats::DayInfo today() {
  readingstats::DayInfo d;
  uint32_t epoch = 0;
  if (!halClock.trustedUtcNow(epoch, kClockMaxAgeMs)) return d;
  uint8_t q = SETTINGS.clockUtcOffsetQ;
  if (q > 104) q = 104;  // 跟狀態列同一個夾限（HalClock::formatTime）
  const int64_t local = static_cast<int64_t>(epoch) + (static_cast<int32_t>(q) - 48) * 900;
  if (local < 0) return d;
  d.valid = true;
  d.day = static_cast<uint32_t>(local / 86400);
  return d;
}

const char* loadName(const readingstats::LoadResult r) {
  switch (r) {
    case readingstats::LoadResult::Absent:
      return "absent";
    case readingstats::LoadResult::Corrupt:
      return "corrupt";
    case readingstats::LoadResult::Compatible:
      return "ok";
    case readingstats::LoadResult::Newer:
      return "newer";
  }
  return "?";
}

}  // namespace

uint64_t bookIdentity(const std::string& path) { return bookKey(path); }

void begin(const uint8_t* cid16) {
  gCidOk = cid16 != nullptr;
  gCidHash = gCidOk ? readingstats::fnv64(cid16, 16) : readingstats::kFnv64Basis;

  // codex 第三輪阻斷 3：先查長度再讀，分三態。比這一版新的 → 這一段開機禁止寫入。
  size_t len = 0;
  int err = 0;
  readingstats::Snapshot snap;
  readingstats::LoadResult r = readingstats::LoadResult::Absent;
  if (NvsStore::getBlob(KEY, nullptr, &len, &err)) {
    if (len > kMaxReadLen) {
      r = readingstats::LoadResult::Newer;
    } else {
      uint8_t buf[kMaxReadLen];
      size_t got = sizeof(buf);
      if (NvsStore::getBlob(KEY, buf, &got, &err)) {
        r = readingstats::decode(buf, got, snap);
      } else {
        r = readingstats::LoadResult::Corrupt;
      }
    }
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    // namespace 還不存在（從沒寫過任何東西）也是 NOT_FOUND；其他錯誤當讀不到 —— 不知道裡面是什麼，保守不寫
    r = readingstats::LoadResult::Newer;
  }
  taskENTER_CRITICAL(&gMux);
  gLoad = r;
  gWriteBlocked = r == readingstats::LoadResult::Newer;
  if (r == readingstats::LoadResult::Compatible) gRec.load(snap);
  taskEXIT_CRITICAL(&gMux);
}

void observe(const std::string& bookPath, const uint32_t posA, const uint32_t posB, const uint32_t layoutGen) {
  const uint64_t key = bookKey(bookPath);
  const readingstats::DayInfo day = today();
  const uint32_t now = millis();
  taskENTER_CRITICAL(&gMux);
  gRec.observe(key, gCidOk, posA, posB, layoutGen, now, day);
  taskEXIT_CRITICAL(&gMux);
}

void endOfBook() {
  const readingstats::DayInfo day = today();
  const uint32_t now = millis();
  taskENTER_CRITICAL(&gMux);
  gRec.endOfBook(now, day);
  taskEXIT_CRITICAL(&gMux);
}

void pause() {
  const readingstats::DayInfo day = today();
  const uint32_t now = millis();
  taskENTER_CRITICAL(&gMux);
  gRec.pause(now, day);
  taskEXIT_CRITICAL(&gMux);
}

void endSession() {
  const readingstats::DayInfo day = today();
  const uint32_t now = millis();
  taskENTER_CRITICAL(&gMux);
  gRec.endSession(now, day);
  taskEXIT_CRITICAL(&gMux);
}

void save(const char* why) {
  // 短鎖取整份快照＋變動世代 → 鎖外寫 NVS → 成功而且世代沒變才清 dirty（codex 第三輪）。不持 gMux 寫 NVS。
  readingstats::Snapshot snap;
  readingstats::Recorder::Diag diag;
  uint32_t gen = 0;
  bool dirty = false;
  bool blocked = false;
  const readingstats::DayInfo day = today();
  taskENTER_CRITICAL(&gMux);
  dirty = gRec.dirty();
  blocked = gWriteBlocked;
  if (dirty) {
    snap = gRec.snapshot();
    gen = gRec.mutationGen();
  }
  diag = gRec.diag();
  taskEXIT_CRITICAL(&gMux);

  const bool reportLoad = !gLoadReported;
  if (!dirty && !reportLoad) return;
  gLoadReported = true;

  bool ok = false;
  uint32_t us = 0;
  int err = 0;
  if (dirty && !blocked) {
    uint8_t buf[readingstats::kBlobSize];
    readingstats::encode(snap, buf);
    ok = NvsStore::putBlob(KEY, buf, sizeof(buf), &us, &err);
    if (ok) {
      taskENTER_CRITICAL(&gMux);
      gRec.markSaved(gen);
      gRec.clearDiag();
      taskEXIT_CRITICAL(&gMux);
    }
  }
  const int recs = static_cast<int>(std::count_if(std::begin(snap.books), std::end(snap.books),
                                                  [](const readingstats::BookRec& b) { return b.key != 0; }));
  DiagLog::line(
      "RSTAT save why=%s dirty=%d ok=%d us=%lu err=%d add_s=%lu navs=%lu capped=%lu reflow=%lu gaps=%lu back=%lu "
      "clock=%d day=%lu recs=%d total_s=%lu cid=%d load=%s blocked=%d",
      why ? why : "?", dirty ? 1 : 0, ok ? 1 : 0, static_cast<unsigned long>(us), err,
      static_cast<unsigned long>(diag.addMs / 1000u), static_cast<unsigned long>(diag.navs),
      static_cast<unsigned long>(diag.capped), static_cast<unsigned long>(diag.reflows),
      static_cast<unsigned long>(diag.gaps), static_cast<unsigned long>(diag.backDays), day.valid ? 1 : 0,
      static_cast<unsigned long>(day.day), recs, static_cast<unsigned long>(snap.totalSec), gCidOk ? 1 : 0,
      loadName(gLoad), blocked ? 1 : 0);
}

void renameBook(const std::string& oldPath, const std::string& newPath) {
  if (!gCidOk) return;  // 沒有每本累計可搬
  const uint64_t o = bookKey(oldPath);
  const uint64_t n = bookKey(newPath);
  taskENTER_CRITICAL(&gMux);
  gRec.rename(o, n);
  taskEXIT_CRITICAL(&gMux);
  save("move");
}

bool todaySeconds(uint32_t& out) {
  const readingstats::DayInfo day = today();
  if (!day.valid) return false;
  taskENTER_CRITICAL(&gMux);
  out = gRec.daySeconds(day);
  taskEXIT_CRITICAL(&gMux);
  return true;
}

uint32_t bookSeconds(const std::string& bookPath) {
  if (!gCidOk) return 0;
  const uint64_t key = bookKey(bookPath);
  taskENTER_CRITICAL(&gMux);
  const uint32_t s = gRec.bookSeconds(key);
  taskEXIT_CRITICAL(&gMux);
  return s;
}

#ifdef READING_STATS_HOST_TEST
void resetForTest() {
  gRec = readingstats::Recorder{};
  gCidHash = 0;
  gCidOk = false;
  gWriteBlocked = false;
  gLoad = readingstats::LoadResult::Absent;
  gLoadReported = false;
}
#endif

void refreshClock() {
  if (!halClock.isAvailable()) return;
  uint8_t h = 0;
  uint8_t m = 0;
  halClock.getTime(h, m);  // 距上次輪詢超過 CLOCK_POLL_MS 才真的讀 I2C；可信就更新發布值，不可信就撤銷
}

}  // namespace ReadingStats
