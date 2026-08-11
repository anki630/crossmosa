#pragma once
#include <HalStorage.h>

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
  // v120 儀器:這台機器沒有序列埠,LOG_ERR 等於丟掉 —— 而 JPEG 解碼有 11 個各自不同的
  // 失敗出口,現在完全分不出是哪一個。把「階段碼 + 解碼器自己的錯誤碼」記在這裡,
  // 由 src 端(有 DiagLog 的那一層)輸出,維持 lib 不反向依賴 src 的層次規則。
  // 只在失敗路徑寫入;解碼成功時這段程式碼一次都不會執行。
  enum FailStage {
    FAIL_NONE = 0, FAIL_OPEN_DIM, FAIL_NO_SOI, FAIL_BAD_DIM, FAIL_NO_SOF,
    FAIL_LOW_HEAP, FAIL_ALLOC_DEC, FAIL_OPEN_DEC, FAIL_DECODE, FAIL_OTHER,
    FAIL_BOUNDS = 10,      // v122:版面給的框超出螢幕 —— 靜默不畫、也不記失敗的那條路
    FAIL_REMEMBERED = 11,  // 本 session 先前失敗過,直接畫佔位框
    // v125:以下三條在 v124 之前【完全沒有紀錄】—— 它們都是「畫佔位框 + 記住這張失敗了 +
    // return」,所以症狀是白框而 diag.log 一個字都沒有。v124 那份 log 零 IMGFAIL,正是因為
    // 真正的出口不在被儀器覆蓋的那幾條上。
    FAIL_NOT_FOUND = 12,   // 抽出來的圖檔在 /.crossmosa/epub_*/ 裡不存在(抽取失敗 / SD 滿 / 被清掉)
    FAIL_EMPTY = 13,       // 圖檔存在但大小為 0(抽取寫到一半斷掉)
    FAIL_NO_DECODER = 14   // 副檔名沒有對應的解碼器
  };
  static int lastFailStage;
  static int lastFailCode;
  static void noteFailure(const int stage, const int code) { lastFailStage = stage; lastFailCode = code; }
  virtual ~ImageToFramebufferDecoder() = default;

  virtual bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) = 0;

  virtual bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const = 0;

  virtual const char* getFormatName() const = 0;

  // v54:此解碼器單次需要的【最大連續】記憶體(含餘裕)。ImageBlock 用它決定要不要先卸載
  // SD 字型騰空間——卸載會連帶丟掉本頁已預載的 glyph 快取(ensureLoaded 不會重做 prewarm),
  // 代價高,所以門檻必須貼合各解碼器的實際需求,不能共用一個保守值。
  virtual size_t minContiguousHeapForDecode() const = 0;

 protected:
  // Size validation helpers
  // v126:3,145,728(2048×1536)→ 6,291,456(2048×3072)。
  //
  // 為什麼可以放寬:**這道上限擋的是解碼時間,不是記憶體。** JPEG 路徑唯一的配置是 JPEGDEC
  // 物件本身(固定大小,逐 MCU 解碼,不保留整張點陣圖);PNG 路徑的緩衝是依【來源寬度】,
  // 而且它自己另有一道寬度守衛(`PNG_MAX_BUFFERED_PIXELS`,失敗訊息明確)。像素總數對兩者
  // 的記憶體都沒有意義 —— 原本那個「2048×1536」看起來是防數位相機照片的通用值。
  //
  // 為什麼非放寬不可:電子書的**直式封面**輕易就超過它。實測 1600×2244 = 3,590,400 px
  // (超出 14%)被擋下 —— 而擋下的方式是在【版面階段】就把整張圖從頁面移除
  // (`ChapterHtmlSlimParser` 的 `getDimensions` 失敗分支會 `Storage.remove` 抽出來的檔案),
  // 使用者看到的是空白,而 diag.log 因為只有 LOG_ERR 而完全沒有紀錄。
  //
  // 新值的代價:實測解碼約 **1.1 秒/百萬像素**(實測一張 1400×1947 = 2.73 MP 的整頁圖 → bw 3,061 ms),
  // 所以 6.29 MP 的最壞情況約 7 秒,**而且只付一次**(之後走 .pxc 像素快取)。
  // 6,291,456 也涵蓋了 2000×3000 這種常見的封面尺寸並留有餘裕。
  static constexpr int MAX_SOURCE_PIXELS = 6291456;  // 2048 * 3072


  bool validateImageDimensions(int width, int height, const std::string& format);
  void warnUnsupportedFeature(const std::string& feature, const std::string& imagePath);
};
