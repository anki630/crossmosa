#include <BoardConfig.h>
#include <Breadcrumb.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

#include <cstdio>
#include <memory>
#include <new>

// Global HalDisplay instance
HalDisplay display;

#define SD_SPI_MISO 7

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

char HalDisplay::lastForcedDiff[64] = {0};

bool HalDisplay::isSsd1677() const {
  return !gpio.deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::SSD1677;
}

bool HalDisplay::supportsForcedDiffClean() const { return isSsd1677() && !einkDisplay.isInverted(); }

// v357 bench（/x4diff.on）：清底（HALF，X4 上約 1.8 秒）→「RED 寫成新畫面的反相＋快速刷新」（約 0.6 秒）。
//   SSD1677 的快速刷新拿 BW（新）跟 RED（舊）逐點比：相同的像素不推、不同的走完整的推。RED 寫成新畫面的反相 →
//   每一點都「不同」→
//   每一點都被推到目標，想要的效果是接近清底（灰階殘留、殘影），代價是一次快速刷新。乾不乾淨靠照片比對。 RED
//   分帶寫（每帶 24 列，借 writeGrayscalePlaneStrip 的 MSB＝RED RAM 那條路；它不改任何狀態旗標），之後照常快速刷新：
//   單緩衝的快速刷新在刷新前只寫 BW、不動 RED，刷新後兩格都重寫成這張畫面 → 之後的差分基準照舊。
//   驅動自己的升級（開機／醒來第一張、剛離開灰階）照舊變 HALF：那時兩格都先寫成新畫面，這裡寫的 RED 被蓋掉，無害。
//   畫面反相時不做（反相的位元組要驅動自己翻，這裡的 RED 會是反的）；配不到暫存就照舊清底。
int32_t HalDisplay::substituteForcedDiff(RefreshMode& mode) {
  if (!forcedDiffClean_ || mode != RefreshMode::HALF_REFRESH || !isSsd1677() || einkDisplay.isInverted()) {
    return -1;
  }
  const uint32_t t0 = millis();
  const uint16_t wb = einkDisplay.getDisplayWidthBytes();
  const uint16_t h = einkDisplay.getDisplayHeight();
  constexpr uint16_t kRows = 24;
  forcedDiffCount_++;
  std::unique_ptr<uint8_t[]> strip(new (std::nothrow) uint8_t[static_cast<size_t>(wb) * kRows]);
  if (!strip) return -2;
  const uint8_t* fb = einkDisplay.getFrameBuffer();
  for (uint16_t y = 0; y < h; y += kRows) {
    const uint16_t n = static_cast<uint16_t>((h - y < kRows) ? (h - y) : kRows);
    const uint8_t* src = fb + static_cast<size_t>(y) * wb;
    const size_t len = static_cast<size_t>(n) * wb;
    for (size_t i = 0; i < len; i++) strip[i] = static_cast<uint8_t>(~src[i]);
    einkDisplay.writeGrayscalePlaneStrip(EInkDisplay::GRAY_PLANE_MSB, strip.get(), y, n);
  }
  mode = RefreshMode::FAST_REFRESH;
  return static_cast<int32_t>(millis() - t0);
}

// 刷新（或非同步起刷新）之後才記：驅動可能把這次快速刷新升回清底（開機／醒來第一張、剛離開灰階、反相剛改），
//   那時兩格先被寫成新畫面、寫好的 RED 被蓋掉 —— bank=1 就是「沒做成」，bank=2 才是真的強制差分（codex）。
//   所有顯示呼叫都在持 RenderLock 的同一條路上（閱讀器 render task／主任務拿鎖），計數器不必 atomic。
void HalDisplay::publishForcedDiff(const int32_t redMs) {
  if (redMs == -1) return;
  char line[sizeof(lastForcedDiff)];
  if (redMs == -2) {
    snprintf(line, sizeof(line), "n=%lu fallback=alloc", static_cast<unsigned long>(forcedDiffCount_));
  } else {
    snprintf(line, sizeof(line), "n=%lu redms=%ld bank=%u", static_cast<unsigned long>(forcedDiffCount_),
             static_cast<long>(redMs), static_cast<unsigned>(einkDisplay.lastRefreshBank()));
  }
  breadcrumbPublish(lastForcedDiff, sizeof(lastForcedDiff), line);  // 先到先得；n= 看得出有沒有漏印
}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

