#pragma once

// 我的最愛的讀寫（2026-10-08，v367）：一塊 NVS（鍵 "fav"，格式見 FavoriteSet.h），整份寫入。
//   只在首頁／書架期間載入；閱讀器搬書、瀏覽檔案刪書時各讀寫一次。

#include <string>

#include "util/FavoriteSet.h"

namespace Favorites {

// 這本書的身分（同閱讀統計）
uint64_t idOf(const std::string& path);
// 讀進 set（先清空）。Newer＝較新版本寫的：這次唯讀。Corrupt＝從空的重新開始（可以寫）。
favorites::LoadResult load(favorites::FavoriteSet& set);
inline bool writable(const favorites::LoadResult r) { return r != favorites::LoadResult::Newer; }
// 整份寫回。失敗回 false（呼叫端把記憶體裡的改動退回）。
//   info 不是 nullptr：不記 log、把結果填進去，呼叫端放開繪製鎖之後再 logSave（DiagLog 會寫 SD，不放在鎖裡）。
struct SaveInfo {
  bool ok = false;
  uint8_t n = 0;
  uint32_t us = 0;
  int err = 0;
};
bool save(const favorites::FavoriteSet& set, SaveInfo* info);
void logSave(const SaveInfo& info);
// 閱讀器搬書（讀完移到 /Read）之後：在最愛裡就換身分並存檔。
void renameBook(const std::string& from, const std::string& to);
// 瀏覽檔案刪掉一本書之後：在最愛裡就拿掉並存檔。
//   不做「掃描後清掉找不到的」：掃描證明不了完整（SD 讀錯＝看完了、隱藏資料夾、換卡、讀不到
//   CID），清錯＝毀掉使用者的資料； 找不到的身分只佔名額（100 本），不會顯示（codex 複查 v367 第四輪）。
void removeBook(const std::string& path);

}  // namespace Favorites
