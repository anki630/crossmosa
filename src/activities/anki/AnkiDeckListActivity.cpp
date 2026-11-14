#include "AnkiDeckListActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "AnkiReviewActivity.h"
#include "MappedInputManager.h"
#include "anki/AnkiClock.h"
#include "anki/AnkiFonts.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long kLongPressMs = 1000;
constexpr uint16_t kNewValues[] = {0, 5, 10, 15, 20, 30, 50, 100};
constexpr uint16_t kReviewValues[] = {50, 100, 200, 300, 500, 1000, 9999};
constexpr int kNewCount = sizeof(kNewValues) / sizeof(kNewValues[0]);
constexpr int kReviewCount = sizeof(kReviewValues) / sizeof(kReviewValues[0]);
}  // namespace

void AnkiDeckListActivity::onEnter() {
  Activity::onEnter();
  confirmPressSeen_ = false;
  longPressFired_ = false;
  load();
  selected_ = 0;
  for (size_t i = 0; i < decks_.size(); ++i) {
    if (decks_[i].xdkPath == selectPath_) selected_ = static_cast<int>(i);
  }
  requestUpdate();
}

void AnkiDeckListActivity::onExit() {
  Activity::onExit();
  std::vector<anki::DeckInfo>().swap(decks_);
}

void AnkiDeckListActivity::load() {
  decks_ = anki::listDecks();
  uint32_t now = 0;
  clockTrusted_ = anki::trustedLocalNow(now);
  if (!clockTrusted_) {
    // 時鐘不可信：用各牌組最後一次寫入的時間（最新的那個）當「今天」
    for (const auto& d : decks_) {
      if (d.hasProgress && d.progress.lastEpoch > now) now = d.progress.lastEpoch;
    }
  }
  today_ = anki::dayOf(now);
  hasCardFont_ = anki::CardFonts::available();
}

std::string AnkiDeckListActivity::subtitleFor(const anki::DeckInfo& d) const {
  char buf[96];
  if (!d.hasProgress) {
    snprintf(buf, sizeof(buf), tr(STR_ANKI_DECK_UNSTARTED), static_cast<unsigned>(d.cardCount),
             static_cast<unsigned>(d.defaultNewPerDay));
    return buf;
  }
  const auto& p = d.progress;
  const int32_t rev = p.reviewDue(today_);
  if (rev < 0) {
    snprintf(buf, sizeof(buf), tr(STR_ANKI_DECK_COUNTS_NOREV), static_cast<unsigned>(p.newAvailable(today_)),
             static_cast<unsigned>(p.learnCount));
  } else {
    snprintf(buf, sizeof(buf), tr(STR_ANKI_DECK_COUNTS), static_cast<unsigned>(p.newAvailable(today_)),
             static_cast<unsigned>(p.learnCount), static_cast<unsigned>(rev));
  }
  return buf;
}

void AnkiDeckListActivity::openDeck() {
  if (selected_ < 0 || selected_ >= static_cast<int>(decks_.size())) return;
  activityManager.replaceActivity(
      std::make_unique<AnkiReviewActivity>(renderer, mappedInput, decks_[static_cast<size_t>(selected_)]));
}

void AnkiDeckListActivity::openOptions() {
  if (selected_ < 0 || selected_ >= static_cast<int>(decks_.size())) return;
  const auto& d = decks_[static_cast<size_t>(selected_)];
  const uint16_t npd = d.hasProgress ? d.progress.newPerDay : d.defaultNewPerDay;
  const uint16_t mrv = d.hasProgress ? d.progress.maxReviews : d.defaultMaxReviews;
  char a[64];
  char b[64];
  snprintf(a, sizeof(a), "%s：%u", tr(STR_ANKI_NEW_PER_DAY), static_cast<unsigned>(npd));
  snprintf(b, sizeof(b), "%s：%u", tr(STR_ANKI_MAX_REVIEWS), static_cast<unsigned>(mrv));
  const char* options[] = {a, b};
  pendingOption_ = Option::None;
  pickerFor_ = Option::None;
  popup_.show(d.name.c_str(), options, 2, 0, [this](const int idx) {
    pendingOption_ = idx == 0 ? Option::NewPerDay : Option::MaxReviews;
  });
  requestUpdate();
}

void AnkiDeckListActivity::openValuePicker(const Option which) {
  const auto& d = decks_[static_cast<size_t>(selected_)];
  const bool isNew = which == Option::NewPerDay;
  const uint16_t cur = isNew ? (d.hasProgress ? d.progress.newPerDay : d.defaultNewPerDay)
                             : (d.hasProgress ? d.progress.maxReviews : d.defaultMaxReviews);
  const uint16_t* values = isNew ? kNewValues : kReviewValues;
  const int count = isNew ? kNewCount : kReviewCount;
  std::vector<std::string> labels;
  labels.reserve(static_cast<size_t>(count));
  int current = 0;
  for (int i = 0; i < count; ++i) {
    labels.push_back(std::to_string(values[i]));
    if (values[i] == cur) current = i;
  }
  pickerFor_ = which;
  pendingValue_ = -1;
  popup_.show(isNew ? StrId::STR_ANKI_NEW_PER_DAY : StrId::STR_ANKI_MAX_REVIEWS, labels, current,
              [this](const int idx) { pendingValue_ = idx; });
  requestUpdate();
}

