// POSIX-backed implementation of the stub_hal/HalStorage.h declared above.
// See that header for why this exists and what it deliberately does not
// reproduce. Behavior mapping (per test-2 brief step 2):
//   open/openFileFor{Read,Write}  -> POSIX open()
//   openNextFile / rewindDirectory -> opendir()/readdir()/rewinddir()
//   isDirectory                    -> stat()'s S_ISDIR
//   fileSize64                     -> stat()'s st_size
//   mkdir/remove/rename/rmdir      -> same-named POSIX calls
// All paths are resolved under a root directory named by the SMBHOST_ROOT
// environment variable (default "./sdroot") -- see test/host/README.md.

#include <cassert>
#include "HalStorage.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <strings.h>  // strcasecmp -- FAT is case-insensitive, see resolveExistingCase()

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>  // gmtime_r, for getModifyDateTime()'s FAT encoding

namespace {

// Root directory all HalStorage paths are resolved under. Read once per
// process; this harness never needs to react to the env var changing
// mid-run.
const std::string& sdRoot() {
  static const std::string root = [] {
    const char* env = std::getenv("SMBHOST_ROOT");
    std::string r = (env && env[0]) ? env : "./sdroot";
    if (r.size() > 1 && r.back() == '/') r.pop_back();  // avoid double slashes below
    return r;
  }();
  return root;
}

// Device paths are always absolute ("/foo/bar" -- the SD card is the root).
// Map that onto a real directory on the host filesystem.
std::string resolvePath(const char* path) {
  std::string p = path ? path : "";
  if (p.empty() || p[0] != '/') p = "/" + p;
  return sdRoot() + p;
}

// True if `resolved` (a path already run through resolvePath()) names the
// share root itself rather than something inside it. resolvePath("/") yields
// sdRoot() + "/", so both spellings have to be accepted.
bool isShareRootPath(const std::string& resolved) {
  const std::string& root = sdRoot();
  if (resolved == root) return true;
  return resolved.size() == root.size() + 1 && resolved.back() == '/' &&
         resolved.compare(0, root.size(), root) == 0;
}

// FAT IS CASE-INSENSITIVE; POSIX IS NOT. Maps a resolved host path onto the
// entry that actually exists in its parent directory when the only difference
// is letter case, so the harness answers the way the card does.
//
// Without this the stub was LOOSER than the device in a way that hid a real
// bug (found in review): a rename of `source.txt` onto `VICTIM.TXT` while
// `victim.txt` existed SUCCEEDED here and left two files that cannot coexist
// on FAT. On device `Storage.exists()` sees the case-variant, so
// ReplaceIfExists=0 must be refused and ReplaceIfExists=1 must remove the
// variant -- neither path was reachable in the harness. It was also an
// internal inconsistency: SmbFileHandlers.cpp's otherHandleOn() already uses
// strcasecmp *because FAT is case-insensitive*, and the very next line relied
// on POSIX case-sensitivity.
//
// ASCII-only folding is exactly right, and for the same reason the SMB
// wildcard matcher folds only ASCII: every byte of a UTF-8 multi-byte
// sequence is >= 0x80 and compares unchanged, so Traditional Chinese names
// match byte-for-byte.
//
// An exact hit short-circuits before any readdir, so the common path costs one
// access() and nothing else.
std::string resolveExistingCase(const std::string& resolved) {
  if (::access(resolved.c_str(), F_OK) == 0) return resolved;

  auto slash = resolved.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return resolved;
  const std::string parent = resolved.substr(0, slash);
  const std::string name = resolved.substr(slash + 1);
  if (name.empty()) return resolved;  // the share root itself

  DIR* dir = ::opendir(parent.c_str());
  if (!dir) return resolved;
  std::string found;
  struct dirent* entry;
  while ((entry = ::readdir(dir)) != nullptr) {
    if (::strcasecmp(entry->d_name, name.c_str()) == 0) {
      found = parent + "/" + entry->d_name;
      break;
    }
  }
  ::closedir(dir);
  return found.empty() ? resolved : found;
}

std::string basenameOf(const std::string& fullPath) {
  auto pos = fullPath.find_last_of('/');
  return pos == std::string::npos ? fullPath : fullPath.substr(pos + 1);
}

bool mkdirRecursive(const std::string& path) {
  if (path.empty() || path == "/") return true;
  auto slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    if (!mkdirRecursive(path.substr(0, slash))) return false;
  }
  if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }
  return false;
}

