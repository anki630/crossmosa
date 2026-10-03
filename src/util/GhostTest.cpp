#include "GhostTest.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdio>

#include "BenchFlags.h"
#include "CrossPointSettings.h"
#include "DiagLog.h"
#include "components/UITheme.h"
#include "fontIds.h"

// v358 bench（/ghost.on）：殘影測試。
//
// 為什麼要有它：殘影靠眼睛很難判斷 —— 要很專心看，每次看的地方不一樣，也記不住上一次多淡（維護者 2026-09-30）。
//   改成機器畫固定的圖、用固定的方法清成白，停在「應該全白」的畫面讓人拍照。判讀在工作區（scripts/ghost_photo.py）：
//   同一張照片裡「上一張有東西的格子」跟「上一張就是白的格子」差多少，就是殘影 —— 同一張照片內相比，
//   燈光、手機曝光、白平衡都互相抵消，只要整個螢幕入鏡。
//
// 每一項：預洗（全黑、全白兩輪 HALF：每個像素來回各推兩次，前一項的殘影不帶進來）→ 畫測試圖 → 停一段時間
//   → 用這一項的方法換成白畫面＋標籤 → 等按鍵（這時拍照）。棋盤的相位每一項輪替（這一項有東西的格子，
//   下一項是白格），預洗沒洗乾淨的話在下一張照片上會變成「負的殘影」，看得出來（codex）；
//   隔兩項會落回同一個相位 → 殘影最重的 F 排最後（它只當比例尺，不會污染別項），
//   長測試裡最可能留殘影的 L1 排在 LF 後面。
//   0   基準：預洗完直接拍白畫面。兩種相位都算一次＝這組照片本身的偏差（打光、反光、相機的局部調色）
//   短（停 5 秒）：
//     A   有灰階的圖（像畫冊的圖頁；灰階畫法照 XtcReaderActivity）→ 一般清殘影（HALF；X3 上會被 requestResync
//         變成 GC，跟閱讀器清底一樣）
//     B   同上 → 快速清殘影測試版（v357 x4diff：RED 寫成新畫面的反相再快速刷新；只有 X4 SSD1677）
//     LS  黑白圖（像文字頁），畫法、電源跟 L1 一樣，只是停 5 秒 → 一般清殘影（L1 的「停得短」對照）
//   長（停 10 分鐘，黑白圖）。維護者回報：X4 自動休眠時桌布有殘影，手動休眠看不太出來。兩條路畫桌布都是 HALF，
//   差別是自動休眠前那一頁已停 10 分鐘 —— 而快速刷新（X4 的 0xFC）之後面板的類比電源不關
//   （X3 的 UC8279 也是，只有 turnOff 才 POF），那 10 分鐘面板一直通電。
//     L0  停的時候面板斷電（HALF 畫完就關：X4 的 0xD7 本來就會關，X3 帶 turnOff 才關）→ 一般清殘影
//     LF  停的時候通電（同 L1）→ 最強的清除（FULL，X4 是會閃好幾下的 0xF7；只有 X4）
//     L1  停的時候面板通電 → 一般清殘影。X3：HALF 不帶 turnOff 就不 POF，電源本來就留著；
//         X4：0xD7 一定關電，再用同一張畫面快速刷新一次（像素不動）把電源留著開。
//   最後
//     F   有灰階的圖 → 不清，直接快速刷新（沒清的殘影有多重，當比例尺）
//   判讀：L1 vs LS＝停的時間；L1 vs L0＝停的時候通不通電（X4 另外【加上】L1 多的那一次像素不動的快速刷新 ——
//   0xFC 之後不經 SDK 沒有辦法斷電，分不開；X3 沒有這個問題）；LF＝強力清除清不清得掉。
//
// 電源：預洗、說明頁、結尾頁、每張拍照的白畫面都帶 turnOff（X3 刷完就 POF；X4 的 HALF／FULL 本來就會關，
//   快速刷新的 F、B 關不掉 —— 拍照等待期間通電，log 的 key= 記了等多久）。
// 版面（直向，邏輯座標）：四角各一個黑方塊（對位用，左上那塊比較大，照片才分得出方向），中間 4×6 格棋盤，
//   標籤在格子下方。任何等待中，任一鍵按住 3 秒＝中止。證人：GHOST 開頭的行（強制寫進 diag.log，沒放 diag.on 也寫）。
namespace GhostTest {
namespace {

constexpr uint32_t kShortDwellMs = 5 * 1000;
constexpr uint32_t kLongDwellMs = 10 * 60 * 1000;
constexpr uint32_t kPhotoWaitMs = 30 * 60 * 1000;  // 等拍照：30 分鐘沒人按就中止（別讓機器、面板一直醒著）
constexpr uint32_t kAbortHoldMs = 3000;
constexpr uint32_t kReleaseDrainMs = 5000;  // 等「還按著的鍵」放開最多這麼久（卡住的鍵不能讓開機停在這裡）
constexpr int kCols = 4;
constexpr int kRows = 6;
// 對位方塊離螢幕邊 12 px：直向上緣有 9 px 可能被邊框蓋住（GfxRenderer::VIEWABLE_MARGIN_TOP），方塊要整塊看得到、
//   跟邊框之間也要留一條白，照片上才找得到；格子區再往內（40 px），方塊不會壓到格子。
constexpr int kSide = 40;     // 格子區左右留白
constexpr int kTop = 40;      // 格子區上方留白
constexpr int kBottom = 104;  // 格子區下方：兩行標籤＋對位方塊
constexpr int kFid = 20;      // 對位方塊邊長（左上那塊 kFidTL）
constexpr int kFidTL = 26;
constexpr int kFidInset = 12;
static_assert(kFidInset + kFidTL < kSide && kFidInset + kFidTL < kTop, "對位方塊不能壓到格子");

enum class Cell : uint8_t { W, K, DG, LG, T };
enum class Pattern : uint8_t { Image, Text };
enum class Clean : uint8_t { Fast, Half, ForcedDiff, Full };
enum class Power : uint8_t { ReaderLike, On, Off };

struct Geom {
  int w = 0, h = 0;
  int gx = 0, gy = 0, cw = 0, ch = 0;
};

struct Step {
  const char* id;
  const char* title;  // 標籤第一行（照片上看得到的名字）
  Pattern pattern;
  uint32_t dwellMs;
  Power power;  // 停留時的面板電源（ReaderLike＝照閱讀器的畫法，沒有特別控制）
  Clean clean;
};

Geom geometry(const GfxRenderer& r) {
  Geom g;
  g.w = r.getScreenWidth();
  g.h = r.getScreenHeight();
  const int gw = g.w - 2 * kSide;
  const int gh = g.h - kTop - kBottom;
  g.cw = gw / kCols;
  g.ch = gh / kRows;
  g.gx = (g.w - g.cw * kCols) / 2;
  g.gy = kTop + (gh - g.ch * kRows) / 2;
  return g;
}

// 棋盤：(row+col)%2 != phase 是白格；其餘依序放內容，往下每列錯開一格（同一種內容不會都在同一欄）。
Cell cellAt(const Pattern p, const int phase, const int row, const int col) {
  if ((row + col) % 2 != phase) return Cell::W;
  const int k = (row * kCols + col) / 2;  // 有內容的格子的流水號 0..11
  if (p == Pattern::Text) return ((k + row) % 2 == 0) ? Cell::K : Cell::T;
  static constexpr Cell kSeq[4] = {Cell::K, Cell::DG, Cell::LG, Cell::T};
  return kSeq[(k + row) % 4];
}

void drawFiducials(const GfxRenderer& r, const Geom& g) {
  r.fillRect(kFidInset, kFidInset, kFidTL, kFidTL, true);
  r.fillRect(g.w - kFidInset - kFid, kFidInset, kFid, kFid, true);
  r.fillRect(kFidInset, g.h - kFidInset - kFid, kFid, kFid, true);
  r.fillRect(g.w - kFidInset - kFid, g.h - kFidInset - kFid, kFid, kFid, true);
}

void drawLabel(const GfxRenderer& r, const Geom& g, const char* line1, const char* line2) {
  const int y = g.gy + g.ch * kRows + 10;
  if (line1) r.drawCenteredText(UI_12_FONT_ID, y, line1, true, EpdFontFamily::BOLD);
  if (line2) r.drawCenteredText(UI_10_FONT_ID, y + 36, line2, true);
}

// 2×2 的黑白棋盤（像細字：一半黑、邊緣很多）
void fillTexture(const GfxRenderer& r, const int x0, const int y0, const int w, const int h) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (((x / 2) + (y / 2)) % 2 == 0) r.drawPixel(x0 + x, y0 + y, true);
    }
  }
}

