#pragma once
#include <HalStorage.h>

#include <atomic>
#include <memory>
#include <string>

#include "Block.h"

class ZipEntryReader;

class ImageBlock final : public Block {
 public:
  // v120/v125/v146 儀器：最後一次圖片失敗的描述（空字串 = 沒有待回報的失敗）。
  // ⚠️ 由 src 端讀走並清空 —— **lib 不能反向依賴 src/util/DiagLog**（分層規則，
  //    v140 我違反過一次，編譯直接失敗）。
  // ⚠️ 先到先得，不覆寫還沒被讀走的紀錄 —— 否則同一頁的第二張圖會蓋掉第一張的原因
  //    （v122 踩過：兩行 IMGFAIL 內容相同，就是被蓋掉的）。
  static char lastFailPath[112];
  static void noteFailure(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  // v246 儀器：每次真的解碼一張圖（快取未命中）就寫一行時間分解，reader 的 loop 讀走印成 IMGDEC。
  // 先到先得（同 lastFailPath）：同一頁的第二張圖不覆寫第一張。
  static char lastDecodeWitness[300];  // v247：200→300（加了抽圖分解 z*= 與 ra=）
  // v249 儀器：一次頁面繪製內，像素快取（.pxc）的 RAM slot 與 SD 串流用量。reader 在 renderContents 結束時印 PXCSLOT
  // 並歸零。 slot 同一時間只給一張圖（先到先得）；total／loaded／ram／sd 講的是【那一張】，other 是同頁其他圖整張從 SD
  // 串流的量 （codex 複查：混在一起，多圖頁的一行會看起來像在描述同一張圖）。
  struct PxcStats {
    uint32_t totalBytes = 0;   // 最後一張試著載入 slot 的圖的快取大小
    uint32_t loadedBytes = 0;  // 載進 RAM 的量（部分載入時 < total；完全放不下 0）
    uint16_t ramPasses = 0;    // 從 RAM slot 畫的趟數
    uint16_t sdPasses = 0;     // slot 那張圖的尾段從 SD 串流的趟數
    uint32_t sdBytes = 0;
    uint32_t sdMs = 0;
    uint16_t otherPasses = 0;  // 沒有 slot 的圖整張從 SD 串流的趟數（含放不下的那張）
    uint32_t otherBytes = 0;
    uint32_t otherMs = 0;
    uint16_t sdBufMin = 0;  // 串流緩衝實際配到的最小位元組數（4KB 配不到時減半）；0＝沒串流
    uint16_t abandons = 0;  // slot 那張圖的尾段讀失敗、slot 被放掉的次數。> 0 時上面 slot 那幾欄可能混到兩張圖
  };
  static PxcStats pxcStats;
  // v352：像素快取 RAM 槽的形狀。閱讀器的灰階帶規劃（planAndAllocGrayStrip）要照同一組數字模擬載入，
  //   放這裡免得兩邊各寫一份。
  //   7 塊＝112 KB：X3 整個螢幕的圖 528×792＝104,544 B 要 7 塊（v351 起固定版面的畫頁、滿寬的封面都是這個大小）；
  //   原本 6 塊（96 KB）差 8 KB → 整張放不進去、每一趟都從 SD 重讀整張（diag351：一頁 6 趟 612 KB、1.7–1.8 秒）。
  //   X4 整個螢幕 480×800＝96,000 B，6 塊就夠。每一塊照舊看堆積才配，放不下的列走 SD 串流。
  static constexpr size_t kPxcChunkSize = 16 * 1024;
  static constexpr size_t kPxcMaxChunks = 7;
  static constexpr size_t kPxcHeapReserve = 24 * 1024;

  // v24/v148：低記憶體紓解 hook。首次看到某頁的圖時要付「懶抽取（inflate 的 11KB state
  // + 32KB window）＋ 解碼器（PNG 約 44KB / JPEG 約 20KB）」，而常駐的 SD 閱讀字型握著
  // 數十 KB（實測 p2 裡的 43,008 mini bitmap）。堆積吃緊時由 reader 掛的 reliefFn 暫時
  // 卸載字型、restoreFn 重載。**用函式指標**讓 lib/Epub 不反向依賴 app 的字型系統（分層）。
  using MemoryReliefFn = void (*)(void* ctx);
  static void setMemoryReliefHooks(MemoryReliefFn reliefFn, MemoryReliefFn restoreFn, void* ctx);
  ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;
  bool hasValidCache() const;
  bool needsDecode() const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y) const;
  static void clearSessionRenderFailures();
  // v191：只清開檔失敗的哨兵項，讓下一頁再給一次抽取機會；永久／記憶體失敗不動。
  static void clearRetryableFailures();
  // v190：本頁 render 走 render-remembered 的次數。身分由閱讀器綁定，這裡只計數。
  static uint32_t rememberedPlaceholderCount();
  static uint32_t decodeAbortCount();  // v260：解碼因按鍵中止的次數
  // v193：app 可設的延後解碼旗標。為真時，快取未命中就畫佔位、只計數，不呼叫解碼器、
  // 不寫失敗表。快取命中那條照舊走完。身分由閱讀器綁定。
  static void setDeferHeavyDecode(bool on);
  // v352：圖還沒解好時畫在圖區中間的圖示（維護者 2026-09-30：「先放一個圖示，畫好再換成真的」；
  //   「是不是使用系統裡面的符號就好？」→ 用 UI 的圖片圖示）。圖示在 app 層，lib 不能反向引用，所以由閱讀器掛函式指標
  //   （同 setExtractor 的分層手法）。沒掛＝不畫（圖區留白，v245 的行為）。只在黑白那一趟、而且圖夠大時呼叫。
  using PendingIconFn = void (*)(GfxRenderer& renderer, int x, int y, int width, int height);
  static void setPendingIconDrawer(PendingIconFn fn);
  static uint32_t pendingIconCount();  // 畫了幾次圖示（只加；呼叫端取差值，證人 IMGDEFER icon=）
  // 「圖還沒好」的佔位：這一場 session 解失敗過的圖畫方框（有意義：這張圖壞了）；其餘畫圖示（夠大才畫，否則留白）。
  void renderPendingPlaceholder(GfxRenderer& renderer, int x, int y) const;