bool removeRecursive(const std::string& path) {
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return false;
  if (!S_ISDIR(st.st_mode)) return ::remove(path.c_str()) == 0;
  DIR* dir = ::opendir(path.c_str());
  if (!dir) return false;
  bool ok = true;
  struct dirent* entry;
  while ((entry = ::readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    ok = removeRecursive(path + "/" + name) && ok;
  }
  ::closedir(dir);
  return ::rmdir(path.c_str()) == 0 && ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// HalFile::Impl -- the POSIX handle backing a HalFile. Private implementation
// detail (see HalStorage.h's friend declaration); nothing outside this file
// touches it. The device version is presumably backed by SdFat's FsFile
// instead -- that's fine, Impl is not part of the public contract.
class HalFile::Impl {
 public:
  int fd = -1;
  DIR* dir = nullptr;
  std::string path;  // resolved, host-filesystem path
  std::string name;  // basename, for getName()
  bool directory = false;

  ~Impl() {
    if (fd >= 0) ::close(fd);
    if (dir) ::closedir(dir);
  }
};

HalStorage HalStorage::instance;

HalStorage::HalStorage() {}

bool HalStorage::begin() {
  initialized = mkdirRecursive(sdRoot());
  return initialized;
}

bool HalStorage::ready() const { return initialized; }

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  std::vector<String> out;
  DIR* dir = ::opendir(resolvePath(path).c_str());
  if (!dir) return out;
  struct dirent* entry;
  while (static_cast<int>(out.size()) < maxFiles && (entry = ::readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    out.emplace_back(name.c_str());
  }
  ::closedir(dir);
  return out;
}

String HalStorage::readFile(const char* path) {
  FILE* f = std::fopen(resolvePath(path).c_str(), "rb");
  if (!f) return String();
  std::string content;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
  std::fclose(f);
  return String(content);
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  FILE* f = std::fopen(resolvePath(path).c_str(), "rb");
  if (!f) return false;
  std::vector<uint8_t> buf(chunkSize > 0 ? chunkSize : 256);
  size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    out.write(buf.data(), n);
  }
  bool ok = std::feof(f) != 0;
  std::fclose(f);
  return ok;
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  if (!buffer || bufferSize == 0) return 0;
  FILE* f = std::fopen(resolvePath(path).c_str(), "rb");
  if (!f) return 0;
  size_t want = bufferSize - 1;
  if (maxBytes > 0 && maxBytes < want) want = maxBytes;
  size_t n = std::fread(buffer, 1, want, f);
  buffer[n] = '\0';
  std::fclose(f);
  return n;
}

bool HalStorage::writeFile(const char* path, const String& content) {
  FILE* f = std::fopen(resolvePath(path).c_str(), "wb");
  if (!f) return false;
  size_t len = content.length();
  bool ok = (len == 0) || (std::fwrite(content.c_str(), 1, len, f) == len);
  std::fclose(f);
  return ok;
}

bool HalStorage::ensureDirectoryExists(const char* path) { return mkdirRecursive(resolvePath(path)); }

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  // Case-folded like FAT: an existing entry is found whatever case the client
  // spelled it in. A path that does not exist comes back unchanged, so an
  // O_CREAT still creates with exactly the name that was asked for.
  std::string full = resolveExistingCase(resolvePath(path));
  struct stat st;
  auto impl = std::make_unique<HalFile::Impl>();
  impl->path = full;
  impl->name = basenameOf(full);
  if (::stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    // Model SdFat's own refusal instead of ignoring oflag for directories.
    // FatFile::open() sets FILE_FLAG_WRITE for O_WRONLY/O_RDWR and then fails
    // outright when the target isSubDir() or isReadOnly()
    // (.pio/libdeps/default/SdFat/src/FatLib/FatFile.cpp:581-585). The share
    // ROOT is the one genuine exception: a path of just "/" short-circuits to
    // FatFile::openRoot() (FatFile.cpp:456-461), which never inspects oflag
    // and always yields a read-only handle -- so reproducing "root always
    // opens" is faithful, not a loophole.
    //
    // This matters more than it looks. Until this was added the stub ignored
    // oflag for every directory, so a write-mode subdirectory open succeeded
    // here and failed on the X3 -- a harness that hid the one device-only
    // failure it exists to catch. (The read-only-attribute half of SdFat's
    // guard needs no special code: POSIX open(O_RDWR) on a mode-0444 file
    // already fails with EACCES.)
    const bool writeMode = (oflag & O_ACCMODE) != O_RDONLY;
    if (writeMode && !isShareRootPath(full)) return HalFile();
    impl->dir = ::opendir(full.c_str());
    if (!impl->dir) return HalFile();
    impl->directory = true;
    return HalFile(std::move(impl));
  }

  // Two more rules POSIX does NOT share with SdFat, and where POSIX is the
  // LOOSER of the two -- so without these the harness would again pass
  // something the X3 rejects:
  //
  //   1. O_CREAT only creates in write mode. SdFat:
  //      `if (!(oflag & O_CREAT) || !isWriteMode(oflag)) goto fail;`
  //      (FatFileLFN.cpp:372-373, FatFileSFN.cpp:99-100; ExFatFile.cpp:432-433
  //      is the same test). POSIX happily creates a file with
  //      O_RDONLY|O_CREAT, so a read-mode FILE_OPEN_IF against an absent path
  //      would succeed here and fail on device.
  //   2. O_TRUNC requires write mode. SdFat rejects O_RDONLY|O_TRUNC outright
  //      in openCachedEntry (FatFile.cpp:552-556); ExFatFile.cpp:407-412 has
  //      an explicit `if (!(m_flags & FILE_FLAG_WRITE)) goto fail`.
  //
  // createCmd is built so neither combination is ever generated. These are
  // here so that if that ever stops being true, the harness says so instead
  // of quietly covering for it.
  const bool writeMode = (oflag & O_ACCMODE) != O_RDONLY;
  const bool targetExists = ::stat(full.c_str(), &st) == 0;
  if (!writeMode && (oflag & O_CREAT) && !targetExists) return HalFile();
  if (!writeMode && (oflag & O_TRUNC)) return HalFile();

  int fd = ::open(full.c_str(), oflag, 0644);
  if (fd < 0) return HalFile();
  if (oflag & O_AT_END) ::lseek(fd, 0, SEEK_END);  // SdFat's "open at EOF" semantics
  impl->fd = fd;
  return HalFile(std::move(impl));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
  std::string full = resolvePath(path);
  if (pFlag) return mkdirRecursive(full);
  return ::mkdir(full.c_str(), 0755) == 0;
}

