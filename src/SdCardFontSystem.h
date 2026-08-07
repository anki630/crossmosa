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

  /// Unload the currently loaded SD reader font from RAM to reclaim heap before
  /// a RAM-hungry phase (e.g. bringing up WiFi). Frees the resident interval
  /// tables, advance and glyph caches and unregisters the font from the
  /// renderer, but does NOT clear the user's saved selection
  /// (SETTINGS.sdFontFamilyName). The font is reloaded automatically afterwards:
  /// by begin() after the WiFi-session reboot, or by ensureLoaded() on the next
  /// reader entry. UI text is unaffected (it uses the flash-resident builtin
  /// fonts). No-op if nothing is loaded. Reclaims roughly 20-90 KB depending on
  /// the loaded family/size.
  void unloadForLowMemory(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// 「改字級會不會真的換到不同字面」——問【登錄表】而非已載入字型。
  /// resolveFontId 回傳的是當下已載入那一級的 fontId(改設定後要到下次 ensureLoaded
  /// 才重載),改前改後比對永遠相同;v38 字級快捷的重啟守門就是這樣被跳過(實機回報)。
  /// 內建備援(無 SD 字型)回 false:OMIT_FONTS 下四級同一字面,重啟純浪費。
  bool sizeChangeTakesEffect(uint8_t oldSizeEnum, uint8_t newSizeEnum) const;

  /// v53 量測:目前載入的內文字型物件(讀 prewarm 統計用;無 SD 字型時 nullptr)。
  SdCardFont* currentReaderFont() const { return manager_.currentFont(); }

  /// v55:目前 SD 內文字型常駐的位元組數(沒載入回 0)。
  /// 供「卸字型換連續堆」的呼叫點先問一句「值不值得」——重載一次約 868ms。
  size_t residentBytes() const;

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

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
  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
