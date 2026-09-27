#pragma once
// 一行的注音替換清單（P2 設計第 1、3 節；codex 修訂 1、8、11）。
// 住在 TextBlock 那一次 arena 配置的尾端；章節檔裡是序列標籤 4 的 PageLine（Page.cpp）。
//
// 一筆 ＝ 第 word 個字詞的第 cp 個碼位（ZhuyinUtf8 的嚴格解碼；每個碼位都算，包括看不見的格式字元）換成 pua。
// checkSwaps 逐條驗的不變量：
//   - 照 (word, cp) 嚴格遞增（排序、不重複）；
//   - 原字是 BMP 的漢字、pua 是 BMP 私用區 —— 兩者都是 3 位元組 UTF-8 → 替換是「3 位元組換 3 位元組」，字詞長度不變；
//   - pua 屬於那個字（引擎的 outputOwned；引擎要已通過自我測試）。
// 不過就「這一行不換」（破音字不標；v2 字型保證不會標錯），不影響這一行本身。

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace zhuyin {

class ZhuyinData;

struct Swap {
  uint16_t word;
  uint16_t pua;
  uint8_t cp;
  uint8_t zero;  // 補到 6 bytes：arena 裡是陣列，不要有看不見的填充位元組
};
static_assert(sizeof(Swap) == 6, "Swap must pack to 6 bytes");
// arena 尾端用 placement new 逐筆建立（codex 複查 ③ F11）：要能平凡複製、標準佈局、對齊不超過尾端的 4 位元組
static_assert(std::is_trivially_copyable_v<Swap> && std::is_standard_layout_v<Swap> && alignof(Swap) <= 4,
              "Swap must stay a plain 2-aligned record");

// 建一筆（codex 複查 ③ F14）：字詞序號 > 65535 或碼位序號 > 255 → false（不能靜默窄化成別的位置），呼叫端這一筆不標。
inline bool makeSwap(size_t word, size_t cp, uint16_t pua, Swap* out) {
  if (word > 0xFFFF || cp > 0xFF) return false;
  *out = Swap{static_cast<uint16_t>(word), pua, static_cast<uint8_t>(cp), 0};
  return true;
}

// 排版交給 TextBlock 的清單：哪一個世代、給哪一個字型算的（跟登記的引擎不同就不掛，codex 複查 ③ F3、F15）
struct SwapBatch {
  const Swap* list = nullptr;
  uint16_t count = 0;
  uint32_t generation = 0;
  int fontId = 0;
};

// 一頁在章節裡的位置（章節檔每一行的綁定帶著它；codex 複查 ③ 第二輪 F4）：頁面記錄被放錯地方
// （頁索引表壞了、讀到別的磁區）→ 讀音是照原本那個位置的上下文算的 → 位置不同就不換。0xFFFF ＝ 不知道。
struct PagePlace {
  uint16_t spine = 0xFFFF;
  uint16_t page = 0xFFFF;
};

// 章節檔每一行存的綁定（codex 複查 ③ F4、F12）：這份清單是哪個資料集、哪個語意版號、哪個字型、哪一頁算的。
// 載入時必須跟目前的引擎與要載入的位置完全相同 —— 章節檔頭那一格 32 位元的身分只決定「要不要重排」，讀音能不能用由這裡決定。
struct SwapBinding {
  uint64_t dataset = 0;
  uint16_t semantics = 0;
  int32_t fontId = 0;
  PagePlace place;
};
constexpr size_t kSwapBindingBytes = 18;
void encodeBinding(const SwapBinding& b, uint8_t out[kSwapBindingBytes]);

// TextBlock arena 的文字區：字詞 i ＝ text + off[i]，NUL 結尾；最後一個字詞結束在 text[textBytes − 1]
struct LineText {
  const uint16_t* off = nullptr;
  const char* text = nullptr;
  uint16_t words = 0;
  uint16_t textBytes = 0;
};

constexpr uint16_t kMaxLineSwaps = 255;       // 一行最多幾筆（一行的漢字遠少於此）
constexpr size_t kMaxLineArenaBytes = 2048;   // TextBlock arena＋替換清單 ≤ 2 KB，超過就這一行不換（不變量 4）
constexpr size_t kSwapDiskBytes = 5;          // 章節檔裡每一筆：u16 word、u8 cp、u16 pua（小端序）

enum class SwapCheck : uint8_t {
  Ok = 0,
  Empty,
  TooMany,
  Unsorted,      // 沒照 (word, cp) 嚴格遞增
  BadWord,       // 字詞序號超出範圍
  BadCp,         // 碼位序號超出那個字詞
  NotIdeograph,  // 原字不是漢字
  NotPua,        // 輸出不在 BMP 私用區
  NoEngine,      // 沒有已通過自我測試的引擎 → 無從驗歸屬
  NotOwned,      // 輸出不屬於那個字
};

// 第 word 個字詞的第 cp 個碼位；*byteOff ＝ 它在 text 裡的位移。超出範圍 → false。
bool locateSwap(const LineText& line, uint16_t word, uint8_t cp, uint32_t* codepoint, uint16_t* byteOff);

SwapCheck checkSwaps(const LineText& line, const Swap* swaps, uint16_t n, const ZhuyinData& data);

uint16_t crc16(const uint8_t* p, size_t len, uint16_t crc = 0xFFFF);

// 章節檔標籤 4 的 CRC（CRC-16/CCITT-FALSE），照這個順序：綁定（14 B）、筆數、字詞數、專注旗標、文字位元組數、
// 【整個】arena（位移、座標、樣式、文字 —— 這一行任何一個位元組變了都抓得到，codex 複查 ③ F1），再接每一筆的 5 B。
// 寫與讀用同一組函式，順序不會分岔。
uint16_t swapCrcBegin(const uint8_t binding[kSwapBindingBytes], uint16_t n, uint16_t words, uint8_t focusFlag,
                      uint16_t textBytes, const uint8_t* arena, size_t arenaBytes);
void encodeSwap(const Swap& s, uint8_t rec[kSwapDiskBytes]);
Swap decodeSwap(const uint8_t rec[kSwapDiskBytes]);

// 繪製（P2 ④）：把一個字詞的那幾筆替換套上去。word ＝ 那個字詞（len 位元組、不含 NUL），swaps ＝ 它的那幾筆（cp 嚴格遞增）。
// 換好的字串（NUL 結尾、長度不變）寫進 out。任何一筆的位置找不到、原字不是 3 位元組的漢字、pua 不在私用區、
// 或 out 放不下（len + 1 > cap）→ false：呼叫端畫原字（破音字不標，不會標錯）。
bool applyWordSwaps(const char* word, size_t len, const Swap* swaps, uint16_t n, char* out, size_t cap);

// 繪製時的暫存（在堆疊上，每個字詞重用）：有替換的字詞實際上就是一個漢字（可能黏著標點），遠小於此；
// 放不下就畫原字（由版面決定、每次一樣）。
constexpr size_t kSwapWordBuffer = 64;

}  // namespace zhuyin
