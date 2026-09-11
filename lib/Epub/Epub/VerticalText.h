#pragma once

// 直排（縦書き）的碼位表與幾何常數。
//
// 這份是 `tools/vertical-oracle/vtables.py` 的移植 —— 那支桌面像素預言機在【零刷機】的
// 前提下把每一條都畫成 PNG 驗過。改這裡之前先在預言機上重跑一次。
//
// ⚠️ 禁則【不在這裡】，在 `lib/Utf8/Utf8.h`（`isNoBreakBeforeVertical` /
//    `isNoBreakAfterVertical`），寫成既有橫排禁則的 delta，避免長出第二份實作。
//
// ═══ 借用政策（維護者 2026-09-08 拍板）════════════════════════════════════
//   「要往日本借，不要借中國。」
//   ⚠️ **但句讀位置這一項，日本與中國是同一邊，台灣才是特例。**
//   `。、，` 放字格右上角是 JIS 與 GB **共同的**做法（Unicode 的 FE10–FE16 就是那個字形），
//   而教育部《重訂標點符號手冊》說「占一個字的位置，**居正中**」。
//   → 無差別套用「往日本借」會得到跟借中國一樣的錯誤結果。按項目分：
//       行間／行長度量、禁則等級、縦中横、ruby 放置  → 日本（台灣沒有排版國家標準）
//       標點位置與字形、書名號、專名號              → 台灣（教育部）
// ══════════════════════════════════════════════════════════════════════

#include <cstddef>
#include <cstdint>