// 黑白底：K、T 照畫；DG、LG 先畫成黑（灰階那一趟再把它們推成灰 —— 同 XtcReaderActivity 的 Pass 1）
void drawPatternBw(const GfxRenderer& r, const Geom& g, const Pattern p, const int phase, const char* line1,
                   const char* line2) {
  r.clearScreen(0xFF);
  drawFiducials(r, g);
  for (int row = 0; row < kRows; row++) {
    for (int col = 0; col < kCols; col++) {
      const int x = g.gx + col * g.cw;
      const int y = g.gy + row * g.ch;
      switch (cellAt(p, phase, row, col)) {
        case Cell::W:
          break;
        case Cell::T:
          fillTexture(r, x, y, g.cw, g.ch);
          break;
        case Cell::K:
        case Cell::DG:
        case Cell::LG:
          r.fillRect(x, y, g.cw, g.ch, true);
          break;
      }
    }
  }
  drawLabel(r, g, line1, line2);
}

// 灰階平面（XtcReaderActivity Pass 2／3）：清成 0x00，要推灰的格子畫成白（＝標記）。LSB 只標深灰，MSB 標深灰＋淺灰。
void drawGrayPlane(const GfxRenderer& r, const Geom& g, const int phase, const bool lsb) {
  r.clearScreen(0x00);
  for (int row = 0; row < kRows; row++) {
    for (int col = 0; col < kCols; col++) {
      const Cell c = cellAt(Pattern::Image, phase, row, col);
      if (c == Cell::DG || (!lsb && c == Cell::LG)) {
        r.fillRect(g.gx + col * g.cw, g.gy + row * g.ch, g.cw, g.ch, false);
      }
    }
  }
}

