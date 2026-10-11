#pragma once

// v379 量測（維護者 2026-10-09：換分頁體感慢，先找根因再修）：書架分頁「最愛｜全部｜資料夾」從按下鍵到畫面出來，
//   每一段各花多久。只寫 diag.log，不改任何行為。
//   一行 TABSW：how＝怎麼換的（ok＝⑥ 放開、hold＝長按 ⑦⑧）、from＝哪個畫面、hold＝按下到觸發（等放開或等長按門檻）、
//   pre＝觸發到畫面可以畫（排書，或關掉舊畫面＋開新畫面：掃卡、讀資料夾）、wait＝可以畫到真的開始畫（繪製任務排隊）、
//   draw＝畫進 framebuffer、disp＝面板刷新、total＝按下到畫面出來。
//   書架第一次顯示時會補產縮圖、產完整頁再畫一次：TABSWTHUMB 記補產花多久，TABSW2 記第二次畫完時距離按下多久。
//   主任務寫 begin／ready，繪製任務寫 drawStart 之後的各點；只在 armed 期間記，一次換分頁一行。

#include <Arduino.h>

#include <atomic>

#include "util/DiagLog.h"

namespace TabSwitchProbe {

struct State {
  std::atomic<bool> armed{false};
  std::atomic<uint32_t> press{0}, trigger{0}, ready{0}, drawStart{0}, drawDone{0};
  const char* how = "";
  const char* from = "";
  // 縮圖補產後的第二次整頁重畫（只在第一次畫完後 5 秒內算）
  std::atomic<bool> thumbRedraw{false};
  std::atomic<uint32_t> followPress{0}, followUntil{0}, redrawStart{0};
};

inline State& st() {
  static State s;
  return s;
}

// 主任務：決定要換分頁的那一刻。pressMs＝那顆鍵按下的時間
inline void begin(const char* how, const char* from, const uint32_t pressMs) {
  State& s = st();
  s.armed.store(false);
  s.how = how;
  s.from = from;
  s.press.store(pressMs);
  s.trigger.store(millis());
  s.ready.store(0);
  s.drawStart.store(0);
  s.drawDone.store(0);
  s.thumbRedraw.store(false);
  s.followUntil.store(0);
  s.armed.store(true);
}

// 主任務：新的內容準備好了（排完書／新畫面 onEnter 做完），接下來的 render 就是這次換分頁的畫面
inline void ready() {
  State& s = st();
  if (!s.armed.load() || s.ready.load() != 0) return;
  const uint32_t now = millis();
  if (now - s.trigger.load() > 10000) {  // 觸發之後一直沒走到這裡（例如開新畫面失敗）：不要把之後無關的進場算進來
    s.armed.store(false);
    return;
  }
  s.ready.store(now);
}

// 繪製任務：整頁重畫開始
inline void drawStart() {
  State& s = st();
  const uint32_t now = millis();
  if (s.armed.load()) {
    if (s.ready.load() != 0 && s.drawStart.load() == 0) s.drawStart.store(now);
    return;
  }
  if (s.thumbRedraw.load() && static_cast<int32_t>(s.followUntil.load() - now) > 0) s.redrawStart.store(now);
}

// 繪製任務：畫完、送面板之前
inline void beforeDisplay() {
  State& s = st();
  if (s.armed.load() && s.drawStart.load() != 0 && s.drawDone.load() == 0) s.drawDone.store(millis());
}

// 繪製任務：面板刷新完成。who＝畫的是哪個畫面（shelf／folder）、tab＝書架的分頁（資料夾畫面給 -1）。
//   回傳 true＝剛剛畫出來的就是這次換分頁的畫面（呼叫端拿它決定要不要記 thumbs）
inline bool afterDisplay(const char* who, const int tab, const unsigned bank) {
  State& s = st();
  const uint32_t now = millis();
  if (s.armed.load() && s.drawDone.load() != 0) {
    s.armed.store(false);
    const uint32_t press = s.press.load(), trig = s.trigger.load(), rdy = s.ready.load();
    const uint32_t ds = s.drawStart.load(), dd = s.drawDone.load();
    DiagLog::line("TABSW how=%s from=%s to=%s tab=%d hold=%lu pre=%lu wait=%lu draw=%lu disp=%lu total=%lu bank=%u",
                  s.how, s.from, who, tab, static_cast<unsigned long>(trig - press),
                  static_cast<unsigned long>(rdy - trig), static_cast<unsigned long>(ds - rdy),
                  static_cast<unsigned long>(dd - ds), static_cast<unsigned long>(now - dd),
                  static_cast<unsigned long>(now - press), bank);
    s.followPress.store(press);
    return true;
  }
  const uint32_t rs = s.redrawStart.load();
  if (s.thumbRedraw.load() && rs != 0) {
    s.thumbRedraw.store(false);
    s.redrawStart.store(0);
    DiagLog::line("TABSW2 redraw=%lu total=%lu", static_cast<unsigned long>(now - rs),
                  static_cast<unsigned long>(now - s.followPress.load()));
  }
  return false;
}

// 繪製任務：換分頁那一頁畫完之後，檢查／補產縮圖花了多久、有沒有要求整頁重畫（只在 afterDisplay 回 true 的那次呼叫）
inline void thumbs(const uint32_t ms, const bool redraw) {
  State& s = st();
  DiagLog::line("TABSWTHUMB ms=%lu redraw=%d", static_cast<unsigned long>(ms), redraw ? 1 : 0);
  s.thumbRedraw.store(redraw);
  s.redrawStart.store(0);
  s.followUntil.store(redraw ? millis() + 5000 : 0);
}

}  // namespace TabSwitchProbe
