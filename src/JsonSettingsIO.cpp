#include "JsonSettingsIO.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <string>

#include <PersistableStore.h>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SettingsList.h"
#include "WifiCredentialStore.h"


// Convert legacy settings.
void applyLegacyStatusBarSettings(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::STATUS_BAR_MODE>(settings.statusBar)) {
    case CrossPointSettings::NONE:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::NO_PROGRESS:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::ONLY_BOOK_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 0;
      settings.statusBarProgressBar = CrossPointSettings::BOOK_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::HIDE_TITLE;
      settings.statusBarBattery = 0;
      break;
    case CrossPointSettings::CHAPTER_PROGRESS_BAR:
      settings.statusBarChapterPageCount = 0;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::CHAPTER_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
    case CrossPointSettings::FULL:
    default:
      settings.statusBarChapterPageCount = 1;
      settings.statusBarBookProgressPercentage = 1;
      settings.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
      settings.statusBarTitle = CrossPointSettings::CHAPTER_TITLE;
      settings.statusBarBattery = 1;
      break;
  }
}

// ---- CrossPointState ----

bool JsonSettingsIO::saveState(const CrossPointState& s, const char* path) {
  JsonDocument doc;
  doc["openEpubPath"] = s.openEpubPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < CrossPointState::SLEEP_RECENT_COUNT; i++) recentArr.add(s.recentSleepImages[i]);
  doc["recentSleepPos"] = s.recentSleepPos;
  doc["recentSleepFill"] = s.recentSleepFill;
  doc["readerActivityLoadCount"] = s.readerActivityLoadCount;
  doc["lastSleepFromReader"] = s.lastSleepFromReader;
  doc["showBootScreen"] = s.showBootScreen;

  return PersistableStoreBase::writeDocAtomic(path, doc);
}

bool JsonSettingsIO::loadState(CrossPointState& s, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  s.openEpubPath = doc["openEpubPath"] | std::string("");
  memset(s.recentSleepImages, 0, sizeof(s.recentSleepImages));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount = recentArr.isNull() ? 0
                                             : std::min(static_cast<int>(recentArr.size()),
                                                        static_cast<int>(CrossPointState::SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) s.recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  s.recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (s.recentSleepPos >= CrossPointState::SLEEP_RECENT_COUNT)
    s.recentSleepPos = actualCount > 0 ? s.recentSleepPos % CrossPointState::SLEEP_RECENT_COUNT : 0;
  s.recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  s.recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(s.recentSleepFill), actualCount));
  // Migrate legacy single-image field from old state.json (pre-recency-buffer).
  // Only seeds the buffer if the new buffer is empty (fresh migration, not a resave).
  if (s.recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) s.pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  s.readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  s.lastSleepFromReader = doc["lastSleepFromReader"] | false;
  s.showBootScreen = doc["showBootScreen"] | true;
  return true;
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  JsonDocument doc;

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      if (info.obfuscated) {
        doc[std::string(info.key) + "_obf"] = obfuscation::obfuscateToBase64(strPtr);
      } else {
        doc[info.key] = strPtr;
      }
    } else {
      doc[info.key] = s.*(info.valuePtr);
    }
  }

  // v57:tiltPageTurn 必須手動存讀。SettingsList 只在 halTiltSensor.isAvailable() 為真時
  // 才插入這個條目(SettingsList.h:249),而 saveSettings 是拿【全新的 JsonDocument】依清單
  // 重建整份檔案 —— 所以只要某次開機 I2C 探測失敗、且那次又存了設定,這個 key 就從
  // settings.json 永久消失。這是 v52 那個 critical 的同一家族(清單驅動的持久化,條目缺席
  // 等於資料被抹掉),照該版的結論用手動條目處理。
  doc["tiltPageTurn"] = s.tiltPageTurn;

  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;
  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  doc["fontFamily"] = s.fontFamily;
  // UI theme — v52 起用 DynamicEnum(泛用迴圈跳過),手動存原值空間(LYRA=1/LYRA_3_COVERS=2;
  // 舊值 0/3=已退役主題,載入時正規化)。漏了這條=主題選擇重開機就重置(v52 複查抓到的 critical)。
  doc["uiTheme"] = s.uiTheme;
  // SD card font family name — not in SettingsList, save manually
  if (s.sdFontFamilyName[0] != '\0') {
    doc["sdFontFamilyName"] = s.sdFontFamilyName;
  }
  // Dictionary folder name — uses dynamic getter/setter in SettingsList, save manually
  if (s.dictionaryName[0] != '\0') {
    doc["dictionaryName"] = s.dictionaryName;
  }

  // BLE remote pairing — managed by BleRemotePairingActivity, not in SettingsList.
  if (s.bleRemotePeerAddr[0] != '\0') {
    doc["bleRemotePeerAddr"] = s.bleRemotePeerAddr;
    doc["bleRemotePeerAddrType"] = s.bleRemotePeerAddrType;
    doc["bleRemotePeerName"] = s.bleRemotePeerName;
  }

  // Language -- managed by LanguageSelectActivity, not in SettingsList.
  // Stored as ISO code string ("EN", "DE", ...) for stability across enum reorders.
  doc["language"] = (s.language < getLanguageCount()) ? LANGUAGE_CODES[s.language] : "EN";

  return PersistableStoreBase::writeDocAtomic(path, doc);
}

