#pragma once

// 單字卡的 SD 卡存取：牌組檔 /anki/*.xdk（唯讀，電腦端 scripts/anki/apkg2x3.py 產生）
// 與進度檔 /anki/*.srs（同檔名，韌體第一次打開牌組時建立）。格式見 AnkiSrs.h 與 apkg2x3.py 的說明。
//
// 記憶體：牌組清單只讀每個檔頭（各 128＋256 bytes）；複習時一次只讀一張卡（兩面合計 ≤ 12 KB）。
// 進度檔全掃描（開始複習時建佇列）用 1 KB 的區塊讀，不整份載入。

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

#include "AnkiSrs.h"

namespace anki {

constexpr const char* kAnkiDir = "/anki";

struct DeckInfo {
  std::string xdkPath;
  std::string srsPath;
  std::string name;
  uint32_t cardCount = 0;
  uint32_t deckId = 0;
  uint32_t indexOffset = 0;
  uint16_t defaultNewPerDay = 20;
  uint16_t defaultMaxReviews = 200;
  bool hasProgress = false;  // .srs 存在而且跟這個牌組對得上
  SrsHeader progress;        // hasProgress 才有意義
};

// 讀 .xdk 檔頭（與同名 .srs 的檔頭，如果有）。
bool readDeckInfo(const std::string& xdkPath, DeckInfo& out);

// 列出 /anki/ 下所有 .xdk（依名稱排序）。
std::vector<DeckInfo> listDecks();

// 一個打開中的牌組：卡片內容＋進度。複習畫面持有一個。
class DeckSession {
 public:
  ~DeckSession() { close(); }

  // 打開牌組；進度檔不存在或對不上就建立（新卡全零），舊檔改名成 .srs.bak 留著。
  bool open(const DeckInfo& info);
  void close();

  const DeckInfo& info() const { return info_; }
  SrsHeader& header() { return header_; }

  bool readCard(uint32_t index, std::string& front, std::string& back);
  bool readRec(uint32_t index, CardRec& out);
  bool writeRec(uint32_t index, const CardRec& rec);
  bool writeHeader();

  // 全掃描：每張卡呼叫 fn(ctx, index, rec)。回傳 false 表示讀檔失敗。
  bool scan(void (*fn)(void* ctx, uint32_t index, const CardRec& rec), void* ctx);

 private:
  bool createProgress();

  DeckInfo info_;
  SrsHeader header_;
  HalFile xdk_;
  HalFile srs_;
};

}  // namespace anki
