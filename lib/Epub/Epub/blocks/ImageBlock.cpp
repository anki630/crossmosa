#include <Arduino.h>
#include <cstdarg>
#include "ImageBlock.h"

#include <Breadcrumb.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "Epub/converters/DirectPixelWriter.h"
#include <ZipFile.h>

#include "Epub/converters/DecodeFile.h"
#include "Epub/converters/DecodeStats.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

char ImageBlock::lastFailPath[112] = {0};
char ImageBlock::lastDecodeWitness[300] = {0};
ImageBlock::PxcStats ImageBlock::pxcStats;

// v24/v148 低記憶體紓解 hook（見標頭）。
// 門檻 48KB：抽取的最大單塊是 32,768 的 LZ77 window，解碼器物件另需 20–44KB（先後配置，
// 非同時）；48KB 涵蓋「window ＋ 小配置」與「解碼器 ＋ 邊際」兩種形狀。實測 v147 在
// maxAlloc=32,160 時 window 差 608 bytes 配不到 —— 卸載字型可騰回 43K+ 的連續塊。
// 過度觸發的代價只是一次字型重載（約 300ms SD 讀取），且只發生在「首次看到這張圖」。
static constexpr size_t IMAGE_RENDER_RELIEF_MAX_ALLOC = 48 * 1024;
// v54 的教訓：解碼器自己另有【總量】門檻（MIN_FREE_HEAP_FOR_PNG ≈ 60KB / JPEG ≈ 36KB，
// 都是對 getFreeHeap）。只看連續塊時，總量落在中間帶 relief 不觸發、卻被解碼器自己擋下
// → 圖片變佔位框，而且 rememberImageFailure 讓它【整個 session 不再重試】。
// 兩側都檢查把這個縫隙補起來；76KB = PNG 的 60KB 門檻 + 16KB 邊際。
static constexpr size_t IMAGE_RENDER_RELIEF_MIN_FREE = 76 * 1024;
static ImageBlock::MemoryReliefFn g_imageReliefFn = nullptr;
static ImageBlock::MemoryReliefFn g_imageRestoreFn = nullptr;
static void* g_imageReliefCtx = nullptr;

void ImageBlock::setMemoryReliefHooks(MemoryReliefFn reliefFn, MemoryReliefFn restoreFn, void* ctx) {
  g_imageReliefFn = reliefFn;
  g_imageRestoreFn = restoreFn;
  g_imageReliefCtx = ctx;
}

void ImageBlock::noteFailure(const char* fmt, ...) {
  if (breadcrumbPending(lastFailPath)) return;  // 先到先得，見標頭
  char local[sizeof(lastFailPath)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(local, sizeof(local), fmt, ap);
  va_end(ap);
  breadcrumbPublish(lastFailPath, sizeof(lastFailPath), local);  // v249：跨 task 交接（見 Breadcrumb.h）
}

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void* ImageBlock::extractCtx = nullptr;
ImageBlock::ExtractFn ImageBlock::extractFn = nullptr;
void* ImageBlock::streamCtx = nullptr;
ImageBlock::StreamOpenFn ImageBlock::streamOpenFn = nullptr;

void ImageBlock::setStreamSource(void* ctx, StreamOpenFn fn) {
  streamCtx = ctx;
  streamOpenFn = fn;
}

void ImageBlock::setExtractor(void* ctx, ExtractFn fn) {
  extractCtx = ctx;
  extractFn = fn;
}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

std::string getCachePath(const std::string& imagePath) {
  // Replace extension with .pxc (pixel cache)
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc";
  }
  return imagePath + ".pxc";
}

bool readValidCacheHeader(HalFile& cacheFile, const int expectedWidth, const int expectedHeight, uint16_t& cachedWidth,
                          uint16_t& cachedHeight) {
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  const int widthDiff = abs(cachedWidth - expectedWidth);
  const int heightDiff = abs(cachedHeight - expectedHeight);
  if (widthDiff > 1 || heightDiff > 1) {
    return false;
  }

  const size_t bytesPerRow = (cachedWidth + 3) / 4;
  const size_t expectedSize = 4 + bytesPerRow * cachedHeight;
  return cacheFile.size() >= expectedSize;
}

