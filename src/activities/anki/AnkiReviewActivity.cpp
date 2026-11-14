#include "AnkiReviewActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "AnkiDeckListActivity.h"
#include "MappedInputManager.h"
#include "anki/AnkiClock.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

using B = MappedInputManager::Button;
using anki::LineStyle;

constexpr unsigned long kLongPressMs = 1000;
constexpr int kSidePad = 24;
constexpr int kIndent = 18;
constexpr int kRuleH = 18;
constexpr int kRowGap = 4;
constexpr int kHalfRefreshEvery = 8;  // 每 8 次換畫面做一次半刷新，清殘影
constexpr int kMaxRows = 120;

int buttonBit(const B b) { return 1 << static_cast<int>(b); }

// 下一次到期顯示成「10分」「3天」「2月」「1.5年」
void formatInterval(char* out, const size_t n, const anki::CardRec& before, const anki::CardRec& after,
                    const uint32_t now) {
  if (after.isLearning()) {
    const uint32_t sec = after.due > now ? after.due - now : 0;
    if (sec < 3600) {
      snprintf(out, n, tr(STR_ANKI_IVL_MIN), static_cast<unsigned>((sec + 59) / 60));
    } else {
      snprintf(out, n, tr(STR_ANKI_IVL_HOUR), static_cast<unsigned>((sec + 1799) / 3600));
    }
    return;
  }
  (void)before;
  const unsigned d = after.ivl;
  if (d < 30) {
    snprintf(out, n, tr(STR_ANKI_IVL_DAY), d);
  } else if (d < 365) {
    snprintf(out, n, tr(STR_ANKI_IVL_MONTH), (d + 15) / 30);
  } else {
    const unsigned tenths = (d * 10 + 182) / 365;
    snprintf(out, n, tr(STR_ANKI_IVL_YEAR), tenths / 10, tenths % 10);
  }
}

}  // namespace

// ---------------------------------------------------------------- 生命週期

void AnkiReviewActivity::onEnter() {
  Activity::onEnter();
  phase_ = Phase::Loading;
  started_ = false;
  armed_ = 0;
  longFired_ = 0;
  rows_.reserve(48);
  pageStarts_.reserve(4);
  requestUpdate();
}

void AnkiReviewActivity::onExit() {
  Activity::onExit();  // ActivityManager 呼叫 onExit 時已持 RenderLock（不可再拿）
  session_.close();
  fonts_.unload(renderer);
  std::vector<Row>().swap(rows_);
}

void AnkiReviewActivity::startSession() {
  // 先讓「準備中」畫面上去，再做可能要幾秒的事（第一次打開大牌組要建進度檔）
  requestUpdateAndWait();
  {
    RenderLock lock(*this);
    fonts_.load(renderer);
  }
  if (!session_.open(deck_)) {
    LOG_ERR("ANKI", "Open failed: %s", deck_.xdkPath.c_str());
    RenderLock lock(*this);
    setPhase(Phase::Error);
    requestUpdate();
    return;
  }
  startMs_ = millis();
  uint32_t t = 0;
  floorEpoch_ = session_.header().lastEpoch;
  if (anki::trustedLocalNow(t) && t > floorEpoch_) floorEpoch_ = t;
  if (!buildQueue(now())) {
    RenderLock lock(*this);
    setPhase(Phase::Error);
    requestUpdate();
    return;
  }
  nextCard();
}

uint32_t AnkiReviewActivity::now() const {
  uint32_t t = 0;
  if (anki::trustedLocalNow(t)) return std::max(t, floorEpoch_);
  // 時鐘不可信：上一次寫進度的時間＋這次開機後經過的時間（只會往前走）
  return floorEpoch_ + static_cast<uint32_t>((millis() - startMs_) / 1000);
}

namespace {
struct ScanCtx {
  anki::SrsHeader* header;
  anki::StudyQueue* queue;
};
void scanOne(void* ctx, const uint32_t index, const anki::CardRec& rec) {
  auto* c = static_cast<ScanCtx*>(ctx);
  c->header->add(rec);
  c->queue->offer(index, rec);
}
}  // namespace