bool HalStorage::exists(const char* path) {
  struct stat st;
  return ::stat(resolveExistingCase(resolvePath(path)).c_str(), &st) == 0;
}

bool HalStorage::remove(const char* path) { return ::remove(resolveExistingCase(resolvePath(path)).c_str()) == 0; }

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  // Same create-exclusive rule as HalFile::rename() below -- FatVolume::rename()
  // is `file.open(vwd(), oldPath, O_RDONLY) && file.rename(vwd(), newPath)`
  // (FatVolume.h:199-201), i.e. the same create-exclusive FatFile::rename() --
  // and the destination check is case-insensitive because FAT is.
  const std::string src = resolveExistingCase(resolvePath(oldPath));
  const std::string dst = resolvePath(newPath);
  struct stat st;
  if (::stat(resolveExistingCase(dst).c_str(), &st) == 0) return false;
  return ::rename(src.c_str(), dst.c_str()) == 0;
}

bool HalStorage::rmdir(const char* path) { return ::rmdir(resolveExistingCase(resolvePath(path)).c_str()) == 0; }

bool HalStorage::openFileForRead(const char* /*moduleName*/, const char* path, HalFile& file) {
  file = open(path, O_RDONLY);
  return file.isOpen();
}
bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char* /*moduleName*/, const char* path, HalFile& file) {
  file = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  return file.isOpen();
}
bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { return removeRecursive(resolvePath(path)); }

