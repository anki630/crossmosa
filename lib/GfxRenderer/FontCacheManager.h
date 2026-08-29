#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

#include "WarmIdentity.h"

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  // v188：同步章節重排前用——clearCache() 保留 SD 字型的 mini 容量，這個真的還回去。回傳釋放位元組數。
  size_t releaseRetainedCache();
  // v189：fontId 對應的 SD 字型目前保留中的 mini bitmap 容量（bytes；非 SD 字型或未載入 = 0）。
  size_t retainedMiniBitmapCapacity(int fontId) const;
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // v110 字型預取:「目前快取裡的字屬於哪一頁」。
  // 不變量:任何 clearCache()/prewarmCache() 都會 invalidate();只有呼叫端在一次
  // 【完整未中止】的 prewarm 之後才顯式 adopt。無人 adopt ⇒ 永遠 invalid ⇒ 永不相符。
  const WarmIdentity& warmIdentity() const { return warmIdentity_; }
  void adoptWarmIdentity(const WarmIdentity& id) { warmIdentity_ = id; }
  void invalidateWarm() { warmIdentity_.invalidate(); }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    // v110:同上,但可被中止。
    // 回傳 false = 被中止【或】發生硬失敗 ⇒ 快取【不完整或已被釋放】。兩種情況本 scope 都會
    //   自動把 retain 解除,解構時照常 clearCache;呼叫端【不得】採用(adopt)身分。
    // 回傳 true = 沒有發生中止。⚠️ 冪等守衛也回 true:對一個【已經結束】的 scope
    //   再呼叫一次會直接回 true 而不做任何事,那代表「這一趟沒有中止」,不代表
    //   「這一次真的做了 prewarm」。
    bool endScanAndPrewarmAbortable(bool (*shouldAbort)(void*), void* abortCtx);
    // v110:預設 false = 解構時清空快取(既有行為)。設 true 才把 prewarm 好的字
    // 留在快取裡給下一次繪製用——只有明確採用(adopt)身分的呼叫端該開。
    void setRetainCacheOnExit(bool retain) { retainCacheOnExit_ = retain; }
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    bool endScanAndPrewarmImpl(bool (*shouldAbort)(void*), void* abortCtx);

    FontCacheManager* manager_;
    bool active_ = true;
    bool retainCacheOnExit_ = false;
  };
  PrewarmScope createPrewarmScope();

 private:
  // v110:prewarmCache() 的實作結果。
  //   Ok      = 完整跑完。「有 N 個字這套字型沒有」也算 Ok —— 重跑一次會得到一模一樣的
  //             快取,採用身分是正確的。
  //   Aborted = 被 shouldAbort 中止(只有 SD 分支會發生)⇒ 剩下的桶不再送。
  //   Failed  = 硬失敗(配置失敗 / 開檔、seek、短讀)⇒ 該字面的快取已被整組釋放。
  //             【必須】與 Ok 分開:失敗的 prewarm 若被當成成功,呼叫端會替一份空快取
  //             蓋上 valid 身分,下一次 render 無聲地整頁走 overflow ring,而
  //             dropped/alloc_fail 全是 0 ⇒ spec §7 的回退判準看不見(最終複審 Finding A)。
  // 公開的 prewarmCache() 以 nullptr 呼叫它並丟棄結果,行為與 v109 逐位元組相同。
  enum class PrewarmOutcome : uint8_t { Ok, Aborted, Failed };
  PrewarmOutcome prewarmCacheImpl(int fontId, const char* utf8Text, uint8_t styleMask, bool (*shouldAbort)(void*),
                                  void* abortCtx);

  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;
  // v110:目前快取內容的身分。預設 invalid;沒有人 adopt 就永遠 invalid。
  WarmIdentity warmIdentity_;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;
  // v54:掃描文字改「每個字重各自一份」。原本是把整頁文字連成一條、餵給每一個出現過的
  // 字重——後果是粗體字型也要載入整頁的字(含它根本不會畫的),SD I/O 與 miniBitmap 單塊
  // 配置都是雙份。實測(v53 diag):混排頁 prewarm 547ms、單塊 33,803 B;拆開後各砍一半。
  std::string scanTextPerStyle_[4];
  uint32_t scanStyleCounts_[4] = {};
  uint8_t lastMainStyle_ = 0;  // 上一頁字數最多的字重(決定下一頁 reserve 給哪個桶)
  int scanFontId_ = -1;
};