void drawWhite(const GfxRenderer& r, const Geom& g, const char* line1, const char* line2) {
  r.clearScreen(0xFF);
  drawFiducials(r, g);
  drawLabel(r, g, line1, line2);
}

struct Shot {
  uint32_t ms = 0;
  uint8_t bank = 0;
};

// 直接叫顯示層（不經 GfxRenderer::displayBuffer）：turnOff 要自己決定，不跟著「日照淡化修正」設定走。
Shot push(const HalDisplay::RefreshMode mode, const bool turnOff) {
  const uint32_t t0 = millis();
  display.displayBuffer(mode, turnOff);
  return Shot{millis() - t0, display.lastRefreshBank()};
}

// 預洗：全黑、全白兩輪 HALF（X4 的 0xD7 是絕對刷新；X3 的 GC 看舊平面，但黑→白每一輪每個像素都變過兩次）。
//   全部帶 turnOff：預洗之後面板斷電，跟下一步怎麼畫無關。
Shot precondition(const GfxRenderer& r, const Geom& g, const char* line1) {
  const uint32_t t0 = millis();
  Shot white;
  for (int cycle = 0; cycle < 2; cycle++) {
    r.clearScreen(0x00);
    push(HalDisplay::HALF_REFRESH, true);
    drawWhite(r, g, line1, nullptr);
    white = push(HalDisplay::HALF_REFRESH, true);
  }
  return Shot{millis() - t0, white.bank};
}

bool anyDown() {
  for (uint8_t b = HalGPIO::BTN_BACK; b <= HalGPIO::BTN_POWER; b++) {
    if (gpio.isPressed(b)) return true;
  }
  return false;
}

enum class Wait : uint8_t { Key, Timeout, Abort };

// 等「放開之後的新按下」：短按（放開）＝下一步，按住 kAbortHoldMs＝中止，untilMs 內沒人按＝Timeout。
//   keyEnds=false：只看中止（停留時間照走完，短按不理）。
//   進來時還按著的鍵（開機那一下、刷新期間開始按的）：放開就當沒發生；進來之後又按滿 kAbortHoldMs ＝ 中止
//   （刷新期間開始的長按不會被吞掉；codex 第三輪）。卡住的鍵因此也只會走到中止，不會讓開機停在這裡。
Wait waitFor(const uint32_t untilMs, const bool keyEnds, uint32_t* waitedMs) {
  const uint32_t tDrain = millis();
  bool heldAtEntry = false;
  while (millis() - tDrain < kAbortHoldMs) {
    gpio.update();
    if (!anyDown()) break;
    heldAtEntry = true;
    delay(20);
  }
  gpio.update();
  if (heldAtEntry && anyDown()) {
    // 進來之後又按著滿 kAbortHoldMs：中止。等它放開（最多 kReleaseDrainMs），免得放開被下一個等待當成短按。
    const uint32_t t1 = millis();
    while (millis() - t1 < kReleaseDrainMs) {
      gpio.update();
      if (!anyDown()) break;
      delay(20);
    }
    if (waitedMs) *waitedMs = 0;
    return Wait::Abort;
  }
  const uint32_t t0 = millis();
  bool armed = !anyDown();  // 保險：剛好在上面那一刻按下的 → 要先看到放開，之後的按下才算數
  bool down = false;
  uint32_t downAt = 0;
  Wait result = Wait::Timeout;
  uint32_t elapsed = 0;  // 決定結果那一刻（中止之後等放開的時間不算進去）
  while (millis() - t0 < untilMs) {
    gpio.update();
    const bool any = anyDown();
    const uint32_t now = millis();
    if (!armed) {
      if (!any) armed = true;
    } else if (any && !down) {
      down = true;
      downAt = now;
    } else if (any && down && now - downAt >= kAbortHoldMs) {
      result = Wait::Abort;
      elapsed = now - t0;
      break;
    } else if (!any && down) {
      down = false;
      if (keyEnds) {
        result = Wait::Key;
        elapsed = now - t0;
        break;
      }
    }
    delay(20);
  }
  if (result == Wait::Timeout) elapsed = millis() - t0;
  // 中止的那一下：等它放開再往下（否則放開會被下一個等待當成短按），但最多等 kReleaseDrainMs
  if (result == Wait::Abort) {
    const uint32_t t1 = millis();
    while (millis() - t1 < kReleaseDrainMs) {
      gpio.update();
      if (!anyDown()) break;
      delay(20);
    }
  }
  if (waitedMs) *waitedMs = elapsed;
  return result;
}

