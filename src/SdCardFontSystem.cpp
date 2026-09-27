#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <ZhuyinEngine.h>
#include <ZhuyinActive.h>
#include <ZhuyinIdentity.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"
#include "util/DiagLog.h"

namespace {

// v189 證人：SD 字型物件在什麼時候、為什麼被（重新）建立。每個 SdCardFont 帶一組常駐表
// （interval 索引 17,408B ＋ advance 表 2×6,400B），建立當下落在哪個池就永遠住那裡；
// 落在 p2 中段就是把 p2 切成兩半的那顆（diag-prev188 的 pmax=28）。mem() 一起印，
// 兩個池的最大連續塊直接對照。開機那一次也印，才有基準線。
void noteFontLoad(const char* why) {
  DiagLog::line("FONTLOAD family=%s pt=%u why=%s", SETTINGS.sdFontFamilyName,
                static_cast<unsigned>(SETTINGS.fontPointSize), why);
  DiagLog::mem("fontload");
}

// Point the reader font size at a size the given family actually ships, and
// persist the change so the settings UI and the loaded font never disagree.
// Guarded by the value-change check: a no-op snap must not write SPIFFS.
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter). Each is paired with a same-size SD fallback so CJK UI
// text matches the surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer, const bool loadSelectedNow) {
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;
  SETTINGS.zhuyinSpecResolver = [](void* ctx, const int fontId, uint32_t* current, uint32_t* off, uint32_t* on) {
    static_cast<const SdCardFontSystem*>(ctx)->zhuyinIdentities(fontId, current, off, on);
  };
  SETTINGS.zhuyinSpecCtx = this;
  // 注音（codex 整合複查 A5、第二輪）：解析器每次進場前問「這個任務的堆疊還剩多少」。載入時的門檻只量到載入的那個任務，
  //   解析器之後在繪製任務（前景排版、TXT）與主任務（背景建置）裡跑，而且最深的一次是從排版裡面進來的
  //   （extractLine → zhuyinTake → session.finish → 解析器 → 讀資料群組 → SD）。
  //   實量（韌體的編譯參數＋-fstack-usage，2026-09-24）：session＋解析器約 0.4 KB（addWord 64、process 80、resolveWindow 32、
  //   run 144、matchLongest 64）；讀卡那一串約 0.6 KB（ZyBlockSource::read 48、HalFile::read 32、ExFatFile::read 80、快取 48、
  //   readSectors 32、cardCommand 48、__spiTransferBytes 96、兩次互斥鎖）；檔柄出錯要重開再加幾百 → 最壞約 1.5 KB → 門檻 3 KB。
  //   不夠 → 那一段不標（降級），不是爆堆疊。實際餘裕看 ZYPAGE／TXTPAGE 的 shwm=、ZYBUILD 的 hwm= 與 stk=（守衛擋下的次數）。
  //   前提：堆疊往低位址長、pxTaskGetStackStart ＝ 最低位址（FreeRTOS 的 portSTACK_GROWTH < 0，下面檢查）。
  static_assert(portSTACK_GROWTH < 0, "the zhuyin stack guard assumes a downward-growing stack");
  zhuyin::setStackGuard([]() -> bool {
    constexpr uintptr_t kResolverStack = 3072;
    const auto start = reinterpret_cast<uintptr_t>(pxTaskGetStackStart(nullptr));
    const auto here = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    return start == 0 || (here > start && here - start >= kResolverStack);
  });

  // If user has a saved SD font selection, load it.
  // v311：loadSelectedNow=false（首頁醒來）→ 跳過載入，只保留探索與解析器；
  //   進閱讀器時 ReaderActivity::onEnter 的 ensureLoaded() 會補載（FONTLOAD why=ensure）。
  if (loadSelectedNow && SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
        noteFontLoad("boot");
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
    }
  }

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.clearSdFontFamily();
      return;
    }
    const auto* selected = family->findNearestSize(SETTINGS.fontPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) {
      ensureZhuyin();
      return;
    }
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
      noteFontLoad(registryWasDirty ? "ensure-dirty" : "ensure");
      ensureZhuyin();
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.clearSdFontFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.clearSdFontFamily();
  }
}

