#include "SleepActivity.h"
#include <DataDir.h>

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LogoBear240.h"
#include "util/BenchFlags.h"
#include "util/DiagLog.h"

#include <cstdio>
#include <cstring>

// ─── v320：桌布平面快取 ──────────────────────────────────────────────────────
// 實機（diag319）：按電源鍵休眠 → 真的睡著，中位 5.9 秒。其中約 3.8 秒是把【同一張 BMP 解碼三趟】
// （黑白一趟＋兩個灰階平面各一趟；SD 讀取約 450KB/s，418KB 的 8-bit 來源每趟都要整個讀一遍）——
// 而桌布是不會變的檔案，算好的平面是 1bpp、只有來源的 1/8。
// 做法：每張桌布第一次用到時，把算好的平面（每個＝一張 framebuffer，52,272 B）存到
// <DataDir>/wallcache/<路徑雜湊>.wpc；之後休眠直接把平面讀回 framebuffer、走原本同一組
// copyGrayscale*／display 呼叫。真關機與淺睡眠共用這條路，兩者同價受惠。
//
// 身分（codex 第一輪判「不可出貨」之後重做；教訓 A-20：快取存在≠快取有效）：
//   ① 進場（畫之前，約 60ms）：來源大小＋32 列整列抽樣雜湊＋兩個獨立的路徑雜湊＋參數雜湊。
//      參數含【最終幾何】（x／y／裁切）、方向、濾鏡、灰階與否，以及【韌體版號字串】——
//      renderer／抖色一改，升級後快取自動全部失效重建，不靠人記得 +1。
//   ② 畫完之後（使用者已經看到桌布）：借 framebuffer 當緩衝把來源【整個檔】雜湊一遍對檔尾的值。
//      不符 → 刪快取。名畫桌布每張大小都一樣，只改局部的同名換圖 ① 可能看不出來；② 保證舊圖
//      最多只會被顯示一次。整檔雜湊約 0.9 秒，但發生在顯示之後，不佔「按下到桌布出現」的時間。
//   ③ 每個平面各一個 FNV-1a，讀回來逐一驗。任何一項不符 → 刪快取、整張走原本的解碼路徑（不拼接）。
//   沒有負快取。寫入失敗（卡滿／壞軌）→ 這次開機不再寫；刪不掉壞快取 → 這次開機不再讀。
//   上限 WALLCACHE_MAX_FILES 個檔，滿了就不再新增（不做淘汰，免得隨機選圖變成每次睡都重寫 157KB）。
//   設定 → 清除快取 會一併清掉 wallcache/。
struct WallCacheSrc {
  const char* path;      // 來源 BMP 的完整路徑（當快取檔名的鍵）
  HalFile* file;         // 來源檔（畫完之後拿來算整檔雜湊；那時解碼已結束）
  uint32_t fileSize;     // 來源檔案大小
  uint32_t fingerprint;  // 32 列抽樣雜湊（同名同大小換一張不同的圖，進場就認得出來）
};

