#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <cstring>

#include <esp_heap_caps.h>

namespace {

// v94: std::string grows through the THROWING operator new, and under
// -fno-exceptions a failed throw does not return -- it aborts. Worse, the
// nothrow forms are no protection either: libstdc++ implements them as
// "call the throwing form, catch bad_alloc", and throwing itself needs a
// ~130-byte allocation for the exception object; when that fails,
// __cxa_allocate_exception calls std::terminate() and the catch is never
// reached. Decoded from a real crash on hardware (v93, 2026-08-05).
//
// So the only thing that works this far down is refusing to allocate at all.
// Same idiom as ParsedText::ensureTokenCapacity (v61/v62).
//
// libstdc++ grows a string to max(needed, 2*capacity) and the new buffer
// coexists with the old one while copying, so the peak is that new capacity.
constexpr size_t kStringHeadroom = 8192;

bool canGrowString(const std::string& s, size_t extra) {
  const size_t needed = s.size() + extra + 1;
  if (needed <= s.capacity()) return true;  // fits in place: no allocation at all
  size_t newCap = s.capacity() * 2;
  if (newCap < needed) newCap = needed;
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > newCap + kStringHeadroom;
}

}  // namespace

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  // v110:快取被清空 ⇒ 身分必然失效。做在機制裡而不是靠每個呼叫點自覺
  // (TxtReader/Dictionary 的既有 scope 因此自動安全)。
  warmIdentity_.invalidate();
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  prewarmCacheImpl(fontId, utf8Text, styleMask, nullptr, nullptr);
}