const char* cleanName(const Clean c) {
  switch (c) {
    case Clean::Fast:
      return "fast";
    case Clean::Half:
      return "half";
    case Clean::ForcedDiff:
      return "x4diff";
    case Clean::Full:
      return "full";
  }
  return "?";
}

// 這一種清法「應該」選到的 bank（1＝GC／HALF／FULL，2＝DU／FAST）—— 驅動偷偷升級了就記 bankok=0，這張照片不算數
uint8_t expectedBank(const Clean c) { return (c == Clean::Fast || c == Clean::ForcedDiff) ? 2 : 1; }

const char* powerName(const Power p) {
  switch (p) {
    case Power::ReaderLike:
      return "rd";
    case Power::On:
      return "on";
    case Power::Off:
      return "off";
  }
  return "?";
}

const char* waitName(const Wait w) {
  switch (w) {
    case Wait::Key:
      return "key";
    case Wait::Timeout:
      return "timeout";
    case Wait::Abort:
      return "abort";
  }
  return "?";
}

// ── v360：桌布殘影測試（/ghostw.on）的畫法 ─────────────────────────────────────────────────────────────
// 「清成桌布」：格子區每一列整列是灰（偶數列淺灰、奇數列深灰），畫法照 SleepActivity 的快取命中路徑：
//   黑白底（灰的地方先畫成黑）→ displayGrayscaleBase(HALF) → LSB／MSB 平面 → displayGrayBuffer()。
//   同一列裡「上一頁有字的格子」跟「上一頁是白的格子」目標灰一樣 → 兩者差多少就是殘影（照片判讀用同一列的左右鄰格比）。
Cell wallCell(const int row) { return (row % 2 == 0) ? Cell::LG : Cell::DG; }

void drawWallBw(const GfxRenderer& r, const Geom& g, const char* line1, const char* line2) {
  r.clearScreen(0xFF);
  drawFiducials(r, g);
  r.fillRect(g.gx, g.gy, g.cw * kCols, g.ch * kRows, true);
  drawLabel(r, g, line1, line2);
}

void drawWallPlane(const GfxRenderer& r, const Geom& g, const bool lsb) {
  r.clearScreen(0x00);
  for (int row = 0; row < kRows; row++) {
    if (wallCell(row) == Cell::DG || !lsb) r.fillRect(g.gx, g.gy + row * g.ch, g.cw * kCols, g.ch, false);
  }
}

// 跟 SleepActivity 一樣走 GfxRenderer（turnOff 跟著「日照淡化修正」設定，跟真的桌布一樣）。回傳整段毫秒。
uint32_t paintWall(GfxRenderer& r, const Geom& g, const char* line1, const char* line2) {
  const uint32_t t0 = millis();
  drawWallBw(r, g, line1, line2);
  r.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  drawWallPlane(r, g, /*lsb=*/true);
  r.copyGrayscaleLsbBuffers();
  drawWallPlane(r, g, /*lsb=*/false);
  r.copyGrayscaleMsbBuffers();
  r.displayGrayBuffer();
  r.setRenderMode(GfxRenderer::BW);
  return millis() - t0;
}

// 拍完桌布：把控制器的兩個平面改寫成桌布的黑白底、退出灰階模式（不刷新）。不做的話 X3 的 UC8279 留著
//   _inGrayscaleMode，下一項畫桌布時 displayGrayscaleBase 會先 grayscaleRevert —— 在提示與桌布之間多一次白的 GC，
//   正好把要量的殘影洗掉（codex v360
//   第一輪）。真的休眠之前閱讀頁不在灰階模式（沒開抗鋸齒），所以每一項開始前都要回到這個狀態。
void leaveWall(const GfxRenderer& r, const Geom& g) {
  drawWallBw(r, g, nullptr, nullptr);
  r.cleanupGrayscaleWithFrameBuffer();
}

}  // namespace