void SdCardFontSystem::ensureZhuyin() {
  SdCardFont* f = manager_.currentFontForStats();
  if (!f || !f->hasZhuyinMarker() || f->zhuyinReady()) return;
  if (zhuyinGaveUpHash_ != 0 && zhuyinGaveUpHash_ == f->contentHash()) return;
  // 除錯用哨兵（不寫進任何對外文件）：SD 根目錄放空檔 zhuyin.off → 引擎不啟用（同字型、同版面、只差有沒有替換）
  if (Storage.exists("/zhuyin.off")) {
    if (!zhuyinOffLogged_) DiagLog::line("ZY load off=sentinel");
    zhuyinOffLogged_ = true;
    return;
  }
  const int fontId = manager_.getFontId(manager_.currentFamilyName());
  const auto r = f->enableZhuyin(fontId);
  static const char* const kWhy[] = {"ready", "not-zhuyin", "low-stack", "low-memory", "bad-marker",
                                     "no-memory", "load-failed", "pair-mismatch"};
  const auto st = static_cast<size_t>(r.status);
  DiagLog::line(
      "ZY load st=%s load=%u case=%u dataset=%08lx%08lx ms=%lu reads=%lu resident=%lu stack=%lu hwm=%lu->%lu "
      "free=%lu->%lu max=%lu->%lu",
      st < std::size(kWhy) ? kWhy[st] : "?", static_cast<unsigned>(r.load), static_cast<unsigned>(r.failedCase),
      static_cast<unsigned long>(f->zhuyinMarkerDataset() >> 32), static_cast<unsigned long>(f->zhuyinMarkerDataset()),
      static_cast<unsigned long>(r.ms), static_cast<unsigned long>(r.reads), static_cast<unsigned long>(r.resident),
      static_cast<unsigned long>(r.stackFree), static_cast<unsigned long>(r.hwmBefore),
      static_cast<unsigned long>(r.hwmAfter), static_cast<unsigned long>(r.freeBefore),
      static_cast<unsigned long>(r.freeAfter), static_cast<unsigned long>(r.maxBefore),
      static_cast<unsigned long>(r.maxAfter));
  // v339：載入時間拆開（diag338 只有總數 ms=2035）。sms＝結構載入、pre＝整塊預讀、tms＝其餘（自我測試＋收尾）；
  //   card＝真的讀卡的次數（結構段＋之後）、fail＝其中失敗（之後改直接讀）；calls＝載入對讀取層的呼叫次數（v338 每一次都讀卡）；
  //   lock／seek／rd＝讀卡時間的三段（毫秒，定義見 SdCardFont.cpp 的 ZyBlockSource）；back＝目標在檔柄目前位置之前的讀卡次數。
  if (r.status != SdCardFont::ZhuyinEnable::NotZhuyin && r.calls > 0) {
    const uint32_t spent = r.structMs + r.preMs;
    DiagLog::line(
        "ZY loadio win=%lu whole=%lu sms=%lu pre=%lu tms=%lu calls=%lu card=%lu+%lu fail=%lu kb=%lu lock=%lu seek=%lu rd=%lu back=%lu",
        static_cast<unsigned long>(r.window), static_cast<unsigned long>(r.whole), static_cast<unsigned long>(r.structMs),
        static_cast<unsigned long>(r.preMs), static_cast<unsigned long>(r.ms > spent ? r.ms - spent : 0),
        static_cast<unsigned long>(r.calls), static_cast<unsigned long>(r.cardStruct),
        static_cast<unsigned long>(r.cardTest), static_cast<unsigned long>(r.cardFails),
        static_cast<unsigned long>(r.cardBytes / 1024), static_cast<unsigned long>(r.waitUs / 1000),
        static_cast<unsigned long>(r.seekUs / 1000), static_cast<unsigned long>(r.readUs / 1000),
        static_cast<unsigned long>(r.backSeeks));
  }
  DiagLog::mem("zy-load");
  using E = SdCardFont::ZhuyinEnable;
  if (r.status == E::BadMarker || r.status == E::LoadFailed || r.status == E::PairMismatch) {
    zhuyinGaveUpHash_ = f->contentHash();  // 這個字型檔本身的問題：換字型之前不再試
  }
}

