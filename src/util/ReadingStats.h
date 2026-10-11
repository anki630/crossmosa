#pragma once

// 閱讀統計・第一步：只記錄、不顯示（2026-10-07）。純邏輯在 ReadingStatsCore.h；這裡是裝置端的膠水：
//   臨界區（render 任務與主任務共用）、晶片 NVS（一塊 `rstat`，整份快照）、時鐘、SD 卡序號、證人 log。
//
// 呼叫規則（審查三輪定下的，改之前先讀工作區 docs/specs/2026-10-07-reading-stats-recorder.md）：
//   - observe()／endOfBook()／pause()：render 任務或主任務都可以呼叫；只做整數運算，不配置、不 I/O、不 log。
//     observe 只在【正文成功繪製】之後、用 render 開頭捕捉的位置快照呼叫。
//   - save()：持久點（離開閱讀器、休眠入口、真關機前）呼叫；寫 NVS、印一行 RSTAT。不在翻頁路徑呼叫。
//   - begin()：開機讀到 SD 卡序號之後呼叫一次。
//   - refreshClock()：主任務（主迴圈每分鐘、淺睡醒來）呼叫，讓時鐘的發布值保持新鮮；會打 I2C。

#include <cstdint>
#include <string>

namespace ReadingStats {

// cid16＝開機讀到的 SD 卡 CID（16 bytes）；nullptr＝讀不到 → 不記每本累計（每日、總數照記）。
// 只拿它的雜湊當書的身分的一部分，原始序號不保存、不寫 log。
void begin(const uint8_t* cid16);

void observe(const std::string& bookPath, uint32_t posA, uint32_t posB, uint32_t layoutGen);
void endOfBook();
// 睡眠入口用：停錶，這一段閱讀還沒結束（醒來同頁重繪會接著計）
void pause();
// 離開閱讀器用（onExit 第一行）：停錶＋結束這一段（再開同一本算一次重新開啟）
void endSession();

// why：exit／sleep／final／move（證人用）。dirty 才寫；寫失敗保持 dirty，下一個持久點整塊重寫。
void save(const char* why);

// 自動搬到 /Read 之後：舊路徑的累計搬到新路徑（新的已存在就相加），然後立刻存。
void renameBook(const std::string& oldPath, const std::string& newPath);
// 書的身分（持久契約，同 rstat：FNV-1a64(路徑, 種子 FNV-1a64(CID))，0 會換成 1）。我的最愛也用它（v367）。
uint64_t bookIdentity(const std::string& path);

void refreshClock();

// 查詢（Formosa Cover 首頁卡片，2026-10-07）。都只讀 RAM，不 I/O。
//   todaySeconds：沒有可信時鐘（X4、未對時）回 false，卡片就不畫「今天」那一行。
//   bookSeconds：讀不到 CID 或這本不在最近 10 本 → 0。在首頁查時閱讀器已離開（onExit 已結算），不含任何未結算的時間。
bool todaySeconds(uint32_t& out);
uint32_t bookSeconds(const std::string& bookPath);

#ifdef READING_STATS_HOST_TEST
void resetForTest();
#endif

}  // namespace ReadingStats