// Pages are deserialized afresh on each visit. Keep a bounded, allocation-free
// record so an image that failed renders its placeholder directly for the rest
// of the reader session instead of paying another placeholder refresh and
// decode. The reader clears this on entry so transient memory/storage failures
// are retried.
constexpr size_t MAX_SESSION_IMAGE_FAILURES = 16;
uint64_t failedImageHashes[MAX_SESSION_IMAGE_FAILURES];
// v190：render-remembered 累計；閱讀器取差值綁定 spine/page，不在這裡清表。
uint32_t rememberedPlaceholderCount_ = 0;
// v193：延後解碼旗標與計數。旗標由閱讀器整輪 renderContents 以 RAII 固定，
// 計數只加、身分由呼叫端綁（與 rememberedPlaceholderCount_ 同一手法）。
bool deferHeavyDecode_ = false;
uint32_t deferredDecodeCount_ = 0;
// v260：解碼因按鍵中止的次數（只加；呼叫端比較前後差值）。
uint32_t decodeAbortCount_ = 0;
// v176：0 = 永久失敗（整個 session 不重試）；非 0 = 暫時性（記憶體）失敗當時的最大連續塊，
// 只在堆積明顯好轉（+8KB）時才重試 —— 否則每次重繪都重試會變成效能懸崖（codex 複查）。
uint32_t failedImageMaxAlloc[MAX_SESSION_IMAGE_FAILURES];
// v255：暫時性配置失敗時那一塊要多大（0＝未知，退回「失敗時最大塊＋8KB」的舊門檻）。與上兩個陣列平行。
uint32_t failedImageNeedBytes[MAX_SESSION_IMAGE_FAILURES];
// v255：記錄當下的畫頁世代（clearRetryableFailures 每次畫頁 +1）。暫時性失敗在【同一次畫頁】裡不重試 ——
//   BW 與灰階各帶都會重入 render，不然堆積稍微回升就在同一頁連解好幾次（codex 複查）。
uint32_t failedImageRenderGen[MAX_SESSION_IMAGE_FAILURES];
uint32_t g_imageRenderGeneration = 0;
size_t failedImageCount = 0;
// v191：1 專屬於「下一頁再試」哨兵，避免開檔失敗誤走 +8KB 記憶體規則。
// ⚠️ 解碼失敗記的是 std::max(2u, maxAlloc)（下面），刻意讓 1 不可能由記憶體路徑產生——
// 原本的 max(1u, …) 在 maxAlloc==0 時會寫出 1，被 clearRetryableFailures 當哨兵清掉（複查抓到）。
constexpr uint32_t IMAGE_FAILURE_RETRY_NEXT_RENDER = 1;
// v191：失敗表 16 格滿了就記不進去，而同一頁的 BW／灰階帶會重入 render 十幾次 —— 沒有備援就是
// 一次翻頁抽十幾次（正是這一版要避免的效能懸崖）。這一格只記「本次 render 已經失敗過的那張」。
uint64_t currentRenderFailHash = 0;
bool currentRenderFailValid = false;
// v191：n= 要跨頁累計才看得出暫時性 SD 失誤有沒有自己好，所以不能跟哨兵項一起清。
// 16×(8+1) BSS、固定大小；不用 heap，因為這是診斷計數、上限已知。
uint64_t openFailHashes[MAX_SESSION_IMAGE_FAILURES];
uint8_t openFailN[MAX_SESSION_IMAGE_FAILURES];
size_t openFailEntries = 0;
// v256：暫時性失敗的重試次數（不論是否看堆積，每張圖每個 session 最多 MAX_TRANSIENT_RETRIES 次）。
//   為什麼不看堆積：是否重試在 render 開頭判斷，而字型 mini 資料（約 26KB）要到真正解碼前的紓解（FONTREL img）才釋放 ——
//   v255 實機：紓解前最大塊 20–21KB，門檻 44KB 永遠到不了，章首圖整個 session 是方框。
//   只在閱讀器說「現在沒有背景排版在跑」時給（setTransientRetryAllowed）；次數有上限，失敗的代價（一次紓解＋解碼嘗試）有界。
constexpr uint8_t MAX_TRANSIENT_RETRIES = 3;
uint64_t transientRetryHashes[MAX_SESSION_IMAGE_FAILURES];
uint8_t transientRetryN[MAX_SESSION_IMAGE_FAILURES];
size_t transientRetryEntries = 0;
bool g_transientRetryAllowed = false;

// 還有額度就記一次並回 true；額度用完或表滿回 false（表滿時寧可不重試，保持有界）。
bool takeTransientRetry(const uint64_t hash) {
  for (size_t i = 0; i < transientRetryEntries; i++) {
    if (transientRetryHashes[i] != hash) continue;
    if (transientRetryN[i] >= MAX_TRANSIENT_RETRIES) return false;
    ++transientRetryN[i];
    return true;
  }
  if (transientRetryEntries >= MAX_SESSION_IMAGE_FAILURES) return false;
  transientRetryHashes[transientRetryEntries] = hash;
  transientRetryN[transientRetryEntries] = 1;
  ++transientRetryEntries;
  return true;
}

uint64_t imagePathHash(const std::string& path) {
  uint64_t hash = 14695981039346656037ull;
  for (const char c : path) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

// 回傳 0 = 記不下了（診斷欄位印 n=0 表示未知，不謊報成 1；複查抓到）。
unsigned bumpOpenFailN(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < openFailEntries; i++) {
    if (openFailHashes[i] != hash) continue;
    if (openFailN[i] < 255) openFailN[i]++;
    return openFailN[i];
  }
  if (openFailEntries < MAX_SESSION_IMAGE_FAILURES) {
    openFailHashes[openFailEntries] = hash;
    openFailN[openFailEntries] = 1;
    openFailEntries++;
    return 1;
  }
  return 0;
}