bool SdCardFontSystem::yieldZhuyin(const char* why) {
  SdCardFont* f = manager_.currentFontForStats();
  if (!f || !f->zhuyinReady()) return false;
  const auto* eng = f->zhuyinEngine();
  const auto resident = static_cast<unsigned long>(eng ? eng->residentBytes() : 0);
  const auto freeBefore = static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
  f->disableZhuyin();
  DiagLog::line("ZY yield why=%s resident=%lu free=%lu->%lu max=%lu", why, resident, freeBefore,
                static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_DEFAULT)),
                static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
  return true;
}

bool SdCardFontSystem::isZhuyinFont(const int fontId) const {
  const SdCardFont* f = manager_.currentFontForStats();
  return f && f->hasZhuyinMarker() && fontId != 0 && fontId == manager_.getFontId(manager_.currentFamilyName());
}

void SdCardFontSystem::zhuyinIdentities(const int fontId, uint32_t* current, uint32_t* off, uint32_t* on) const {
  *current = 0;
  *off = 0;
  *on = 0;
  if (!isZhuyinFont(fontId)) return;
  SdCardFont* f = manager_.currentFontForStats();
  *off = zhuyin::engineIdentity(f->zhuyinMarkerDataset(), zhuyin::EngineMode::Off);
  *on = zhuyin::engineIdentity(f->zhuyinMarkerDataset(), zhuyin::EngineMode::On);
  *current = f->zhuyinReady() ? *on : *off;
}

void SdCardFontSystem::unloadForLowMemory(GfxRenderer& renderer) {
  if (!manager_.currentFamilyName().empty()) {
    LOG_DBG("SDFS", "Unloading SD reader font to free heap (reloads on next reader entry)");
    manager_.unloadAll(renderer);
  }
}

bool SdCardFontSystem::reloadReaderFontAfterRelief(GfxRenderer& renderer) {
  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  if (wantedFamily[0] == '\0') return true;  // 本來就沒選 SD 字型
  if (manager_.currentFamilyName() == wantedFamily) return true;  // 已載著（relief 沒真的卸）
  const auto* family = registry_.findFamily(wantedFamily);
  if (!family) return false;  // 卡不見了？不清設定，讓下次 ensureLoaded 走完整判斷
  // loadFamily 與 ensureLoaded 用同一個入口（它自己會取最接近的尺寸檔）。
  if (!manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
    LOG_ERR("SDFS", "Relief restore failed for %s (keeping setting; builtin fallback until next reader entry)",
            wantedFamily);
    return false;
  }
  // v189 證人：這條路徑重建 SdCardFont 物件（interval 索引 17,408B ＋ advance 表 2×6,400B），
  // 落在哪個池全看那一刻誰有空位——diag-prev188 的 p2 中段那組常駐表最可能就是從這裡來的。
  // ⚠️ 這裡刻意不 setupUiFallbacks（原本就沒有）：UI 備援字面在 unloadAll 時一起沒了，
  // 到下一次 ensureLoaded 才回來；v189 只加證人不改行為。
  noteFontLoad("relief");
  return true;
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on CJK codepoints, so a Latin-only family
  // can never act as a fallback and its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script: Han, Hiragana, Katakana, Hangul.
  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}