void run(GfxRenderer& renderer) {
  // 先開強制記錄（刪不掉哨兵的原因也要留得下來），再刪哨兵：之後任何一步當機，下次開機都不會再跑。
  const bool prevForced = DiagLog::setForced(true, "ghost");
  if (!Storage.remove("/ghost.on")) {
    DiagLog::line("GHOST skip why=rm-fail");
    DiagLog::setForced(prevForced, "ghost-end");
    return;
  }
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  // /x4diff.on 同時在的話，一般的 HALF 全都會被換掉（A、預洗、L 的畫圖都會變成 B）→ 測試期間先關，只在 B 那一次開。
  display.setForcedDiffClean(false);
  // 開機的「前兩次強制 GC」預算歸零（保留一次性的 resync）：第一張（說明頁）照樣是乾淨的清底，
  //   之後每一次刷新都是這裡要求的那種 —— F 的「快速刷新」不會被偷偷升級成 GC。
  display.defuseInitialFullSyncsKeepResync();

  const bool x3 = gpio.deviceIsX3();
  const bool forcedDiffOk = display.supportsForcedDiffClean();
  const Geom g = geometry(renderer);

  // 順序見檔頭（F 最後；scripts/ghost_photo.py 的 SEQUENCE 要跟這裡一樣，相位＝這個順序裡的位置 %2）
  Step steps[8];
  int n = 0;
  steps[n++] = {"A", "A　一般清殘影", Pattern::Image, kShortDwellMs, Power::ReaderLike, Clean::Half};
  if (forcedDiffOk) {
    steps[n++] = {"B", "B　快速清殘影（測試版）", Pattern::Image, kShortDwellMs, Power::ReaderLike, Clean::ForcedDiff};
  }
  steps[n++] = {"LS", "LS　停 5 秒，通電，一般清殘影", Pattern::Text, kShortDwellMs, Power::On, Clean::Half};
  steps[n++] = {"L0", "L0　停 10 分鐘，斷電，一般清殘影", Pattern::Text, kLongDwellMs, Power::Off, Clean::Half};
  if (!x3) {
    steps[n++] = {"LF", "LF　停 10 分鐘，通電，強力清除", Pattern::Text, kLongDwellMs, Power::On, Clean::Full};
  }
  steps[n++] = {"L1", "L1　停 10 分鐘，通電，一般清殘影", Pattern::Text, kLongDwellMs, Power::On, Clean::Half};
  steps[n++] = {"F", "F　不清，直接快速刷新", Pattern::Image, kShortDwellMs, Power::ReaderLike, Clean::Fast};
  int longCount = 0;
  for (int i = 0; i < n; i++) longCount += steps[i].dwellMs == kLongDwellMs ? 1 : 0;
  const int photos = n + 1;  // ＋基準那一張

  DiagLog::line("GHOST start dev=%s steps=%d long=%d w=%d h=%d grid=%d,%d,%d,%d fid=%d/%d/%d fading=%u x4diff=%d",
                x3 ? "x3" : "x4", n, longCount, g.w, g.h, g.gx, g.gy, g.cw, g.ch, kFidInset, kFidTL, kFid,
                static_cast<unsigned>(SETTINGS.fadingFix), BenchFlags::x4Diff ? 1 : 0);

  // 說明頁
  {
    char line[96];
    renderer.clearScreen(0xFF);
    int y = 120;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "殘影測試", true, EpdFontFamily::BOLD);
    y += 56;
    snprintf(line, sizeof(line), "共 %d 張要拍，約 %d 分鐘。", photos, longCount * 11 + 4);
    renderer.drawCenteredText(UI_10_FONT_ID, y, line);
    y += 30;
    snprintf(line, sizeof(line), "其中 %d 張要等 10 分鐘才出現（不要碰機器）。", longCount);
    renderer.drawCenteredText(UI_10_FONT_ID, y, line);
    y += 48;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "畫面下方出現代號（例如 A）時：");
    y += 30;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "拍一張照片，拍完按任意鍵。", true, EpdFontFamily::BOLD);
    y += 48;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "拍照：整個螢幕入鏡、手機跟螢幕平行，");
    y += 30;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "每張同一個位置和燈光，不要開閃光燈。");
    y += 48;
    renderer.drawCenteredText(UI_10_FONT_ID, y, "想停下來：任一鍵按住 3 秒。");
    y += 48;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "按任意鍵開始", true, EpdFontFamily::BOLD);
    push(HalDisplay::HALF_REFRESH, true);
  }
  Wait w = waitFor(kPhotoWaitMs, true, nullptr);
  bool aborted = w != Wait::Key;
  if (aborted) DiagLog::line("GHOST abort at=intro wait=%s", waitName(w));
  int done = 0;        // 做完的測試項（不含基準）
  int photosDone = 0;  // 拍完按了鍵的畫面（含基準）
  char l2[96];

  // 基準：預洗完直接拍（這組照片本身的偏差）
  if (!aborted) {
    const Shot prep = precondition(renderer, g, "準備中…");
    snprintf(l2, sizeof(l2), "第 1／%d 張　拍一張照片，拍完按任意鍵", photos);
    drawWhite(renderer, g, "0　基準（全白）", l2);
    const Shot base = push(HalDisplay::HALF_REFRESH, true);
    uint32_t keyMs = 0;
    w = waitFor(kPhotoWaitMs, true, &keyMs);
    DiagLog::line("GHOST id=0 n=1/%d prep=%lu/%u clean=half:%lu/%u key=%lu wait=%s", photos,
                  static_cast<unsigned long>(prep.ms), static_cast<unsigned>(prep.bank),
                  static_cast<unsigned long>(base.ms), static_cast<unsigned>(base.bank),
                  static_cast<unsigned long>(keyMs), waitName(w));
    if (w != Wait::Key) {
      aborted = true;
    } else {
      photosDone++;
    }
  }

  for (int i = 0; i < n && !aborted; i++) {
    const Step& s = steps[i];
    const int phase = i % 2;  // 每一項輪替（scripts/ghost_photo.py 照同一條規則算）
    char l1[80];

    // 1. 預洗
    const Shot prep = precondition(renderer, g, "準備中…");

    // 2. 測試圖
    snprintf(l1, sizeof(l1), "測試圖（不用拍）　%d／%d", i + 2, photos);
    snprintf(l2, sizeof(l2), "%s", s.dwellMs == kLongDwellMs ? "停 10 分鐘，不要碰機器" : "5 秒後自動換下一張");
    Shot pat;
    Shot hold;  // X4 的 LS／L1／LF：同一張畫面再快速刷新一次（像素不動，面板電源留著開）
    uint32_t grayMs = 0;
    drawPatternBw(renderer, g, s.pattern, phase, l1, l2);
    if (s.pattern == Pattern::Image) {
      // 像畫冊的圖頁：黑白底用快速刷新，再推灰階，最後把控制器的舊畫面同步成黑白底（跟閱讀器一樣，電源也不特別管）
      pat = push(HalDisplay::FAST_REFRESH, false);
      const uint32_t tg = millis();
      drawGrayPlane(renderer, g, phase, /*lsb=*/true);
      renderer.copyGrayscaleLsbBuffers();
      drawGrayPlane(renderer, g, phase, /*lsb=*/false);
      renderer.copyGrayscaleMsbBuffers();
      display.displayGrayBuffer(false);
      drawPatternBw(renderer, g, s.pattern, phase, l1, l2);
      renderer.cleanupGrayscaleWithFrameBuffer();
      grayMs = millis() - tg;
    } else {
      // 像文字頁，從乾淨的 HALF 開始（通電／斷電兩組的像素歷史才一樣）；斷電那組帶 turnOff
      pat = push(HalDisplay::HALF_REFRESH, /*turnOff=*/s.power == Power::Off);
      // X4 的 0xD7 一定關電 → 同一張畫面再快速刷新一次把電源留著；X3 的 HALF 不帶 turnOff 本來就不 POF，
      //   不做這一次（多一次刷新只會讓 L1 跟 L0 多一個差別；codex 第二輪）
      if (s.power == Power::On && !x3) hold = push(HalDisplay::FAST_REFRESH, false);
    }

    // 3. 停留（短按不理；按住 3 秒中止）
    uint32_t dwelt = 0;
    w = waitFor(s.dwellMs, false, &dwelt);
    if (w == Wait::Abort) {
      aborted = true;
      DiagLog::line("GHOST abort at=%s dwell=%lu", s.id, static_cast<unsigned long>(dwelt));
      break;
    }

    // 4. 換成白畫面＋標籤（這一項要比的清法）。turnOff：X3 刷完就 POF（拍照等待不通電）；X4 的快速刷新關不掉。
    snprintf(l2, sizeof(l2), "第 %d／%d 張　拍一張照片，拍完按任意鍵", i + 2, photos);
    drawWhite(renderer, g, s.title, l2);
    Shot clean;
    switch (s.clean) {
      case Clean::Fast:
        clean = push(HalDisplay::FAST_REFRESH, true);
        break;
      case Clean::Half:
        clean = push(HalDisplay::HALF_REFRESH, true);
        break;
      case Clean::ForcedDiff:
        display.setForcedDiffClean(true);
        clean = push(HalDisplay::HALF_REFRESH, true);  // 顯示層把這次 HALF 換成「RED 反相＋快速刷新」
        display.setForcedDiffClean(false);
        DiagLog::crumb("X4DIFF", HalDisplay::lastForcedDiff, sizeof(HalDisplay::lastForcedDiff));
        break;
      case Clean::Full:
        clean = push(HalDisplay::FULL_REFRESH, true);
        break;
    }

    // 5. 等拍照
    uint32_t keyMs = 0;
    w = waitFor(kPhotoWaitMs, true, &keyMs);
    DiagLog::line(
        "GHOST id=%s n=%d/%d phase=%d pat=%s prep=%lu/%u draw=%lu/%u hold=%lu/%u gray=%lu dwell=%lu power=%s "
        "clean=%s:%lu/%u bankok=%d key=%lu wait=%s",
        s.id, i + 2, photos, phase, s.pattern == Pattern::Image ? "img" : "txt", static_cast<unsigned long>(prep.ms),
        static_cast<unsigned>(prep.bank), static_cast<unsigned long>(pat.ms), static_cast<unsigned>(pat.bank),
        static_cast<unsigned long>(hold.ms), static_cast<unsigned>(hold.bank), static_cast<unsigned long>(grayMs),
        static_cast<unsigned long>(dwelt), powerName(s.power), cleanName(s.clean), static_cast<unsigned long>(clean.ms),
        static_cast<unsigned>(clean.bank), clean.bank == expectedBank(s.clean) ? 1 : 0,
        static_cast<unsigned long>(keyMs), waitName(w));
    if (w != Wait::Key) {
      aborted = true;
      break;
    }
    done++;
    photosDone++;
  }

  // 結尾頁
  renderer.clearScreen(0xFF);
  renderer.drawCenteredText(UI_12_FONT_ID, 300, aborted ? "殘影測試：已停止" : "殘影測試完成", true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 360, "照片和 diag.log 一起傳回來。");
  renderer.drawCenteredText(UI_10_FONT_ID, 400, "按任意鍵繼續開機（1 分鐘後自動繼續）");
  push(HalDisplay::HALF_REFRESH, true);
  waitFor(60 * 1000, true, nullptr);
  DiagLog::line("GHOST end steps=%d/%d photos=%d/%d aborted=%d", done, n, photosDone, photos, aborted ? 1 : 0);

  renderer.setOrientation(savedOrientation);
  display.setForcedDiffClean(BenchFlags::x4Diff);
  display.requestResync();  // 接下來的開機畫面走清底
  DiagLog::setForced(prevForced, "ghost-end");
}