bool AnkiReviewActivity::buildQueue(const uint32_t t) {
  auto& h = session_.header();
  const uint32_t today = anki::dayOf(t);
  h.rollDay(today);
  const uint32_t newQuota = h.newPerDay > h.newToday ? h.newPerDay - h.newToday : 0;
  const uint32_t revQuota = h.maxReviews > h.revToday ? h.maxReviews - h.revToday : 0;
  h.beginRecount(today);
  queue_.begin(today, newQuota, revQuota);
  ScanCtx ctx{&h, &queue_};
  const unsigned long t0 = millis();
  if (!session_.scan(&scanOne, &ctx)) return false;
  queue_.finish();
  h.lastEpoch = t;
  session_.writeHeader();
  LOG_INF("ANKI", "Queue %s: new=%u learn=%u review=%u (%lums)", deck_.name.c_str(),
          static_cast<unsigned>(queue_.newLeft()), static_cast<unsigned>(queue_.learnLeft()),
          static_cast<unsigned>(queue_.reviewLeft()), millis() - t0);
  return true;
}

// ---------------------------------------------------------------- 出卡、評分、撤銷

void AnkiReviewActivity::setPhase(const Phase p) {
  phase_ = p;
  armed_ = 0;
  longFired_ = 0;
  page_ = 0;
  layoutDirty_ = true;
}

void AnkiReviewActivity::nextCard(const bool ignoreWait) {
  uint32_t t = now();
  if (anki::dayOf(t) != queue_.today()) {
    if (!buildQueue(t)) {
      RenderLock lock(*this);
      setPhase(Phase::Error);
      requestUpdate();
      return;
    }
  }
  uint32_t index = 0;
  anki::Source src = anki::Source::None;
  uint32_t waitUntil = 0;
  auto pick = queue_.next(t, index, src, waitUntil);
  if (pick == anki::StudyQueue::Pick::Wait && ignoreWait) {
    pick = queue_.next(waitUntil, index, src, waitUntil);  // 使用者說「現在就複習」
  }
  if (pick == anki::StudyQueue::Pick::Card) {
    showCard(index, src);
    return;
  }
  RenderLock lock(*this);
  waitUntil_ = waitUntil;
  lastWaitCheckMs_ = millis();
  setPhase(pick == anki::StudyQueue::Pick::Wait ? Phase::Wait : Phase::Done);
  requestUpdate();
}

void AnkiReviewActivity::showCard(const uint32_t index, const anki::Source src) {
  std::string front;
  std::string back;
  anki::CardRec rec;
  if (!session_.readCard(index, front, back) || !session_.readRec(index, rec)) {
    RenderLock lock(*this);
    setPhase(Phase::Error);
    requestUpdate();
    return;
  }
  RenderLock lock(*this);
  curIndex_ = index;
  curSrc_ = src;
  curRec_ = rec;
  front_.swap(front);
  back_.swap(back);
  prepareIntervalLabels();
  setPhase(Phase::Question);
  requestUpdate();
}

void AnkiReviewActivity::prepareIntervalLabels() {
  const uint32_t t = now();
  const char* names[4] = {tr(STR_ANKI_AGAIN), tr(STR_ANKI_HARD), tr(STR_ANKI_GOOD), tr(STR_ANKI_EASY)};
  for (int i = 0; i < 4; ++i) {
    const auto r = static_cast<anki::Rating>(i + 1);
    const anki::CardRec after = anki::answer(curRec_, r, t, curIndex_);
    char ivl[16];
    formatInterval(ivl, sizeof(ivl), curRec_, after, t);
    snprintf(ivlLabel_[i], sizeof(ivlLabel_[i]), "%s %s", names[i], ivl);
  }
}

void AnkiReviewActivity::rate(const anki::Rating r) {
  const uint32_t t = now();
  const anki::CardRec before = curRec_;
  const anki::CardRec after = anki::answer(before, r, t, curIndex_);
  auto& h = session_.header();

  undo_.valid = true;
  undo_.index = curIndex_;
  undo_.src = curSrc_;
  undo_.before = before;
  undo_.header = h;

  h.account(before, after);
  if (before.state == anki::CardState::New) {
    h.newToday = static_cast<uint16_t>(h.newToday + 1);
  } else if (before.state == anki::CardState::Review) {
    h.revToday = static_cast<uint16_t>(h.revToday + 1);
  }
  h.lastEpoch = t;
  if (!session_.writeRec(curIndex_, after) || !session_.writeHeader()) {
    LOG_ERR("ANKI", "Progress write failed for card %u", static_cast<unsigned>(curIndex_));
  }
  queue_.answered(curIndex_, after);
  ++reviewedCount_;
  nextCard();
}

