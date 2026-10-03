#pragma once

class GfxRenderer;

// v358 bench（/ghost.on）：開機時跑一次的殘影測試。流程、判讀方式見 GhostTest.cpp 檔頭。
namespace GhostTest {
// 顯示與字型初始化之後、路由之前呼叫（呼叫端先確認 BenchFlags::ghost、不是救援模式、不是當機重開）。
// 哨兵檔在畫任何東西之前就刪掉；刪不掉就不跑（免得每次開機都跑）。
// 跑完（或中止）後要求下一次刷新走清底，開機照常往下走。
void run(GfxRenderer& renderer);
// v360（/ghostw.on）：桌布殘影測試 —— 快速刷新的一頁字停 5 秒／10 分鐘，再照休眠的畫法畫灰的桌布（見 GhostTest.cpp）。
void runWallpaper(GfxRenderer& renderer);
}  // namespace GhostTest
