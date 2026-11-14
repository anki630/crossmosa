#pragma once

// 單字卡：複習畫面。
//
// X3 按鍵（前面四顆由左到右：返回、確認、左、右；側邊上一頁／下一頁）：
//   正面（題目）：確認／左／右／側鍵＝翻面；返回＝離開（進度每張都已寫入）；長按確認＝撤銷上一張
//   背面（答案）：返回＝重來、確認＝困難、左＝良好、右＝簡單（同 Anki 的 1～4）；
//                側鍵＝背面太長時換頁；長按返回＝不評分直接離開
//   沒有卡了：確認／返回＝回牌組清單；長按確認＝撤銷上一張
// 每次評分立刻寫進 .srs（12 bytes＋檔頭），隨時關機都不會掉進度。

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "anki/AnkiDeck.h"
#include "anki/AnkiFonts.h"
#include "anki/AnkiQueue.h"

class AnkiReviewActivity final : public Activity {
 public:
  AnkiReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, anki::DeckInfo deck)
      : Activity("AnkiReview", renderer, mappedInput), deck_(std::move(deck)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Phase : uint8_t { Loading, Question, Answer, Wait, Done, Error };

  struct Row {
    std::string text;
    char style;
    int fontId;
    int height;
  };

  struct Undo {
    bool valid = false;
    uint32_t index = 0;
    anki::Source src = anki::Source::None;
    anki::CardRec before;
    anki::SrsHeader header;
  };

  void startSession();
  bool buildQueue(uint32_t now);
  void nextCard(bool ignoreWait = false);
  void showCard(uint32_t index, anki::Source src);
  void rate(anki::Rating r);
  void undo();
  void exitToList();
  void setPhase(Phase p);
  uint32_t now() const;
  void prepareIntervalLabels();

  // 按鍵：這個階段裡按下過才算放開（避免上一個畫面的放開被接到）；長按各自計時
  bool released(MappedInputManager::Button b);
  bool held(MappedInputManager::Button b, unsigned long ms);

  // render 任務用
  void layout(const std::string& face);
  void drawCard();
  void drawStatus(const char* title, const char* detail);
  int bodyTop() const;
  int bodyBottom() const;

  anki::DeckInfo deck_;
  anki::DeckSession session_;
  anki::StudyQueue queue_;
  anki::CardFonts fonts_;
  Phase phase_ = Phase::Loading;
  bool started_ = false;

  uint32_t curIndex_ = 0;
  anki::Source curSrc_ = anki::Source::None;
  anki::CardRec curRec_;
  std::string front_;
  std::string back_;
  Undo undo_;
  uint32_t waitUntil_ = 0;
  uint32_t reviewedCount_ = 0;
  uint32_t floorEpoch_ = 0;
  unsigned long startMs_ = 0;
  unsigned long lastWaitCheckMs_ = 0;
  char ivlLabel_[4][32] = {};

  // 按鍵狀態
  uint16_t armed_ = 0;
  uint16_t longFired_ = 0;
  unsigned long downAt_[16] = {};

  // 版面（render 任務算；卡片／翻面時主任務在 RenderLock 裡設 layoutDirty_）
  std::vector<Row> rows_;
  std::vector<int> pageStarts_;
  bool layoutDirty_ = true;
  int page_ = 0;
  int pageCount_ = 1;
  int displays_ = 0;
};