void AnkiReviewActivity::undo() {
  if (!undo_.valid) return;
  undo_.valid = false;  // 只撤一層
  // 畫面上那張（還沒答）放回它的佇列；被撤銷的那張直接出
  if (phase_ == Phase::Question || phase_ == Phase::Answer) queue_.putBack(curIndex_, curSrc_, curRec_.due);
  queue_.unanswer(undo_.index);
  session_.header() = undo_.header;
  session_.writeRec(undo_.index, undo_.before);
  session_.writeHeader();
  if (reviewedCount_ > 0) --reviewedCount_;
  showCard(undo_.index, undo_.src);
}

void AnkiReviewActivity::exitToList() {
  activityManager.replaceActivity(std::make_unique<AnkiDeckListActivity>(renderer, mappedInput, deck_.xdkPath));
}

// ---------------------------------------------------------------- 按鍵

bool AnkiReviewActivity::released(const B b) {
  const int m = buttonBit(b);
  if (mappedInput.wasPressed(b)) {
    armed_ = static_cast<uint16_t>(armed_ | m);
    downAt_[static_cast<int>(b)] = millis();
  }
  if (!mappedInput.wasReleased(b)) return false;
  const bool wasArmed = (armed_ & m) != 0;
  const bool wasLong = (longFired_ & m) != 0;
  armed_ = static_cast<uint16_t>(armed_ & ~m);
  longFired_ = static_cast<uint16_t>(longFired_ & ~m);
  return wasArmed && !wasLong;
}

bool AnkiReviewActivity::held(const B b, const unsigned long ms) {
  const int m = buttonBit(b);
  if ((armed_ & m) == 0 || (longFired_ & m) != 0 || !mappedInput.isPressed(b)) return false;
  if (millis() - downAt_[static_cast<int>(b)] < ms) return false;
  longFired_ = static_cast<uint16_t>(longFired_ | m);
  return true;
}

void AnkiReviewActivity::loop() {
  if (!started_) {
    started_ = true;
    startSession();
    return;
  }

  switch (phase_) {
    case Phase::Loading:
      return;

    case Phase::Question: {
      // 先收 Confirm 的按下（released 會記時間），再看長按
      const bool confirmUp = released(B::Confirm);
      if (held(B::Confirm, kLongPressMs)) {
        undo();
        return;
      }
      const bool flip =
          confirmUp | released(B::Left) | released(B::Right) | released(B::PageForward) | released(B::PageBack);
      if (flip) {
        RenderLock lock(*this);
        setPhase(Phase::Answer);
        requestUpdate();
        return;
      }
      if (released(B::Back)) exitToList();
      return;
    }

    case Phase::Answer: {
      const bool backUp = released(B::Back);
      if (held(B::Back, kLongPressMs)) {
        exitToList();
        return;
      }
      if (backUp) return rate(anki::Rating::Again);
      if (released(B::Confirm)) return rate(anki::Rating::Hard);
      if (released(B::Left)) return rate(anki::Rating::Good);
      if (released(B::Right)) return rate(anki::Rating::Easy);
      const bool fwd = released(B::PageForward);
      const bool bwd = released(B::PageBack);
      if (fwd || bwd) {
        RenderLock lock(*this);
        const int target = page_ + (fwd ? 1 : -1);
        if (target >= 0 && target < pageCount_) {
          page_ = target;
          requestUpdate();
        }
      }
      return;
    }

    case Phase::Wait:
    case Phase::Done: {
      const bool confirmUp = released(B::Confirm);
      if (held(B::Confirm, kLongPressMs)) {
        undo();
        return;
      }
      if (phase_ == Phase::Wait) {
        if (confirmUp) {
          nextCard(/*ignoreWait=*/true);
          return;
        }
        if (millis() - lastWaitCheckMs_ > 5000) {  // 學習卡到期了就自己出
          lastWaitCheckMs_ = millis();
          if (now() >= waitUntil_) {
            nextCard();
            return;
          }
        }
      } else if (confirmUp) {
        exitToList();
        return;
      }
      if (released(B::Back)) exitToList();
      return;
    }

    case Phase::Error:
      if (released(B::Back) || released(B::Confirm)) exitToList();
      return;
  }
}