void HalDisplay::defuseInitialFullSyncsKeepResync() {
  // 見 HalDisplay.h 的說明。只在 begin(seamless=false) 之後、第一次繪製之前呼叫。
  // 8279：_oldPlaneValid=true、_initialFullsRemaining=0；8253：_initialFullSyncsRemaining=0、_redRamSynced=true。
  // 兩者都不碰 _forceFullSyncNext。
  // ⚠️ v316 更正：codex 第二輪說「GC 不看舊平面」是錯的。8279 的 GC 是 KW 兩平面波形，
  //    displayStart 只在 !_oldPlaneValid 時才把 DTM1 填白；v312 用 skipInitialResync() 把
  //    _oldPlaneValid 硬設成 true → 第一次 GC 拿【開機後的 RAM 垃圾】當舊幀差分 → 使用者看到
  //    「像電視開機的橫線」。改成只歸零初繪預算（defuseInitialFulls），_oldPlaneValid 留 false：
  //    首頁那次 GC 先填白舊平面再刷（多一次 52KB SPI ≈ 30ms），之後計數 0 → 下一次 FAST 走 DU。
  //    「保留一次性 resync」仍是前提；若哪天 begin() 不再對電源鍵喚醒 requestResync()，這裡要自己補。
  einkDisplay.defuseInitialFulls();
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    // v55：驅動端仍收到 Half；差別在呼叫端不 requestResync（見 HalDisplay.h）。
    case HalDisplay::HALF_REFRESH_SCRUB:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  // v269：面板上若還留著抗鋸齒的灰，這一次整頁刷新就要走清潔路徑（見 noteGrayPanelDirty）。
  //   exchange：誰先取到誰負責清，不會兩個畫面各清一次。
  const bool grayDirty = grayPanelDirty_.exchange(false, std::memory_order_relaxed);
  const int32_t forcedDiff = substituteForcedDiff(mode);  // v357 bench（只有 X4 SSD1677、放了 /x4diff.on 才有作用）
  if (gpio.deviceIsX3() && (mode == RefreshMode::HALF_REFRESH || grayDirty)) {
    einkDisplay.requestResync(1);
  }

  frameSeq_++;
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
  publishForcedDiff(forcedDiff);
}

void HalDisplay::displayBufferAsync(HalDisplay::RefreshMode mode) {
  const bool grayDirty = grayPanelDirty_.exchange(false, std::memory_order_relaxed);  // v269，同 displayBuffer
  const int32_t forcedDiff = substituteForcedDiff(mode);                              // v357 bench
  if (forcedDiff >= 0) {
    // 強制差分改走阻塞刷新：非同步路徑刷新後驅動不重寫 BW／RED（要呼叫端自己重建），RED 會留著反相 ——
    //   閱讀器現有的呼叫端都會重建，但顯示層自己不擔保，bench 不冒這個險（codex）。只有 X4 SSD1677 會走到這裡。
    frameSeq_++;
    einkDisplay.displayBuffer(convertRefreshMode(mode), false);
    publishForcedDiff(forcedDiff);
    return;
  }
  if (gpio.deviceIsX3() && (mode == RefreshMode::HALF_REFRESH || grayDirty)) {
    einkDisplay.requestResync(1);
  }

  frameSeq_++;
  einkDisplay.displayBufferAsyncNoShadow(convertRefreshMode(mode));
}

void HalDisplay::waitRefreshComplete() { einkDisplay.waitRefreshComplete(); }