// ---------------------------------------------------------------------------
// HalFile

HalFile::HalFile() : impl(nullptr) {}
HalFile::HalFile(std::unique_ptr<Impl> i) : impl(std::move(i)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

void HalFile::flush() {
  if (impl && impl->fd >= 0) ::fsync(impl->fd);
}

// Marker in a filename that makes sync() below report failure. HARNESS-ONLY
// FAULT INJECTION, and the only way this path can be reached at all: the
// device case it models is "the SD card failed while writing back", and POSIX
// fsync() on a healthy scratch file on a local filesystem simply does not
// fail, so without an injector the new failure branch in flushCmd()/writeCmd()
// would be present but never executed by any test.
//
// A filename marker rather than an environment variable because the harness
// server is started as a separate process, usually before the test script, so
// the test cannot influence its environment -- but it can choose what it names
// the file it asks the server to open.
constexpr char kSyncFailMarker[] = "SYNCFAIL";

bool HalFile::sync() {
  if (!impl) return false;
  if (impl->name.find(kSyncFailMarker) != std::string::npos) return false;
  // A DIRECTORY handle must SUCCEED, not fail. SdFat's FatFile::sync() works
  // on directories -- it writes the directory entry back when dirty and
  // returns true otherwise (`if (!isOpen()) return true;` then a
  // FILE_FLAG_DIR_DIRTY branch that only skips the file-size field for
  // non-files, FatLib/FatFile.cpp:1231-1261) -- and MS-SMB2 3.3.5.11 makes
  // FLUSH valid on any open, which clients use. Here a directory is an
  // opendir() with no descriptor, so an fd-only implementation would report
  // failure for every directory flush: a harness-invented failure the device
  // does not have. (Caught by test_flush the moment sync() replaced flush().)
  if (impl->fd < 0) return impl->dir != nullptr;
  return ::fsync(impl->fd) == 0;
}

bool HalFile::getModifyDateTime(uint16_t* date, uint16_t* time) {
  if (!impl) return false;

  // THE SHARE ROOT IS DELIBERATELY NOT SPECIAL-CASED, and that is the faithful
  // choice even though the mechanism differs. On device the FAT root directory
  // has no directory entry at all: FatFile::openRoot() memsets the object
  // (FatFile.cpp:697-724) so m_dirSector stays 0, and getModifyDateTime() then
  // returns TRUE having decoded sector 0 -- the boot sector -- as though it
  // were a directory entry. The observable shape is "succeeds, with a date
  // that means nothing", and returning the host root's own mtime here has
  // exactly that shape. Refusing instead would be tidier but would make the
  // handler's skip-the-root guard untestable: the guard would be the only
  // thing standing between a client and a meaningless date, and nothing would
  // notice if it were deleted. test_share_root_reports_no_timestamp() in
  // smb_smoke_test.py fails if it is.
  struct stat st;
  if (::stat(impl->path.c_str(), &st) != 0) return false;

  // gmtime, NOT localtime. FAT's fields carry no zone, and
  // src/util/FatTimestamp.h reads them back as UTC, so the round trip is only
  // lossless if they are written as UTC. Using localtime here would make the
  // harness's own timestamp assertions pass or fail depending on the machine's
  // TZ setting -- a test that is a function of the environment is not a test.
  struct tm tmv;
  if (::gmtime_r(&st.st_mtime, &tmv) == nullptr) return false;

  // FAT cannot represent anything outside 1980-2107 (7 bits of year offset).
  // A host file dated outside that -- an epoch-0 file, say -- has no encoding,
  // so this reports failure rather than wrapping it into a wrong year.
  const int year = tmv.tm_year + 1900;
  if (year < 1980 || year > 2107) return false;

  *date = static_cast<uint16_t>(((year - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
  // tm_sec can be 60 on a leap second; /2 would give 30, which does not fit
  // the 5-bit field. Clamped to 58 (the same two-second truncation FAT applies
  // to every other value) rather than allowed to corrupt the minute field.
  const int sec = tmv.tm_sec > 58 ? 58 : tmv.tm_sec;
  *time = static_cast<uint16_t>((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (sec / 2));
  return true;
}

bool HalFile::setTimestamp(uint8_t flags, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                           uint8_t second) {
  if (!impl) return false;

  // THE SHARE ROOT IS REFUSED, because SdFat refuses it. FatFile::timestamp()
  // opens with `if (!isFileOrSubDir() || ...) goto fail;`
  // (FatLib/FatFile.cpp:1280), and the FAT root directory is NEITHER: it
  // carries FILE_ATTR_ROOT_FIXED / FILE_ATTR_ROOT32 and never
  // FILE_ATTR_SUBDIR (FatFile.h:454-460, :1014-1022). POSIX has no such notion
  // -- utimensat() on the root of the tree works fine -- so an unguarded stub
  // ACCEPTS a stamp the X3 rejects. Same class as the getter's share-root row
  // in README.md's divergence table, which this is the setter counterpart of;
  // found in review after the first version of this function reproduced
  // SdFat's calendar validation but not its object-kind guard.
  if (isShareRootPath(impl->path)) return false;

  // Reproduce SdFat's own validation, field for field
  // (FatLib/FatFile.cpp:1278-1283) -- including the 2099 upper bound, which is
  // NARROWER than the 2107 the packed date can hold, and the per-month day
  // check. Without these the harness would accept a 2100 stamp that the device
  // refuses.
  static constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year < 1980 || year > 2099 || month < 1 || month > 12) return false;
  unsigned maxDay = kDays[month - 1];
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) maxDay = 29;
  if (day < 1 || day > maxDay || hour > 23 || minute > 59 || second > 59) return false;

  // FAT stores three stamps; POSIX exposes two through utimes(). The mapping
  // is therefore lossy in a way the device is not, and it is chosen to keep
  // the harness's OBSERVABLE behaviour identical for what the tests read back:
  // getModifyDateTime() is the only reader, and it reads the modify stamp,
  // which is st_mtime. T_CREATE has no POSIX equivalent at all (birthtime is
  // not settable), so it is accepted and dropped -- a divergence that cannot
  // affect any assertion, because nothing in this harness can read a creation
  // time back. Recorded in README.md's divergence table all the same.
  struct stat st;
  if (::stat(impl->path.c_str(), &st) != 0) return false;

  struct tm tmv {};
  tmv.tm_year = static_cast<int>(year) - 1900;
  tmv.tm_mon = static_cast<int>(month) - 1;
  tmv.tm_mday = static_cast<int>(day);
  tmv.tm_hour = static_cast<int>(hour);
  tmv.tm_min = static_cast<int>(minute);
  tmv.tm_sec = static_cast<int>(second);
  // timegm, not mktime: getModifyDateTime() reads back with gmtime_r, so the
  // round trip is only lossless if both ends agree the fields are UTC. Using
  // mktime here would make every timestamp assertion a function of the
  // machine's TZ.
  const time_t when = ::timegm(&tmv);
  if (when == static_cast<time_t>(-1)) return false;

  struct timespec times[2];
  times[0].tv_sec = (flags & T_ACCESS) ? when : st.st_atime;
  times[0].tv_nsec = 0;
  times[1].tv_sec = (flags & T_WRITE) ? when : st.st_mtime;
  times[1].tv_nsec = 0;
  return ::utimensat(AT_FDCWD, impl->path.c_str(), times, 0) == 0;
}

bool HalFile::truncate(uint64_t length) {
  if (!impl || impl->fd < 0) return false;

  // A FIFTH rule where POSIX is LOOSER than SdFat: ftruncate() GROWS a file
  // (zero-filling), SdFat cannot grow one at all. Both SdFat implementations
  // are literally
  //     bool truncate(uint64_t length) { return seekSet(length) && truncate(); }
  // (FatLib/FatFile.h:957, ExFatLib/ExFatFile.h:786), and seekSet() `goto
  // fail`s for any position past end-of-file (FatFile.cpp:1184-1188,
  // ExFatFile.cpp:715-719) -- the same guard this stub already models in
  // seek64(). Without this check the harness would certify a
  // FILE_END_OF_FILE_INFORMATION that extends a file, which the X3 refuses
  // outright, and set_info_cmd's explicit zero-fill path would never run here.
  struct stat st;
  if (::fstat(impl->fd, &st) != 0) return false;
  if (length > static_cast<uint64_t>(st.st_size)) return false;

  // FAT's file size field is 32-bit; FsFile::truncate() enforces that for the
  // FAT case (`length < (1ULL << 32) && ...`, FsLib/FsFile.h:869).
  if (length >= (1ULL << 32)) return false;

  if (::ftruncate(impl->fd, static_cast<off_t>(length)) != 0) return false;
  // SIDE EFFECT, reproduced deliberately: SdFat leaves the file position at
  // the new end of file (seekSet moved it there, then truncate() sets
  // m_fileSize = m_curPosition). POSIX ftruncate() does not move the position
  // at all, so without this the harness would hide a device-only behaviour
  // from any caller that reads or writes without seeking first.
  ::lseek(impl->fd, static_cast<off_t>(length), SEEK_SET);
  return true;
}

size_t HalFile::getName(char* name, size_t len) {
  if (!impl || !name || len == 0) return 0;
  const size_t n = impl->name.size();
  // FAIL rather than truncate when the buffer is short -- SdFat's getName8()
  // does exactly that (`if (str + 4 > end) goto fail;` ... `fail: name[0] =
  // '\0'; return 0;`, .pio/libdeps/gh_release/SdFat/src/FatLib/FatName.cpp:
  // 99-160), and getName7() the same. This stub used to truncate instead,
  // which would have silently handed the SMB directory-listing code a
  // half-name where the device hands it an empty one -- the harness inventing
  // a passing case for a device-only failure, exactly the divergence class
  // this stub exists to avoid.
  if (n >= len) {
    name[0] = '\0';
    return 0;
  }
  std::memcpy(name, impl->name.c_str(), n);
  name[n] = '\0';
  return n;
}

uint64_t HalFile::fileSize64() {
  if (!impl) return 0;
  struct stat st;
  if (::stat(impl->path.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.st_size);
}

size_t HalFile::size() { return static_cast<size_t>(fileSize64()); }
size_t HalFile::fileSize() { return size(); }

bool HalFile::seek64(uint64_t pos) {
  if (!impl || impl->fd < 0) return false;
  // A third rule where POSIX is LOOSER than SdFat, added for Task 6.
  //
  // SdFat REFUSES to seek past the end of a file: FatFile::seekSet() does
  // `if (isFile()) { if (pos > m_fileSize) { DBG_FAIL_MACRO; goto fail; } }`
  // (.pio/libdeps/gh_release/SdFat/src/FatLib/FatFile.cpp:1184-1188) and
  // ExFatFile::seekSet() the same against m_dataLength
  // (ExFatLib/ExFatFile.cpp:715-719). POSIX lseek() allows it and a following
  // write() then creates a sparse hole -- which is exactly the SMB2 semantics
  // for a write beyond EOF, so without this the harness would take the free
  // ride and certify a path the X3 cannot execute at all.
  //
  // It matters twice over. On the device the failed seek leaves the file
  // position wherever the previous request left it, so a handler that ignored
  // the result would write the client's bytes at the WRONG OFFSET and report
  // success -- silent corruption, the worst outcome in this subsystem. With
  // this check the harness exercises write_cmd's explicit zero-fill path
  // (see extendWithZeros() in src/network/SmbFileHandlers.cpp) instead.
  struct stat st;
  if (::fstat(impl->fd, &st) != 0) return false;
  if (pos > static_cast<uint64_t>(st.st_size)) return false;
  return ::lseek(impl->fd, static_cast<off_t>(pos), SEEK_SET) == static_cast<off_t>(pos);
}
bool HalFile::seek(size_t pos) { return seek64(pos); }
bool HalFile::seekSet(size_t offset) { return seek(offset); }

bool HalFile::seekCur(int64_t offset) {
  if (!impl || impl->fd < 0) return false;
  return ::lseek(impl->fd, static_cast<off_t>(offset), SEEK_CUR) >= 0;
}

int HalFile::available() const {
  if (!impl || impl->fd < 0) return 0;
  off_t cur = ::lseek(impl->fd, 0, SEEK_CUR);
  struct stat st;
  if (cur < 0 || ::fstat(impl->fd, &st) != 0) return 0;
  return static_cast<int>(st.st_size - cur);
}

size_t HalFile::position() const {
  if (!impl || impl->fd < 0) return 0;
  off_t cur = ::lseek(impl->fd, 0, SEEK_CUR);
  return cur < 0 ? 0 : static_cast<size_t>(cur);
}

int HalFile::read(void* buf, size_t count) {
  if (!impl || impl->fd < 0) return -1;
  return static_cast<int>(::read(impl->fd, buf, count));
}

int HalFile::read() {
  uint8_t b;
  int n = read(&b, 1);
  return n == 1 ? b : -1;
}

size_t HalFile::write(const void* buf, size_t count) {
  if (!impl || impl->fd < 0) return 0;
  ssize_t n = ::write(impl->fd, buf, count);
  return n < 0 ? 0 : static_cast<size_t>(n);
}

size_t HalFile::write(uint8_t b) { return write(&b, 1); }

bool HalFile::rename(const char* newPath) {
  if (!impl) return false;
  std::string full = resolvePath(newPath);

  // A SIXTH rule where POSIX is LOOSER than SdFat, and this one silently
  // destroys data if left unmodelled.
  //
  // SdFat's rename is CREATE-EXCLUSIVE: FatFile::rename() makes the new
  // directory entry with `file.open(dirFile, newPath, O_CREAT | O_EXCL |
  // O_WRONLY)` for a file, or `file.mkdir(dirFile, newPath, false)` for a
  // directory (FatLib/FatFile.cpp:970-984). Both FAIL if the target name
  // already exists. POSIX rename(2) instead REPLACES an existing destination
  // atomically and without a word -- so an unmodelled stub would let the
  // harness certify "rename onto an existing name works", while on the X3 it
  // fails, and worse, would hide the fact that a caller wanting
  // ReplaceIfExists semantics has to delete the destination itself first.
  //
  // The second half of that mkdir() call matters too: `false` is pFlag, "do
  // not create missing path prefix components", so a rename into a directory
  // that does not exist fails rather than creating it. POSIX agrees here
  // (ENOENT), so no extra modelling is needed for that half.
  // Case-INSENSITIVE, because FAT is: renaming onto "VICTIM.TXT" while
  // "victim.txt" exists is a collision on the card even though POSIX would
  // happily create both. See resolveExistingCase().
  struct stat st;
  if (::stat(resolveExistingCase(full).c_str(), &st) == 0) return false;

  if (::rename(impl->path.c_str(), full.c_str()) != 0) return false;
  impl->path = full;
  impl->name = basenameOf(full);
  return true;
}

bool HalFile::isDirectory() const { return impl && impl->directory; }

void HalFile::rewindDirectory() {
  if (impl && impl->dir) ::rewinddir(impl->dir);
}

bool HalFile::close() {
  // v74 (review): the DEVICE asserts here, so the stub must too.
  //
  // lib/hal/HalStorage.cpp's HAL_FILE_WRAPPED_CALL is
  //     StorageLock lock; assert(impl != nullptr); return impl->file.method(...)
  // with no null branch, and the assert is compiled into the shipping firmware.
  // This stub used to answer `return false`, which is the OPPOSITE, and that
  // divergence hid a change that would have rebooted the X3 at the close of
  // every named stream: the 73-check suite was green while the device would
  // have panicked. A stub that is more forgiving than the device does not merely
  // fail to catch a bug -- it certifies one.
  //
  // The eighth row of README.md's divergence table, and the first where the
  // difference was fatal rather than cosmetic. Do not soften it back.
  assert(impl != nullptr);
  // close() IS a write-back, and its return value is that write-back's result
  // -- not "did I let go of the handle". SdFat, all three layers:
  //
  //   FsBaseFile::close(): `bool rtn = m_fFile->close(); m_fFile = nullptr;
  //                         m_xFile = nullptr; return rtn;`  (FsFile.cpp:58-63)
  //   FatFile::close():    `bool rtn = sync(); m_attributes = FILE_ATTR_CLOSED;
  //                         m_flags = 0; return rtn;`        (FatFile.cpp:128-132)
  //   ExFatFile::close():  identical                          (ExFatFile.cpp:75-80)
  //
  // Two properties to reproduce, and they are separate: the result is the
  // SYNC's, and the handle is released REGARDLESS. Returning a flat `true`
  // (what this used to do) modelled neither, and made the harness certify a
  // close_cmd that reported success for a file whose tail never reached the
  // card -- the last data of every upload lands at close.
  const bool synced = sync();  // honours the SYNCFAIL injector, so the close path is testable
  if (impl->fd >= 0) {
    ::close(impl->fd);
    impl->fd = -1;
  }
  if (impl->dir) {
    ::closedir(impl->dir);
    impl->dir = nullptr;
  }
  return synced;
}

HalFile HalFile::openNextFile() {
  if (!impl || !impl->dir) return HalFile();
  struct dirent* entry;
  while ((entry = ::readdir(impl->dir)) != nullptr) {
    std::string name = entry->d_name;
    // Skipping '.' and '..' IS faithful: SdFat's FatFile::openNext() never
    // returns them either (`if (dir->name[0] == '.' ...) lfnOrd = 0;` and
    // loops, FatLib/FatFile.cpp:669-671).
    if (name == "." || name == "..") continue;
    std::string childFull = impl->path + "/" + name;
    struct stat st;
    auto child = std::make_unique<Impl>();
    child->path = childFull;
    child->name = name;
    if (::stat(childFull.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      // STOP, do not skip, when a real entry cannot be opened. This used to
      // `continue`, which is looser than the device: SdFat's openNext() does
      // `if (!openCachedEntry(...)) { DBG_FAIL_MACRO; goto fail; }`
      // (FatLib/FatFile.cpp:676-679) and returns an invalid file, which is
      // indistinguishable from end-of-directory -- so on the X3 one
      // un-openable entry TRUNCATES the rest of the listing. A stub that
      // skipped instead would list the remaining files and certify a listing
      // the device would cut short.
      if (!(child->dir = ::opendir(childFull.c_str()))) return HalFile();
      child->directory = true;
      return HalFile(std::move(child));
    }
    int fd = ::open(childFull.c_str(), O_RDONLY);
    if (fd < 0) return HalFile();  // same reasoning as the opendir case above
    child->fd = fd;
    return HalFile(std::move(child));
  }
  return HalFile();
}

bool HalFile::isOpen() const { return impl && (impl->fd >= 0 || impl->dir != nullptr); }

HalFile::operator bool() const { return isOpen(); }