// ---------------------------------------------------------------- 畫面

int AnkiReviewActivity::bodyTop() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + renderer.getLineHeight(UI_10_FONT_ID) + 16;
}

int AnkiReviewActivity::bodyBottom() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing -
         renderer.getLineHeight(UI_10_FONT_ID);
}

void AnkiReviewActivity::layout(const std::string& face) {
  rows_.clear();
  pageStarts_.clear();
  const int W = renderer.getScreenWidth();
  const int body = fonts_.body() ? fonts_.body() : UI_12_FONT_ID;
  const int head = fonts_.head() ? fonts_.head() : body;
  // SD 字型：先把整面用到的字寬載進常駐表，量字才不會一個一個去 SD 讀
  renderer.ensureSdCardFontReady(body, face.c_str(), 0x01);
  if (head != body) renderer.ensureSdCardFontReady(head, face.c_str(), 0x01);

  size_t pos = 0;
  while (pos < face.size() && static_cast<int>(rows_.size()) < kMaxRows) {
    size_t end = face.find('\n', pos);
    if (end == std::string::npos) end = face.size();
    if (end > pos) {
      const char style = face[pos];
      const std::string text = face.substr(pos + 1, end - pos - 1);
      if (style == static_cast<char>(LineStyle::Rule)) {
        rows_.push_back({std::string(), style, 0, kRuleH});
      } else {
        int fontId = body;
        int maxW = W - 2 * kSidePad;
        switch (static_cast<LineStyle>(style)) {
          case LineStyle::Head:
            fontId = head;
            break;
          case LineStyle::Tag:
          case LineStyle::Meta:
            fontId = UI_10_FONT_ID;
            break;
          case LineStyle::Trans:
            fontId = UI_12_FONT_ID;
            maxW -= kIndent;
            break;
          case LineStyle::Example:
            maxW -= kIndent;
            break;
          default:
            break;
        }
        const int lh = renderer.getLineHeight(fontId);
        const auto lines = renderer.wrappedText(fontId, text.c_str(), maxW, 12);
        for (size_t i = 0; i < lines.size(); ++i) {
          const bool last = i + 1 == lines.size();
          rows_.push_back({lines[i], style, fontId, lh + (last ? kRowGap : 0)});
        }
      }
    }
    pos = end + 1;
  }

  // 分頁：塞不下就換頁（分隔線不放在頁首）
  const int avail = bodyBottom() - bodyTop();
  int y = 0;
  pageStarts_.push_back(0);
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (y > 0 && y + rows_[i].height > avail) {
      pageStarts_.push_back(static_cast<int>(i));
      y = 0;
    }
    if (y == 0 && rows_[i].style == static_cast<char>(LineStyle::Rule)) continue;
    y += rows_[i].height;
  }
  pageCount_ = static_cast<int>(pageStarts_.size());
  if (page_ >= pageCount_) page_ = pageCount_ - 1;
}

void AnkiReviewActivity::drawCard() {
  const int W = renderer.getScreenWidth();
  const int top = bodyTop();
  const size_t from = static_cast<size_t>(pageStarts_[static_cast<size_t>(page_)]);
  const size_t to =
      page_ + 1 < pageCount_ ? static_cast<size_t>(pageStarts_[static_cast<size_t>(page_ + 1)]) : rows_.size();
  int y = top;
  for (size_t i = from; i < to; ++i) {
    const Row& row = rows_[i];
    if (row.style == static_cast<char>(LineStyle::Rule)) {
      if (y != top) renderer.drawLine(kSidePad, y + row.height / 2, W - kSidePad, y + row.height / 2);
      y += row.height;
      continue;
    }
    const int tw = renderer.getTextWidth(row.fontId, row.text.c_str());
    int x = kSidePad;
    switch (static_cast<LineStyle>(row.style)) {
      case LineStyle::Head:
      case LineStyle::Sub:
      case LineStyle::Tag:
        x = (W - tw) / 2;
        break;
      case LineStyle::Meta:
        x = W - kSidePad - tw;
        break;
      case LineStyle::Example:
      case LineStyle::Trans:
        x = kSidePad + kIndent;
        break;
      default:
        break;
    }
    renderer.drawText(row.fontId, x, y, row.text.c_str());
    y += row.height;
  }
  if (pageCount_ > 1) {
    char pg[16];
    snprintf(pg, sizeof(pg), "%d/%d", page_ + 1, pageCount_);
    const int pw = renderer.getTextWidth(UI_10_FONT_ID, pg);
    renderer.drawText(UI_10_FONT_ID, W - kSidePad - pw, bodyBottom(), pg);
  }
}