namespace {
constexpr uint16_t WALLCACHE_VERSION = 1;        // 檔案佈局的版本
// ⚠️ 像素語意的版本：GfxRenderer::drawBitmap／drawBitmap1Bit／Bitmap 的抖色或平面對映改了就 +1，
//    否則舊快取會用舊的算法顯示。v320 一度用韌體版號字串，但那讓【每次刷韌體】所有桌布都要重算——
//    對每天刷機的人等於沒有快取。（GfxRenderer.cpp 的 drawBitmap 上方有指回這裡的註解。）
constexpr uint16_t WALLCACHE_PIXEL_VERSION = 1;
constexpr uint16_t WALLCACHE_MAX_FILES = 400;  // 約 63MB 上限

struct WallCacheFooter {  // 放【檔尾】：寫入時不需要回頭 seek 改檔頭
  char magic[4];          // "CMWP"
  uint16_t version;
  uint8_t planes;         // 1＝只有 BW；3＝BW＋LSB＋MSB
  uint8_t reserved;
  uint32_t bufSize;       // 每個平面的位元組數（＝renderer.getBufferSize()）
  uint32_t srcSize;
  uint32_t srcFingerprint;
  uint32_t srcFullHash;   // 來源整檔 FNV-1a（畫完之後才驗）
  uint32_t pathHash2;     // 第二個獨立的路徑雜湊：檔名用的那個 32-bit 撞號時認得出來
  uint32_t pathLen;
  uint32_t paramsHash;
  uint32_t planeHash[3];
};

char gWcPath[112];  // 單執行緒（休眠畫面只在主任務上畫）；放 static 是為了不吃堆疊
char gWcTmp[112];
char gWcDir[96];
bool gWcWriteDisabled = false;  // 這次開機寫入失敗過 → 不再寫（防止卡滿／壞軌時每次睡都重試 157KB）
bool gWcReadDisabled = false;   // 壞快取刪不掉 → 不再讀

uint32_t wcFnv(const uint8_t* d, size_t n, uint32_t h = 2166136261u) {
  for (size_t i = 0; i < n; i++) {
    h ^= d[i];
    h *= 16777619u;
  }
  return h;
}

// 檔頭＋平均分佈的整列取樣。buf 借呼叫端的（framebuffer：此刻上面只剩已經顯示過的「進入休眠」提示，
// 接下來不管走哪條路都會先 clearScreen）。結束後把檔案位置歸零；任何一步失敗回 false。
bool wallFingerprint(HalFile& f, uint8_t* buf, size_t bufLen, uint32_t* out) {
  constexpr size_t SAMPLE = 528;
  constexpr int SAMPLES = 32;
  if (bufLen < SAMPLE) return false;
  const size_t sz = f.size();
  if (sz < SAMPLE) return false;
  uint32_t h = 2166136261u;
  for (int i = 0; i < SAMPLES; i++) {
    const size_t off = static_cast<size_t>((static_cast<uint64_t>(sz - SAMPLE) * i) / (SAMPLES - 1));
    if (!f.seek(off)) return false;
    const int n = f.read(buf, SAMPLE);
    if (n != static_cast<int>(SAMPLE)) return false;
    h = wcFnv(buf, SAMPLE, h);
  }
  if (!f.seek(0)) return false;
  *out = h;
  return true;
}

// 整檔雜湊。只在【顯示完成之後】呼叫：那時 framebuffer 已經沒有人要用，借來當 52KB 的讀取緩衝。
bool wallFullHash(HalFile& f, uint8_t* buf, size_t bufLen, uint32_t expectSize, uint32_t* out) {
  if (!f.seek(0)) return false;
  uint32_t h = 2166136261u;
  size_t total = 0;
  for (;;) {
    const int n = f.read(buf, bufLen);
    if (n < 0) return false;
    if (n == 0) break;
    h = wcFnv(buf, static_cast<size_t>(n), h);
    total += static_cast<size_t>(n);
    if (total > expectSize) return false;
  }
  if (total != expectSize) return false;
  *out = h;
  return true;
}

bool wallCachePaths(const char* src) {
  // 檔名用兩個獨立的 32-bit 雜湊（等同 64-bit 鍵）：只用一個的話，撞號的兩張桌布會輪流互刪對方的快取。
  const unsigned long key = static_cast<unsigned long>(wcFnv(reinterpret_cast<const uint8_t*>(src), strlen(src)));
  const unsigned long key2 =
      static_cast<unsigned long>(wcFnv(reinterpret_cast<const uint8_t*>(src), strlen(src), 0x9747b28cu));
  const int d = snprintf(gWcDir, sizeof(gWcDir), "%s/wallcache", DataDir::path());
  const int a = snprintf(gWcPath, sizeof(gWcPath), "%s/wallcache/%08lx%08lx.wpc", DataDir::path(), key, key2);
  const int b = snprintf(gWcTmp, sizeof(gWcTmp), "%s/wallcache/%08lx%08lx.tmp", DataDir::path(), key, key2);
  return d > 0 && static_cast<size_t>(d) < sizeof(gWcDir) && a > 0 && static_cast<size_t>(a) < sizeof(gWcPath) &&
         b > 0 && static_cast<size_t>(b) < sizeof(gWcTmp);
}

uint32_t wallPathHash2(const char* src) {
  return wcFnv(reinterpret_cast<const uint8_t*>(src), strlen(src), 0x9747b28cu);  // 不同的起始值＝獨立的第二個雜湊
}

void wallCacheDrop() {
  if (!Storage.remove(gWcPath) && Storage.exists(gWcPath)) {
    // 刪不掉＝檔案系統出問題。讀寫都停：不停寫的話，接下來那次 miss 會白寫 157KB 再卡在 rename（codex 第二輪）。
    gWcReadDisabled = true;
    gWcWriteDisabled = true;
  }
}

bool wcReadPlane(HalFile& f, uint8_t* dst, size_t len, uint32_t expectHash) {
  const int got = f.read(dst, len);
  return got > 0 && static_cast<size_t>(got) == len && wcFnv(dst, len) == expectHash;
}

// 回傳 true＝整張由快取完成（含顯示）。false＝沒用到快取或中途失敗；呼叫端一律走完整解碼
// （不拼接：黑白底若已上面板，解碼路徑會再刷一次——只在快取檔損壞時發生）。
// *fullHashOut：命中時帶回檔尾記的來源整檔雜湊，給顯示之後的驗證用。
bool wallCachePaint(GfxRenderer& renderer, const WallCacheSrc& src, uint32_t paramsHash, bool hasGray,
                    const char** why, uint32_t* fullHashOut) {
  *why = "off";
  if (gWcReadDisabled) return false;
  *why = "path";
  if (!wallCachePaths(src.path)) return false;
  *why = "none";
  if (!Storage.exists(gWcPath)) return false;
  HalFile f;
  if (!Storage.openFileForRead("WPC", gWcPath, f)) return false;

  const size_t bufSize = renderer.getBufferSize();
  const uint8_t planes = hasGray ? 3 : 1;
  WallCacheFooter ft{};
  const size_t want = static_cast<size_t>(planes) * bufSize + sizeof(ft);
  *why = "size";
  bool ok = f.size() == want && f.seek(want - sizeof(ft)) &&
            f.read(&ft, sizeof(ft)) == static_cast<int>(sizeof(ft));
  if (ok) {
    *why = "ident";
    ok = memcmp(ft.magic, "CMWP", 4) == 0 && ft.version == WALLCACHE_VERSION && ft.planes == planes &&
         ft.bufSize == bufSize && ft.srcSize == src.fileSize && ft.srcFingerprint == src.fingerprint &&
         ft.pathHash2 == wallPathHash2(src.path) && ft.pathLen == strlen(src.path) && ft.paramsHash == paramsHash;
  }
  if (ok) {
    *why = "bw";
    ok = f.seek(0) && wcReadPlane(f, renderer.getFrameBuffer(), bufSize, ft.planeHash[0]);
  }
  if (!ok) {
    f.close();
    wallCacheDrop();  // 身分不符或內容壞了 → 丟掉，這次解碼完會重建
    return false;
  }

  // 與解碼路徑完全相同的顯示呼叫；差別只在平面是讀回來的不是算出來的。
  if (hasGray) {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
    *why = "lsb";
    ok = wcReadPlane(f, renderer.getFrameBuffer(), bufSize, ft.planeHash[1]);
    if (ok) {
      renderer.copyGrayscaleLsbBuffers();
      *why = "msb";
      ok = wcReadPlane(f, renderer.getFrameBuffer(), bufSize, ft.planeHash[2]);
    }
    if (!ok) {
      f.close();
      wallCacheDrop();
      return false;
    }
    renderer.copyGrayscaleMsbBuffers();
    f.close();
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  } else {
    f.close();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
  *fullHashOut = ft.srcFullHash;
  *why = "hit";
  return true;
}

// 快取目錄裡有幾個檔（只在要新增時數；命中的那條路不數）。
uint16_t wallCacheCount() {
  auto dir = Storage.open(gWcDir);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }
  uint16_t n = 0;
  for (auto e = dir.openNextFile(); e; e = dir.openNextFile()) {
    n++;
    e.close();
    if (n >= WALLCACHE_MAX_FILES) break;
  }
  dir.close();
  return n;
}

// 解碼路徑順手把每個平面寫出去。任何一步失敗就整個放棄（刪 tmp），並且這次開機不再寫。
struct WallCacheWriter {
  HalFile f;
  WallCacheFooter ft{};
  bool active = false;
  uint8_t written = 0;
  unsigned long ms = 0;
  const char* why = "off";