bool imageFailedThisSession(const std::string& path) {
  const uint64_t hash = imagePathHash(path);
  if (currentRenderFailValid && currentRenderFailHash == hash) return true;  // v191：滿表備援
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] != hash) continue;
    // v191：同一頁 BW／灰階帶會重入 render，哨兵必須仍擋下來，否則一次翻頁抽十幾次。
    if (failedImageMaxAlloc[i] == IMAGE_FAILURE_RETRY_NEXT_RENDER) return true;
    if (failedImageMaxAlloc[i] == 0) return true;                                      // 永久
    if (failedImageRenderGen[i] == g_imageRenderGeneration) return true;               // v255：同一次畫頁不重試
    const uint32_t maxNow = ESP.getMaxAllocHeap();
    bool heapLooksBetter;
    if (failedImageNeedBytes[i] > 0) {
      // v255：知道缺的是多大的一塊 → 最大塊夠放它（＋4KB 給 TLSF 取整與其後的小配置）【而且】比失敗當時大，才重試。
      heapLooksBetter = maxNow >= failedImageNeedBytes[i] + 4 * 1024 && maxNow > failedImageMaxAlloc[i];
    } else {
      heapLooksBetter = maxNow >= failedImageMaxAlloc[i] + 8 * 1024;
    }
    // v256：堆積看起來沒好轉（這個數字看不到紓解之後的空間）→ 沒有背景排版在跑時，也可以重試。
    //   兩種重試都吃同一份額度（每張圖每個 session 最多 3 次；codex：只限制其中一種，另一種仍可能每次畫頁都試一次）。
    if (!heapLooksBetter && !g_transientRetryAllowed) return true;
    if (!takeTransientRetry(hash)) return true;
    // 好轉了：移除紀錄讓它重試；再失敗會以新水位重新記錄。
    failedImageHashes[i] = failedImageHashes[failedImageCount - 1];
    failedImageMaxAlloc[i] = failedImageMaxAlloc[failedImageCount - 1];
    failedImageNeedBytes[i] = failedImageNeedBytes[failedImageCount - 1];
    failedImageRenderGen[i] = failedImageRenderGen[failedImageCount - 1];
    failedImageCount--;
    return false;
  }
  return false;
}

// transientMaxAlloc = 0 → 永久失敗；否則記錄當時的最大連續塊，作為重試門檻。
void rememberImageFailure(const std::string& path, const uint32_t transientMaxAlloc = 0, const uint32_t needBytes = 0) {
  if (failedImageCount == MAX_SESSION_IMAGE_FAILURES) return;
  const uint64_t hash = imagePathHash(path);
  for (size_t i = 0; i < failedImageCount; i++) {
    if (failedImageHashes[i] == hash) return;
  }
  failedImageHashes[failedImageCount] = hash;
  failedImageMaxAlloc[failedImageCount] = transientMaxAlloc;
  failedImageNeedBytes[failedImageCount] = needBytes;
  failedImageRenderGen[failedImageCount] = g_imageRenderGeneration;
  failedImageCount++;
}

// --- Per-page-render RAM slot for the pixel cache ----------------------------
// The tiled grayscale flow re-renders an image page once for the BW
// double-refresh and again for every band of both gray planes, and each pass
// re-read the whole .pxc off SD (~100 ms for a full-page image, ~13 passes).
// Column clipping cannot reduce the SD traffic: the row stride (~100 B) is
// smaller than an SD sector, so every sector is touched regardless of the band
// window. Instead the first pass loads the payload into RAM and later passes
// render from it. Chunked allocation because a single full-image block (up to
// 96 KB) rarely fits the fragmented mid-render heap; each chunk is heap-gated
// and any failure falls back to the streaming path unchanged. The reader
// releases the slot when the page render completes, so nothing stays resident
// across page turns.
constexpr size_t PXC_CHUNK_SHIFT = 14;  // 16 KB chunks
constexpr size_t PXC_CHUNK_SIZE = 1u << PXC_CHUNK_SHIFT;
constexpr size_t PXC_MAX_CHUNKS = 6;  // 96 KB: a full-screen 2bpp image
constexpr size_t PXC_HEAP_RESERVE = 24 * 1024;
constexpr size_t PXC_MAX_ALLOC_RESERVE = 8 * 1024;
// Rows can straddle a chunk boundary; they are reassembled into a stack
// buffer. (screenWidth + 3) / 4 caps at 200 B for an 800px panel.
constexpr int PXC_MAX_BYTES_PER_ROW = 208;

std::unique_ptr<uint8_t[]> pxcChunks[PXC_MAX_CHUNKS];
uint64_t pxcSlotHash = 0;
uint16_t pxcSlotWidth = 0;
uint16_t pxcSlotHeight = 0;
// v249：RAM 裡有幾列（其餘列每一趟從 SD 串流）。部分載入時 < pxcSlotHeight。
uint16_t pxcSlotRows = 0;
// v249：部分載入時，載入那一趟的快取檔柄留給後面每一趟串流尾段用（省掉每趟 exists＋open 約 15ms × 15 趟）。
// 只活到這一頁畫完（releasePxcSlot）。唯讀；同一頁內不會有寫入者 —— slot 在就表示快取有效、不會重新解碼，
// 而尾段讀失敗（呼叫端會退回解碼、重寫這個檔）之前一律先 releasePxcSlot 關掉它。
HalFile pxcTailFile;

void releasePxcSlot() {
  for (auto& chunk : pxcChunks) chunk.reset();
  pxcTailFile = HalFile();  // 解構 Impl ＝ 持 StorageLock close；空檔柄也安全
  pxcSlotHash = 0;
  pxcSlotWidth = 0;
  pxcSlotHeight = 0;
  pxcSlotRows = 0;
}

const uint8_t* pxcRowPtr(size_t rowStart, int bytesPerRow, uint8_t* tempRow) {
  const size_t chunk = rowStart >> PXC_CHUNK_SHIFT;
  const size_t offset = rowStart & (PXC_CHUNK_SIZE - 1);
  if (offset + bytesPerRow <= PXC_CHUNK_SIZE) {
    return pxcChunks[chunk].get() + offset;
  }
  const size_t firstPart = PXC_CHUNK_SIZE - offset;
  memcpy(tempRow, pxcChunks[chunk].get() + offset, firstPart);
  memcpy(tempRow + firstPart, pxcChunks[chunk + 1].get(), bytesPerRow - firstPart);
  return tempRow;
}

