#pragma once

// 最近閱讀的封面縮圖：沒有就產生（EPUB／XTC），舊比例的自動重產。原本是 HomeActivity::loadRecentCovers，
//   2026-10-07 搬出來給 Formosa Cover 的首頁與書架共用（三種尺寸：240、168、216）。
//   會打 SD、會解 JPEG（每本 1–3 秒）、記憶體不夠時卸載內文字型 —— 只在首頁／書架的 render 裡呼叫。

#include <functional>
#include <vector>

#include "RecentBooksStore.h"

class GfxRenderer;

namespace RecentCoverLoader {

// onChanged：真的產生（或試著產生）了一張縮圖之後呼叫 —— 呼叫端丟掉自己的封面快照、要求重畫。
void ensureThumbs(GfxRenderer& renderer, std::vector<RecentBook>& books, int coverHeight,
                  const std::function<void()>& onChanged);

}  // namespace RecentCoverLoader
