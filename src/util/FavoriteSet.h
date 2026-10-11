#pragma once

// Formosa Cover 的「我的最愛」（2026-10-08，v367）：第一個內建收藏集。純邏輯，電腦端測試在 test/formosa_cover。
//
// 存的是書的身分（64 位元雜湊），不是路徑：同閱讀統計的身分契約（ReadingStats::bookIdentity＝FNV-1a64(路徑, 種子
// FNV-1a64(CID))）。
//   換卡＝另一組身分（不同卡的最愛自然分開）；書被搬走或改名就對不上（閱讀器自己搬書時會改，見
//   Favorites::renameBook）。
// 為什麼不用檔案：v367 第一版存 SD 的路徑清單，codex 連三輪在檔案格式、tmp／改名、壞檔、記憶體上挖出新阻斷
//   （memory small-feature-no-new-persistence：小需求不蓋新的持久化層）。改成一塊固定大小的 NVS（同 rstat 閱讀統計）：
//   沒有字串、沒有配置、沒有檔案格式，整份寫入由 NVS 保證原子。
//
// 持久格式（NVS 鍵 "fav"，小端）：[0]='F' [1]=版本 1 [2]=本數 n（≤100） [3]=0，之後 n 個 uint64 身分。長度＝4＋8n。
//   順序：最近加入的在前。
// 讀回三態（同
// rstat）：Absent（沒有）／Compatible／Corrupt（從空的重新開始、可以寫）／Newer（較新版本寫的：這一段開機唯讀）。

#include <cstddef>
#include <cstdint>

namespace favorites {

constexpr uint8_t kMagic = 'F';
constexpr uint8_t kVersion = 1;
constexpr size_t kMax = 100;
constexpr size_t kHeader = 4;
constexpr size_t kMaxBlob = kHeader + 8 * kMax;  // 804 B

enum class LoadResult : uint8_t { Absent, Corrupt, Compatible, Newer };
enum class Toggle : uint8_t { Added, Removed, Full };

class FavoriteSet {
 public:
  bool contains(const uint64_t id) const { return find(id) >= 0; }
  size_t size() const { return n_; }
  uint64_t at(const size_t i) const { return ids_[i]; }
  void clear() { n_ = 0; }

  // 有就拿掉、沒有就加到最前面；滿了不加（回 Full，什麼都不改）。id 0 不是合法身分（ReadingStats 會避開 0）。
  Toggle toggle(const uint64_t id) {
    const int i = find(id);
    if (i >= 0) {
      erase(static_cast<size_t>(i));
      return Toggle::Removed;
    }
    if (n_ >= kMax || id == 0) return Toggle::Full;
    for (size_t k = n_; k > 0; --k) ids_[k] = ids_[k - 1];
    ids_[0] = id;
    ++n_;
    return Toggle::Added;
  }

  // 書搬家：換身分、位置不變；新身分已經在裡面就只留一份。回 true＝有改。
  bool rename(const uint64_t from, const uint64_t to) {
    const int i = find(from);
    if (i < 0 || from == to) return false;
    if (to == 0 || contains(to)) {
      erase(static_cast<size_t>(i));
    } else {
      ids_[i] = to;
    }
    return true;
  }

  bool remove(const uint64_t id) {
    const int i = find(id);
    if (i < 0) return false;
    erase(static_cast<size_t>(i));
    return true;
  }

  // 只留 keep(id) 為真的。回 true＝有拿掉。
  template <typename Keep>
  bool retain(Keep keep) {
    size_t w = 0;
    for (size_t r = 0; r < n_; ++r)
      if (keep(ids_[r])) ids_[w++] = ids_[r];
    const bool changed = w != n_;
    n_ = w;
    return changed;
  }

  // 編碼成 NVS blob；回傳長度（≤ kMaxBlob）
  size_t encode(uint8_t* buf) const {
    buf[0] = kMagic;
    buf[1] = kVersion;
    buf[2] = static_cast<uint8_t>(n_);
    buf[3] = 0;
    for (size_t i = 0; i < n_; ++i)
      for (int b = 0; b < 8; ++b) buf[kHeader + 8 * i + b] = static_cast<uint8_t>(ids_[i] >> (8 * b));
    return kHeader + 8 * n_;
  }

  // 解碼；只有 Compatible 會改 *this（其他結果 *this 不動）
  LoadResult decode(const uint8_t* buf, const size_t len) {
    if (len < kHeader || buf[0] != kMagic || buf[1] == 0) return LoadResult::Corrupt;
    if (buf[1] > kVersion) return LoadResult::Newer;
    const size_t n = buf[2];
    if (n > kMax || buf[3] != 0 || len != kHeader + 8 * n) return LoadResult::Corrupt;
    uint64_t tmp[kMax];
    for (size_t i = 0; i < n; ++i) {
      uint64_t v = 0;
      for (int b = 0; b < 8; ++b) v |= static_cast<uint64_t>(buf[kHeader + 8 * i + b]) << (8 * b);
      if (v == 0) return LoadResult::Corrupt;
      for (size_t k = 0; k < i; ++k)
        if (tmp[k] == v) return LoadResult::Corrupt;  // 我們寫不出重複的
      tmp[i] = v;
    }
    for (size_t i = 0; i < n; ++i) ids_[i] = tmp[i];
    n_ = n;
    return LoadResult::Compatible;
  }

 private:
  int find(const uint64_t id) const {
    for (size_t i = 0; i < n_; ++i)
      if (ids_[i] == id) return static_cast<int>(i);
    return -1;
  }
  void erase(const size_t i) {
    for (size_t k = i; k + 1 < n_; ++k) ids_[k] = ids_[k + 1];
    --n_;
  }

  uint64_t ids_[kMax] = {};
  size_t n_ = 0;
};

}  // namespace favorites