// cacheFile is positioned just past the header. True when the slot holds at least one full row of this cache
// path afterward (pxcSlotRows says how many; the rest stream from SD on every pass).
//
// v249：部分載入。原本門檻是「剩餘總量 ≥ 整張快取＋24KB」才開始配 —— diag248 閱讀中的堆是剩餘約 92KB、最大塊 52KB
// （p2：52,292／17,408／6,400／6,400），512×726 的快取 92,928B 永遠過不了，灰階 14 趟每一趟都從 SD 重讀整張
// （首次開圖頁 lsb≈1,250 msb≈1,250；快取整張進 RAM 的頁 lsb≈130）。
// 改成一塊一塊來，放得下幾塊就載幾塊：
//   - 配之前：剩餘總量 ≥ 這一塊＋24KB（載完的下限仍是 24KB，與原本「整張放得下」時相同）。
//   - 配之後：最大塊仍 ≥ 8KB（之後才配的灰階帶狀暫存 X3 為 99×80＝7,920B）；不符就把這塊還回去、停。
//     原本是配之前要求「最大塊 ≥ 這一塊＋8KB」，等於假設這塊一定從最大塊切 —— 上面那個堆只載得進 2 塊；
//     直接檢查配完的結果可以載 3 塊（48KB），而且之後的暫存與串流緩衝都還配得到。剛切出來的塊立刻還回去會與鄰居合併，不留洞。
bool loadPxcSlot(uint64_t cacheHash, HalFile& cacheFile, uint16_t cachedWidth, uint16_t cachedHeight, int bytesPerRow) {
  releasePxcSlot();
  if (bytesPerRow > PXC_MAX_BYTES_PER_ROW || bytesPerRow <= 0) {
    return false;
  }
  const size_t total = (size_t)bytesPerRow * cachedHeight;
  const size_t chunkCount = (total + PXC_CHUNK_SIZE - 1) >> PXC_CHUNK_SHIFT;
  if (chunkCount == 0 || chunkCount > PXC_MAX_CHUNKS) {
    return false;
  }
  ImageBlock::pxcStats.totalBytes = static_cast<uint32_t>(total);
  size_t loaded = 0;
  for (size_t i = 0; i < chunkCount; i++) {
    const size_t want = (total - loaded) < PXC_CHUNK_SIZE ? (total - loaded) : PXC_CHUNK_SIZE;
    if (ESP.getFreeHeap() < want + PXC_HEAP_RESERVE) break;  // 放不下這一塊：已載入的留著，其餘列走 SD
    auto chunk = makeUniqueNoThrow<uint8_t[]>(want);
    if (!chunk) break;
    // 配完再驗一次（codex：配置本身的額外開銷、或別的 task 剛好在中間配置，都可能吃掉配前算好的餘裕）。
    // 不符：chunk 離開範圍即還回去。
    if (ESP.getMaxAllocHeap() < PXC_MAX_ALLOC_RESERVE || ESP.getFreeHeap() < PXC_HEAP_RESERVE) break;
    if (cacheFile.read(chunk.get(), want) != static_cast<int>(want)) {
      releasePxcSlot();  // 讀檔失敗：不留半套，整張走 SD 串流（它會自己再讀一次、自己報錯）
      return false;
    }
    pxcChunks[i] = std::move(chunk);
    loaded += want;
  }
  const size_t rows = loaded / (size_t)bytesPerRow;
  if (rows == 0) {
    releasePxcSlot();
    return false;
  }
  pxcSlotHash = cacheHash;
  pxcSlotWidth = cachedWidth;
  pxcSlotHeight = cachedHeight;
  pxcSlotRows = static_cast<uint16_t>(rows);
  ImageBlock::pxcStats.loadedBytes = static_cast<uint32_t>(loaded);
  return true;
}

void renderRowsFromPxcSlot(GfxRenderer& renderer, int x, int y) {
  const int bytesPerRow = (pxcSlotWidth + 3) / 4;
  uint8_t tempRow[PXC_MAX_BYTES_PER_ROW];

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < pxcSlotRows; row++) {  // v249：只畫 RAM 裡的列；其餘列由呼叫端串流
    const uint8_t* rowBuffer = pxcRowPtr((size_t)row * bytesPerRow, bytesPerRow, tempRow);
    pw.beginRow(y + row);
    int colStart, colEnd;
    pw.bandColRange(x, pxcSlotWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      const uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;
      pw.writePixel(x + col, pixelValue);
    }
  }
}

