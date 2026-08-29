#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

class GfxRenderer;

struct ImageDimensions {
  int16_t width;
  int16_t height;
};

struct RenderConfig {
  int x, y;
  int maxWidth, maxHeight;
  bool useGrayscale = true;
  bool useDithering = true;
  bool performanceMode = false;
  bool useExactDimensions = false;  // If true, use maxWidth/maxHeight as exact output size (no recalculation)
  std::string cachePath;            // If non-empty, decoder will write pixel cache to this path
};

class ImageToFramebufferDecoder {
 public:
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

  // v176：最近一次解碼失敗的原因（靜態、給 ImageBlock 組進 IMGFAIL）。transient=true 代表
  // 記憶體類失敗（地板／配置），ImageBlock 不把它記進 session 封殺名單 —— 否則一次瞬時
  // 低記憶體就讓同一張章首圖整個 session 都是方框（實機 diag175）。
  static char lastError[64];
  static bool lastErrorTransient;
  static void setLastError(bool transient, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  static void clearLastError() {
    lastError[0] = '\0';
    lastErrorTransient = false;
  }

  // 上游 #2959：解碼回呼裡每 250ms 讓一個 tick，幾秒的大圖解碼不會把 idle task 的看門狗餓死。
  // lastYieldMs 由呼叫端持有，初值＝解碼開始時間。（free function 回呼要用 → public）
  static void yieldDuringDecode(uint32_t& lastYieldMs);

  // 上游 #2959：把解碼器／檔頭給的寬高在【縮成 int16 之前】驗過（<=0、超過單邊上限、超過像素上限），
  // 檔頭探測、getDimensionsStatic、decodeToFramebuffer 三處共用；失敗會寫 lastError。
  // applyPixelCap=false 給【排版階段】（檔頭探測／getDimensionsStatic）：只擋 <=0 與單邊超過 int16；
  // 像素上限只在 render 階段（decodeToFramebuffer）生效 —— 排版階段丟圖會固化進 section 快取、
  // 放寬上限也救不回來（教訓 A-10／A-11），render 階段丟圖只是佔位框、會自癒。
  static bool validateAndStoreDimensions(int64_t width, int64_t height, ImageDimensions& out, const char* format,
                                         bool applyPixelCap = true);

 protected:
  // Size validation helpers
  // v126：3,145,728（2048×1536）擋掉的是【解碼時間】不是記憶體 —— JPEG 唯一的配置是
  // 固定大小的 JPEGDEC 物件（逐 MCU），PNG 的緩衝依【來源寬度】且另有 PNG_MAX_BUFFERED_PIXELS
  // 守著；像素總數對兩者的記憶體都沒有意義。
  // 而電子書的【直式封面】輕易超過原值（1600×2244 = 3,590,400，超出 14%）—— 實掃書庫 299 本，
  // 79 本（26%）有圖超標、50 本封面就超標。
  // 成本實測約 1.1 秒／百萬像素，而且【只付一次】（之後走 .pxc 快取，同張圖 4,191ms -> 243ms）。
  // ⚠️ 要調低之前先想清楚你是在省時間還是省記憶體。
  // ℹ️ 這個像素守衛只在 decodeToFramebuffer（render 階段）生效（validateAndStoreDimensions 的
  //    applyPixelCap）；排版階段只驗單邊 int16 —— 所以放寬它【不需要】bump SECTION_FILE_VERSION，
  //    既有快取會自癒（教訓 A-10）。
  // v171：6,291,456 → 8,388,608。全書庫實掃（299 本）有 3 本雜誌/圖表書的圖落在
  // 6.29M–8.33M 之間（最大 8,328,660）。守的是解碼時間不是記憶體（見上），
  // 成本 ~1.1 秒/百萬像素且只付一次（.pxc 快取）；render 階段守衛 → 不 bump、自癒。
  static constexpr int64_t MAX_SOURCE_DIMENSION = INT16_MAX;  // ImageDimensions 是 int16
  static constexpr int64_t MAX_SOURCE_PIXELS = 8388608;

  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
