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
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

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

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
