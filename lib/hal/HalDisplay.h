#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

class HalDisplay {
 public:
  // Constructor with pin configuration
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // Refresh modes
  enum RefreshMode {
    FULL_REFRESH,  // Full refresh with complete waveform
    HALF_REFRESH,  // Half refresh (1720ms) - balanced quality and speed
    FAST_REFRESH,  // Fast refresh using custom LUT
    // v55（v185 移植回來）：送給驅動的仍是 Half，差別是呼叫端【不】requestResync ——
    // 沒有那個旗標，X3 驅動才會走狀態驅動的 scrub bank（全像素朝目標態刷、不看舊態、
    // 無反相閃黑），而不是被升級成 GC 全同步鏈。用途限定閱讀器的週期性清殘影
    // （ReaderUtils::displayWithRefreshCycle）；封面／待機等轉場仍走 HALF_REFRESH。
    // ⚠️ UC8279 上這條在 v185 是 bench 候選（/scrub.on），bank 未經實機驗證。
    HALF_REFRESH_SCRUB
  };

  // Pass seamless=true on any path where the panel already shows the
  // content it should after begin() returns (silent reboot's popup,
  // sleep-wake with a restored buffer). Skips the wakeup-gated
  // requestResync() and defuses the SDK's X3 _x3InitialFullSyncsRemaining
  // counter; otherwise the first two paints get promoted to FULL
  // (~770ms each on X3).
  void begin(bool seamless = false);

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::DISPLAY_HEIGHT;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  // Non-blocking refresh (shadow-free): starts the panel waveform and returns
  // while the panel refreshes on its own. The framebuffer must stay untouched
  // until waitRefreshComplete(), and the caller must rebuild the differential
  // baseline before the next differential update (the tiled grayscale cleanup
  // does). Panels without deferral fall back to a blocking refresh.
  void displayBufferAsync(RefreshMode mode = RefreshMode::FAST_REFRESH);
  // Block until a pending deferred refresh completes (no-op when none is).
  void waitRefreshComplete();
  // True when displayBufferAsync() genuinely overlaps (panel driver defers);
  // false where it falls back to a blocking refresh.
  bool supportsAsyncRefresh() const;
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // Power management
  void deepSleep();

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  // Lend the framebuffer's ~48 KB STORAGE to a memory-hungry phase (chapter
  // builds) without freeing it: the allocation never moves, so repeated loans
  // cannot fragment the heap (free+realloc measurably did). No display calls
  // between lend and return; the panel keeps its last refreshed image. The
  // buffer comes back white — redraw fully. Returns nullptr if already lent.
  uint8_t* lendFrameBufferStorage(uint32_t* sizeOut);
  void returnFrameBufferStorage();

  // X3 grayscale preconditioning (OEM "AA-pre-BW(mid)" settle pass), windowed
  // to the gray region in physical panel coordinates (no-arg = full frame).
  // Call after the BW base frame is displayed and before the grayscale planes
  // are written; no-op on X4. See EInkDisplay::preconditionGrayscale.
  void preconditionGrayscale();
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows. On X3, HALF fallback first requests a resync to match
  // displayBuffer(HALF); FAST fallback keeps the OEM differential base waveform
  // ("AA-pre-BW(mid)"). Other panels display normally with `fallback` mode.
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  // absolute=true：X3 走驅動的「絕對四階」通道（UC8279 = 原廠 XTH4 表；平面直接編碼
  // 四階、不需要先畫 B/W 底）。v185 待機壁紙 bench（/wall4.on）用，其他面板忽略。
  void displayGrayBuffer(bool turnOffScreen = false, bool absolute = false);
  // v185 bench：選 UC8279 的灰階推力表（0 = 正式；1..N 見 Uc8279X3Luts.h）。其他面板 no-op。
  void setGrayscaleVariant(uint8_t variant);
  // v185 bench 證人：最近一次 B/W 刷新選到的 bank（0 無 / 1 GC / 2 DU / 3 scrub）。
  uint8_t lastRefreshBank() const;
  // 只有 UC8279 X3 的 factoryMode 是真的絕對四階（XTH4）；其他面板對 factoryMode 另有語意，
  // 餵階碼平面會畫出垃圾。SleepActivity 的 /wall4.on 分支以此閘門。
  bool supportsAbsoluteGrayscale() const;
  // v186：面板驅動擔保「不 resync 的 Half」是實證過的週期清殘影（UC8253 X3 的 _half，v55–v130）。
  // UC8279 的 scrub bank 仍是 bench（/scrub.on）→ 回 false，預設走 GC。
  bool prefersScrubClean() const;

  // Tiled grayscale: stream one band of a plane (lsbPlane selects LSB/MSB RAM)
  // straight to the controller; supportsStripGrayscale() gates the path. See
  // EInkDisplay::writeGrayscalePlaneStrip.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows);
  bool supportsStripGrayscale() const;

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

 private:
  EInkDisplay einkDisplay;
};

extern HalDisplay display;
