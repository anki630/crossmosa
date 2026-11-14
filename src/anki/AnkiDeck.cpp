#include "AnkiDeck.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

namespace anki {

namespace {

constexpr size_t kXdkHeaderSize = 128;
constexpr size_t kXdkNameOffset = 30;
constexpr size_t kXdkNameMax = 98;
constexpr uint32_t kMaxSideBytes = 6000;  // 跟 apkg2x3.py 的 MAX_SIDE_BYTES 一致；超過就是壞檔
constexpr size_t kScanRecs = 85;          // 85 × 12 ＝ 1020 bytes 一塊

std::string srsPathFor(const std::string& xdkPath) {
  const size_t dot = xdkPath.rfind('.');
  return (dot == std::string::npos ? xdkPath : xdkPath.substr(0, dot)) + ".srs";
}

bool endsWith(const char* s, const char* suffix) {
  const size_t n = strlen(s);
  const size_t m = strlen(suffix);
  if (n < m) return false;
  for (size_t i = 0; i < m; ++i) {
    char a = s[n - m + i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (a != suffix[i]) return false;
  }
  return true;
}

bool readExact(HalFile& f, void* buf, size_t n) { return f.read(buf, n) == static_cast<int>(n); }

}  // namespace

bool readDeckInfo(const std::string& xdkPath, DeckInfo& out) {
  HalFile f;
  if (!Storage.openFileForRead("ANKI", xdkPath, f)) return false;
  uint8_t h[kXdkHeaderSize];
  if (!readExact(f, h, sizeof(h)) || memcmp(h, "XDK1", 4) != 0) {
    LOG_ERR("ANKI", "Not an xdk file: %s", xdkPath.c_str());
    return false;
  }
  out.xdkPath = xdkPath;
  out.srsPath = srsPathFor(xdkPath);
  out.cardCount = get32(h + 8);
  out.deckId = get32(h + 12);
  out.indexOffset = get32(h + 16);
  out.defaultNewPerDay = get16(h + 24);
  out.defaultMaxReviews = get16(h + 26);
  const size_t nameLen = std::min<size_t>(h[28], kXdkNameMax);
  out.name.assign(reinterpret_cast<const char*>(h + kXdkNameOffset), nameLen);
  if (out.name.empty()) out.name = "Deck";

  out.hasProgress = false;
  HalFile s;
  if (Storage.exists(out.srsPath.c_str()) && Storage.openFileForRead("ANKI", out.srsPath, s)) {
    uint8_t sh[kHeaderSize];
    SrsHeader hdr;
    if (readExact(s, sh, sizeof(sh)) && hdr.decode(sh) && hdr.deckId == out.deckId &&
        hdr.cardCount == out.cardCount) {
      out.progress = hdr;
      out.hasProgress = true;
    }
  }
  return true;
}

std::vector<DeckInfo> listDecks() {
  std::vector<DeckInfo> decks;
  auto dir = Storage.open(kAnkiDir);
  if (!dir || !dir.isDirectory()) return decks;
  dir.rewindDirectory();
  std::vector<std::string> paths;
  paths.reserve(16);
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    if (file.isDirectory() || name[0] == '.' || !endsWith(name, ".xdk")) continue;
    paths.push_back(std::string(kAnkiDir) + "/" + name);
  }
  dir.close();
  std::sort(paths.begin(), paths.end());
  decks.reserve(paths.size());
  for (const auto& p : paths) {
    DeckInfo info;
    if (readDeckInfo(p, info)) decks.push_back(std::move(info));
  }
  return decks;
}

// ---------------------------------------------------------------- DeckSession

bool DeckSession::open(const DeckInfo& info) {
  close();
  info_ = info;
  if (!Storage.openFileForRead("ANKI", info_.xdkPath, xdk_)) return false;

  bool ok = false;
  if (Storage.exists(info_.srsPath.c_str())) {
    srs_ = Storage.open(info_.srsPath.c_str(), O_RDWR);
    uint8_t sh[kHeaderSize];
    if (srs_ && srs_.seek(0) && readExact(srs_, sh, sizeof(sh)) && header_.decode(sh) &&
        header_.deckId == info_.deckId && header_.cardCount == info_.cardCount &&
        srs_.fileSize() >= kHeaderSize + static_cast<size_t>(info_.cardCount) * kRecSize) {
      ok = true;
    } else {
      // 牌組換過（重新轉換了不同的 apkg）：舊進度留著，改名，重建
      LOG_INF("ANKI", "Progress mismatch, rebuilding: %s", info_.srsPath.c_str());
      if (srs_) srs_.close();
      const std::string bak = info_.srsPath + ".bak";
      if (Storage.exists(bak.c_str())) Storage.remove(bak.c_str());
      Storage.rename(info_.srsPath.c_str(), bak.c_str());
    }
  }
  if (!ok && !createProgress()) return false;
  return true;
}

bool DeckSession::createProgress() {
  header_ = SrsHeader{};
  header_.cardCount = info_.cardCount;
  header_.deckId = info_.deckId;
  header_.newPerDay = info_.defaultNewPerDay ? info_.defaultNewPerDay : 20;
  header_.maxReviews = info_.defaultMaxReviews ? info_.defaultMaxReviews : 200;
  header_.newRemaining = info_.cardCount;

  HalFile f;
  if (!Storage.openFileForWrite("ANKI", info_.srsPath, f)) return false;
  uint8_t hdr[kHeaderSize];
  header_.encode(hdr);
  if (f.write(hdr, sizeof(hdr)) != sizeof(hdr)) return false;
  auto zeros = makeUniqueNoThrow<uint8_t[]>(kScanRecs * kRecSize);
  if (!zeros) {
    LOG_ERR("ANKI", "OOM: progress init buffer");
    return false;
  }
  memset(zeros.get(), 0, kScanRecs * kRecSize);
  uint32_t left = info_.cardCount;
  while (left > 0) {
    const uint32_t n = std::min<uint32_t>(left, kScanRecs);
    if (f.write(zeros.get(), n * kRecSize) != n * kRecSize) {
      LOG_ERR("ANKI", "Progress init write failed: %s", info_.srsPath.c_str());
      return false;
    }
    left -= n;
  }
  f.flush();
  f.close();
  srs_ = Storage.open(info_.srsPath.c_str(), O_RDWR);
  LOG_INF("ANKI", "Created progress %s (%u cards)", info_.srsPath.c_str(), static_cast<unsigned>(info_.cardCount));
  return static_cast<bool>(srs_);
}

void DeckSession::close() {
  if (xdk_) xdk_.close();
  if (srs_) {
    srs_.flush();
    srs_.close();
  }
}

bool DeckSession::readCard(const uint32_t index, std::string& front, std::string& back) {
  if (!xdk_ || index >= info_.cardCount) return false;
  uint8_t b4[4];
  if (!xdk_.seek(info_.indexOffset + static_cast<size_t>(index) * 4) || !readExact(xdk_, b4, 4)) return false;
  const uint32_t off = get32(b4);
  if (!xdk_.seek(off) || !readExact(xdk_, b4, 4)) return false;
  const uint16_t fl = get16(b4);
  const uint16_t bl = get16(b4 + 2);
  if (fl > kMaxSideBytes || bl > kMaxSideBytes) {
    LOG_ERR("ANKI", "Card %u too large (%u/%u)", static_cast<unsigned>(index), fl, bl);
    return false;
  }
  front.resize(fl);
  back.resize(bl);
  if (fl && !readExact(xdk_, &front[0], fl)) return false;
  if (bl && !readExact(xdk_, &back[0], bl)) return false;
  return true;
}

bool DeckSession::readRec(const uint32_t index, CardRec& out) {
  if (!srs_ || index >= info_.cardCount) return false;
  uint8_t b[kRecSize];
  if (!srs_.seek(kHeaderSize + static_cast<size_t>(index) * kRecSize) || !readExact(srs_, b, kRecSize)) return false;
  out = decodeRec(b);
  return true;
}

bool DeckSession::writeRec(const uint32_t index, const CardRec& rec) {
  if (!srs_ || index >= info_.cardCount) return false;
  uint8_t b[kRecSize];
  encodeRec(rec, b);
  if (!srs_.seek(kHeaderSize + static_cast<size_t>(index) * kRecSize) || srs_.write(b, kRecSize) != kRecSize) {
    LOG_ERR("ANKI", "writeRec %u failed", static_cast<unsigned>(index));
    return false;
  }
  return true;
}

bool DeckSession::writeHeader() {
  if (!srs_) return false;
  uint8_t b[kHeaderSize];
  header_.encode(b);
  if (!srs_.seek(0) || srs_.write(b, kHeaderSize) != kHeaderSize) {
    LOG_ERR("ANKI", "writeHeader failed");
    return false;
  }
  // 每次評分都 flush：使用者隨時可能直接關機，進度不能停在快取裡
  srs_.flush();
  return true;
}

bool DeckSession::scan(void (*fn)(void* ctx, uint32_t index, const CardRec& rec), void* ctx) {
  if (!srs_) return false;
  auto buf = makeUniqueNoThrow<uint8_t[]>(kScanRecs * kRecSize);
  if (!buf) {
    LOG_ERR("ANKI", "OOM: scan buffer");
    return false;
  }
  if (!srs_.seek(kHeaderSize)) return false;
  uint32_t index = 0;
  while (index < info_.cardCount) {
    const uint32_t n = std::min<uint32_t>(info_.cardCount - index, kScanRecs);
    if (!readExact(srs_, buf.get(), n * kRecSize)) {
      LOG_ERR("ANKI", "scan read failed at %u", static_cast<unsigned>(index));
      return false;
    }
    for (uint32_t i = 0; i < n; ++i) fn(ctx, index + i, decodeRec(buf.get() + i * kRecSize));
    index += n;
  }
  return true;
}

}  // namespace anki
