#pragma once
#include <ArduinoJson.h>
#include <cstdio>
#include <DataDir.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct OpdsServer {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
};

/**
 * Singleton class for storing OPDS server configurations on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON.
 */
class OpdsServerStore : public PersistableStore<OpdsServerStore> {
 private:
  std::vector<OpdsServer> servers;

  static constexpr size_t MAX_SERVERS = 8;

  OpdsServerStore() = default;

  friend class PersistableStore<OpdsServerStore>;

 public:
  static const char* getFilePath() {
    // v36/v186：掛在開機解析出的資料目錄上；首用必在 DataDir::resolve() 之後（開機順序）。
    static char p[40] = "";
    if (!p[0]) snprintf(p, sizeof(p), "%s/opds.json", DataDir::path());
    return p;
  }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool addServer(const OpdsServer& server);
  bool updateServer(size_t index, const OpdsServer& server);
  bool removeServer(size_t index);

  const std::vector<OpdsServer>& getServers() const { return servers; }
  const OpdsServer* getServer(size_t index) const;
  size_t getCount() const { return servers.size(); }
  bool hasServers() const { return !servers.empty(); }
};

#define OPDS_STORE OpdsServerStore::getInstance()
