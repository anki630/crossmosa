#pragma once

// 卡面字型。優先用 SD 卡上的 AnkiCJK 家族（scripts/anki/build_anki_font.py 產生：繁中＋日文＋韓文＋IPA），
// 沒有就退回使用者的閱讀字型。
//
// ⚠️ 任何一個 SdCardFontManager::unloadAll 都會清掉 renderer 上【所有】SD 字型（clearSdCardFonts）。
//    所以載入 AnkiCJK 之前先把閱讀字型整個卸掉，離開時卸掉 AnkiCJK、再用 ensureLoaded 把閱讀字型
//    （含 UI 備援）裝回去 —— 兩套不同時存在，記憶體也只多出卡面字型那一份常駐表。
// 呼叫端必須持 RenderLock（會改 renderer 的字型表）。

#include <SdCardFontManager.h>

#include <cstdint>

class GfxRenderer;

namespace anki {

constexpr const char* kCardFontFamily = "AnkiCJK";
constexpr uint8_t kBodyPt = 14;
constexpr uint8_t kHeadPt = 22;

class CardFonts {
 public:
  ~CardFonts() = default;

  // 回傳 true＝用 AnkiCJK；false＝退回閱讀字型（body()/head() 仍可用）
  bool load(GfxRenderer& renderer);
  void unload(GfxRenderer& renderer);

  int body() const { return body_; }
  int head() const { return head_; }
  bool dedicated() const { return dedicated_; }

  // SD 卡上有沒有 AnkiCJK（牌組清單用來提示）
  static bool available();

 private:
  SdCardFontManager manager_;
  int body_ = 0;
  int head_ = 0;
  bool dedicated_ = false;
  bool loaded_ = false;
};

}  // namespace anki
