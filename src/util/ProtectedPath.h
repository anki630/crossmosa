#pragma once

// 網路可及的檔案系統介面共用的路徑保護規則（網頁檔案管理與 WebDAV）。
//
// v77/v78 從 WebDAVHandler::isProtectedPath 抽出來成為單一副本 —— 否則每個介面
// 各自手抄一份規則，久了必然漂移。事實上這條規則在 v76/v77/v78 連修四次，
// 每一次都是「同一個判斷的另一個入口沒守到」。
//
// ⚠️ 上游的守衛【只檢查最後一段檔名】：
//     const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
//     if (itemName.startsWith(".")) { 403 }
//   而 WiFi 憑證在 /.crosspoint/wifi.json —— itemName 是 "wifi.json"，不以 . 開頭、
//   也不在 HIDDEN_ITEMS -> 兩道守衛都穿過去，同網段任何人都下載得到。
//   本檔的 isProtected() 走【逐段】比對，/.crosspoint/ 那一段就擋掉了。
//
// 刻意不依賴 Arduino/HAL 型別，方便日後在桌面測試中直接編譯。
#include <cstddef>

namespace ProtectedPath {

// `name` 是【單一路徑片段】（不含 '/'）。true 表示這些介面應該隱藏或拒絕寫入：
// 點開頭的檔案／目錄（例如資料目錄 .crosspoint），或裝置自己的保留目錄名。
bool isProtectedName(const char* name);

// isProtectedName() 涵蓋的一切【扣掉】點開頭那條規則。
//
// v78：只給「要尊重使用者的『顯示隱藏檔』偏好」的【列表】用。這個分離不是講究：
// v77 把網頁檔案管理的列表過濾指向 isProtectedName()，它的點規則讓【每一個】
// 點檔案永久隱形，那個偏好就靜默失效了。
// ⚠️ 存取控制一律用 isProtectedName()/isProtected() —— 列表選擇「顯示」某個東西，
//    從來不代表它可以被讀取。
bool isSystemName(const char* name);

// `path` 可為多段、以 '/' 分隔、開頭斜線可有可無。只要【任何一段】的
// isProtectedName() 為 true 就回傳 true。
// 這才是正確的做法（不是只看最後一段）—— 例如 "/.crosspoint/foo" 與
// "/foo/.crosspoint/bar" 兩者都會被擋。
bool isProtected(const char* path);

}  // namespace ProtectedPath