bool JsonSettingsIO::loadSettings(CrossPointSettings& s, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  auto clamp = [](uint8_t val, uint8_t maxVal, uint8_t def) -> uint8_t { return val < maxVal ? val : def; };

  // Legacy migration: if statusBarChapterPageCount is absent this is a pre-refactor settings file.
  // Populate s with migrated values now so the generic loop below picks them up as defaults and clamps them.
  if (doc["statusBarChapterPageCount"].isNull()) {
    applyLegacyStatusBarSettings(s);
  }

  for (const auto& info : getSettingsList()) {
    if (!info.key) continue;
    // Dynamic entries (KOReader etc.) are stored in their own files — skip.
    if (!info.valuePtr && !info.stringOffset) continue;

    if (info.stringOffset) {
      const char* strPtr = (const char*)&s + info.stringOffset;
      const std::string fieldDefault = strPtr;  // current buffer = struct-initializer default
      std::string val;
      if (info.obfuscated) {
        bool ok = false;
        val = obfuscation::deobfuscateFromBase64(doc[std::string(info.key) + "_obf"] | "", &ok);
        if (!ok || val.empty()) {
          val = doc[info.key] | fieldDefault;
          if (val != fieldDefault && needsResave) *needsResave = true;
        }
      } else {
        val = doc[info.key] | fieldDefault;
      }
      char* destPtr = (char*)&s + info.stringOffset;
      if (info.stringMaxLen == 0) {
        LOG_ERR("CPS", "Misconfigured SettingInfo: stringMaxLen is 0 for key '%s'", info.key);
        destPtr[0] = '\0';
        if (needsResave) *needsResave = true;
        continue;
      }
      strncpy(destPtr, val.c_str(), info.stringMaxLen - 1);
      destPtr[info.stringMaxLen - 1] = '\0';
    } else {
      const uint8_t fieldDefault = s.*(info.valuePtr);  // struct-initializer default, read before we overwrite it
      uint8_t v = doc[info.key] | fieldDefault;
      if (info.type == SettingType::ENUM) {
        v = clamp(v, (uint8_t)info.enumValues.size(), fieldDefault);
      } else if (info.type == SettingType::TOGGLE) {
        v = clamp(v, (uint8_t)2, fieldDefault);
      } else if (info.type == SettingType::VALUE) {
        if (v < info.valueRange.min)
          v = info.valueRange.min;
        else if (v > info.valueRange.max)
          v = info.valueRange.max;
      }
      s.*(info.valuePtr) = v;
    }
  }

  if (doc["sleepTimeoutMinutes"].isNull() && !doc["sleepTimeout"].isNull()) {
    const uint8_t legacyValue =
        clamp(doc["sleepTimeout"] | (uint8_t)CrossPointSettings::SLEEP_10_MIN, CrossPointSettings::SLEEP_TIMEOUT_COUNT,
              (uint8_t)CrossPointSettings::SLEEP_10_MIN);
    s.sleepTimeoutMinutes = CrossPointSettings::sleepTimeoutEnumToMinutes(legacyValue);
    if (needsResave) *needsResave = true;
  }
  // Front button remap — managed by RemapFrontButtons sub-activity, not in SettingsList.
  using S = CrossPointSettings;
  s.frontButtonBack =
      clamp(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clamp(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM, S::FRONT_BUTTON_HARDWARE_COUNT,
                               S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clamp(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight =
      clamp(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_RIGHT);
  CrossPointSettings::validateFrontButtonMapping(s);

  // v57:tiltPageTurn 手動載入(存檔端同理,見上)。清單在 IMU 缺席時不含這個條目,
  // 泛用迴圈就讀不到它——即使 settings.json 裡有值也會被還原成預設。
  s.tiltPageTurn = clamp(doc["tiltPageTurn"] | (uint8_t)S::TILT_OFF, S::TILT_PAGE_TURN_COUNT, S::TILT_OFF);

  // UI theme — 手動載入+正規化(非 LYRA/LYRA_3_COVERS 的殘值(含退役的 Classic/RoundedRaff)→ Formosa)
  const uint8_t storedUiTheme = doc["uiTheme"] | (uint8_t)CrossPointSettings::UI_THEME::LYRA;
  s.uiTheme = (storedUiTheme == CrossPointSettings::UI_THEME::LYRA_3_COVERS)
                  ? (uint8_t)CrossPointSettings::UI_THEME::LYRA_3_COVERS
                  : (uint8_t)CrossPointSettings::UI_THEME::LYRA;

  // Font family — uses dynamic getter/setter in SettingsList so the generic loop skips it.
  const uint8_t storedFontFamily = doc["fontFamily"] | (uint8_t)0;
  s.fontFamily = clamp(storedFontFamily, CrossPointSettings::BUILTIN_FONT_COUNT, 0);
  // SD card font family name — not in SettingsList, load manually
  const char* sfn = doc["sdFontFamilyName"] | "";
  strncpy(s.sdFontFamilyName, sfn, sizeof(s.sdFontFamilyName) - 1);
  s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
  if (storedFontFamily == CrossPointSettings::LEGACY_OPENDYSLEXIC && s.sdFontFamilyName[0] == '\0') {
    s.fontFamily = CrossPointSettings::NOTOSERIF;
    strncpy(s.sdFontFamilyName, "OpenDyslexic", sizeof(s.sdFontFamilyName) - 1);
    s.sdFontFamilyName[sizeof(s.sdFontFamilyName) - 1] = '\0';
    if (needsResave) *needsResave = true;
  } else if (storedFontFamily >= CrossPointSettings::BUILTIN_FONT_COUNT) {
    if (needsResave) *needsResave = true;
  }

  // Dictionary folder name — uses dynamic getter/setter in SettingsList, load manually
  const char* dictName = doc["dictionaryName"] | "";
  strncpy(s.dictionaryName, dictName, sizeof(s.dictionaryName) - 1);
  s.dictionaryName[sizeof(s.dictionaryName) - 1] = '\0';

  // BLE remote pairing (absent key -> stays unpaired/defaults).
  const char* bleAddr = doc["bleRemotePeerAddr"] | "";
  strncpy(s.bleRemotePeerAddr, bleAddr, sizeof(s.bleRemotePeerAddr) - 1);
  s.bleRemotePeerAddr[sizeof(s.bleRemotePeerAddr) - 1] = '\0';
  s.bleRemotePeerAddrType = doc["bleRemotePeerAddrType"] | (uint8_t)0;
  const char* bleName = doc["bleRemotePeerName"] | "";
  strncpy(s.bleRemotePeerName, bleName, sizeof(s.bleRemotePeerName) - 1);
  s.bleRemotePeerName[sizeof(s.bleRemotePeerName) - 1] = '\0';

  // Language -- stored as code string for stability across enum reorders.
  if (doc["language"].is<const char*>()) {
    s.language = static_cast<uint8_t>(I18n::languageFromCode(doc["language"].as<const char*>()));
  }

  LOG_DBG("CPS", "Settings loaded from file");

  return true;
}

// ---- Bookmarks ----

bool JsonSettingsIO::saveBookmarks(const std::vector<BookmarkEntry>& bookmarks, const char* path) {
  JsonDocument doc;
  JsonArray arr = doc["bookmarks"].to<JsonArray>();
  LOG_DBG("BKM", "Saving %zu bookmarks to file", bookmarks.size());
  for (const auto& bookmark : bookmarks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["xpath"] = bookmark.xpath;
    obj["percentage"] = bookmark.percentage;
    obj["summary"] = bookmark.summary;
    obj["si"] = bookmark.computedSpineIndex;
    obj["pc"] = bookmark.computedChapterPageCount;
    obj["pp"] = bookmark.computedChapterProgress;
  }

  return PersistableStoreBase::writeDocAtomic(path, doc);
}

bool JsonSettingsIO::loadBookmarks(std::vector<BookmarkEntry>& bookmarks, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("BKM", "JSON parse error: %s", error.c_str());
    return false;
  }

  JsonArray arr = doc["bookmarks"].as<JsonArray>();
  bookmarks.clear();
  bookmarks.reserve(arr.size());
  for (JsonObject obj : arr) {
    bookmarks.emplace_back();
    auto& bookmark = bookmarks.back();
    bookmark.xpath = obj["xpath"] | std::string("");
    bookmark.percentage = obj["percentage"] | static_cast<float>(0);
    bookmark.summary = obj["summary"] | std::string("");
    bookmark.computedSpineIndex = obj["si"] | static_cast<uint16_t>(0);
    bookmark.computedChapterPageCount = obj["pc"] | static_cast<uint16_t>(0);
    bookmark.computedChapterProgress = obj["pp"] | static_cast<uint16_t>(0);
  }

  LOG_DBG("BKM", "Loaded %zu bookmarks from file", bookmarks.size());
  return true;
}