void AnkiReviewActivity::drawStatus(const char* title, const char* detail) {
  const int W = renderer.getScreenWidth();
  const int mid = (bodyTop() + bodyBottom()) / 2;
  renderer.drawCenteredText(UI_12_FONT_ID, mid - renderer.getLineHeight(UI_12_FONT_ID), title, true,
                            EpdFontFamily::BOLD);
  if (detail && *detail) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, detail, W - 2 * kSidePad, 4);
    int y = mid + 8;
    for (const auto& ln : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, ln.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID) + 2;
    }
  }
}

void AnkiReviewActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int W = renderer.getScreenWidth();
  renderer.clearScreen();

  // 頂列：牌組名稱＋剩餘張數
  renderer.drawText(UI_10_FONT_ID, kSidePad, metrics.topPadding,
                    renderer.truncatedText(UI_10_FONT_ID, deck_.name.c_str(), W / 2 - kSidePad).c_str());
  if (phase_ != Phase::Loading && phase_ != Phase::Error) {
    char counts[64];
    snprintf(counts, sizeof(counts), tr(STR_ANKI_REMAINING), static_cast<unsigned>(queue_.newLeft()),
             static_cast<unsigned>(queue_.learnLeft()), static_cast<unsigned>(queue_.reviewLeft()));
    const int cw = renderer.getTextWidth(UI_10_FONT_ID, counts);
    renderer.drawText(UI_10_FONT_ID, W - kSidePad - cw, metrics.topPadding, counts);
  }
  const int lineY = metrics.topPadding + renderer.getLineHeight(UI_10_FONT_ID) + 6;
  renderer.drawLine(kSidePad, lineY, W - kSidePad, lineY);

  const char* b1 = "";
  const char* b2 = "";
  const char* b3 = "";
  const char* b4 = "";
  char detail[96];
  switch (phase_) {
    case Phase::Loading:
      drawStatus(tr(STR_ANKI_LOADING), "");
      break;
    case Phase::Error:
      drawStatus(tr(STR_ANKI_OPEN_FAILED), deck_.xdkPath.c_str());
      b1 = tr(STR_BACK);
      break;
    case Phase::Question:
    case Phase::Answer: {
      if (layoutDirty_) {
        layout(phase_ == Phase::Question ? front_ : back_);
        layoutDirty_ = false;
      }
      drawCard();
      if (phase_ == Phase::Question) {
        b1 = tr(STR_EXIT);
        b2 = b3 = b4 = tr(STR_ANKI_SHOW_ANSWER);
      } else {
        b1 = ivlLabel_[0];
        b2 = ivlLabel_[1];
        b3 = ivlLabel_[2];
        b4 = ivlLabel_[3];
      }
      break;
    }
    case Phase::Wait: {
      const uint32_t t = now();
      const unsigned mins = waitUntil_ > t ? static_cast<unsigned>((waitUntil_ - t + 59) / 60) : 0;
      snprintf(detail, sizeof(detail), tr(STR_ANKI_WAIT_DETAIL), mins);
      drawStatus(tr(STR_ANKI_WAIT), detail);
      b1 = tr(STR_EXIT);
      b2 = tr(STR_ANKI_STUDY_NOW);
      break;
    }
    case Phase::Done:
      snprintf(detail, sizeof(detail), tr(STR_ANKI_DONE_DETAIL), static_cast<unsigned>(reviewedCount_));
      drawStatus(tr(STR_ANKI_DONE), detail);
      b1 = tr(STR_EXIT);
      b2 = tr(STR_ANKI_DECKS);
      break;
  }

  // 可以撤銷時在底部提示一行（不佔按鍵）
  if (undo_.valid && phase_ != Phase::Answer && phase_ != Phase::Loading && phase_ != Phase::Error) {
    renderer.drawText(UI_10_FONT_ID, kSidePad, bodyBottom(), tr(STR_ANKI_UNDO_HINT));
  }

  const auto labels = mappedInput.mapLabels(b1, b2, b3, b4);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (++displays_ % kHalfRefreshEvery == 0) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer();
  }
}
