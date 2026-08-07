#pragma once
#include <cstdarg>

// v53 量測儀器(暫時性,量完即移除)。
//
// 背景:2026-07-26 的效能/穩定性研究從 ELF 直接解出這台的內部堆積其實是【三個永不合併的池】
// (Pool A "RAM" ~143KB、Pool B "Retention RAM" ~116KB、另一小塊),而 ESP.getMaxAllocHeap()
// 回傳的是「各池取大者」——也就是說 v20 以來十幾個版本一直在對一個無法歸因的數字做二分法。
// 在拿到 per-pool 數字之前,任何記憶體佈局改動(framebuffer 移池、IRAM 回收)都無法驗收。
//
// 這台裝置沒有序列埠,所以輸出走 SD 檔:<資料目錄>/diag.log,把卡插電腦回傳即可。
namespace DiagLog {

// v57:儀器改成【預設關閉、靠 SD 上的哨兵檔開啟】。
//
// 為什麼不直接整包刪掉(v53 原本寫「量完即移除」):這台機器沒有序列埠,diag.log 是
// 唯一的遙測管道。全刪之後,下次要查任何實機問題都得先出一版量測韌體、等他用幾天再回傳,
// 一個來回好幾天——而他明確表示未來還要「從 log 來優化」。
//
// 開啟方式:在 **SD 卡的根目錄**放一個空檔 `diag.on`(即 `/diag.on`)後重開機。
// 關閉:刪掉那個檔再重開機。判定只在 begin() 做一次(開機時),之後不再碰 SD。
// (刻意不放資料目錄——那裡有 v36 的遷移判定邏輯,手建目錄放檔會擋住遷移;見 DiagLog.cpp)
// 輸出仍然寫在資料目錄的 diag.log(那是資料,不是開關)。每次 begin() 啟用時會記一行
// BOOT version=…,所以同一個 diag.log 裡不同韌體版本的資料可以靠版號切段。
//
// 關閉時的成本:mem() / line() / dumpPools() 全部在第一行就 return——
// 不開檔、不寫入、【不做 heap_caps_walk】(每 10 頁兩次全堆走訪就是關掉之後省下的主要成本)。
// 呼叫端仍會算出參數(幾個 millis 差值與字串常數),那是可忽略的。
void begin();

// v64(SMB2):在【沒有哨兵檔】的卡上也強制開啟儀器,限定於「開了就是為了被連線」的
// 活動存活期間(目前只有檔案傳輸模式)。
//
// 為什麼需要:哨兵檔的設計(v57)對閱讀是對的——平常一毛錢都不該花。但 SMB2 伺服器
// 的頭號失敗情境正是**第一次接觸**:使用者拿 iPhone 指過來、連不上,而他手上那張卡
// 當然沒有 /diag.on。那一次失敗如果不留任何證據,Task 4-7 為了「這台沒有序列埠」而
// 一路補上的每一行 DiagLog(每個不支援的 info class、每個拒絕的原因)全部等於沒寫。
// 要求使用者「先放個檔再試」則是把診斷的前提押在他預先知道會失敗上。
//
// 為什麼代價可接受:①這個活動一開始就不是在閱讀,SD 本來就在被傳輸打滿,診斷行的
// 開檔/寫入/關檔是同一個數量級的雜訊;②絕大多數 DiagLog 呼叫點都在【拒絕/失敗】
// 路徑上,一次成功的讀寫不寫任何一行,所以穩定傳輸期間是安靜的;③檔案仍受
// MAX_DIAG_BYTES(192KB)上限保護;④離開檔案傳輸模式就關掉(而且那條路徑接著
// silentRestart(),連狀態都不會殘留)。
//
// 打開的那一刻會做兩件事(關掉時都不做):
// ①記一行 `BOOT version=… forced=<reason>` 橫幅。**必要,不是裝飾**:main.cpp 的
//   BOOT 行本身也受 active() 管,所以一份純靠 setForced 產生的 log 會完全沒有版號——
//   而這個專案的工作流程就是「刷 vN、把 diag.log 寄回來」,已經 60 幾版了,寫進同一個
//   只會 append 的檔案裡。沒有版號的 log 幾乎無法判讀。
// (②之前先講清楚成本:SmbServer 的服務失敗路徑會過濾掉「對方正常斷線」——那在
//  libsmb2 眼裡也是一次 service 失敗,而一次乾淨的 session 收尾就會產生一次。
//  真正的失敗照記。每個連線收尾仍有 destructionEvent 的一行(slot 結算),那是
//  Task 4 就有的、有用的,不在這次的成本裡。)
// ②**沒有哨兵檔時**,若 diag.log 已經逼近 MAX_DIAG_BYTES 就先輪替(改名成
//   diag-prev.log)。哨兵檔是使用者自己放的,他知道那個檔在長大;但 setForced 會在
//   【每一台機器、每一次檔案傳輸】都寫入,使用者從頭到尾不知道有這個檔。沒有輪替的話,
//   到了上限整套機制就無聲死掉,而那正好是需要它的時候。上限仍在,只是變成兩代共 2×。
//
// 回傳呼叫前的狀態,方便呼叫端還原(而不是硬寫成 false)。
bool setForced(bool on, const char* reason = "forced");

// 記一次記憶體快照。tag 例:"boot"、"reader-enter"、"opds-fetch-pre"。
void mem(const char* tag);

// 記一行自由格式文字(翻頁分段耗時、字型 prewarm 統計等)。
void line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// v55:傾印各大池的「大型已用區塊」位址與大小。mem() 只說「最大連續塊剩多少」,
// 答不出【是誰卡在池中間】——diag3.log 那顆把 p2 從 115,616 砍到 42,312 的
// ~9,984B 常駐塊,身分至今只是推論(advance table)而非實證。
// 只列 >= minBytes 的已用區塊(上限 16 筆/池),對 total > 64KB 的池各出一行。
void dumpPools(unsigned minBytes, const char* tag);

// v79: is `path` one of the two diagnostic logs (diag.log / diag-prev.log)?
//
// The web file manager asks this to make ONE narrow exception to the
// protected-path rule. v77 changed /download from testing the basename to
// testing the whole path -- which closed a real hole (the basename of
// `/.crossmosa/wifi.json` is the innocent `wifi.json`, and the credentials were
// being served over unauthenticated HTTP) and, in the same stroke, closed the
// only convenient way to get these logs off a device that has no serial port.
// Every diagnosis in this project's SMB work came through that download.
//
// Deliberately an EXACT, case-insensitive, whole-path match against the two
// canonical names -- not a prefix, not a basename, not a pattern. Anything that
// does not match exactly (an 8.3 alias like `/CROSSM~1/diag.log`, a leading
// space, any other file in the data directory) falls through to the ordinary
// refusal. Fail-closed by construction: this can only ever widen access to two
// specific files, and neither contains a credential.
bool isDiagnosticPath(const char* path);

}  // namespace DiagLog