// ⭐ v360 桌布殘影測試（/ghostw.on）。v359 X4 的 /ghost.on 一整輪：通電停 10 分鐘再 HALF 清，白畫面量不到殘影 ——
//   但維護者看到的是「自動休眠時，桌布一出來就有殘影；手動休眠沒有」（沒開抗鋸齒）。兩條休眠畫桌布的方式完全一樣
//   （先提示、再 HALF 底＋灰階），差別只有「前一頁已經通電停了約 9 分鐘」。跟 L1 不同的兩點：
//   ① 閱讀頁是快速刷新畫的（L1 用 HALF 畫）；② 桌布大多是灰的，灰階那一趟是照面板狀態推的差分 —— 白畫面看不到的
//   殘留，灰上可能看得到。所以這裡照真的路徑走：快速刷新翻到一頁字 → 停（通電，同閱讀器）→「進入休眠」提示 → 畫灰的桌布
//   → 拍。
//     0   基準：預洗完直接畫桌布（兩種相位都算＝偏差）
//     WS  停 5 秒（手動休眠）　WL  停 10 分鐘（自動休眠）　WS2 停 5 秒、跟 WL 同相位、排在後面（位置與順序的對照）
//   ⚠️ 只測「停得久會不會讓桌布有殘影」：真的自動休眠畫完桌布還會 display.deepSleep()（關電＋控制器深睡），
//   這裡為了拍照不做（深睡之後要重跑 begin，跟 SD 共用 SPI，不在執行中做）。WL 沒殘影的話，下一個嫌疑就是那一步。
void runWallpaper(GfxRenderer& renderer) {
  const bool prevForced = DiagLog::setForced(true, "ghostw");
  if (!Storage.remove("/ghostw.on")) {
    DiagLog::line("GHOST skip why=rm-fail mode=wall");
    DiagLog::setForced(prevForced, "ghostw-end");
    return;
  }
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  display.setForcedDiffClean(false);
  display.defuseInitialFullSyncsKeepResync();
  const bool x3 = gpio.deviceIsX3();
  const Geom g = geometry(renderer);
  // 真的休眠前，GfxRenderer 的 fadingFix 是 loop() 每圈設的；這裡在 setup() 裡跑，還沒設過 → 先照設定設好（codex
  // v360）。
  renderer.setFadingFix(SETTINGS.fadingFix);
  struct WStep {
    const char* id;
    const char* title;
    uint32_t dwellMs;
    int phase;
  };
  // WS2 跟 WL 同相位（同一批格子）、排在 WL 之後：WL 比 WS2 深＝停得久的關係；WS 與 WS2
  // 不同相位、不同順序＝位置與順序的對照。
  static constexpr WStep kSteps[3] = {{"WS", "WS　停 5 秒（像手動休眠）", kShortDwellMs, 0},
                                      {"WL", "WL　停 10 分鐘（像自動休眠）", kLongDwellMs, 1},
                                      {"WS2", "WS2　停 5 秒（對照）", kShortDwellMs, 1}};
  constexpr int n = 3;
  constexpr int photos = n + 1;
  DiagLog::line("GHOST start dev=%s mode=wall steps=%d w=%d h=%d grid=%d,%d,%d,%d fid=%d/%d/%d fading=%u",
                x3 ? "x3" : "x4", n, g.w, g.h, g.gx, g.gy, g.cw, g.ch, kFidInset, kFidTL, kFid,
                static_cast<unsigned>(SETTINGS.fadingFix));

  // 說明頁
  renderer.clearScreen(0xFF);
  int y = 140;
  renderer.drawCenteredText(UI_12_FONT_ID, y, "桌布殘影測試", true, EpdFontFamily::BOLD);
  y += 56;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "共 4 張要拍，約 17 分鐘。");
  y += 30;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "其中 1 張要等 10 分鐘才出現（不要碰機器）。");
  y += 48;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "畫面變成灰色、下方出現代號時：");
  y += 30;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "拍一張照片，拍完按任意鍵。", true, EpdFontFamily::BOLD);
  y += 48;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "想停下來：任一鍵按住 3 秒。");
  y += 48;
  renderer.drawCenteredText(UI_12_FONT_ID, y, "按任意鍵開始", true, EpdFontFamily::BOLD);
  push(HalDisplay::HALF_REFRESH, true);
  Wait w = waitFor(kPhotoWaitMs, true, nullptr);
  bool aborted = w != Wait::Key;
  if (aborted) DiagLog::line("GHOST abort at=intro wait=%s", waitName(w));
  int done = 0;
  int photosDone = 0;
  char l2[96];

  if (!aborted) {
    const Shot prep = precondition(renderer, g, "準備中…");
    snprintf(l2, sizeof(l2), "第 1／%d 張　拍一張照片，拍完按任意鍵", photos);
    const uint32_t wallMs = paintWall(renderer, g, "0　基準（桌布）", l2);
    uint32_t keyMs = 0;
    w = waitFor(kPhotoWaitMs, true, &keyMs);
    DiagLog::line("GHOST id=0 n=1/%d mode=wall prep=%lu/%u wall=%lu key=%lu wait=%s", photos,
                  static_cast<unsigned long>(prep.ms), static_cast<unsigned>(prep.bank),
                  static_cast<unsigned long>(wallMs), static_cast<unsigned long>(keyMs), waitName(w));
    leaveWall(renderer, g);
    if (w != Wait::Key) {
      aborted = true;
    } else {
      photosDone++;
    }
  }

  for (int i = 0; i < n && !aborted; i++) {
    const WStep& s = kSteps[i];
    const int phase = s.phase;
    char l1[80];
    const Shot prep = precondition(renderer, g, "準備中…");
    // 像翻到一頁字：快速刷新，關不關電照「日照淡化修正」（跟閱讀器一樣；預設關 → X4 的 0xFC 不關、X3 也不 POF）
    snprintf(l1, sizeof(l1), "測試圖（不用拍）　%d／%d", i + 2, photos);
    snprintf(l2, sizeof(l2), "%s", s.dwellMs == kLongDwellMs ? "停 10 分鐘，不要碰機器" : "5 秒後自動換下一張");
    drawPatternBw(renderer, g, Pattern::Text, phase, l1, l2);
    const bool pageOff = SETTINGS.fadingFix != 0;  // 跟閱讀器一樣：turnOff＝日照淡化修正（codex v360 第二輪）
    const Shot pat = push(HalDisplay::FAST_REFRESH, pageOff);
    uint32_t dwelt = 0;
    w = waitFor(s.dwellMs, false, &dwelt);
    if (w == Wait::Abort) {
      aborted = true;
      DiagLog::line("GHOST abort at=%s dwell=%lu", s.id, static_cast<unsigned long>(dwelt));
      break;
    }
    // 跟真的休眠一樣（main.cpp 進入休眠那段）：存下這一頁 → 畫「進入休眠」提示 → 還原這一頁（restore 會把控制器的
    //   兩個平面改寫成這一頁，畫面上仍是提示），再畫桌布。存不下就照樣畫提示（真的路徑也是）。
    const uint32_t tp = millis();
    const bool stored = renderer.storeBwBuffer();
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    const bool restored = stored && renderer.restoreBwBuffer();
    const uint32_t popMs = millis() - tp;
    snprintf(l2, sizeof(l2), "第 %d／%d 張　拍一張照片，拍完按任意鍵", i + 2, photos);
    const uint32_t wallMs = paintWall(renderer, g, s.title, l2);
    uint32_t keyMs = 0;
    w = waitFor(kPhotoWaitMs, true, &keyMs);
    DiagLog::line(
        "GHOST id=%s n=%d/%d mode=wall phase=%d pat=txt prep=%lu/%u draw=%lu/%u dwell=%lu off=%d pop=%lu/%d/%d "
        "wall=%lu "
        "key=%lu wait=%s",
        s.id, i + 2, photos, phase, static_cast<unsigned long>(prep.ms), static_cast<unsigned>(prep.bank),
        static_cast<unsigned long>(pat.ms), static_cast<unsigned>(pat.bank), static_cast<unsigned long>(dwelt),
        pageOff ? 1 : 0, static_cast<unsigned long>(popMs), stored ? 1 : 0, restored ? 1 : 0,
        static_cast<unsigned long>(wallMs), static_cast<unsigned long>(keyMs), waitName(w));
    leaveWall(renderer, g);
    if (w != Wait::Key) {
      aborted = true;
      break;
    }
    done++;
    photosDone++;
  }

  renderer.clearScreen(0xFF);
  renderer.drawCenteredText(UI_12_FONT_ID, 300, aborted ? "桌布殘影測試：已停止" : "桌布殘影測試完成", true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, 360, "照片和 diag.log 一起傳回來。");
  renderer.drawCenteredText(UI_10_FONT_ID, 400, "按任意鍵繼續開機（1 分鐘後自動繼續）");
  push(HalDisplay::HALF_REFRESH, true);
  waitFor(60 * 1000, true, nullptr);
  DiagLog::line("GHOST end mode=wall steps=%d/%d photos=%d/%d aborted=%d", done, n, photosDone, photos,
                aborted ? 1 : 0);

  renderer.setOrientation(savedOrientation);
  display.setForcedDiffClean(BenchFlags::x4Diff);
  display.requestResync();
  DiagLog::setForced(prevForced, "ghostw-end");
}

}  // namespace GhostTest
