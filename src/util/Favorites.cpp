#include "util/Favorites.h"

#include <nvs.h>

#include "util/DiagLog.h"
#include "util/NvsStore.h"
#include "util/ReadingStats.h"

namespace Favorites {
namespace {
constexpr char KEY[] = "fav";
}  // namespace

uint64_t idOf(const std::string& path) { return ReadingStats::bookIdentity(path); }

favorites::LoadResult load(favorites::FavoriteSet& set) {
  set.clear();
  size_t len = 0;
  int err = 0;
  favorites::LoadResult r = favorites::LoadResult::Absent;
  // 同 rstat（閱讀統計）：先查長度再讀，分三態
  if (NvsStore::getBlob(KEY, nullptr, &len, &err)) {
    if (len > favorites::kMaxBlob) {
      r = favorites::LoadResult::Newer;
    } else {
      uint8_t buf[favorites::kMaxBlob];
      size_t got = sizeof(buf);
      // 讀得到長度、讀內容卻失敗＝I/O 問題，不是壞資料：保守，這次不寫（只有讀到了、格式不對才算 Corrupt）
      r = NvsStore::getBlob(KEY, buf, &got, &err) ? set.decode(buf, got) : favorites::LoadResult::Newer;
    }
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    r = favorites::LoadResult::Newer;  // 讀不了、不知道裡面是什麼：保守，這次不寫
  }
  if (r != favorites::LoadResult::Compatible && r != favorites::LoadResult::Absent) {
    DiagLog::line("FAV load result=%d err=%d len=%u", static_cast<int>(r), err, static_cast<unsigned>(len));
    set.clear();
  }
  return r;
}

bool save(const favorites::FavoriteSet& set, SaveInfo* info) {
  uint8_t buf[favorites::kMaxBlob];
  const size_t len = set.encode(buf);
  uint32_t us = 0;
  int err = 0;
  const bool ok = NvsStore::putBlob(KEY, buf, len, &us, &err);
  if (info) {
    *info = SaveInfo{ok, static_cast<uint8_t>(set.size()), us, err};
  } else {
    logSave(SaveInfo{ok, static_cast<uint8_t>(set.size()), us, err});
  }
  return ok;
}

void logSave(const SaveInfo& s) {
  DiagLog::line("FAV save n=%u ok=%d us=%lu err=%d", static_cast<unsigned>(s.n), s.ok ? 1 : 0,
                static_cast<unsigned long>(s.us), s.err);
}

void renameBook(const std::string& from, const std::string& to) {
  favorites::FavoriteSet set;
  if (load(set) != favorites::LoadResult::Compatible) return;
  if (set.rename(idOf(from), idOf(to))) save(set, nullptr);
}

void removeBook(const std::string& path) {
  favorites::FavoriteSet set;
  if (load(set) != favorites::LoadResult::Compatible) return;
  if (set.remove(idOf(path))) save(set, nullptr);
}

}  // namespace Favorites
