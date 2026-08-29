#include "ProtectedPath.h"

#include <cstring>
#include <strings.h>  // strncasecmp -- see isProtectedNameView

namespace ProtectedPath {

namespace {
// Same list WebDAVHandler.cpp originally hardcoded as HIDDEN_ITEMS -- kept
// here now as the single copy. The data directory (".crossmosa", or a legacy
// ".crosspoint") is covered by the leading-dot check below, not by name, so it
// doesn't need an entry.
constexpr const char* kHiddenItems[] = {"System Volume Information", "XTCache"};

// v77: does this segment have the shape of a FAT-generated 8.3 alias?
//
// WHY THIS IS HERE. Every protected name below is protected by the name the
// USER sees, but FAT gives each of them a second, machine-generated name and
// SdFat will open either. Traced in the vendored SdFat: makeSFN() strips
// leading dots and marks the name "not 8.3" (FatFileLFN.cpp:156-159), so
// `.crosspoint` lands on the card with a short name like `CROSSM~1`; and the
// open path compares a requested name's computed SFN against the directory
// entry byte-for-byte and opens on a match (FatFileLFN.cpp:352-357). So a
// client asking for `/CROSSM~1` reached the data directory -- Wi-Fi
// credentials, reading progress, settings -- past a check that only ever saw
// the string the client typed. `System Volume Information` has the same second
// name, `SYSTEM~1`.
//
// TWO FORMS, and the first draft of this knew only one. SdFat emits `STEM~1`
// while the name is unique, but on a collision makeUniqueSfn() clamps the stem
// to THREE characters and appends FOUR HEX digits --
// `fname->sfn[i] = h < 10 ? h + '0' : h + 'A' - 10` (FatFileLFN.cpp:236) -- so
// the real second form is `CRO~1A2F`. Review measured a digit-only test against
// all 65,536 collision values: it caught 15.3%. Hence hex.
//
// SHAPE, NOT NAME. Matching `CROSSM~` specifically would be a guess about
// collision order. "stem, tilde, hex digits, no extension" covers every alias
// of every currently protected name without enumerating any. The cost is that
// a real file genuinely named `backup~1` (no extension) becomes invisible over
// the network -- accepted: it stays readable on the device, and the alternative
// is a credential directory reachable by a name a client can guess.
// ⚠️ A DOTTED dotfile would break the assumption (`.diag.on` -> `DIAG~1.ON`).
// Nothing at the SD root has that shape today; this comment is the warning for
// whoever adds the next one.
bool looksLikeShortNameAlias(const char* start, size_t len) {
  const char* tilde = nullptr;
  for (size_t i = 0; i < len; i++) {
    if (start[i] == '.') return false;  // has an extension -- not our shape
    if (start[i] == '~') {
      if (tilde != nullptr) return false;  // only one
      tilde = start + i;
    }
  }
  if (tilde == nullptr) return false;
  const size_t stemLen = static_cast<size_t>(tilde - start);
  // 6 is the max stem SdFat keeps for the `~1` form (seqPos only advances under
  // `if (i < 7)`, FatFileLFN.cpp:196-197); the collision form clamps to 3,
  // which this range already covers.
  if (stemLen == 0 || stemLen > 6) return false;
  const size_t digits = len - stemLen - 1;
  if (digits == 0) return false;
  for (size_t i = 0; i < digits; i++) {
    const char c = tilde[1 + i];
    const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!isHex) return false;
  }
  return true;
}

// v78: the normalisation and every rule EXCEPT the leading dot. Split out so a
// listing that honours "show hidden files" can ask the narrower question -- see
// isSystemName() in the header for why that separation had to become explicit.
bool isSystemNameView(const char* start, size_t len) {
  while (len > 0 && *start == ' ') {
    start++;
    len--;
  }
  while (len > 0 && (start[len - 1] == '.' || start[len - 1] == ' ')) {
    len--;
  }
  if (len == 0) return false;
  if (looksLikeShortNameAlias(start, len)) return true;
  // v76: strncasecmp, not memcmp. FAT is case-insensitive, so "xtcache" and
  // "XTCache" are the SAME directory on the card -- a case-sensitive compare
  // here let a client name the protected directory in another case and walk
  // straight past this check. Measured over SMB: `rmdir xtcache` removed the
  // real XTCache.
  for (const auto* item : kHiddenItems) {
    size_t itemLen = std::strlen(item);
    if (len == itemLen && strncasecmp(start, item, len) == 0) return true;
  }
  return false;
}

// Segment-view variant of isProtectedName: takes a (start, len) pair instead
// of a NUL-terminated string, so isProtected()'s per-segment walk over one
// caller-owned buffer never has to copy a segment out just to compare it.
//
// v77: NORMALISE THE WAY FAT DOES, FIRST. This is the bug the alias work above
// turned out to be only one instance of, and it is the bigger one.
// `FatFile::parsePathName()` skips LEADING SPACES (FatFileLFN.cpp:461-463) and
// trims TRAILING dots and spaces (:487-491, whose own comment reads "Need to
// trim trailing dots spaces") before any name is compared. Our guard saw the
// raw string the client sent. So
//
//     "/ .crosspoint/wifi.json"   <- one leading space
//     "/CROSSM~1./wifi.json"     <- one trailing dot
//     "/XTCache /x"              <- one trailing space
//
// all walked past the leading-dot rule, past the v76 case fix and past the
// alias rule, and SdFat then opened exactly the protected object. No alias
// guessing, no collision -- one space.
//
// Testing the TRIMMED form is strictly stronger than testing the raw one: it is
// the name the filesystem will actually resolve to. On entries that came from
// getName() (real long names read off the card) it is a no-op.
bool isProtectedNameView(const char* start, size_t len) {
  while (len > 0 && *start == ' ') {
    start++;
    len--;
  }
  while (len > 0 && (start[len - 1] == '.' || start[len - 1] == ' ')) {
    len--;
  }
  if (len == 0) return false;
  if (start[0] == '.') return true;
  return isSystemNameView(start, len);
}

}  // namespace

bool isProtectedName(const char* name) {
  if (name == nullptr) return false;
  return isProtectedNameView(name, std::strlen(name));
}

bool isSystemName(const char* name) {
  if (name == nullptr) return false;
  return isSystemNameView(name, std::strlen(name));
}

bool isProtected(const char* path) {
  if (path == nullptr) return false;
  size_t len = std::strlen(path);
  size_t start = 0;
  while (start < len) {
    if (path[start] == '/') {
      start++;
      continue;
    }
    size_t end = start;
    while (end < len && path[end] != '/') end++;
    if (isProtectedNameView(path + start, end - start)) return true;
    start = end + 1;
  }
  return false;
}

}  // namespace ProtectedPath
