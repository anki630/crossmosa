#pragma once
#include <ArduinoJson.h>
#include <cstdio>
#include <DataDir.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct RecentBook {
  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;

  bool operator==(const RecentBook& other) const { return path == other.path; }
  // v31/v155：全書閱讀進度（0–100）。主畫面續讀卡的作者行顯示成「作者 (45%)」。
  uint8_t progressPercent = 0;
};

class RecentBooksStore : public PersistableStore<RecentBooksStore> {
 private:
  std::vector<RecentBook> recentBooks;

  static constexpr int MAX_RECENT_BOOKS = 10;

  RecentBooksStore() = default;
  ~RecentBooksStore() = default;

  friend class PersistableStore<RecentBooksStore>;

 public:
  static const char* getFilePath() {
    // v36/v186：掛在開機解析出的資料目錄上；首用必在 DataDir::resolve() 之後（開機順序）。
    static char p[40] = "";
    if (!p[0]) snprintf(p, sizeof(p), "%s/recent.json", DataDir::path());
    return p;
  }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Add a book to the recent list (moves to front if already exists)
  void addBook(const std::string& path, const std::string& title, const std::string& author,
               const std::string& coverBmpPath);

  void updateBook(const std::string& path, const std::string& title, const std::string& author,
                  const std::string& coverBmpPath);

  /// v31/v155：更新某本書的進度百分比（存在才更新；變了才標 dirty）。
  void setProgress(const std::string& path, uint8_t progressPercent);

  // Remove the entry whose path matches (used when a book is removed from recents or finished/read).
  // Returns true if an entry was found and removed (no-op + false otherwise).
  // Persistence is best-effort: a failed save is logged, not reflected in the return.
  bool removeByPath(const std::string& path);

  // Repoint an entry's path (and coverBmpPath, if it lived under the old cache dir) after the
  // backing file and cache dir were moved on disk. No-op if no entry matches oldPath.
  // Persists on success. Keeps the entry's list position (does not reorder).
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  // True if the book's backing file is no longer present on the SD card.
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist — caller decides.
  bool pruneMissing();

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const { return recentBooks; }

  // Get the count of recent books
  int getCount() const { return static_cast<int>(recentBooks.size()); }

  RecentBook getDataFromBook(std::string path) const;
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
