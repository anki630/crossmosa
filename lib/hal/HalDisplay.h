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
    // v55:清殘影用的半刷,但【不要求控制器重新同步】。
    //
    // X3 上 HALF_REFRESH 會觸發 requestResync(1),驅動的 doFullSync 因此為真,走的是
    // 【_full 波形 bank + 白底 DTM1 + condition pass + 事後 fast settle】——實測 3,192ms
    // (一般翻頁 441ms)。不 resync 時走的是 _half bank(WW==BW、WB==BB:忽略 DTM1、
    // 把【每個】像素驅到目標),清殘影的語意一樣達成,但少掉整條全同步鏈。
    // ⚠️ 所以這不是「同波形」——是【不同 bank、同樣全像素驅動】。
    //
    // 對灰階的影響:兩條路徑結束時的控制器狀態相同(Uc8253X3Driver::display 尾端無條件
    // 把 DTM1 同步成當前畫面、並清 lsbValid / 設 redRamSynced),所以後續 AA 的前提不變。
    //
    // 用途限定:閱讀器(EPUB/TXT/XTC 共用 ReaderUtils::displayWithRefreshCycle)的週期性
    // 清殘影。封面/待機等轉場仍走 HALF_REFRESH——上游是靠那條全同步鏈修掉轉場 ghosting 的。
    HALF_REFRESH_SCRUB
  };

  // Pass seamless=true on any path where the panel already shows the
  // content it should after begin() returns (silent reboot's popup,
  // sleep-wake with a restored buffer). Skips the wakeup-gated
  // requestResync() and defuses the SDK's X3 _x3InitialFullSyncsRemaining
  // counter; otherwise the first two paints get promoted to FULL
  // (~770ms each on X3).
  void begin(bool seamless = false);

  // v85 measurement: reserve the framebuffer allocation EARLY, before the SD
  // and JSON stores fragment the default heap pool. Only the allocation — no
  // SPI, no panel traffic; begin() later is unaffected because the SDK guards
  // its alloc with `if (!frameBuffer0)`.
  //
  // Why this exists: linking NimBLE costs 28,256 B of RAM, which shrinks the
  // prio-0 pool from 140,992 to 112,736. By the time the normal
  // setupDisplayAndFonts() runs, that pool no longer has a 52,272-byte hole,
  // so the framebuffer spills into the prio-1 "retention" pool — the only pool
  // that can supply the 40-55 KB contiguous blocks WiFi/OPDS/SMB need. Doing
  // the allocation first keeps the framebuffer in prio-0 and leaves retention
  // free. Measured effect is what this build is for.
  bool reserveFrameBufferEarly();

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

  // v56:取消 SDK 在冷開機時預備的【第二次】強制全同步,並把【下一次 FAST 繪製】升級成
  // HALF_REFRESH_SCRUB。
  //
  // 背景:SDK 開機時 _initialFullSyncsRemaining = 2 —— 開機動畫吃掉第一次,之後第一個
  // 走全同步的畫面吃掉第二次,實測 disp=2,991ms。第一次是必要的(冷開機後面板電荷狀態
  // 未知,差分刷新無基準);第二次則是保險。開機動畫畫完後 DTM1 已同步成動畫畫面,
  // 所以下一次其實可以走差分——但那會把熊 logo 的殘影留在畫面上,這正是第二次全同步在防的。
  // scrub 兩者兼顧:全像素驅動(不留 logo 殘影)+ 只要一次刷新(約 723ms)。
  //
  // 「下一次 FAST 繪製」未必是書的第一頁——也可能是「建立索引中」彈窗或圖片頁的佔位框。
  // 這是【對的】:v56 之前那第二次強制全同步落在的正是同一格(driver 的 doFullSync 不看
  // 呼叫端要求什麼模式),而那些過場畫面都是全螢幕的,logo 一樣被刷掉,殘影不會漏到後面。
  //
  // ⚠️ 只在 X3 生效,且只升級 FAST_REFRESH;若下一次本來就是 HALF/FULL,旗標只被消耗掉、
  // 不會把較強的刷新【降級】。
  void skipInitialResyncAndScrubNext();

  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows. On X3, HALF fallback first requests a resync to match
  // displayBuffer(HALF); FAST fallback keeps the OEM differential base waveform
  // ("AA-pre-BW(mid)"). Other panels display normally with `fallback` mode.
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false);

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
  bool scrubNextFastPaint_ = false;  // v56 一次性旗標,見 skipInitialResyncAndScrubNext()
};

extern HalDisplay display;