  // v354：背景預解 —— 畫完一頁之後、使用者在讀的時候，先把「下一頁」的圖解成 .pxc
  //   （見 EpubReaderActivity::predecodeNextImage）。
  //   只寫快取、不碰 framebuffer（RenderConfig::cacheOnly）；只走「直接從書裡解」或「原圖已經抽到 SD」，不抽檔；
  //   不畫佔位框、不寫這一場的失敗表。呼叫端負責 arm 按鍵中止與記憶體閘門。
  enum class PredecodeOutcome : uint8_t { Done, Aborted, Skipped, NoStream, Failed, NoCache };
  struct PredecodeResult {
    PredecodeOutcome outcome;
    const char* why;  // 證人 NEXTIMG why=（靜態字串或 ImageToFramebufferDecoder::lastError）
  };
  // 值不值得預解：沒有有效快取、這一場沒失敗過、不在預解抑制表、格式可以按鍵中止（JPEG／PNG；GIF 不行）。
  //   會開 SD 驗快取。
  bool predecodeCandidate() const;
  // 前提：呼叫端剛用 predecodeCandidate() 挑過這一張（這裡不再驗，免得同一個快取檔開兩次）。
  PredecodeResult predecode(GfxRenderer& renderer, int x, int y) const;
  // v256：暫時性解碼失敗可否「不看堆積」直接重試（有次數上限）。閱讀器每次畫頁設：沒有背景排版在跑才給。
  static void setTransientRetryAllowed(bool on);
  // v350：記憶體類（transient）的解碼失敗發生過 —— 閱讀器讀走之後拍一次池子。v348 實機：解 PNG 要 39,896 的連續塊，
  //   p2 被一塊約 37KB 的東西切成兩半、最大只剩 39,476（差約 400 B），而那塊是誰 log 裡看不出來。
  //   render task 寫、main loop 讀（lib 不能反向呼叫 DiagLog，分層規則）。
  static std::atomic<bool> memFailPending;
  static uint32_t deferredDecodeCount();

  // A page render draws its image up to ~13 times (BW double-refresh plus every
  // grayscale band pass), and each draw streams the whole .pxc off SD. The
  // first draw caches the pixel payload in RAM (chunked, heap-gated, falls back
  // to streaming when it doesn't fit); the reader calls this when the page
  // render completes so nothing stays resident between pages.
  static void releaseRenderCache();

  // Lazy extraction hook: the section build only header-probes images for their
  // dimensions; the file at imagePath is extracted out of the book on first
  // render, via this callback (function pointer + context, not std::function —
  // this is render-loop code). Registered by the reader activity that owns the
  // Epub, cleared on its exit.
  using ExtractFn = bool (*)(void* ctx, const char* srcPath, const char* destPath);
  static void setExtractor(void* ctx, ExtractFn fn);
  // v248：直接從書裡解碼（JPEG／PNG 還沒抽到 SD 時）。與 DecodeFile.h 的 DecodeStreamOpenFn 同型。
  using StreamOpenFn = bool (*)(void* ctx, const char* srcPath, ZipEntryReader& reader, size_t readBufSize);
  static void setStreamSource(void* ctx, StreamOpenFn fn);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  std::string srcPath;  // book-internal source href; empty once known-extracted
  int16_t width;
  int16_t height;

  static void* extractCtx;
  static ExtractFn extractFn;
  static void* streamCtx;
  static StreamOpenFn streamOpenFn;
};