namespace vtext {

// ── 幾何 ────────────────────────────────────────────────────────────────
//
// ⭐ 欄距與字距一律從 **em** 推（`U+3000` 全形空格的 advance），**不用 advanceY**。
//    實測五套出貨字型 20pt：advanceY = 42／47／60／60／83，而 em 恆為 41.688 —— 差到 ±100%。
constexpr uint32_t EM_PROBE_CODEPOINT = 0x3000;  // 量 em 用；零墨水、五套皆全形
// ℹ️ em 的**實際量測**在 `VerticalEm.h`（`probeEmFP`）—— 那支需要 GfxRenderer，而
//    **本檔必須維持在桌面編得動**：`tools/vertical-oracle/cross_check` 直接編它，
//    拿韌體的碼位表跟 Python 預言機逐項比對。那是直排三張表唯一的自動守門員。
//    ⚠️ 2026-09-10 我為了放 probeEmFP 在這裡加了 `#include <GfxRenderer.h>`，
//      桌面比對當場編不過。**不要再讓它斷。**

// 欄距三檔，是【獨立的設定項】`readerColumnPitch`（緊／標準／寬）。
// ⚠️ 曾規劃重用既有的 `lineSpacing` 以免新增設定，已放棄：`getReaderLineCompression()`
//    對 SD 字型／NOTOSERIF 回 0.95/1.0/1.1、對 NOTOSANS 回 0.90/0.95/1.0，值域重疊，
//    同一個 1.0 在兩邊是不同檔位 —— 反推必錯。
//
// ⭐ 值由【量化】決定，不是抄來的：528 px 寬的螢幕上欄數是整數，所以欄距是量化的，
//    同一字級只有少數幾個不同結果。實測只有這一組讓三檔落在三個【相鄰】的欄數上：
//      16pt 10/9/8 欄　18pt 9/8/7　20pt 8/7/6　22pt 7/6/5
//    其他候選會塌掉：比照橫排的 (1.37,1.44,1.58) 在 22pt 上前兩者同為 7 欄；
//    (1.40,1.50,1.65) 則是後兩者同為 6 欄。
// 三檔設定 → 欄距係數。⚠️ 這三個值是【量出來的】，不是挑好看的：只有它們能在
// 16/18/20/22 pt 四個字級都給出三個互不相同的欄數（更近的檔位在某些字級會量化成同一欄數，
// 使用者切了沒反應）。上限也是量出來的 —— 1.9em 以上在 22 pt 會把每頁壓到 80 字以下，
// 低於大字版那一輪實機接受的 85 字。
constexpr float COLUMN_PITCH_TIGHT = 1.35f;
constexpr float COLUMN_PITCH_NORMAL = 1.50f;
constexpr float COLUMN_PITCH_WIDE = 1.75f;

// 基線在 em 格頂下方多少。⭐ 這個常數是【算得出來】的，不是視覺猜的：
//   判準是漢字的墨水框要在 em 格裡垂直置中 → cellAscent = em/2 + glyph.top − glyph.height/2。
//   取 103 個常用漢字的統計，σ ≈ 0.5–0.7 px 在 41.69 px 的 em 上 ＝ 1.5%。
//   五套實測：NotoSans .878／NotoSerif .882／Iansui .854／IBMPlex .877／原俠 .882。
//   ⚠️ 單一探針（永／FE33）會互相矛盾 —— 那是 n=1 的問題，不是量不到。
inline float columnPitchForTier(const uint8_t tier) {
  switch (tier) {
    case 0: return COLUMN_PITCH_TIGHT;
    case 2: return COLUMN_PITCH_WIDE;
    default: return COLUMN_PITCH_NORMAL;
  }
}

constexpr float CELL_ASCENT_FACTOR = 0.88f;

// 縦中横一組字的【墨水】寬度上限，以 em 為單位。超過就退回①直立逐字。
//
// 幾何：格心到隔壁欄那一格的近邊 ＝ pitch − em/2，所以不侵入鄰欄的條件是
//       inkW ≤ 2·pitch − em。最窄的一檔 COLUMN_PITCH_TIGHT = 1.35 → **1.70 em**。
//       取最窄那檔的值，三檔欄距就都安全（不必把 pitch 一路傳進 planToken）。
// 實測五套字型 × 四個字級的墨水寬（tools/vertical-oracle）：
//   兩位數 0.93–1.17 em（全數通過）／三位數 1.46–1.77 em
//   → IBMPlexSansTC 的三位數（1.73–1.77）會退回直立逐字，其餘四套通過。
//   clreq「原則上僅應用於二到三位數字」，退回的那一種仍是規範內的①。
constexpr float TATE_CHU_YOKO_MAX_INK_EM = 1.70f;

// 段首縮排（clreq §6.2.1.1 Note：「中文出版品上，段首縮排以**兩個漢字**的空間為標準」）。
// ⚠️ 但書：「每欄字數較少時，視覺上縮排兩字將顯突兀，時有改用縮排一字」——我們一欄 16–17 字，
//    是邊界。實機看起來突兀就改 1.0，有依據。
constexpr float FIRST_LINE_INDENT_EM = 2.0f;

// ⚠️⚠️ 段落已以【全形空格】開頭時，**不要再加縮排**。
//   日本規範規定段首縮排用實體 U+3000 打在內文裡（官方 reset CSS 甚至把 text-indent 寫死 0）。
//   中文書也一樣而且比例更高：實測中文直排書的段落裡
//   **約三分之二以全形空格開頭**，而且不少書是全部段落都這樣。
//   → 兩者相加就變成三格。
inline bool startsWithEmbeddedIndent(const uint32_t firstCp) { return firstCp == 0x3000; }

// ── 句讀：不替換，用橫排字形居正中 ───────────────────────────────────────
//
// 教育部《重訂標點符號手冊》修訂版，**九處明文**：「位置　占一個字的位置，居正中。」
// 而且手冊在需要區分直橫時【會明講】（書名號那條寫「直行標在書名左旁，橫行標在書名之下」），
// 句讀那幾條**沒有區分** ＝ 直橫皆同。
//
// 四份獨立來源一致：①教育部手冊 ②clreq §2.1.2「港台的排版位於字面正中」
// ③字型度量（九個來源字型的 `。、，` 墨水中心一律 x = 0.50em，且 vert feature 都不替換它們）
// ④UAX #50（`。、，！？` = Tu 直立）
//
// ⭐ 第三方實作獨立印證：KOReader/crengine 的繁中壓縮表註解寫著
//    「all TC glyphs are **centered** … better **kept fullwidth to keep this feeling of centering**」。
inline bool isCenteredPunctuation(const uint32_t cp) {
  switch (cp) {
    case 0x3002:  // 。句號
    case 0xFF0C:  // ，逗號
    case 0x3001:  // 、頓號
    case 0xFF1B:  // ；分號
    case 0xFF1A:  // ：冒號   ⚠️ Noto 的 vert 形在右上角 (0.75,+0.51)，我們【不跟】，跟教育部
    case 0xFF1F:  // ？問號
    case 0xFF01:  // ！驚嘆號
    case 0xFF0E:  // ．
    case 0x00B7:  // ·間隔號
    case 0x2027:  // ‧間隔號
      return true;
    default:
      return false;
  }
}

// ── 直排字形替換（24 個碼位）─────────────────────────────────────────────
//
// 只有真的要轉向／改形的才替換：括號類、破折號、刪節號。回傳 0 表示不替換。
// 這些 FE 字形的落點【已編進它自己的 left/top】，替換之後不需要額外位移（實測確認）。
//
// ⚠️ **不是「FE10–FE19 全部有毒」** —— FE10–FE16 是大陸／日式的右上角句讀形（不用），
//    但 FE17（〖）／FE18（〗）／FE19（…）實測墨水中心 cx = 0.50，是正常的直排形，可以用。
//
// ⚠️ 兩個**反向**的陷阱（大陸做法，刻意不做）：
//    ① GB §5.2.4：直排把《》改成左側浪線 `﹏` —— **我們不得自動改寫**。
//       我們只做旋轉，那正是教育部書名號乙式「直行標在書名上下」。
//    ② GB §5.2.3 的引號巢狀對映與台灣**相反**（台灣外層「」內層『』）——
//       日後若替彎引號做轉換，**絕不可照抄**。
inline uint32_t verticalForm(const uint32_t cp) {
  switch (cp) {
    case 0x3016: return 0xFE17;  // 〖
    case 0x3017: return 0xFE18;  // 〗
    case 0x2026: return 0xFE19;  // …刪節號（實測跨格點距 0.98×，無接縫）
    case 0x22EF: return 0xFE19;  // ⋯中線刪節號（v219）。FE19 的相容分解正是 <vertical> 2026，
                                 //   兩者在直排都該是「三點直排」；用現成的直排形比旋轉乾淨，
                                 //   而且 FE19 已由 2026 實測過。中文書相當常見。
    case 0x2025: return 0xFE30;  // ‥
    case 0x2014: return 0xFE31;  // —破折號
    case 0x2013: return 0xFE32;  // –
    case 0xFF08: return 0xFE35;  // （
    case 0xFF09: return 0xFE36;  // ）
    case 0xFF5B: return 0xFE37;  // ｛
    case 0xFF5D: return 0xFE38;  // ｝
    case 0x3014: return 0xFE39;  // 〔
    case 0x3015: return 0xFE3A;  // 〕
    case 0x3010: return 0xFE3B;  // 【
    case 0x3011: return 0xFE3C;  // 】
    case 0x300A: return 0xFE3D;  // 《書名號乙式
    case 0x300B: return 0xFE3E;  // 》
    case 0x3008: return 0xFE3F;  // 〈篇名號
    case 0x3009: return 0xFE40;  // 〉
    case 0x300C: return 0xFE41;  // 「
    case 0x300D: return 0xFE42;  // 」
    case 0x300E: return 0xFE43;  // 『
    case 0x300F: return 0xFE44;  // 』
    case 0xFF3B: return 0xFE47;  // ［
    case 0xFF3D: return 0xFE48;  // ］
    default: return 0;
  }
}

// ── 直排時需要【旋轉 90° 順時針】的符號：UAX #50 的 vo=R／Tr ──────────────
//
// v218 以前這裡只有四個手寫碼位（～ ─ － ―），所以 → ↑ ↓ ⏎ │ ▌ ⤴ ➔ 全部立著不動。
// 使用者回報：「這一頁的最後一行的箭頭看起來怪怪的」（2026-09-08）。
// **修法不是「補上箭頭」，是改成範圍判斷** —— 手寫清單的失敗模式就是永遠少一個。
//
// 這張表由 `tools/uax50-rotate/gen.py`（工作區）從 Unicode 官方資料機械產生：
// `VerticalOrientation.txt` 17.0.0（2025-07-24）。判準：
//
//     vo ∈ {R, Tr}  ∩  General_Category ∈ {P*, S*}（標點與符號），BMP 內
//
// 185 個區間 ＝ 740 bytes flash。**不要手改** —— 改判準就改 gen.py 並重跑 cross_check。
//
// ⚠️ 為什麼要 gc 那一刀：UAX #50 的 `@missing` 是「0000..10FFFF; R」＝
//    **除了明列為直立的以外全部旋轉**，含拉丁／希臘／西里爾**字母與數字**。
//    那些在繁體中文走 clreq §2.1.2 的①直立逐字／②整串旋轉／③縦中横三條路
//    （見 `planToken`），不從這張表判。
//    → 這一刀等於「字母數字歸 clreq，標點符號歸 UAX #50」。
//
// 兩處刻意偏離：
//    ① **注音聲調符號 U+02B0–02FF**（ˊ ˇ ˋ ˙）—— UAX #50 是 R，但它們跟著注音直立。
//    ② `verticalForm()`（有 FE 直排字形）與 `isCenteredPunctuation()`（教育部居正中）
//       擁有的碼位 —— 守衛就在函式開頭。少了它，U+2013／2014 會被轉而不是換成
//       FE32／FE31，、。，；：？！ 會被轉而不是居正中。
//
// ⛔ **曾經有第三處偏離，2026-09-09 已撤銷，不要再繞回來。**
//    我原本讓數學關係運算子（＝ ＜ ＞ ≠ ≈ ≒ ≥ ≦ ∼ −）保持直立，依據是書庫實證
//    （`＝` 在中文書相當常見，抽樣**全部**是行文裡的行內算式）。
//    維護者反駁並拍板：讀者看到直排裡的符號本來就會照直排讀，
//    橫的等號擺在直排裡只會更怪 —— **標準是對的**。
inline bool needsRotationInVertical(const uint32_t cp) {
  if (cp < 0x00A1 || cp > 0xFFEE) return false;
  if (verticalForm(cp) != 0) return false;      // 有直排字形的用字形
  if (isCenteredPunctuation(cp)) return false;  // 句讀居正中
  struct RotateRange {
    uint16_t lo;
    uint16_t hi;
  };
  // 已排序、不重疊 —— 下面是二分搜尋，靠這個成立。
  static constexpr RotateRange kRotate[] = {
      {0x00A1, 0x00A6},
      {0x00A8, 0x00A8},
      {0x00AB, 0x00AC},
      {0x00AF, 0x00B0},
      {0x00B4, 0x00B4},
      {0x00B6, 0x00B6},
      {0x00B8, 0x00B8},
      {0x00BB, 0x00BB},
      {0x00BF, 0x00BF},
      {0x0375, 0x0375},
      {0x037E, 0x037E},
      {0x0384, 0x0385},
      {0x0387, 0x0387},
      {0x03F6, 0x03F6},
      {0x0482, 0x0482},
      {0x055A, 0x055F},
      {0x0589, 0x058A},
      {0x058D, 0x058F},
      {0x05BE, 0x05BE},
      {0x05C0, 0x05C0},
      {0x05C3, 0x05C3},
      {0x05C6, 0x05C6},
      {0x05F3, 0x05F4},
      {0x0606, 0x060F},
      {0x061B, 0x061B},
      {0x061D, 0x061F},
      {0x066A, 0x066D},
      {0x06D4, 0x06D4},
      {0x06DE, 0x06DE},
      {0x06E9, 0x06E9},
      {0x06FD, 0x06FE},
      {0x0700, 0x070D},
      {0x07F6, 0x07F9},
      {0x07FE, 0x07FF},
      {0x0830, 0x083E},
      {0x085E, 0x085E},
      {0x0888, 0x0888},
      {0x0964, 0x0965},
      {0x0970, 0x0970},
      {0x09F2, 0x09F3},
      {0x09FA, 0x09FB},
      {0x09FD, 0x09FD},
      {0x0A76, 0x0A76},
      {0x0AF0, 0x0AF1},
      {0x0B70, 0x0B70},
      {0x0BF3, 0x0BFA},
      {0x0C77, 0x0C77},
      {0x0C7F, 0x0C7F},
      {0x0C84, 0x0C84},
      {0x0D4F, 0x0D4F},
      {0x0D79, 0x0D79},
      {0x0DF4, 0x0DF4},
      {0x0E3F, 0x0E3F},
      {0x0E4F, 0x0E4F},
      {0x0E5A, 0x0E5B},
      {0x0F01, 0x0F17},
      {0x0F1A, 0x0F1F},
      {0x0F34, 0x0F34},
      {0x0F36, 0x0F36},
      {0x0F38, 0x0F38},
      {0x0F3A, 0x0F3D},
      {0x0F85, 0x0F85},
      {0x0FBE, 0x0FC5},
      {0x0FC7, 0x0FCC},
      {0x0FCE, 0x0FDA},
      {0x104A, 0x104F},
      {0x109E, 0x109F},
      {0x10FB, 0x10FB},
      {0x1360, 0x1368},
      {0x1390, 0x1399},
      {0x1400, 0x1400},
      {0x169B, 0x169C},
      {0x16EB, 0x16ED},
      {0x1735, 0x1736},
      {0x17D4, 0x17D6},
      {0x17D8, 0x17DB},
      {0x1800, 0x180A},
      {0x1940, 0x1940},
      {0x1944, 0x1945},
      {0x19DE, 0x19FF},
      {0x1A1E, 0x1A1F},
      {0x1AA0, 0x1AA6},
      {0x1AA8, 0x1AAD},
      {0x1B5A, 0x1B6A},
      {0x1B74, 0x1B7E},
      {0x1BFC, 0x1BFF},
      {0x1C3B, 0x1C3F},
      {0x1C7E, 0x1C7F},
      {0x1CC0, 0x1CC7},
      {0x1CD3, 0x1CD3},
      {0x1FBD, 0x1FBD},
      {0x1FBF, 0x1FC1},
      {0x1FCD, 0x1FCF},
      {0x1FDD, 0x1FDF},
      {0x1FED, 0x1FEF},
      {0x1FFD, 0x1FFE},
      {0x2010, 0x2012},
      {0x2015, 0x2015},
      {0x2017, 0x201F},
      {0x2022, 0x2024},
      {0x2032, 0x203A},
      {0x203D, 0x2041},
      {0x2043, 0x2046},
      {0x204A, 0x2050},
      {0x2052, 0x205E},
      {0x207A, 0x207E},
      {0x208A, 0x208E},
      {0x20A0, 0x20C0},
      {0x2118, 0x2118},
      {0x2140, 0x2144},
      {0x214B, 0x214B},
      {0x218A, 0x218B},
      {0x2190, 0x221D},
      {0x221F, 0x2233},
      {0x2236, 0x22EE},
      {0x22F0, 0x22FF},
      {0x2308, 0x230B},
      {0x2320, 0x2323},
      {0x2329, 0x232A},
      {0x232C, 0x237C},
      {0x239B, 0x23BD},
      {0x23CE, 0x23CE},
      {0x23D0, 0x23D0},
      {0x23DC, 0x23E1},
      {0x2423, 0x2423},
      {0x2500, 0x259F},
      {0x261A, 0x261F},
      {0x2768, 0x2775},
      {0x2794, 0x2B11},
      {0x2B30, 0x2B4F},
      {0x2B5A, 0x2B73},
      {0x2B76, 0x2B95},
      {0x2B98, 0x2BB7},
      {0x2BD2, 0x2BD2},
      {0x2BEC, 0x2BEF},
      {0x2CE5, 0x2CEA},
      {0x2CF9, 0x2CFC},
      {0x2CFE, 0x2CFF},
      {0x2D70, 0x2D70},
      {0x2E00, 0x2E2E},
      {0x2E30, 0x2E4F},
      {0x2E52, 0x2E5D},
      {0x3018, 0x301F},
      {0x3030, 0x3030},
      {0x30A0, 0x30A0},
      {0xA4FE, 0xA4FF},
      {0xA60D, 0xA60F},
      {0xA673, 0xA673},
      {0xA67E, 0xA67E},
      {0xA6F2, 0xA6F7},
      {0xA700, 0xA716},
      {0xA720, 0xA721},
      {0xA789, 0xA78A},
      {0xA828, 0xA82B},
      {0xA836, 0xA839},
      {0xA874, 0xA877},
      {0xA8CE, 0xA8CF},
      {0xA8F8, 0xA8FA},
      {0xA8FC, 0xA8FC},
      {0xA92E, 0xA92F},
      {0xA95F, 0xA95F},
      {0xA9C1, 0xA9CD},
      {0xA9DE, 0xA9DF},
      {0xAA5C, 0xAA5F},
      {0xAA77, 0xAA79},
      {0xAADE, 0xAADF},
      {0xAAF0, 0xAAF1},
      {0xAB5B, 0xAB5B},
      {0xAB6A, 0xAB6B},
      {0xABEB, 0xABEB},
      {0xFB29, 0xFB29},
      {0xFBB2, 0xFBC2},
      {0xFD3E, 0xFD4F},
      {0xFDCF, 0xFDCF},
      {0xFDFC, 0xFDFF},
      {0xFE49, 0xFE4F},
      {0xFE58, 0xFE5E},
      {0xFE63, 0xFE66},
      {0xFF0D, 0xFF0D},
      {0xFF1C, 0xFF1E},
      {0xFF3F, 0xFF3F},
      {0xFF5C, 0xFF5C},
      {0xFF5E, 0xFF65},
      {0xFFE3, 0xFFE3},
      {0xFFE8, 0xFFEE},
  };
  constexpr size_t kCount = sizeof(kRotate) / sizeof(kRotate[0]);
  size_t lo = 0;
  size_t hi = kCount;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (cp < kRotate[mid].lo) {
      hi = mid;
    } else if (cp > kRotate[mid].hi) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

// ── 什麼算「西文」：切段的判準 ────────────────────────────────────────────
//
// ⚠️ **不是「ASCII／非 ASCII」。**（維護者 2026-09-11 實機回報，範例 `Golěm`）
//    原本用位元組是否 < 0x80 切段，於是 `Golěm` 被切成三段、三段各走不同分支：
//      `Gol`        → 有小寫 → ② 整串旋轉（躺著）
//      `ě`（U+011B）→ 非 ASCII → 落到全形那條 → **直立占一個 em 格**
//      `m`          → 單一字母 → ① 直立逐字
//    一個字裡出現三種配置。而**帶變音符號的拉丁字母在中文書裡到處都是**
//    （捷克、越南、德文、法文的人名／地名／術語），不是罕例。
//
// → 判準改成「這個碼位屬於西文書寫系統嗎」：拉丁（含補充、擴充 A/B、越南用的
//   擴充附加）、IPA、組合變音符號、希臘、西里爾。UAX #50 把這些字母全標成 `R`
//   ＝ 整串旋轉，與 clreq §2.1.2 ② 一致。
//
// ⚠️ **只收字母與變音符號，不收標點與符號**（`×` `÷` `«»` `°` 全部留在原路）——
//    標點的直排處置由 `verticalForm`／`isCenteredPunctuation`／
//    `needsRotationInVertical` 三張表決定，併進西文串等於繞過那些規則。
//    ASCII **整段**（含數字與標點）維持原樣，否則縦中横的「1.」會被拆掉。
inline bool isWesternCodepoint(const uint32_t cp) {
  if (cp < 0x80) return true;                                            // ASCII 整段（維持 v218 行為）
  if (cp >= 0x00C0 && cp <= 0x024F) return cp != 0x00D7 && cp != 0x00F7;  // 拉丁補充／擴充 A／B（× ÷ 除外）
  if (cp >= 0x0250 && cp <= 0x02AF) return true;                         // IPA
  if (cp >= 0x0300 && cp <= 0x036F) return true;                         // 組合變音符號
  if (cp >= 0x0370 && cp <= 0x03FF) return true;                         // 希臘
  if (cp >= 0x0400 && cp <= 0x04FF) return true;                         // 西里爾
  if (cp >= 0x1E00 && cp <= 0x1EFF) return true;                         // 拉丁擴充附加（越南）
  if (cp >= 0x1F00 && cp <= 0x1FFF) return true;                         // 希臘擴充
  return false;
}

// 最小 UTF-8 解碼器。**本檔必須在桌面編得動**（cross_check 直接編它），所以不引
// `lib/Utf8` —— 那份會把韌體的相依一路拉進來。語意與 `utf8NextCodepoint` 相同：
// 讀一個碼位、推進指標、回傳 0 表示結束。
inline uint32_t nextCodepoint(const char*& p, const char* const end) {
  if (p >= end) return 0;
  const auto c0 = static_cast<unsigned char>(*p);
  int extra = 0;
  uint32_t cp = c0;
  if (c0 >= 0xF0) {
    extra = 3;
    cp = c0 & 0x07u;
  } else if (c0 >= 0xE0) {
    extra = 2;
    cp = c0 & 0x0Fu;
  } else if (c0 >= 0xC0) {
    extra = 1;
    cp = c0 & 0x1Fu;
  }
  ++p;
  for (int i = 0; i < extra && p < end; ++i, ++p) {
    cp = (cp << 6) | (static_cast<unsigned char>(*p) & 0x3Fu);
  }
  return cp;
}

// ── 直排中的西文與數字：clreq §2.1.2 的【三種】配置方式 ──────────────────
//
// > ① 與漢字採相同的書寫方向，依字母逐個排列，主要用於**單一西文字母或阿拉伯數字，
// >    以及首字母縮略詞**等。
// > ② 文字以順時針方向旋轉 90°，主要用於**西文的單詞、語句**。
// > ③ 保持正常方向橫排處理（縱中橫排）—— **原則上僅應用於二到三位數字**。
//
// ⚠️ 只做②會讓 `AI`／`3C`／`PDF`／`5G` 全部躺著，而它們在中文裡極常見。
// ℹ️ 電書連 EPUB 3 制作ガイド 的製作端與閱讀器端【兩邊都寫 3 位上限】，與 clreq 一致。

// ① 直立逐字排：單一字母／單一數字／2–4 字的首字母縮略詞（無小寫、至少含一個字母）。
//    ⚠️ 純數字的多位數不走這條 —— clreq ① 只說「單一阿拉伯數字」。
//    ⚠️ `len` 是**位元組**數，內容逐**碼位**判斷 —— 2026-09-11 起這一段可能收到
//       非 ASCII 的西文字母（見 isWesternCodepoint）。兩者混用就是 Golěm 那個缺陷。
inline bool isUprightLatinRun(const char* s, const int len) {
  if (len <= 0) return false;
  const char* p = s;
  const char* const end = s + len;
  uint32_t cps[4];
  int n = 0;
  while (p < end) {
    const uint32_t cp = nextCodepoint(p, end);
    if (cp == 0) break;
    if (n >= 4) return false;  // 五個字以上 → ② 整串旋轉
    cps[n++] = cp;
  }
  if (n == 0) return false;
  if (n == 1) {
    const uint32_t cp = cps[0];
    if (cp < 0x80) {
      return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9');
    }
    // 單一西文字母也走 ①。clreq 寫的是「單一西文字母」，沒有限定 ASCII ——
    // 一個孤立的 é／ö 立著排，與孤立的 A 同樣處置。
    return isWesternCodepoint(cp);
  }
  bool hasAlpha = false;
  for (int i = 0; i < n; i++) {
    const uint32_t cp = cps[i];
    if (cp >= 'a' && cp <= 'z') return false;  // 有小寫就不是縮略詞
    if (cp >= 'A' && cp <= 'Z') { hasAlpha = true; continue; }
    if (cp >= '0' && cp <= '9') continue;
    return false;  // 含非 ASCII（Ölü…）→ 不當縮略詞，走 ② 整串旋轉
  }
  return hasAlpha;
}

// ③ 縦中横：兩位數字塞進一個 em 格。
//    ⭐ 幾何實測：兩個數字並排 46.88 px，而 em 只有 41.69 → **塞不進字格，但塞得進欄距**
//       （pitch 1.50 ＝ 62.5 px）。三位數 70.31 px 怎樣都塞不下 → 退回直立逐字。
//    ⚠️ 出版社**已經標好了**：相當比例的中文 EPUB 的 CSS 宣告 `text-combine-upright`
//       且 HTML 真的掛上 class；只做②會把其中**約八成**排錯 —— 章節編號的
//       「10」「15」這類就是典型。
// 縦中横（tate-chu-yoko）：把短的西文數字併進一個 em 格。
// clreq 2.1.2 ③：**兩到三位數**用縦中横；四位以上（例如年份 2026）不用。
//
// ⚠️ 允許結尾帶一個 `.` 或 `:` —— 列點的「1.」「2.」與時刻的「3:」都是這個形狀，
//    而出版社正是用 `<span class="tcy">1.</span>` 明確要求併排（實測**多數**中文 EPUB
//    的 CSS 宣告了 text-combine-*，其中不少真的用在標記上）。
//    ⚠️ 我們目前【沒有】讀那個 class，這條規則是它的近似。真正忠實的作法是聽
//       `text-combine-upright`，但那需要新增一條 per-word 通道（13 個接觸點）。
//       見帳本「縦中横的忠實化」。
inline int tateChuYokoLen(const char* s, const int len) {
  // ⚠️ 逐【位元組】判斷在這裡是安全的：只認 0-9 與結尾的 . : ，任何 ≥ 0x80 的位元組
  //    都會落到最後的 `return 0`（不論 char 有號無號）。縦中横只吃 ASCII。
  if (len < 2 || len > 4) return 0;
  int digits = 0;
  for (int i = 0; i < len; i++) {
    if (s[i] >= '0' && s[i] <= '9') {
      digits++;
      continue;
    }
    // 只容許【最後一個】字元是句點或冒號
    if (i == len - 1 && (s[i] == '.' || s[i] == ':')) continue;
    return 0;
  }
  // ⚠️ **至少一個數字就夠，不是兩個。** clreq 講的是「兩到三位數」，但列點的
  //    「1.」只有一位數字加句點 —— 而那正是出版社用 <span class="tcy"> 標起來的東西
  //    （實機回報）。上限仍守 clreq：數字最多三位，所以年份「2026」不會被併。
  if (digits < 1 || digits > 3) return 0;
  return len;
}

// ── clreq §2.1.2：一段【西文】在直排裡走哪一條 ──────────────────────────
//
// ⚠️ 抽成函式的理由是教訓 `cross-check-must-use-the-production-path`：
//    桌面比對必須呼叫**產品路徑上的這一份**。直排字形表、直排禁則、懸掛表
//    都曾經「表寫了沒人叫」而桌面全綠，這條路已經踩過三次。
enum class WesternRunPlan : uint8_t {
  TateChuYoko,     // ③ 兩到三位數併進一格
  UprightPerChar,  // ① 單一字母／數字／首字母縮略詞，逐字一格直立
  Rotated,         // ② 西文的單詞、語句，整串順時針轉 90°
};

inline WesternRunPlan classifyWesternRun(const char* s, const int len) {
  // ⚠️ 縦中横要排在直立逐字【之前】：數字兩者都符合，先判到哪個就是哪個。
  //    列點的「1.」與「第10章」的「10」都該走 ③。
  if (tateChuYokoLen(s, len) > 0) return WesternRunPlan::TateChuYoko;
  if (isUprightLatinRun(s, len)) return WesternRunPlan::UprightPerChar;
  return WesternRunPlan::Rotated;
}

// 依「西文段／漢字段」把一個 token 切開，對每一段呼叫 fn(起始位元組, 長度)。
//
// ⭐ 存在的理由：中文裡的數字與西文名字幾乎都貼著漢字或全形括號
//    （「第10章」「（Jason」是**一個** token），而 `isWesternToken` 對它們為 false
//    → 整個 token 走全形那條 → **逐碼位一格、字母立著排**。
//    v218 之前縦中横與整串旋轉因此對真實內容從來沒生效過。
// ⚠️ v232 起判準是 `isWesternCodepoint` 而非「位元組 < 0x80」——
//    帶變音符號的字母（Golěm 的 ě）曾經把一個西文字切成三段三種配置。
template <typename F>
inline void forEachWesternSegment(const char* s, const size_t len, F&& fn) {
  if (len == 0) return;
  const char* p = s;
  const char* const end = s + len;
  const char* segStart = s;
  const char* cur = p;
  uint32_t cp = nextCodepoint(p, end);
  if (cp == 0) return;
  bool segWestern = isWesternCodepoint(cp);
  while (cp != 0) {
    const bool western = isWesternCodepoint(cp);
    if (western != segWestern) {
      fn(static_cast<size_t>(segStart - s), static_cast<size_t>(cur - segStart));
      segStart = cur;
      segWestern = western;
    }
    cur = p;
    cp = nextCodepoint(p, end);
  }
  fn(static_cast<size_t>(segStart - s), static_cast<size_t>(end - segStart));
}


// ── 直排西文的座標映射（renderCharImpl<VerticalCW> 與桌面測試共用的【同一份】碼）──
//
// ⚠️ 抽出來不是為了好看，是為了**能被驗證**。留在 renderCharImpl 裡的話，
//    `--gc-sections` 在沒人呼叫時會整段丟掉，而映射寫錯的代價是一輪刷機
//    （只有一台機器、USB 是 eFuse 鎖死的）。教訓 B-22：儀器要先證明自己會被執行。
//    `constexpr` ＋ 呼叫端 `if constexpr` → 編譯期折掉，逐像素熱迴圈零額外成本。
//
// 推導：把水平的一串字繞基線原點【順時針】轉 90°，(dx,dy) → (−dy,dx)。
//   字形像素 (gx,gy) 相對基線原點在 (pen+left+gx, −top+gy)，轉完落在
//     screenX = 原點x + top − gy   （字頂朝【右】）
//     screenY = 原點y + left + gx  （字串往【下】走）
//
// ⚠️ 既有的 Rotated90CW 是【相反】的（字頂朝左、字串往上＝視覺逆時針，服務側鍵標籤）。

struct VerticalCwOrigin {
  int outerBase;  // screenX = outerBase − glyphY
  int innerBase;  // screenY = innerBase + glyphX
};

constexpr VerticalCwOrigin verticalCwOrigin(const int cursorX, const int cursorY, const int left, const int top) {
  return {cursorX + top, cursorY + left};
}

constexpr int verticalCwScreenX(const VerticalCwOrigin o, const int glyphY) { return o.outerBase - glyphY; }
constexpr int verticalCwScreenY(const VerticalCwOrigin o, const int glyphX) { return o.innerBase + glyphX; }

// ── per-word 旗標：借 Style 的第 7 位 ────────────────────────────────────
//
// `EpdFontFamily::Style` 用到 bit 0-6（REGULAR/BOLD/ITALIC/UNDERLINE/STRIKETHROUGH/
// SUP/SUB/RUBY_CONTINUE = 1,2,4,8,16,32,64），**bit 7 是空的**。
// 直排借它標「這個 token 要整串順時針旋轉」（clreq §2.1.2 ②）。
//
// ⭐ 這樣就【不必動 TextBlock 的 arena 格式，也不必動 .bin 序列化】——
//    直排的三個 per-word 值全部塞得進既有欄位：
//      xpos[i]          → 沿欄的位移
//      focusSuffixX[i]  → 跨軸（欄內）位移（focusBoundary 全 0，所以 focus 邏輯不會跑）
//      styles[i] bit 7  → 這個 token 要旋轉
//
// ⚠️ **傳給字型之前必須遮掉**（`style & STYLE_MASK`），否則字型的 style 解析會拿到
//    一個它不認得的位元。橫排從不設這一位，所以橫排零影響。
constexpr uint8_t STYLE_BIT_ROTATED = 0x80;
constexpr uint8_t STYLE_MASK = 0x7F;

}  // namespace vtext