void AnkiDeckListActivity::applyValue(const Option which, const uint16_t value) {
  auto& d = decks_[static_cast<size_t>(selected_)];
  anki::DeckSession s;
  if (!s.open(d)) return;  // 沒有進度檔就順便建立
  auto& h = s.header();
  if (which == Option::NewPerDay) {
    if (h.newPerDay == value) return;
    h.newPerDay = value;
  } else {
    if (h.maxReviews == value) return;
    h.maxReviews = value;
  }
  s.writeHeader();
  d.progress = h;
  d.hasProgress = true;
}

void AnkiDeckListActivity::loop() {
  using B = MappedInputManager::Button;
  if (popup_.isActive()) {
    Option openPicker = Option::None;
    Option apply = Option::None;
    int applyIndex = -1;
    {
      RenderLock lock(*this);
      popup_.handleInput(mappedInput, [this] { requestUpdate(); });
      if (!popup_.isActive()) {
        if (pickerFor_ == Option::None && pendingOption_ != Option::None) {
          openPicker = pendingOption_;
        } else if (pickerFor_ != Option::None && pendingValue_ >= 0) {
          apply = pickerFor_;
          applyIndex = pendingValue_;
        }
        pendingOption_ = Option::None;
        pendingValue_ = -1;
        pickerFor_ = Option::None;
        requestUpdate();
      }
    }
    // SD 寫入放在鎖外
    if (apply != Option::None) {
      const uint16_t* values = apply == Option::NewPerDay ? kNewValues : kReviewValues;
      applyValue(apply, values[applyIndex]);
      requestUpdate();
    }
    if (openPicker != Option::None) openValuePicker(openPicker);
    longPressFired_ = true;  // 選單關掉時確認鍵可能還按著：等放開
    return;
  }

  if (longPressFired_) {
    if (!mappedInput.isPressed(B::Confirm)) longPressFired_ = false;
    return;
  }

  if (mappedInput.wasPressed(B::Confirm)) {
    confirmPressSeen_ = true;
    confirmDownAt_ = millis();  // getHeldTime() 是全域的（任何鍵），長按要看確認鍵自己按了多久
  }
  if (confirmPressSeen_ && !decks_.empty() && mappedInput.isPressed(B::Confirm) &&
      millis() - confirmDownAt_ >= kLongPressMs) {
    longPressFired_ = true;
    confirmPressSeen_ = false;
    openOptions();
    return;
  }
  if (mappedInput.wasReleased(B::Confirm) && confirmPressSeen_) {
    confirmPressSeen_ = false;
    openDeck();
    return;
  }
  if (mappedInput.wasReleased(B::Back)) {
    onGoHome();
    return;
  }

  const int n = static_cast<int>(decks_.size());
  if (n == 0) return;
  nav_.onNextRelease([this, n] {
    selected_ = ButtonNavigator::nextIndex(selected_, n);
    requestUpdate();
  });
  nav_.onPreviousRelease([this, n] {
    selected_ = ButtonNavigator::previousIndex(selected_, n);
    requestUpdate();
  });
}

void AnkiDeckListActivity::render(RenderLock&&) {
  // 每次都整頁重畫（兩層選單大小不同，只蓋上去會留下前一層的邊）
  renderer.clearScreen();
  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, W, metrics.headerHeight}, tr(STR_ANKI_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int footerLines = 0;
  if (!clockTrusted_) ++footerLines;
  if (!hasCardFont_) ++footerLines;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int footerH = footerLines * (lineH + 4);
  const int contentHeight = H - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - footerH;

  if (decks_.empty()) {
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_ANKI_NO_DECKS));
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, tr(STR_ANKI_NO_DECKS_HINT),
                                            W - 2 * metrics.contentSidePadding, 6);
    int y = contentTop + 20 + renderer.getLineHeight(UI_12_FONT_ID) + 12;
    for (const auto& ln : lines) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, ln.c_str());
      y += lineH + 2;
    }
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, W, contentHeight}, static_cast<int>(decks_.size()), selected_,
        [this](int i) { return decks_[static_cast<size_t>(i)].name; },
        [this](int i) { return subtitleFor(decks_[static_cast<size_t>(i)]); },
        [](int) { return UIIcon::Book; });
  }

  int fy = H - metrics.buttonHintsHeight - metrics.verticalSpacing - footerH;
  if (!hasCardFont_) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, fy, tr(STR_ANKI_FONT_MISSING));
    fy += lineH + 4;
  }
  if (!clockTrusted_) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, fy, tr(STR_ANKI_CLOCK_UNTRUSTED));
  }

  if (popup_.isActive()) {
    popup_.processRender(renderer, mappedInput);  // 畫選單＋選單自己的按鍵提示，然後送上面板
    return;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), decks_.empty() ? "" : tr(STR_ANKI_STUDY), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