bool HalDisplay::refreshBusy() { return einkDisplay.refreshBusy(); }

bool HalDisplay::resyncAfterAsyncRefresh(const bool frameIsTrusted) {
  if (!frameIsTrusted) {
    einkDisplay.requestResync();
    return false;
  }
  // SSD1677 的 displayFinish 只等刷新結束（驅動註解：「X4 post-waveform needs nothing from the host frame」），
  //   刷新後的 RAM 同步只在阻塞路徑做：BW 與 RED 兩格都重寫成剛上面板的畫面（驅動註解：不能假設 BW 撐過刷新沒變）。
  //   這裡照做兩格（codex 複查：只寫 RED 的話，之後的局部刷新會拿到沒同步的 BW）。
  //   SDK 在單緩衝模式沒有「只寫 RAM、不刷新」的公開入口，借兩個剛好就是這件事的呼叫：
  //   copyGrayscaleLsbBuffers → SSD1677 copyGrayscaleLsb＝整張寫進 BW RAM（不改任何狀態旗標）；
  //   cleanupGrayscaleBuffers → SSD1677 cleanupGrayscaleBuffers＝整張寫進 RED、清 _inGrayscaleMode（刷完黑白本來就是
  //   false）。 畫面反相時兩者都不寫 —— 但那時非同步本來就退回阻塞路徑（驅動自己同步過了）。
  if (gpio.deviceIsX3() || BoardConfig::ACTIVE.displayController != BoardConfig::DisplayController::SSD1677) {
    return false;
  }
  const uint8_t* fb = einkDisplay.getFrameBuffer();
  einkDisplay.copyGrayscaleLsbBuffers(fb);
  einkDisplay.cleanupGrayscaleBuffers(fb);
  return true;
}

bool HalDisplay::supportsAsyncRefresh() const { return einkDisplay.supportsAsyncRefresh(); }

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  const bool grayDirty = grayPanelDirty_.exchange(false, std::memory_order_relaxed);  // v269，同 displayBuffer
  const int32_t forcedDiff =
      substituteForcedDiff(mode);  // v357 bench（FreeInkDisplay::refreshDisplay 就是 displayBuffer）
  if (gpio.deviceIsX3() && (mode == RefreshMode::HALF_REFRESH || grayDirty)) {
    einkDisplay.requestResync(1);
  }

  frameSeq_++;
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
  publishForcedDiff(forcedDiff);
}

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) { return einkDisplay.lendBuildStorage(sizeOut); }

void HalDisplay::returnFrameBufferStorage() { einkDisplay.returnBuildStorage(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  // X3: a HALF fallback means the caller wants a clean base (e.g. the sleep
  // cover, a full-screen swap from arbitrary prior content). Without this, the
  // X3 grayscale base takes its gentle differential happy path and the prior
  // home/reader frame ghosts through the soft aa_pre_bw_mid waveform. Forcing a
  // resync makes displayGrayscaleBase clear first, matching displayBuffer(HALF).
  // The reader's FAST path is deliberately left on the differential path so
  // per-page grayscale stays cheap.
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  frameSeq_++;
  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::preconditionGrayscale() {
  frameSeq_++;
  einkDisplay.preconditionGrayscale();
}

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  frameSeq_++;
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen, bool absolute) {
  frameSeq_++;
  einkDisplay.displayGrayBuffer(turnOffScreen, nullptr, absolute);
}

void HalDisplay::setGrayscaleVariant(uint8_t variant) { einkDisplay.setGrayscaleVariant(variant); }

uint8_t HalDisplay::lastRefreshBank() const { return einkDisplay.lastRefreshBank(); }

bool HalDisplay::supportsAbsoluteGrayscale() const { return einkDisplay.supportsAbsoluteGrayscale(); }

bool HalDisplay::prefersScrubClean() const { return einkDisplay.prefersScrubClean(); }

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

bool HalDisplay::supportsStripGrayscale() const { return einkDisplay.supportsStripGrayscale(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