// v249：從 SD 串流畫 [startRow, cachedHeight) 這些列（原本 renderFromCache 裡的串流迴圈，改成可以從任一列開始）。
// slotTail：這是 slot 那張圖的尾段（計入 sd*），否則是沒有 slot 的圖整張串流（計入 other*）。
bool streamPxcRows(GfxRenderer& renderer, HalFile& cacheFile, const int x, const int y, const uint16_t cachedWidth,
                   const uint16_t cachedHeight, const int bytesPerRow, const int startRow, const bool slotTail) {
  if (startRow >= cachedHeight) return true;
  if (!cacheFile.seek(4 + (size_t)startRow * (size_t)bytesPerRow)) {
    LOG_ERR("IMG", "Cache seek error at row %d", startRow);
    return false;
  }
  const uint32_t t0 = millis();

  // Read several rows per SD access. A one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat; batching
  // rows into a ~4KB buffer cuts that to ~20 reads per pass without holding the
  // whole image.
  const int rowsLeft = cachedHeight - startRow;
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > rowsLeft) rowsPerRead = rowsLeft;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  // v249：配不到就逐次減半，不直接掉到一列 —— 部分載入時 slot 已把堆吃到下限，一列一讀的尾段每趟要上百次 SD 讀取。
  while (!readBuffer && rowsPerRead > 1) {
    rowsPerRead /= 2;
    readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  }
  if (!readBuffer) {
    // v194：LOG_ERR 在這台等於丟掉。走既有 noteFailure，閱讀器每圈倒進 diag.log。
    ImageBlock::noteFailure("cache-rowbuf bytes=%u max=%u", static_cast<unsigned>(bytesPerRow),
                            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }
  if (ImageBlock::pxcStats.sdBufMin == 0 || (size_t)rowsPerRead * bytesPerRow < ImageBlock::pxcStats.sdBufMin) {
    ImageBlock::pxcStats.sdBufMin = static_cast<uint16_t>((size_t)rowsPerRead * bytesPerRow);
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = startRow; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  auto& st = ImageBlock::pxcStats;
  const uint32_t bytes = static_cast<uint32_t>((size_t)rowsLeft * bytesPerRow);
  if (slotTail) {
    st.sdPasses++;
    st.sdBytes += bytes;
    st.sdMs += millis() - t0;
  } else {
    st.otherPasses++;
    st.otherBytes += bytes;
    st.otherMs += millis() - t0;
  }
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  // A later pass of the same page render: the payload is already in RAM, skip
  // the file entirely.
  const uint64_t cacheHash = imagePathHash(cachePath);
  if (pxcSlotHash == cacheHash && pxcSlotWidth != 0) {
    renderRowsFromPxcSlot(renderer, x, y);
    ImageBlock::pxcStats.ramPasses++;
    if (pxcSlotRows >= pxcSlotHeight) return true;
    // v249：部分載入 —— RAM 裡沒有的列從 SD 串流（載入那一趟留下的檔柄，只讀那一段）。
    if (pxcTailFile &&
        streamPxcRows(renderer, pxcTailFile, x, y, pxcSlotWidth, pxcSlotHeight, (pxcSlotWidth + 3) / 4, pxcSlotRows,
                      true)) {
      return true;
    }
    releasePxcSlot();  // 呼叫端接著會重新解碼、重寫這個快取檔 —— 先關掉握著的檔柄
    ImageBlock::pxcStats.abandons++;
    return false;
  }

  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (!readValidCacheHeader(cacheFile, expectedWidth, expectedHeight, cachedWidth, cachedHeight)) {
    LOG_ERR("IMG", "Invalid image cache: %s", cachePath.c_str());
    return false;
  }

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte

  // First pass of a page render: try to pull the payload into the RAM slot so
  // the remaining ~12 passes skip SD entirely. Only an EMPTY slot is claimed:
  // the slot lives until the page render completes, so a populated slot with a
  // different hash means another image on this same page owns it. Evicting it
  // here would make 2+ image pages reload each other from SD on every pass
  // (all the SD traffic of streaming plus the slot alloc churn); instead later
  // images take the streaming path below, unchanged from pre-cache behavior.
  if (pxcSlotHash == 0 && loadPxcSlot(cacheHash, cacheFile, cachedWidth, cachedHeight, bytesPerRow)) {
    renderRowsFromPxcSlot(renderer, x, y);
    ImageBlock::pxcStats.ramPasses++;
    LOG_DBG("IMG", "Cache render complete (payload now in RAM)");
    if (pxcSlotRows >= cachedHeight) return true;
    if (!streamPxcRows(renderer, cacheFile, x, y, cachedWidth, cachedHeight, bytesPerRow, pxcSlotRows, true)) {
      releasePxcSlot();
      ImageBlock::pxcStats.abandons++;
      return false;
    }
    pxcTailFile = std::move(cacheFile);  // 後面每一趟沿用（見 pxcTailFile）
    return true;
  }

  // Streaming fallback (slot didn't fit at all): every row from SD.
  return streamPxcRows(renderer, cacheFile, x, y, cachedWidth, cachedHeight, bytesPerRow, 0, false);
}

}  // namespace

bool ImageBlock::hasValidCache() const {
  const auto cachePath = getCachePath(imagePath);
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  return readValidCacheHeader(cacheFile, width, height, cachedWidth, cachedHeight);
}

bool ImageBlock::needsDecode() const { return !imageFailedThisSession(imagePath) && !hasValidCache(); }

void ImageBlock::clearSessionRenderFailures() {
  failedImageCount = 0;
  rememberedPlaceholderCount_ = 0;  // v190：與失敗表同一 session 起點，否則新 activity 的差值會吃到上一本
  deferredDecodeCount_ = 0;         // v193：同上，避免新 activity 的差值吃到上一本
  decodeAbortCount_ = 0;            // v260：同上
  openFailEntries = 0;              // v191：n= 跟 session 走，進閱讀器時才歸零
  transientRetryEntries = 0;        // v256：重試額度同樣跟 session 走
  currentRenderFailValid = false;
}

void ImageBlock::clearRetryableFailures() {
  ++g_imageRenderGeneration;       // v255：新的一次畫頁（暫時性失敗在上一次畫頁記下的，這一次才准重試）
  currentRenderFailValid = false;  // v191：備援格與哨兵同壽命——每頁 render 給一次機會
  size_t i = 0;
  while (i < failedImageCount) {
    if (failedImageMaxAlloc[i] != IMAGE_FAILURE_RETRY_NEXT_RENDER) {
      i++;
      continue;
    }
    failedImageHashes[i] = failedImageHashes[failedImageCount - 1];
    failedImageMaxAlloc[i] = failedImageMaxAlloc[failedImageCount - 1];
    failedImageNeedBytes[i] = failedImageNeedBytes[failedImageCount - 1];
    failedImageRenderGen[i] = failedImageRenderGen[failedImageCount - 1];
    failedImageCount--;
  }
}