  void begin(const WallCacheSrc& src, uint32_t paramsHash, uint8_t planes, size_t bufSize) {
    if (gWcWriteDisabled) {
      why = "disabled";
      return;
    }
    if (!wallCachePaths(src.path)) {
      why = "path";
      return;
    }
    const unsigned long t0 = millis();
    Storage.mkdir(gWcDir);
    if (!Storage.exists(gWcPath) && wallCacheCount() >= WALLCACHE_MAX_FILES) {
      why = "full";
      ms += millis() - t0;
      return;
    }
    Storage.remove(gWcTmp);
    if (!Storage.openFileForWrite("WPC", gWcTmp, f)) {
      why = "open";
      gWcWriteDisabled = true;
      return;
    }
    memcpy(ft.magic, "CMWP", 4);
    ft.version = WALLCACHE_VERSION;
    ft.planes = planes;
    ft.bufSize = static_cast<uint32_t>(bufSize);
    ft.srcSize = src.fileSize;
    ft.srcFingerprint = src.fingerprint;
    ft.pathHash2 = wallPathHash2(src.path);
    ft.pathLen = static_cast<uint32_t>(strlen(src.path));
    ft.paramsHash = paramsHash;
    active = true;
    why = "ok";
    ms += millis() - t0;
  }
  void addPlane(const uint8_t* d, size_t len) {
    if (!active || written >= ft.planes) return;
    const unsigned long t0 = millis();
    if (f.write(d, len) != len) {
      fail("write");
      return;
    }
    ft.planeHash[written++] = wcFnv(d, len);
    ms += millis() - t0;
  }
  bool commit(uint32_t srcFullHash) {
    if (!active) return false;
    const unsigned long t0 = millis();
    ft.srcFullHash = srcFullHash;
    bool ok = written == ft.planes && f.write(&ft, sizeof(ft)) == sizeof(ft);
    f.flush();
    f.close();
    active = false;
    if (ok) {
      Storage.remove(gWcPath);
      ok = Storage.rename(gWcTmp, gWcPath);
    }
    if (!ok) {
      Storage.remove(gWcTmp);
      gWcWriteDisabled = true;
      why = "commit";
    }
    ms += millis() - t0;
    return ok;
  }
  void fail(const char* w) {
    if (!active) return;
    f.close();
    active = false;
    Storage.remove(gWcTmp);
    gWcWriteDisabled = true;
    why = w;
  }
  void abandon() {  // 不是 I/O 失敗（例如來源整檔雜湊算不出來）→ 丟掉這一份，但不停用寫入
    if (!active) return;
    f.close();
    active = false;
    Storage.remove(gWcTmp);
  }
  ~WallCacheWriter() { abandon(); }
};
// 桌布的最終幾何（置中／等比縮放／裁切）。從 renderBitmapSleepScreen 原樣搬出來，讓預建路徑算同一份。
struct WallGeometry {
  int x = 0, y = 0;
  float cropX = 0, cropY = 0;
};
void wallGeometry(const Bitmap& bitmap, const int pageWidth, const int pageHeight, WallGeometry* g) {
  int x, y;
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  g->x = x;
  g->y = y;
  g->cropX = cropX;
  g->cropY = cropY;
}

uint32_t wallParamsHash(const Bitmap& bitmap, const int pageWidth, const int pageHeight, const WallGeometry& g,
                        const bool hasGray, const uint8_t orientation, const size_t bufSize) {
  struct {
    int32_t bmpW, bmpH, pageW, pageH, x, y;
    float cropX, cropY;
    uint32_t bufSize;
    uint16_t version, pixelVersion;
    uint8_t coverMode, coverFilter, gray, orientation, dither, pad;
  } p;
  memset(&p, 0, sizeof(p));  // padding 也要歸零，否則雜湊不穩定
  p.bmpW = bitmap.getWidth();
  p.bmpH = bitmap.getHeight();
  p.pageW = pageWidth;
  p.pageH = pageHeight;
  p.x = g.x;
  p.y = g.y;
  p.cropX = g.cropX;
  p.cropY = g.cropY;
  p.bufSize = static_cast<uint32_t>(bufSize);
  p.version = WALLCACHE_VERSION;
  p.pixelVersion = WALLCACHE_PIXEL_VERSION;
  p.coverMode = static_cast<uint8_t>(SETTINGS.sleepScreenCoverMode);
  p.coverFilter = static_cast<uint8_t>(SETTINGS.sleepScreenCoverFilter);
  p.gray = hasGray ? 1 : 0;
  p.orientation = orientation;
  p.dither = 1;  // 兩個自訂桌布入口都是 Bitmap(file, true)
  return wcFnv(reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}

// 只讀檔尾驗身分（預建時用來跳過已經有效的項目）。
bool wallCacheFooterValid(const WallCacheSrc& src, const uint32_t paramsHash, const bool hasGray,
                          const size_t bufSize) {
  if (gWcReadDisabled || !wallCachePaths(src.path) || !Storage.exists(gWcPath)) return false;
  HalFile f;
  if (!Storage.openFileForRead("WPC", gWcPath, f)) return false;
  WallCacheFooter ft{};
  const size_t want = static_cast<size_t>(hasGray ? 3 : 1) * bufSize + sizeof(ft);
  const bool ok = f.size() == want && f.seek(want - sizeof(ft)) && f.read(&ft, sizeof(ft)) == static_cast<int>(sizeof(ft)) &&
                  memcmp(ft.magic, "CMWP", 4) == 0 && ft.version == WALLCACHE_VERSION &&
                  ft.planes == (hasGray ? 3 : 1) && ft.bufSize == bufSize && ft.srcSize == src.fileSize &&
                  ft.srcFingerprint == src.fingerprint && ft.pathHash2 == wallPathHash2(src.path) &&
                  ft.pathLen == strlen(src.path) && ft.paramsHash == paramsHash;
  f.close();
  return ok;
}

// v322：目錄項目是不是桌布候選（不是目錄、名字非空、不以 '.' 開頭、副檔名 .bmp）。內容在選中那張時才驗。
//   只用呼叫端的 char 緩衝，不配置（hasBmpExtension 吃 string_view）。
bool wallCandidateName(HalFile& dirFile, char* name, const size_t nameLen) {
  if (dirFile.isDirectory()) return false;
  name[0] = '\0';
  dirFile.getName(name, nameLen);
  if (name[0] == '\0' || name[0] == '.') return false;
  return FsHelpers::hasBmpExtension(name);
}
}  // namespace

bool SleepActivity::skipEnteringPopup = false;

void SleepActivity::onEnter() {
  Activity::onEnter();

  // Show popup with reader orientation only when going to sleep from reader
  // v322：enterDeepSleep() 可能已經先把「進入休眠」畫過了（在存 wake frame 之前，見 main.cpp）→ 這裡不重畫，
  //   但方向仍要切成 Portrait 給桌布用。旗標用完即清，下一次休眠重新決定。
  const bool popupDone = skipEnteringPopup;
  skipEnteringPopup = false;
  if (APP_STATE.lastSleepFromReader) {
    if (!popupDone) {
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
      GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    }
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else if (!popupDone) {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    // v320：身分取不到（讀取失敗）就不給 cacheSrc → 整張照原路解碼，不碰快取。
    WallCacheSrc cacheSrc{"/sleep.bmp", &file, static_cast<uint32_t>(file.size()), 0};
    const bool cacheOk = wallFingerprint(file, renderer.getFrameBuffer(), renderer.getBufferSize(), &cacheSrc.fingerprint);
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap, cacheOk ? &cacheSrc : nullptr);
      file.close();
      if (dir) dir.close();
      return;
    }
    file.close();
  }

  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    // v322：兩趟走目錄、不存清單。v321 以前把所有檔名存進 vector<std::string>（每次休眠都配置，沒有上限——
    //   codex 指出 400 個長檔名可達 200KB），而且原本還逐檔 parseHeaders（50 張每次休眠付 0.5–0.9s）。
    //   現在第一趟只【數】有幾個候選、第二趟走到第 k 個；兩趟都只列目錄、不讀檔頭、不累積檔名
    //   （openNextFile() 每項配一個 HalFile::Impl、當輪即釋放 → 額外 live heap 是常數，不隨張數成長）。
    char name[500];
    uint16_t numFiles = 0;
    for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
      const bool ok = wallCandidateName(dirFile, name, sizeof(name));
      dirFile.close();
      if (ok && numFiles < UINT16_MAX) numFiles++;
    }
    // 選中的那張才驗內容。壞檔（名字像 BMP、內容不是）最多換三張再退回預設待機畫面；壞檔一樣進 recent，
    //   之後【高機率】不再抽到（recent 記的是索引、迴避 20 次抽不到就放棄 —— 是機率不是保證，codex 第二輪）。
    //   wallFingerprint 對任意內容都安全（太小回 false、每次 read 檢查長度），framebuffer 只是暫存、之後一定被清掉。
    bool recentDirty = false;
    for (uint8_t pick = 0; pick < 3 && numFiles > 0; pick++) {
      // Pick a random wallpaper, excluding recently shown ones.
      // Window: up to SLEEP_RECENT_COUNT entries, capped at numFiles-1.
      const uint8_t window =
          static_cast<uint8_t>(std::min<size_t>(APP_STATE.recentSleepFill, static_cast<size_t>(numFiles) - 1));
      auto randomFileIndex = static_cast<uint16_t>(random(numFiles));
      for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
        randomFileIndex = static_cast<uint16_t>(random(numFiles));
      }
      APP_STATE.pushRecentSleep(randomFileIndex);
      recentDirty = true;
      // 第二趟：走到第 k 個候選，拿它的名字
      dir.rewindDirectory();
      bool found = false;
      uint16_t seen = 0;
      for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
        const bool ok = wallCandidateName(dirFile, name, sizeof(name));
        dirFile.close();
        if (ok && seen++ == randomFileIndex) {
          found = true;
          break;
        }
      }
      if (!found) break;  // 兩趟之間目錄變了 —— 這台機器上不會發生，但要有出口
      const auto filename = std::string(sleepDir) + "/" + name;  // 冷路徑，只為選中的那一張組路徑
      HalFile randFile;
      if (Storage.openFileForRead("SLP", filename, randFile)) {
        LOG_DBG("SLP", "Randomly loading: %s", filename.c_str());
        delay(100);
        // v320：快取的鍵＝完整路徑；身分＝大小＋內容抽樣（名畫桌布每張都是同一個大小，只看大小認不出換圖）。
        WallCacheSrc cacheSrc{filename.c_str(), &randFile, static_cast<uint32_t>(randFile.size()), 0};
        const bool cacheOk =
            wallFingerprint(randFile, renderer.getFrameBuffer(), renderer.getBufferSize(), &cacheSrc.fingerprint);
        Bitmap bitmap(randFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          APP_STATE.save();  // recent 一次休眠只存一次（codex 第二輪），且在畫桌布之前
          renderBitmapSleepScreen(bitmap, cacheOk ? &cacheSrc : nullptr);
          randFile.close();
          dir.close();
          return;
        }
        randFile.close();
      }
      DiagLog::line("WALLPICK bad idx=%u n=%u pick=%u", static_cast<unsigned>(randomFileIndex),
                    static_cast<unsigned>(numFiles), static_cast<unsigned>(pick));
      if (numFiles == 1) break;
    }
    if (recentDirty) APP_STATE.save();
  }
  if (dir) dir.close();

  renderDefaultSleepScreen();
}

