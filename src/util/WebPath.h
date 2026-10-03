#pragma once

// v361：網頁檔案管理收到的路徑與名字，一律先過這裡（上游 1.6.5 #3353 的想法，照我們的規則重寫）。
//
// 上游用 FsHelpers::normalisePath()：遇到 ".." 就退一層、退到根目錄就默默停住、"." 原樣保留。
// 但 SdFat 自己【根本不認】"." 與 ".."：FatFile::parsePathName() 會先去掉開頭空白、去掉結尾的點與空白，
// 去完是空的就開檔失敗（FatFileLFN.cpp）。所以「退一層」是上游替 SdFat 發明的語意 —— 我們不發明，
// 而是把 SdFat 開不起來的段直接拒絕，讓守衛（ProtectedPath、診斷檔例外）看到的就是 SdFat 會開的那條路。
//
// ⚠️ WebDAV 仍用 FsHelpers::normalisePath()（會壓回根目錄、不拒絕）。兩套是刻意的：WebDAV 的客戶端是
//    Finder／檔案 app，送來的路徑格式固定；網頁的參數任何人都能手打。要統一之前先看這段。
// ⚠️ 這裡只管「路徑長得對不對」。能不能碰（資料目錄、系統資料夾、8.3 別名）一律由 ProtectedPath 判斷，
//    而且要在 canonicalize() 【之後】判斷。
//
// 刻意不依賴 Arduino 型別，主機測試直接編譯（test/web_path）。
#include <string>
#include <string_view>

namespace WebPath {

// 成功時 out ＝ "/" 或 "/a/b"（開頭一個斜線、結尾沒有、連續斜線合併），各段原樣保留。
// 失敗（回 false、out 不動）：含反斜線或 NUL；或任何一段去掉開頭空白、結尾的點與空白之後是空的
// （"."、".."、"..."、" . " 都算 —— SdFat 開不起來的段）。空字串視為根目錄。
bool canonicalize(std::string_view in, std::string& out);

// 使用者給的【單一名字】（上傳檔名、新資料夾名、改名的新名字）：不能含 '/'、'\\'、NUL，
// 去掉開頭空白、結尾的點與空白之後不能是空的。呼叫端先 trim，再呼叫這個，再查 ProtectedPath。
bool isSafeComponent(std::string_view name);

}  // namespace WebPath