uint32_t ImageBlock::rememberedPlaceholderCount() { return rememberedPlaceholderCount_; }

void ImageBlock::setDeferHeavyDecode(bool on) { deferHeavyDecode_ = on; }  // v193
void ImageBlock::setTransientRetryAllowed(const bool on) { g_transientRetryAllowed = on; }  // v256

uint32_t ImageBlock::deferredDecodeCount() { return deferredDecodeCount_; }  // v193
uint32_t ImageBlock::decodeAbortCount() { return decodeAbortCount_; }        // v260

void ImageBlock::releaseRenderCache() { releasePxcSlot(); }

void ImageBlock::renderPlaceholder(GfxRenderer& renderer, const int x, const int y) const {
  renderer.fillRect(x, y, width, height, true);
  if (width > 2 && height > 2) {
    renderer.fillRect(x + 1, y + 1, width - 2, height - 2, false);
  }
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    noteFailure("render-bounds x=%d y=%d w=%d h=%d %s", x, y, width, height, imagePath.c_str());
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // Tiled grayscale (#2190): skip the whole image when it doesn't touch the
  // active band. The per-pixel writer already clips off-band pixels, but without
  // this each of the ~7 bands per plane re-ran the full cache load / pixel walk
  // and discarded the result — the dominant cost of AA on image pages. The check
  // is orientation-aware and returns true when no strip is active, so the BW
  // pass and non-tiled controllers render the image exactly as before.
  if (!renderer.glyphIntersectsStrip(x, y, x + width - 1, y + height - 1)) {
    return;
  }

  if (imageFailedThisSession(imagePath)) {
    noteFailure("render-remembered y=%d %s", y, imagePath.c_str());  // v256：y＝方框畫在哪（圖片位置的實機回報）
    rememberedPlaceholderCount_++;  // v190：只加計數，身分由呼叫端綁定
    renderPlaceholder(renderer, x, y);
    return;
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;  // Successfully rendered from cache
  }

  // v193：只延後「快取未命中、接下來要真的解碼」這條路。不是失敗，不准寫進失敗表。
  // v245：延後時【不畫框】，圖區留白 —— 那一遍當文字頁上面板（EpubReaderActivity 的 deferredTextOnlyDraw），
  //   解碼後的補圖那一遍才畫圖。畫框是使用者回報的「先出方框」；失敗的圖（render-remembered）仍畫框，那是有意義的。
  if (deferHeavyDecode_) {
    deferredDecodeCount_++;
    return;
  }
  // v260：同一遍裡前一張圖已經因為按鍵中止 → 後面的圖不要再開始（開讀取器、紓解字型、讀檔頭都省下）。
  if (ImageToFramebufferDecoder::inputAbortRequested()) {
    decodeAbortCount_++;
    return;
  }

  // v24/v148：紓解窗口涵蓋【懶抽取＋解碼】兩段（v147 實測 render 時的抽取在
  // maxAlloc=32,160 下差 608 bytes 失敗 —— 舊樹只包解碼，因為舊樹在建置期抽取）。
  // RAII 讓下面每一條 return 路徑都會 restore；restore = ensureLoaded，在同一個
  // render 內把字型載回來，圖之後的文字照常畫（字型 ID 是內容雜湊，重載後不變）。
  struct ReliefWindow {
    bool active = false;
    ~ReliefWindow() {
      if (active && g_imageRestoreFn) g_imageRestoreFn(g_imageReliefCtx);
    }
  } relief;
  // v246 儀器：這張圖從這裡開始算「第一次打開的成本」。
  const uint32_t heavyStartMs = millis();
  uint32_t reliefMs = 0;
  uint32_t extractMs = 0;
  ZipStreamStats zipStats;  // v247 儀器：抽圖分解（沒有抽＝method 0xFFFF）
  if (g_imageReliefFn && (ESP.getMaxAllocHeap() < IMAGE_RENDER_RELIEF_MAX_ALLOC ||
                          ESP.getFreeHeap() < IMAGE_RENDER_RELIEF_MIN_FREE)) {
    const uint32_t t = millis();
    g_imageReliefFn(g_imageReliefCtx);
    relief.active = true;
    reliefMs = millis() - t;
  }

  // v246／v247 儀器：IMGDEC 麵包屑（先到先得）。src＝sd（抽到 SD 再解）／zip（直接從書裡解）／
  // fb:<原因>（試過直接從書裡解、失敗退回抽到 SD）。
  const auto writeDecodeWitness = [&](const char* src, const uint32_t extractMsArg, const ZipStreamStats& zs,
                                      const size_t fileBytes, const uint32_t decoderStartMsArg, const bool ok) {
    if (breadcrumbPending(lastDecodeWitness)) return;
    // 單位 ms（rd／wr 後面是呼叫次數）。tot＝從紓解開始到解碼器返回；conv＝轉換器整段（含 setup／dec／fin）；
    // dec 內含 rd／wr／yld，dec−rd−wr−yld ≈ 解碼＋縮放＋抖色的 CPU。src=zip 時 zrd／zinf 是 rd 裡面的讀書＋解壓。
    const DecodeStats& st = g_decodeStats;
    const uint32_t nowMs = millis();
    const size_t slash = imagePath.find_last_of('/');
    const char* base = slash == std::string::npos ? imagePath.c_str() : imagePath.c_str() + slash + 1;
    char local[sizeof(lastDecodeWitness)];  // v249：本地組好再交接（見 Breadcrumb.h）
    snprintf(local, sizeof(local),
             "%c %ux%u>%ux%u 1/%u prog=%u file=%uKB rel=%u ext=%u zm=%d zc=%uKB zset=%u zrd=%u zinf=%u zwr=%u "
             "tot=%u conv=%u setup=%u ra=%u dec=%u rd=%u/%u wr=%u/%u yld=%u/%u fin=%u rs=%u ok=%u y=%d src=%s %s",
             st.fmt, st.srcW, st.srcH, st.dstW, st.dstH, st.scaleDenom, st.progressive,
             static_cast<unsigned>((fileBytes + 512) / 1024), static_cast<unsigned>(reliefMs),
             static_cast<unsigned>(extractMsArg), zs.method == 0xFFFF ? -1 : static_cast<int>(zs.method),
             static_cast<unsigned>((zs.compressed + 512) / 1024), static_cast<unsigned>(zs.setupUs / 1000),
             static_cast<unsigned>(zs.readUs / 1000), static_cast<unsigned>(zs.inflateUs / 1000),
             static_cast<unsigned>(zs.writeUs / 1000), static_cast<unsigned>(nowMs - heavyStartMs),
             static_cast<unsigned>(nowMs - decoderStartMsArg), static_cast<unsigned>(st.setupUs / 1000),
             static_cast<unsigned>(st.readAheadCap / 1024), static_cast<unsigned>(st.decodeUs / 1000),
             static_cast<unsigned>(st.readUs / 1000), static_cast<unsigned>(st.readCalls),
             static_cast<unsigned>(st.writeUs / 1000), static_cast<unsigned>(st.writeCalls),
             static_cast<unsigned>(st.yieldUs / 1000), static_cast<unsigned>(st.yields),
             static_cast<unsigned>(st.finalizeUs / 1000), static_cast<unsigned>(st.streamRestarts), ok ? 1u : 0u, y, src,
             base);
    breadcrumbPublish(lastDecodeWitness, sizeof(lastDecodeWitness), local);
  };
  const auto makeConfig = [&]() {
    RenderConfig c;
    c.x = x;
    c.y = y;
    c.maxWidth = width;
    c.maxHeight = height;
    c.useGrayscale = true;
    c.useDithering = true;
    c.performanceMode = false;
    c.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
    c.cachePath = cachePath;      // Enable caching during decode
    return c;
  };

  // v248：還沒抽到 SD 的 JPEG／PNG → 先試【直接從書裡解碼】（v247 實機：封面抽圖寫 SD 1,076ms＋解碼器讀回 1,127ms）。
  //   任何失敗（開不起來、記憶體不夠配快取帶、讀檔出錯、壞圖）都不寫失敗表，照舊走下面的「抽到 SD 再解」。
  //   失敗那次可能已經在 framebuffer 畫了一部分 —— 下面的舊路會整張重畫，或畫佔位框蓋掉。
  char streamNote[48] = "sd";
  if (!srcPath.empty() && streamOpenFn && extractFn && !Storage.exists(imagePath.c_str())) {
    ImageToFramebufferDecoder* sdecoder = ImageDecoderFactory::getDecoder(imagePath);
    const bool streamable = sdecoder && (strcmp(sdecoder->getFormatName(), "JPEG") == 0 ||
                                         strcmp(sdecoder->getFormatName(), "PNG") == 0);
    if (streamable) {
      const RenderConfig sconfig = makeConfig();
      ImageToFramebufferDecoder::clearLastError();
      g_decodeStats = DecodeStats{};
      g_zipStreamStats = ZipStreamStats{};
      g_decodeStreamRequest.open = streamOpenFn;
      g_decodeStreamRequest.ctx = streamCtx;
      g_decodeStreamRequest.src = srcPath.c_str();
      const uint32_t streamStartMs = millis();
      const bool streamOk = sdecoder->decodeToFramebuffer(imagePath, renderer, sconfig);
      g_decodeStreamRequest = DecodeStreamRequest{};
      // v260：按鍵中止 —— 不退回「抽到 SD 再解」、不記失敗、不畫框；這一遍的呼叫端（閱讀器）會放棄整頁的補圖。
      if (!streamOk && ImageToFramebufferDecoder::lastDecodeAborted) {
        writeDecodeWitness("abort", 0, g_zipStreamStats, g_decodeStats.sourceBytes, streamStartMs, false);
        decodeAbortCount_++;
        return;
      }
      if (streamOk) {
        const ZipStreamStats zs = g_zipStreamStats;
        // v250：zip:b8／zip:b4…＝壓縮讀取緩衝實際配到幾 KB（stored 項目沒有讀取緩衝，印 b0）。
        char zipNote[16];
        snprintf(zipNote, sizeof(zipNote), "zip:b%u", static_cast<unsigned>(zs.inBufBytes / 1024));
        writeDecodeWitness(zipNote, 0, zs, g_decodeStats.sourceBytes, streamStartMs, true);
        return;
      }
      if (g_decodeStats.streamOpenFailed) {
        // v250：fb:open:s<步驟>@<失敗當下最大塊 bytes>（步驟見 ZipStreamStats::openFailStage；0＝不是 ZipEntryReader 擋的）。
        //   用 bytes 不用 KB：這條要看的邊界只差幾十 bytes（codex 複查）。
        snprintf(streamNote, sizeof(streamNote), "fb:open:s%u@%u", static_cast<unsigned>(g_zipStreamStats.openFailStage),
                 static_cast<unsigned>(g_zipStreamStats.openFailMax));
      } else {
        snprintf(streamNote, sizeof(streamNote), "fb:%s",
                 ImageToFramebufferDecoder::lastError[0] != '\0' ? ImageToFramebufferDecoder::lastError : "?");
      }
    }
  }

  // The build only header-probed the image for dimensions; pull the actual
  // file out of the book now, on first visit to the page.
  if (!srcPath.empty() && extractFn && !Storage.exists(imagePath.c_str())) {
    LOG_DBG("IMG", "Lazy-extracting %s -> %s", srcPath.c_str(), imagePath.c_str());
    const uint32_t extractStartMs = millis();
    const bool extracted = extractFn(extractCtx, srcPath.c_str(), imagePath.c_str());
    extractMs = millis() - extractStartMs;
    zipStats = g_zipStreamStats;
    if (!extracted) {
      LOG_ERR("IMG", "Lazy extraction failed: %s", srcPath.c_str());
      // v191：抽取器說失敗就是失敗，不要再去開那個檔 —— 抽到一半的殘檔【開得起來】，
      // 於是原本的寫法會完全沒有證據（而且 Storage.exists 之後永遠不再重抽）。複查抓到。
      // 印的是【來源路徑】（EPUB 內的項目名），那才是能拿去對書查的東西。
      Storage.remove(imagePath.c_str());
      const unsigned n = bumpOpenFailN(imagePath);
      // v251：s<步驟>@<失敗當下最大塊 bytes>（ZipFile::readFileToStream 的 noteExtractFail；s0＝zip 之前就失敗，例如開不了輸出檔）、
      //   st=前面「直接從書裡解碼」那次的結果（IMGDEC 在抽圖失敗時不會印，這是唯一看得到它的地方）。
      noteFailure("render-extract n=%u s%u@%u st=%s %s", n, static_cast<unsigned>(zipStats.openFailStage),
                  static_cast<unsigned>(zipStats.openFailMax), streamNote, srcPath.c_str());
      rememberImageFailure(imagePath, IMAGE_FAILURE_RETRY_NEXT_RENDER);
      currentRenderFailHash = imagePathHash(imagePath);
      currentRenderFailValid = true;
      renderPlaceholder(renderer, x, y);
      return;
    }
  }

  // No cache - need to decode the image
  // Check if image file exists
  HalFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    // v191：ex= 放前面，長路徑截斷仍留得住原因；n= 看得出暫時性 SD 失誤有沒有自己好。
    const unsigned n = bumpOpenFailN(imagePath);
    noteFailure("render-open n=%u %s", n, imagePath.c_str());
    rememberImageFailure(imagePath, IMAGE_FAILURE_RETRY_NEXT_RENDER);
    currentRenderFailHash = imagePathHash(imagePath);
    currentRenderFailValid = true;
    renderPlaceholder(renderer, x, y);
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    noteFailure("render-empty %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  const RenderConfig config = makeConfig();

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    noteFailure("render-nodecoder %s", imagePath.c_str());
    rememberImageFailure(imagePath);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  ImageToFramebufferDecoder::clearLastError();
  g_decodeStats = DecodeStats{};
  const uint32_t decoderStartMs = millis();
  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  writeDecodeWitness(streamNote, extractMs, zipStats, fileSize, decoderStartMs, success);
  if (!success && ImageToFramebufferDecoder::lastDecodeAborted) {
    decodeAbortCount_++;  // v260：按鍵中止，不是失敗（同上）
    return;
  }
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    // v176：render 階段的方框終於有證人（v125 那套在 v145 移植時掉了 —— diag175 那次實機的方框
    // 零紀錄）。記憶體類失敗（transient）不進 session 封殺名單，下一頁重試。
    // （需要的大小已在 lastError 文字裡，例如 png-alloc-zlib 39896；不另加欄位，免得 112B 的行把路徑截掉。）
    noteFailure("render-decode %s tr=%u max=%u free=%u %s", ImageToFramebufferDecoder::lastError,
                ImageToFramebufferDecoder::lastErrorTransient ? 1u : 0u, static_cast<unsigned>(ESP.getMaxAllocHeap()),
                static_cast<unsigned>(ESP.getFreeHeap()), imagePath.c_str());
    rememberImageFailure(imagePath,
                         ImageToFramebufferDecoder::lastErrorTransient
                             ? std::max(2u, static_cast<unsigned>(ESP.getMaxAllocHeap()))
                             : 0u,
                         ImageToFramebufferDecoder::lastErrorTransient ? ImageToFramebufferDecoder::lastErrorNeedBytes : 0u);
    renderPlaceholder(renderer, x, y);
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(HalFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writeString(file, srcPath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile& file) {
  std::string path;
  std::string src;
  serialization::readString(file, path);
  serialization::readString(file, src);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  auto block = std::unique_ptr<ImageBlock>(new (std::nothrow) ImageBlock(path, src, w, h));
  if (!block) {
    // v194：nothrow 配不到。LOG_ERR 在這台等於丟掉，走既有 noteFailure 進 diag.log。
    noteFailure("deserialize-alloc max=%u", static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return nullptr;
  }
  return block;
}