// Sleep screens paint with a single HALF refresh (stock parity): the OEM X4
// firmware's only clean refresh in normal operation is the single-pass 0xD7
// sequence, used once for the sleep image. It never runs the multi-flash GC
// waveform (0xF7) that FULL_REFRESH selects (#2471's blinking complaint).
void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  {
    // v34/v155 品牌：熊 logo（1-bit 畫法，灰階像素退成黑 —— 待機畫面接受）
    const int logoX = (pageWidth - LOGO_BEAR_240_SIZE) / 2;
    const int logoY = (pageHeight - LOGO_BEAR_240_SIZE) / 2;
    renderer.drawImageGray(LogoBearGray240, logoX, logoY, LOGO_BEAR_240_SIZE, LOGO_BEAR_240_SIZE);
  }
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const WallCacheSrc* cacheSrc,
                                             const bool displayPlanes) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  WallGeometry geom;
  wallGeometry(bitmap, pageWidth, pageHeight, &geom);
  const int x = geom.x;
  const int y = geom.y;
  const float cropX = geom.cropX;
  const float cropY = geom.cropY;
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  // v185 bench（/wall4.on）：UC8279 的絕對四階（原廠 XTH4 表）。不畫 B/W 底——那張表自己
  // 從白／黑重置後把每個像素推到階；兩張平面直接編碼階碼（GRAYSCALE_ABS_LO/HI）。
  // 只在【未反相濾鏡】且圖有灰階時走這條；其他情況沿用下面的原路。
  if (hasGreyscale && BenchFlags::wall4 && renderer.supportsAbsoluteGrayscale() &&
      SETTINGS.sleepScreenCoverFilter != CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_ABS_LO);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_ABS_HI);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBufferAbsolute();
    renderer.setRenderMode(GfxRenderer::BW);
    return;
  }

  // ─── v320：先試桌布平面快取（見檔頭說明）。cacheSrc 只有自訂桌布那兩條路會給；書封路徑不快取。
  const unsigned long paintT0 = millis();
  const size_t bufSize = renderer.getBufferSize();
  const uint32_t paramsHash =
      wallParamsHash(bitmap, pageWidth, pageHeight, geom, hasGreyscale, static_cast<uint8_t>(renderer.getOrientation()), bufSize);

  const char* cacheWhy = "off";
  uint32_t expectFullHash = 0;
  if (displayPlanes && cacheSrc && wallCachePaint(renderer, *cacheSrc, paramsHash, hasGreyscale, &cacheWhy, &expectFullHash)) {
    const unsigned long shownMs = millis() - paintT0;
    // 使用者已經看到桌布了 —— 現在才驗來源整檔（借 framebuffer 當緩衝，它此刻已經沒人要用）。
    uint32_t got = 0;
    const bool hashed = wallFullHash(*cacheSrc->file, renderer.getFrameBuffer(), bufSize, cacheSrc->fileSize, &got);
    const bool stale = hashed && got != expectFullHash;
    // 驗不出來（來源讀取失敗）也當成不可信 —— fail-closed：不然一個讀不到的磁區會讓舊圖永遠留著（codex 第二輪）。
    if (stale || !hashed) wallCacheDrop();
    DiagLog::line("WALLPAINT ms=%lu cache=1 gray=%d why=%s verify=%lums stale=%d hashed=%d stackfree=%u", shownMs,
                  hasGreyscale ? 1 : 0, cacheWhy, (millis() - paintT0) - shownMs, stale ? 1 : 0, hashed ? 1 : 0,
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    return;
  }

  WallCacheWriter writer;
  if (cacheSrc) writer.begin(*cacheSrc, paramsHash, hasGreyscale ? 3 : 1, bufSize);

  // 三趟解碼都要【整張畫完】才准存（drawBitmap 配不到列緩衝或讀列失敗會提早返回、rewind 也可能失敗）：
  //   否則半張壞圖會配上正確的來源雜湊被永久快取（codex 第二輪）。
  bool decodeOk = true;
  renderer.clearScreen();  // 快取讀到一半才失敗時 framebuffer 是髒的 → 重新清一次（沒試快取時等於多清一次，無害）
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
  decodeOk = decodeOk && renderer.lastBitmapDrawOk();

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  if (displayPlanes) {
    if (hasGreyscale) {
      // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
      // calibrated against the pixel state the single-pass HALF waveform leaves
      // behind. A FULL (GC) base parks pixels in a different charge state and
      // the differential nudge then lands unevenly (blotchy noise in gray areas).
      renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }
  // 先上面板再寫快取：第一次用這張桌布時，畫面不必等 SD 寫入（display 不會動 framebuffer）。
  writer.addPlane(renderer.getFrameBuffer(), bufSize);

  if (hasGreyscale) {
    decodeOk = decodeOk && bitmap.rewindToData() == BmpReaderError::Ok;
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    decodeOk = decodeOk && renderer.lastBitmapDrawOk();
    if (displayPlanes) renderer.copyGrayscaleLsbBuffers();
    writer.addPlane(renderer.getFrameBuffer(), bufSize);

    decodeOk = decodeOk && bitmap.rewindToData() == BmpReaderError::Ok;
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    decodeOk = decodeOk && renderer.lastBitmapDrawOk();
    if (displayPlanes) renderer.copyGrayscaleMsbBuffers();
    writer.addPlane(renderer.getFrameBuffer(), bufSize);

    if (displayPlanes) renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
  const unsigned long shownMs = millis() - paintT0;
  if (!decodeOk && writer.active) {
    writer.abandon();
    writer.why = "decode";
  }

  // 顯示完成之後才算來源整檔雜湊（第一次用這張桌布多付約 0.9 秒，之後不再）。算不出來就不存。
  bool stored = false;
  if (writer.active) {
    uint32_t full = 0;
    if (wallFullHash(*cacheSrc->file, renderer.getFrameBuffer(), bufSize, cacheSrc->fileSize, &full)) {
      stored = writer.commit(full);
    } else {
      writer.abandon();
      writer.why = "srchash";
    }
  }
  DiagLog::line("WALLPAINT ms=%lu cache=0 gray=%d why=%s store=%d storewhy=%s storems=%lu tail=%lums stackfree=%u",
                shownMs, hasGreyscale ? 1 : 0, cacheWhy, stored ? 1 : 0, writer.why, writer.ms,
                (millis() - paintT0) - shownMs, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, DataDir::path());
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, DataDir::path());
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, DataDir::path());
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

