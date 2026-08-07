#include "HalStorage.h"

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <Memory.h>  // makeUniqueNoThrow -- see the HalFile::Impl allocation note below
#include <SDCardManager.h>

#include <cassert>

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  // Recursive so the same task can re-enter StorageLock without self-deadlock.
  // openFileForRead/Write take the lock and then assign to a HalFile&
  // out-param; if that out-param already held an Impl, its destructor takes
  // the lock again to close the prior FsFile under serialization (see
  // HalFile::Impl::~Impl below). Priority inheritance still applies to
  // recursive mutexes.
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() { return SDCard.begin(); }

bool HalStorage::ready() const { return SDCard.ready(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  // SdFat is not thread-safe; FsFile::close() touches SD/SPI and must run
  // under StorageLock or it races SdSpiCard::m_spiActive across tasks and
  // trips FreeRTOS's xTaskPriorityDisinherit assert. The FsFile member
  // destructor (DESTRUCTOR_CLOSES_FILE=1) will close() again after the lock
  // releases, but close() on an already-closed FsFile is a no-op. See SdFat
  // issue #518 and the HAL note in CLAUDE.md.
  ~Impl() {
    HalStorage::StorageLock lock;
    file.close();
  }
  FsFile file;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

// ---------------------------------------------------------------------------
// Every HalFile::Impl allocation below is nothrow, and that is not cosmetic.
// This firmware builds with -fno-exceptions, so a failed `new` inside
// std::make_unique calls abort() -- an instant reboot with no log line, at
// exactly the moment the device is most short of heap (CLAUDE.md hard limit
// 2: WiFi up). openNextFile() in particular is now on a PER-DIRECTORY-ENTRY
// path with WiFi up, via SMB2's query_directory: a 300-entry folder is 300 of
// these allocations while a network stack holds the heap.
//
// A null Impl is already a meaningful, and correct, result at every call
// site: HalFile::isOpen() (and therefore operator bool) returns false when
// impl is null, which is exactly what "could not open" and "end of directory"
// already look like to every caller. For openNextFile() that also happens to
// match what SdFat itself does for an entry it cannot read -- it returns an
// invalid file indistinguishable from end-of-directory (FatFile.cpp:676-680),
// which src/network/SmbFileHandlers.cpp's listing loop already documents and
// handles. openFileForRead/openFileForWrite additionally force their bool
// result to false, because there the caller reads the bool rather than the
// file's truthiness.
HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  auto impl = makeUniqueNoThrow<HalFile::Impl>(SDCard.open(path, oflag));
  if (!impl) {
    LOG_ERR("HalStorage", "OOM allocating file handle for %s", path);
    return HalFile();
  }
  return HalFile(std::move(impl));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  auto impl = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile));
  if (!impl) {
    LOG_ERR("HalStorage", "OOM allocating file handle for %s (%s)", path, moduleName);
    file = HalFile();
    return false;  // the caller reads this bool, not file's truthiness
  }
  file = HalFile(std::move(impl));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  auto impl = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile));
  if (!impl) {
    LOG_ERR("HalStorage", "OOM allocating file handle for %s (%s)", path, moduleName);
    file = HalFile();
    return false;  // the caller reads this bool, not file's truthiness
  }
  file = HalFile(std::move(impl));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
bool HalFile::sync() { HAL_FILE_WRAPPED_CALL(sync, ); }  // flush() discards this bool; see the header
// WRAPPED, not FORWARD: FatFile::getModifyDateTime() -> dirEntry() calls
// sync() and then reads a cached SD sector (FatFile.cpp:200-219), so it
// touches the card and must hold the storage mutex like every other SD access.
bool HalFile::getModifyDateTime(uint16_t* date, uint16_t* time) {
  HAL_FILE_WRAPPED_CALL(getModifyDateTime, date, time);
}
// sync()s and then writes the cached directory entry (FatFile.cpp:1287-1310),
// so WRAPPED like every other SD access.
bool HalFile::setTimestamp(uint8_t flags, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                           uint8_t second) {
  HAL_FILE_WRAPPED_CALL(timestamp, flags, year, month, day, hour, minute, second);
}
// Writes the FAT/exFAT allocation chain and the directory entry -- squarely an
// SD access, so WRAPPED. Cannot grow a file; see the header.
bool HalFile::truncate(uint64_t length) { HAL_FILE_WRAPPED_CALL(truncate, length); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
bool HalFile::close() { HAL_FILE_WRAPPED_CALL(close, ); }
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  // Nothrow: see the note above HalStorage::open(). A null result is
  // end-of-directory to every caller, which is a truncated listing rather
  // than an abort() -- and the one shape SdFat itself already produces here.
  auto next = makeUniqueNoThrow<Impl>(impl->file.openNextFile());
  if (!next) {
    LOG_ERR("HalStorage", "OOM allocating directory entry handle; listing truncated");
    return HalFile();
  }
  return HalFile(std::move(next));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
