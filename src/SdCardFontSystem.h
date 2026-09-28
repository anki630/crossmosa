#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>

class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and (when loadSelectedNow) load user's saved selection. Call once during setup.
  /// v311：首頁醒來不需要內文字型 → 傳 false 只探索＋註冊解析器；之後 ensureLoaded() 會載入。
  void begin(GfxRenderer& renderer, bool loadSelectedNow = true);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + reader point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  // v121/v161：診斷統計用（TXTPAGE 折算預取的 afail/dropped）。可能為 nullptr（未載入 SD 字型）。
  SdCardFont* currentReaderFont() const { return manager_.currentFontForStats(); }

  // 注音（P2 I7）：記憶體不夠時引擎先退（「引擎讓記憶體，不是讓排版失敗」）。卸掉引擎（約 23 KB）、世代換掉 →
  //   這一章之後的行都不換、章節提交時記成「沒注音」；下一次 ensureLoaded（進閱讀器、醒來）記憶體夠就再開。
  //   不動字型本身：排版量寬用的是字型的 advance，卸字型會排出不同分頁（handleLowMemoryBuild 的註解）；引擎不影響量寬。
  //   ⚠️ 只能在持 RenderLock 的地方呼叫（建置 tick、render）：TXT 的游標與 EPUB 的 session 在排版當中拿著引擎的暫存。
  //   回 true ＝ 真的卸了（有印 ZY yield）。
  bool yieldZhuyin(const char* why);

  // 注音（P2）：fontId 是目前的閱讀字型、而且是注音字型（檔頭有標記）—— 不管引擎開了沒有。
  // 漢字格一律 1.5 em，所以直排欄距（vtext::columnPitchPx）只看這個；章節身分（readerRenderSpec）也用同一個判斷。
  bool isZhuyinFont(int fontId) const;

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// v5/v148：釋放常駐的 SD 閱讀字型（interval 表 + advance/glyph 快取，數十 KB —— 實測
  /// p2 裡的 43,008 mini bitmap 就是它的）。給 WiFi 啟動或圖片解碼這類需要大連續塊的
  /// 階段用。已存的選擇（SETTINGS.sdFontFamilyName）不動，所以 begin()（WiFi session 後
  /// 的重開機）或 ensureLoaded()（下次進閱讀器）會自動重載 —— 這裡不需要顯式 reload。
  /// 字型 ID 是內容雜湊（SdCardFontManager::computeFontId），重載後不變。
  void unloadForLowMemory(GfxRenderer& renderer);

  /// v148（codex 複查後新增）：relief 之後的專用重載 —— 與 ensureLoaded() 有三個刻意的差異：
  ///  ① 【絕不】清除 SETTINGS.sdFontFamilyName —— 暫時性低記憶體不是使用者改了選擇，
  ///     清掉會把一次 OOM 變成永久設定遺失（ensureLoaded 失敗時會 clearSdFontFamily）。
  ///  ② 只載 reader 尺寸，不做 setupUiFallbacks（那最多再讀三個 UI 尺寸檔，
  ///     在 RenderLock 下的 render 中途做太重；UI 備援等下次 ensureLoaded 補）。
  ///  ③ 回傳 bool —— 失敗時呼叫端知道，內文暫時落回內建字型（下次進閱讀器自癒）。
  bool reloadReaderFontAfterRelief(GfxRenderer& renderer);

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

 private:
  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched CJK fallback for the corresponding UI font, so CJK book
  // titles/list rows render at the same size as the surrounding Latin UI text.
  // No-op when no SD family is loaded. Safe to call repeatedly (sizes already
  // loaded are reused).
  void setupUiFallbacks(GfxRenderer& renderer);
  // 注音（P2）：閱讀字型是注音字型、引擎還沒好 → 試著啟用（ensureLoaded 的每一個出口都呼叫；開機的 begin 不做，
  // 開機路徑一行都不動）。資料或配對壞了 → 記住這個字型檔（內容雜湊）別再試；記憶體或堆疊不夠 → 下次進閱讀器再試。
  void ensureZhuyin();
  // readerRenderSpec 用：這個字型（必須是目前的閱讀字型）現在的注音身分、「引擎沒開」與「引擎開著」時的身分；非注音 →
  // 0／0／0
  void zhuyinIdentities(int fontId, uint32_t* current, uint32_t* off, uint32_t* on) const;

  SdCardFontRegistry registry_;
  uint32_t zhuyinGaveUpHash_ = 0;
  bool zhuyinOffLogged_ = false;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
