#pragma once

// Formosa Cover 書架（2026-10-07）：卡上所有的書，攤平（Kindle 模式）。純邏輯，電腦端測試在 test/shelf_index。
//
// 記憶體有上限、不隨書量成長（維護者：「我自己好用別人也要好用」；體驗目標 200 本，超過可以變慢、可以提示，絕不當機）：
//   - 最多 kMaxBooks 本；路徑放在一塊連續的 arena（總長上限 kMaxArenaBytes），另存每本的起點。
//   - 掃到上限就停（capped＝true），書架顯示「只顯示前 N 本」。
//   - 只在首頁以外的書架畫面存在；離開書架就整個釋放（進閱讀器時不佔記憶體）。
// 排序：最近閱讀（照最近閱讀的順序）在前，其他照檔名（位元組順序，跟瀏覽檔案一樣的穩定順序）。

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

class ShelfIndex {
 public:
  static constexpr size_t kMaxBooks = 300;
  static constexpr size_t kMaxArenaBytes = 24 * 1024;

  ShelfIndex() = default;
  ShelfIndex(const ShelfIndex&) = delete;
  ShelfIndex& operator=(const ShelfIndex&) = delete;
  ~ShelfIndex() { release(); }

  // 一次配好 arena（上限 kMaxArenaBytes，不超過 maxBytes）。配不到就減半再試，最小 2 KB；都配不到回 false。
  //   -fno-exceptions：vector 長大失敗＝abort，所以 arena 用 malloc、索引用固定陣列（硬限制第 2 條）。
  bool begin(const size_t maxBytes) {
    release();
    size_t cap = std::min(kMaxArenaBytes, maxBytes);
    while (cap >= 2048 && !arena_) {
      arena_ = static_cast<char*>(std::malloc(cap));
      if (!arena_) cap /= 2;
    }
    arenaCap_ = arena_ ? cap : 0;
    return arena_ != nullptr;
  }

  void release() {
    std::free(arena_);
    arena_ = nullptr;
    arenaCap_ = arenaUsed_ = 0;
    count_ = 0;
    view_ = 0;
    pinned_ = 0;
    capped_ = false;
    sorted_ = false;
  }

  // 最近閱讀（與我的最愛）先放（掃描之前呼叫）：書多到撞上限時，它們也一定在書架上；最近閱讀排在最前面。
  //   之後 add() 遇到同一個路徑就跳過（不重複）。
  bool addPinned(const char* path, const size_t len) {
    if (isPinned(path, len)) return true;
    if (!append(path, len)) return false;
    pinned_ = count_;
    return true;
  }

  // 加一本（完整路徑，len＝位元組數）。已經在最近閱讀裡的回 true 但不重複放；滿了回 false 並記 capped。
  bool add(const char* path, const size_t len) {
    if (isPinned(path, len)) return true;
    return append(path, len);
  }
  bool add(const std::string& path) { return add(path.c_str(), path.size()); }

  // 目前檢視的本數（sort 之後：「全部」＝全部、「最愛」＝最愛那幾本）；total()＝索引裡的全部
  size_t size() const { return sorted_ ? view_ : count_; }
  size_t total() const { return count_; }
  bool capped() const { return capped_; }
  size_t arenaBytes() const { return arenaUsed_; }
  size_t arenaCapacity() const { return arenaCap_; }

  // 排序後第 i 本（i 是顯示順序）
  const char* path(const size_t i) const { return arena_ + offs_[sorted_ ? order_[i] : i]; }

  // recents：最近閱讀的路徑，最近的在前（RecentBooksStore 的順序）。
  // isFavorite(path)：是不是我的最愛（v367）；favoritesOnly＝只留最愛（「最愛」分頁），順序規則相同。
  void sort(const std::vector<std::string>& recents) {
    sort(recents, [](const char*) { return false; }, false);
  }
  template <typename IsFav>
  void sort(const std::vector<std::string>& recents, IsFav isFavorite, const bool favoritesOnly) {
    for (size_t i = 0; i < count_; ++i) fav_[i] = isFavorite(static_cast<const char*>(arena_ + offs_[i]));
    for (size_t i = 0; i < count_; ++i) {
      order_[i] = static_cast<uint16_t>(i);
      rank_[i] = static_cast<uint8_t>(recents.size() < 255 ? recents.size() : 255);
      for (size_t r = 0; r < recents.size() && r < 255; ++r) {
        if (recents[r] == arena_ + offs_[i]) {
          rank_[i] = static_cast<uint8_t>(r);
          break;
        }
      }
    }
    std::stable_sort(order_, order_ + count_, [&](const uint16_t a, const uint16_t b) {
      if (rank_[a] != rank_[b]) return rank_[a] < rank_[b];
      return std::strcmp(baseName(arena_ + offs_[a]), baseName(arena_ + offs_[b])) < 0;
    });
    view_ = count_;
    if (favoritesOnly) {
      size_t n = 0;
      for (size_t i = 0; i < count_; ++i)
        if (fav_[order_[i]]) order_[n++] = order_[i];
      view_ = n;
    }
    sorted_ = true;
  }

  // 第 i 本（顯示順序）是不是最愛
  bool isFavorite(const size_t i) const { return sorted_ && fav_[order_[i]]; }

  // 索引裡（不管目前檢視）有幾本是最愛 —— sort 之後才有意義（v369：有最愛才顯示「最愛」分頁、預設停在那裡）
  size_t favoriteCount() const {
    if (!sorted_) return 0;
    size_t n = 0;
    for (size_t i = 0; i < count_; ++i) n += fav_[i] ? 1 : 0;
    return n;
  }

  size_t pinnedCount() const { return pinned_; }

  // 第 i 本（顯示順序）是不是最近閱讀
  bool isRecent(const size_t i, const size_t recentCount) const { return sorted_ && rank_[order_[i]] < recentCount; }

  // 檔名（最後一個 / 之後）
  static const char* baseName(const char* p) {
    const char* s = std::strrchr(p, '/');
    return s ? s + 1 : p;
  }

  // 顯示用書名：檔名去掉副檔名（沒開過的書只知道檔名）
  static std::string titleFromPath(const char* p) {
    std::string t = baseName(p);
    const size_t dot = t.rfind('.');
    if (dot != std::string::npos && dot > 0) t.resize(dot);
    return t;
  }

 private:
  bool isPinned(const char* path, const size_t len) const {
    for (size_t i = 0; i < pinned_; ++i) {
      const char* p = arena_ + offs_[i];
      if (std::strncmp(p, path, len) == 0 && p[len] == '\0') return true;
    }
    return false;
  }

  bool append(const char* path, const size_t len) {
    if (!arena_ || count_ >= kMaxBooks || arenaUsed_ + len + 1 > arenaCap_) {
      capped_ = true;
      return false;
    }
    offs_[count_++] = static_cast<uint32_t>(arenaUsed_);
    std::memcpy(arena_ + arenaUsed_, path, len);
    arena_[arenaUsed_ + len] = '\0';
    arenaUsed_ += len + 1;
    sorted_ = false;
    return true;
  }

  char* arena_ = nullptr;
  size_t arenaCap_ = 0;
  size_t arenaUsed_ = 0;
  size_t count_ = 0;
  size_t pinned_ = 0;  // 前 pinned_ 本是 addPinned 放的最近閱讀
  bool capped_ = false;
  bool sorted_ = false;
  uint32_t offs_[kMaxBooks] = {};
  uint16_t order_[kMaxBooks] = {};
  uint8_t rank_[kMaxBooks] = {};
  bool fav_[kMaxBooks] = {};
  size_t view_ = 0;  // sort 之後目前檢視的本數
};