FontCacheManager::PrewarmOutcome FontCacheManager::prewarmCacheImpl(int fontId, const char* utf8Text,
                                                                    uint8_t styleMask, bool (*shouldAbort)(void*),
                                                                    void* abortCtx) {
  // v110:快取被直接改寫 ⇒ 舊身分失效。採用(adopt)永遠是之後由呼叫端顯式做。
  warmIdentity_.invalidate();

  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask, false, shouldAbort, abortCtx);
    // v110:只有 SD 分支吃中止。PREWARM_ABORTED(-2)不會落進下面的 missed > 0。
    if (missed == SdCardFont::PREWARM_ABORTED) return PrewarmOutcome::Aborted;
    // v110 複審修正:硬失敗(PREWARM_FAILED,-3)過去與「缺了 N 個字」共用同一個回傳通道,
    // 於是一份【已經被 freeStyleMiniData 釋放掉】的快取照樣被呼叫端蓋上 valid 身分。
    if (missed == SdCardFont::PREWARM_FAILED) {
      LOG_ERR("FCM", "prewarmCache(SD): hard failure (styleMask=0x%02X) -- cache freed", styleMask);
      return PrewarmOutcome::Failed;
    }
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return PrewarmOutcome::Ok;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  // v110:內建字型是純 CPU 解壓(無 SD I/O),快到不值得為它開中止路徑。
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return PrewarmOutcome::Ok;

  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
  return PrewarmOutcome::Ok;
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (scanFontId_ < 0) scanFontId_ = fontId;
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;
  // v94: dropping text here is harmless -- those codepoints simply miss the
  // prewarm and get drawn one at a time through the existing glyph-miss ring.
  // Slower for that page, which is strictly better than aborting.
  if (canGrowString(scanTextPerStyle_[baseStyle], strlen(text))) {
    scanTextPerStyle_[baseStyle] += text;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cpCount = 0;
  while (*p) {
    if ((*p & 0xC0) != 0x80) cpCount++;
    p++;
  }
  scanStyleCounts_[baseStyle] += cpCount;
  // 記住這一頁字數最多的字重,下一頁的 reserve 直接給對桶(v31 粗體閱讀時主桶是粗體)
  if (scanStyleCounts_[baseStyle] > scanStyleCounts_[lastMainStyle_]) lastMainStyle_ = baseStyle;
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  // 主桶預先配置 2048(與改版前的單一緩衝相同):一頁 800-1000 個中文字約 2.4-3KB;
  // 拆成四桶後若各給一半,主桶反而要多一次 realloc。開頭無從得知哪個字重是主桶,
  // 故先給正體;真正的主桶由 scanStyleCounts_ 在下一頁生效(見 recordText)。
  for (uint8_t i = 0; i < 4; i++) manager_->scanTextPerStyle_[i].clear();
  // v94: guarded -- reserve() throws on failure like any other growth.
  if (canGrowString(manager_->scanTextPerStyle_[manager_->lastMainStyle_], 2048)) {
    manager_->scanTextPerStyle_[manager_->lastMainStyle_].reserve(2048);
  }
  memset(manager_->scanStyleCounts_, 0, sizeof(manager_->scanStyleCounts_));
  manager_->scanFontId_ = -1;
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() { endScanAndPrewarmImpl(nullptr, nullptr); }

bool FontCacheManager::PrewarmScope::endScanAndPrewarmAbortable(bool (*shouldAbort)(void*), void* abortCtx) {
  return endScanAndPrewarmImpl(shouldAbort, abortCtx);
}

// v110:原 endScanAndPrewarm() 的本體,一字未改地搬進來(合併邏輯就是 v54 那個 critical
// 的修法,不得更動),只多了把 shouldAbort/abortCtx 穿給送出去的那幾桶。
// shouldAbort == nullptr 時 prewarmCacheImpl 仍可能回 Failed(硬體/配置失敗),那會清掉
// completed 與 retain 旗標但【不會】提早中斷迴圈(其餘桶照送=v109 的逐桶獨立性);
// 對四個 retain=false 的既有使用者,旗標寫入是 no-op,解構子照樣 clearCache=v109 行為。
bool FontCacheManager::PrewarmScope::endScanAndPrewarmImpl(bool (*shouldAbort)(void*), void* abortCtx) {
  // 冪等:解構子會再呼叫一次;改版後各 per-style 字串已清空,但仍以掃描模式當守衛
  // (原本靠 scanText_.empty() 判斷,拆成四條後語意不再單一)。
  if (manager_->scanMode_ != ScanMode::Scanning) return true;
  manager_->scanMode_ = ScanMode::None;

  // v54:逐字重各自預載自己的文字,但**必須先依「實際解析到的字面」合併**。
  //
  // 為什麼不能直接逐桶送:SD 中文字型只有正體+粗體,resolveStyle 會把斜體→正體、
  // 粗斜體→粗體(Iansui 更是全部→正體);而 prewarmStyle 開頭就 freeStyleMiniData(s),
  // 是【完全取代】語意。若整頁內文記在桶 0、頁面又有幾個 <em> 斜體字記在桶 2,
  // 兩者都解析到字面 0 → 後送的斜體那幾個字會把整頁內文的快取整個抹掉 →
  // 內文每個字都 glyph miss → 走只有 8 格的 overflow ring,每次都重開 .cpfont,
  // 一頁數百次開檔,從「快一點」變成「慢好幾秒」。
  // 內建字型路徑同理:v29 斜體別名化後 getData(ITALIC) 回傳的就是正體那份資料,
  // 而 FontDecompressor 每次呼叫佔一個新 slot、查找時第一個資料相符的 slot 找不到就 break。
  uint8_t targetOf[4] = {0, 1, 2, 3};
  auto sdIt = manager_->sdCardFonts_.find(manager_->scanFontId_);
  if (sdIt != manager_->sdCardFonts_.end() && sdIt->second) {
    for (uint8_t i = 0; i < 4; i++) targetOf[i] = sdIt->second->resolveStyle(i) & 0x03;
  } else if (manager_->fontMap_.count(manager_->scanFontId_) != 0) {
    const auto& fam = manager_->fontMap_.at(manager_->scanFontId_);
    const EpdFontData* data[4];
    for (uint8_t i = 0; i < 4; i++) data[i] = fam.getData(static_cast<EpdFontFamily::Style>(i));
    for (uint8_t i = 0; i < 4; i++) {
      for (uint8_t j = 0; j < i; j++) {
        if (data[j] != nullptr && data[j] == data[i]) {
          targetOf[i] = targetOf[j];
          break;
        }
      }
    }
  }

  // 合併到目標字面。安全性依據:resolveStyle 的 fallback 鏈首項是自己,
  // 故任何 present 的字面都是【不動點】(resolveStyle(t)==t)→ 目標桶那一圈必定 t==i
  // 直接跳過、永不被清空,與迴圈方向無關(不是「targetOf[i] <= i」)。
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t t = targetOf[i];
    if (t == i || manager_->scanTextPerStyle_[i].empty()) continue;
    if (manager_->scanTextPerStyle_[t].empty()) {
      // 目標桶空(例:Iansui + 粗體閱讀,整頁都在桶 1 而目標是桶 0):直接搬,
      // 不要 += 出第二份整頁文字(這一版的主題就是峰值與碎片)
      manager_->scanTextPerStyle_[t].swap(manager_->scanTextPerStyle_[i]);
      continue;
    }
    // v94: if the merge cannot be afforded, DROP the source bucket rather than
    // leaving it for its own prewarm pass. Leaving it would re-create the v54
    // critical: both buckets resolve to the same face, and prewarmStyle() opens
    // with freeStyleMiniData() (replace semantics), so the second pass would
    // wipe the first one's cache and send a whole page through the glyph-miss
    // ring. Dropping costs only these glyphs — they still draw, one SD read at
    // a time — and leaves the target bucket's prewarm intact.
    if (!canGrowString(manager_->scanTextPerStyle_[t], manager_->scanTextPerStyle_[i].size())) {
      manager_->scanTextPerStyle_[i].clear();
      manager_->scanTextPerStyle_[i].shrink_to_fit();
      continue;
    }
    manager_->scanTextPerStyle_[t] += manager_->scanTextPerStyle_[i];
    manager_->scanTextPerStyle_[i].clear();
    manager_->scanTextPerStyle_[i].shrink_to_fit();
  }

  // 每個實際字面各預載一次:只載它真正要畫的字(混排頁 SD 讀取量與 miniBitmap 單塊各砍約一半)
  bool completed = true;
  for (uint8_t i = 0; i < 4; i++) {
    if (manager_->scanTextPerStyle_[i].empty()) continue;
    const PrewarmOutcome outcome =
        manager_->prewarmCacheImpl(manager_->scanFontId_, manager_->scanTextPerStyle_[i].c_str(),
                                   static_cast<uint8_t>(1u << i), shouldAbort, abortCtx);
    if (outcome == PrewarmOutcome::Ok) continue;
    completed = false;
    // v110:中止或硬失敗 → 快取是半成品(已送出去的桶已經寫進去了;失敗的那一桶已被
    // freeStyleMiniData 整組釋放)。這裡就把 retain 解除,讓解構子的 clearCache() 自動跑
    // ——與「clearCache ⇒ invalidate」同樣做在機制裡,不靠呼叫端記得在 false 分支收拾。
    // (呼叫端仍可自己提早 clearCache;那是冪等的。)
    retainCacheOnExit_ = false;
    // v110 複審修正:兩者【接下來要做的事不同】。
    //   中止 = 使用者已經按了鍵,剩下的桶一個位元組都不該再讀 → break。
    //   硬失敗 = 沒有人叫我們停,只是這一個字面倒了 → 其餘字面照常送完,與 v109
    //     「單一字面失敗不影響其他字面」逐條相同。這一趟 render 仍靠它們把畫面畫對;
    //     retain 已解除,整份快取會在解構時清掉 ⇒ 身分永遠不會蓋在空快取上。
    if (outcome == PrewarmOutcome::Aborted) break;
  }

  // Free scan string memory
  for (uint8_t i = 0; i < 4; i++) {
    manager_->scanTextPerStyle_[i].clear();
    manager_->scanTextPerStyle_[i].shrink_to_fit();
  }
  return completed;
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called (scanMode_ guard)
    // v110:預設仍然清空(既有行為)。只有明確開了 retain 的呼叫端才把字留下來。
    if (!retainCacheOnExit_) manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_), retainCacheOnExit_(other.retainCacheOnExit_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
