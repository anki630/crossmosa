#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include <string>

/**
 * @brief Non-template core of PersistableStore.
 *
 * All ArduinoJson parse/serialize machinery is instantiated once here (in
 * PersistableStore.cpp) instead of in every store's translation unit. GCC
 * emits the JSON serializer/parser templates as local .isra clones per TU
 * (~0.5KB each), so keeping serializeJson/deserializeJson out of the stores
 * is what makes the abstraction flash-neutral.
 */
class PersistableStoreBase {
 public:
  // v53:JSON 持久化的單一落地路徑,三個完整性缺陷一次修掉——
  // ①序列化直接串流進檔案(不經中介 Arduino String:低記憶體下 String 配置失敗會被靜默截斷,
  //   寫出半截 JSON 卻回報成功);②比對 measureJson 預期值偵測短寫;③tmp + rename 原子落地
  //   (舊路徑先 remove 目標再寫,寫到一半失敗/斷電就永久失去該檔)。
  // 手法同 ProgressFile::writeAtomic(已在進度檔驗證過)。呼叫端負責確保父目錄存在。
  // 注意:rename 失敗的極短窗口內主檔不存在(與舊路徑同風險,ProgressFile 明文接受的取捨)。
  static bool writeDocAtomic(const char* path, const JsonDocument& doc);

 protected:
  PersistableStoreBase() = default;
  ~PersistableStoreBase() = default;

  // Serializes doc and writes it to path (ensures the data dir exists). Logs on failure.
  static bool writeDocToFile(const char* path, const JsonDocument& doc);

  // Reads path and parses it into doc. Returns false silently when the file
  // does not exist (expected on first boot); logs on read/parse failure.
  static bool readDocFromFile(const char* path, JsonDocument& doc);

  /**
   * Helper function for extracting an obfuscated password from a JSON value.
   * Accepts JsonVariantConst so callers can pass either a whole JsonDocument
   * or a JsonObject element (e.g. inside an array iteration).
   * If the decoded password requires a resave (e.g. from plaintext fallback), `needsResave` is set to true.
   */
  static std::string extractPassword(JsonVariantConst doc, bool& needsResave);
};

/**
 * @brief Base class for persistable singletons using CRTP.
 *
 * Derived classes must provide:
 * - A private default constructor
 * - friend class PersistableStore<Derived>;
 * - static const char* getFilePath();
 * - void toJson(JsonDocument& doc) const;
 * - bool fromJson(JsonVariantConst doc);
 *
 * Note for implementers: read string values as `const char*` (e.g.
 * `obj["name"] | ""`), never as `| std::string("")` — ArduinoJson's
 * std::string converter drags a per-TU copy of the whole JSON serializer
 * into flash via its serializeJson fallback.
 */
template <typename T>
class PersistableStore : public PersistableStoreBase {
 protected:
  PersistableStore() = default;
  ~PersistableStore() = default;

 public:
  // Delete copy constructor and assignment
  PersistableStore(const PersistableStore&) = delete;
  PersistableStore& operator=(const PersistableStore&) = delete;

  static T& getInstance() {
    static T instance;
    return instance;
  }

  bool saveToFile() const {
    JsonDocument doc;
    static_cast<const T*>(this)->toJson(doc);
    return writeDocToFile(T::getFilePath(), doc);
  }

  bool loadFromFile() {
    JsonDocument doc;
    if (!readDocFromFile(T::getFilePath(), doc)) {
      return false;
    }
    return static_cast<T*>(this)->fromJson(doc.as<JsonVariantConst>());
  }
};
