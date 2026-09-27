#include "ZhuyinIdentity.h"

namespace zhuyin {

namespace {

constexpr uint32_t kFnvBasis = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

uint32_t fnv(uint32_t h, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; i++) {
    h ^= static_cast<uint8_t>(v >> (8 * i));
    h *= kFnvPrime;
  }
  return h;
}

}  // namespace

uint32_t engineIdentity(const uint64_t datasetId, const EngineMode mode) {
  uint32_t h = fnv(kFnvBasis, 0x4E45595Au, 4);  // "ZYEN"
  h = fnv(h, datasetId, 8);
  h = fnv(h, ZHUYIN_SEMANTICS_VERSION, 2);
  h = fnv(h, static_cast<uint8_t>(mode), 1);
  // 模式放在最低兩個位元（Off＝1、On＝2）：同一個資料集的開與關【構造上】就不同、也永遠不是 0（codex 整合複查第二輪）
  return (h & ~3u) | static_cast<uint32_t>(mode);
}

int sectionIdentity(const int fontId, const uint32_t zhuyinIdentity) {
  if (zhuyinIdentity == 0) return fontId;
  uint32_t h = fnv(kFnvBasis, 0x4449595Au, 4);  // "ZYID"
  h = fnv(h, static_cast<uint32_t>(fontId), 4);
  h = fnv(h, zhuyinIdentity, 4);
  // 留下引擎身分的最低兩個位元（模式）：同一個字型的開與關的章節身分【構造上】就不同，也永遠不是 0（0 是「找不到字型」的哨兵）
  h = (h & ~3u) | (zhuyinIdentity & 3u);
  if (h == static_cast<uint32_t>(fontId)) h ^= 4u;  // 跟非注音的同一個字型永遠不同（動第 2 位元，不動模式）
  return static_cast<int>(h);
}

bool sectionIdentityAccepted(const int fileIdentity, const int fontId, const uint32_t current, const uint32_t off,
                             const uint32_t on) {
  if (fileIdentity == sectionIdentity(fontId, current)) return true;
  return current != 0 && current == off && on != 0 && on != off && fileIdentity == sectionIdentity(fontId, on);
}

}  // namespace zhuyin
