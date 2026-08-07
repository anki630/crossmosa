// warm_identity_test.cpp — v110 glyph prefetch 的身分比對測試。
// 為什麼每個欄位一條:spec 2026-08-07-glyph-prefetch-design.md §6 —— prewarm_ms=0
// 既是成功訊號也是拿錯快取的症狀,身分比對是唯一防線,漏比任何一欄位 = 拿別頁的字。
#include <cstdio>
#include "WarmIdentity.h"

namespace {
int gFailures = 0;
void expectTrue(const char* what, bool v) {
  if (v) std::printf("PASS (%s)\n", what);
  else { std::printf("FAIL (%s)\n", what); gFailures++; }
}
WarmIdentity base() {
  WarmIdentity w;
  w.bookHash = WarmIdentity::fnv1a("/.crossmosa/epub_123456");
  w.spineIndex = 7; w.pageNumber = 42; w.fontId = 0x1234ABCD;
  w.viewportWidth = 528; w.viewportHeight = 754;
  w.lineCompressionBits = WarmIdentity::floatBits(1.2f);
  w.paragraphAlignment = 1; w.imageRendering = 0;
  w.extraParagraphSpacing = false; w.hyphenationEnabled = false;
  w.embeddedStyle = true; w.focusReadingEnabled = false; w.boldBodyText = false;
  w.valid = true;
  return w;
}
}  // namespace

int main() {
  { auto a = base(), b = base(); expectTrue("identical identities match", a.matches(b)); }
  // 逐欄位:任何一欄位不同都必須不相符(14 條獨立斷言,不是一條)
  { auto a = base(), b = base(); b.bookHash ^= 1;            expectTrue("bookHash mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.spineIndex++;             expectTrue("spineIndex mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.pageNumber++;             expectTrue("pageNumber mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.fontId ^= 1;              expectTrue("fontId mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.viewportWidth -= 2;       expectTrue("viewportWidth mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.viewportHeight -= 2;      expectTrue("viewportHeight mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.lineCompressionBits = WarmIdentity::floatBits(0.95f);
                                                             expectTrue("lineCompression mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.paragraphAlignment = 0;   expectTrue("paragraphAlignment mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.imageRendering = 1;       expectTrue("imageRendering mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.extraParagraphSpacing = true; expectTrue("extraParagraphSpacing mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.hyphenationEnabled = true; expectTrue("hyphenation mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.embeddedStyle = false;    expectTrue("embeddedStyle mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.focusReadingEnabled = true; expectTrue("focusReading mismatch rejected", !a.matches(b)); }
  { auto a = base(), b = base(); b.boldBodyText = true;      expectTrue("boldBodyText mismatch rejected", !a.matches(b)); }
  // 未完成的預取絕不被採用:valid 未設(= 中斷後的狀態)即使欄位全同也不相符
  { auto a = base(), b = base(); a.valid = false;            expectTrue("stored-invalid never matches", !a.matches(b)); }
  { auto a = base(), b = base(); b.valid = false;            expectTrue("probe-invalid never matches", !a.matches(b)); }
  { WarmIdentity fresh;                                      expectTrue("default-constructed is invalid", !fresh.matches(base())); }
  // invalidate() 語意(unloadForLowMemory / clearCache / 中止都收斂到這一個入口)
  { auto a = base(); a.invalidate(); expectTrue("invalidate clears valid", !a.matches(base())); }
  // fnv1a 基本性質(身分的 bookHash 來源)
  expectTrue("fnv1a distinguishes paths",
             WarmIdentity::fnv1a("/a/epub_1") != WarmIdentity::fnv1a("/a/epub_2"));
  expectTrue("fnv1a deterministic",
             WarmIdentity::fnv1a("/a/epub_1") == WarmIdentity::fnv1a("/a/epub_1"));
  // floatBits 位元精確(1.2f 與 0.95f 等常數必須可區分;不做 epsilon 比較)
  expectTrue("floatBits exact", WarmIdentity::floatBits(1.2f) != WarmIdentity::floatBits(1.0f));

  std::printf(gFailures ? "warm_identity_test: %d FAILURE(S)\n" : "warm_identity_test: all passed\n", gFailures);
  return gFailures ? 1 : 0;
}
