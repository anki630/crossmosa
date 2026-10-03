#include "HalStorage.h"

#include <Arduino.h>
#include <Breadcrumb.h>
#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <Memory.h>
#include <SDCardManager.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <cstdio>

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;
char HalStorage::lastAllocFail[96] = {0};

// v358：見 HalStorage.h 的 IoStats。
//   ・arm 是 CAS：已經有 task 在量就拒絕（回 false），disarm 只有 owner 自己做得到 —— 兩個 task 同時建索引時
//     不會互相歸零、互相關掉（codex）。只有 owner 會加數字（單一寫入者），計數本身不用 atomic。
//   ・同一個 task 的計時會巢狀：openFileForRead 的輸出參數原本握著一個檔，指派新檔時舊的 Impl 解構，
//     它的 close 又開一個計時。只算最外層（內層的時間本來就包在外層裡），否則同一段時間算兩次（codex）。
//   ・計時從拿鎖之前開始：等別的 task 放開 SD 也是這個 task 在等 SD；等鎖的部分另外累計在 lockUs。
namespace {
std::atomic<TaskHandle_t> g_ioTask{nullptr};
HalStorage::IoStats g_io;
uint8_t g_ioDepth = 0;  // 只有 owner 會動
enum class IoKind : uint8_t { Read, Write, Seek, Meta };

class IoTimer {
 public:
  explicit IoTimer(const IoKind kind, const size_t bytes = 0, const bool enabled = true) {
    if (!enabled) return;
    const TaskHandle_t owner = g_ioTask.load(std::memory_order_relaxed);
    if (owner == nullptr || owner != xTaskGetCurrentTaskHandle()) return;
    if (g_ioDepth++ > 0) {
      nested_ = true;
      return;
    }
    active_ = true;
    kind_ = kind;
    bytes_ = static_cast<uint32_t>(bytes);
    t0_ = micros();
  }
  // 剛拿到 StorageLock（t0 到這裡＝等鎖）
  void locked() {
    if (active_) tLock_ = micros();
  }
  // 實際讀到的位元組（讀完才知道）
  void setBytes(const size_t bytes) { bytes_ = static_cast<uint32_t>(bytes); }
  ~IoTimer() {
    if (nested_) {
      g_ioDepth--;
      return;
    }
    if (!active_) return;
    g_ioDepth--;
    const uint32_t now = micros();
    const uint32_t us = now - t0_;
    if (tLock_ != 0) g_io.lockUs += tLock_ - t0_;
    switch (kind_) {
      case IoKind::Read:
        g_io.reads++;
        g_io.readUs += us;
        g_io.readBytes += bytes_;
        break;
      case IoKind::Write:
        g_io.writes++;
        g_io.writeUs += us;
        if (us >= HalStorage::kIoSlowUs) g_io.writeSlow++;
        if (us > g_io.writeMaxUs) g_io.writeMaxUs = us;
        if (us > g_io.winWriteMaxUs) g_io.winWriteMaxUs = us;
        break;
      case IoKind::Seek:
        g_io.seeks++;
        g_io.seekUs += us;
        break;
      case IoKind::Meta:
        g_io.metas++;
        g_io.metaUs += us;
        if (us > g_io.metaMaxUs) g_io.metaMaxUs = us;
        if (us > g_io.winMetaMaxUs) g_io.winMetaMaxUs = us;
        break;
    }
  }
  IoTimer(const IoTimer&) = delete;
  IoTimer& operator=(const IoTimer&) = delete;

 private:
  bool active_ = false;
  bool nested_ = false;
  IoKind kind_ = IoKind::Meta;
  uint32_t bytes_ = 0;
  uint32_t t0_ = 0;
  uint32_t tLock_ = 0;
};
}  // namespace

// acquire／release：前一個 owner 在 disarm 之前寫的 g_io，對下一個 arm 成功的 task 可見（它接著要清零）—— 不然兩個 task
//   先後量測時，清零跟前一個 owner 的最後幾筆更新之間沒有先後關係（codex 第二輪）。
bool HalStorage::armIoStats(const bool on) {
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  if (on) {
    TaskHandle_t expected = nullptr;
    if (!g_ioTask.compare_exchange_strong(expected, self, std::memory_order_acquire, std::memory_order_relaxed)) {
      return false;
    }
    g_io = IoStats{};
    g_ioDepth = 0;
    return true;
  }
  TaskHandle_t expected = self;
  return g_ioTask.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed);
}

// 只有 owner 讀得到（別的 task 讀 g_io 就是跟 owner 的寫入搶；codex 第二輪）—— 不是 owner 回全 0。
HalStorage::IoStats HalStorage::ioStats() {
  if (g_ioTask.load(std::memory_order_relaxed) != xTaskGetCurrentTaskHandle()) return IoStats{};
  return g_io;
}

// v359：同 ioStats()，只有 owner 動得到（別的 task 寫就是跟 owner 搶）。
void HalStorage::ioResetWindowPeaks() {
  if (g_ioTask.load(std::memory_order_relaxed) != xTaskGetCurrentTaskHandle()) return;
  g_io.winWriteMaxUs = 0;
  g_io.winMetaMaxUs = 0;
}

void HalStorage::noteAllocFail(const char* where, size_t bytes) {
  // v194：HAL 不能碰 DiagLog；先到先得，src 讀走寫成 ALLOCFAIL。
  LOG_ERR("HAL", "ALLOCFAIL where=%s bytes=%u max=%u", where, static_cast<unsigned>(bytes),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (breadcrumbPending(lastAllocFail)) return;
  char line[sizeof(lastAllocFail)];
  snprintf(line, sizeof(line), "where=%s bytes=%u max=%u", where, static_cast<unsigned>(bytes),
           static_cast<unsigned>(ESP.getMaxAllocHeap()));
  breadcrumbPublish(lastAllocFail, sizeof(lastAllocFail), line);  // v249：跨 task 交接（見 Breadcrumb.h）
}

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

// v358：每個操作外面包一層計時（計時在拿鎖之前開始 —— 等鎖也是 SD 的等待，另記在 lockUs）
#define HAL_STORAGE_TIMED_CALL(kind, bytes, method, ...) \
  IoTimer ioTimer_(kind, bytes);                         \
  HalStorage::StorageLock lock;                          \
  ioTimer_.locked();                                     \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_TIMED_CALL(IoKind::Meta, 0, listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) {
  IoTimer ioTimer_(IoKind::Read);
  HalStorage::StorageLock lock;
  ioTimer_.locked();
  String out = SDCard.readFile(path);
  ioTimer_.setBytes(out.length());
  return out;
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_TIMED_CALL(IoKind::Read, 0, readFileToStream, path, out, chunkSize);  // 讀了多少不知道：bytes 記 0
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  IoTimer ioTimer_(IoKind::Read);
  HalStorage::StorageLock lock;
  ioTimer_.locked();
  const size_t n = SDCard.readFileToBuffer(path, buffer, bufferSize, maxBytes);
  ioTimer_.setBytes(n);
  return n;
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_TIMED_CALL(IoKind::Write, 0, writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  HAL_STORAGE_TIMED_CALL(IoKind::Meta, 0, ensureDirectoryExists, path);
}

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
    // v358：已經 close() 過的（再 close 本來就是 no-op）不計時；計時在拿鎖之前開始，跟其他操作同一個口徑
    IoTimer ioTimer_(IoKind::Meta, 0, file.isOpen());
    HalStorage::StorageLock lock;
    ioTimer_.locked();
    file.close();
  }
  FsFile file;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  IoTimer ioTimer_(IoKind::Meta);  // v358
  StorageLock lock;                // ensure thread safety for the duration of this function
  ioTimer_.locked();
  // v194：make_unique 是 throwing new，OOM 會 abort。失敗回空檔柄（impl=nullptr）。
  auto implPtr = makeUniqueNoThrow<HalFile::Impl>(SDCard.open(path, oflag));
  if (!implPtr) {
    noteAllocFail("HalFile::Impl:open", sizeof(HalFile::Impl));
    return HalFile();
  }
  return HalFile(std::move(implPtr));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
  HAL_STORAGE_TIMED_CALL(IoKind::Meta, 0, mkdir, path, pFlag);
}

bool HalStorage::exists(const char* path) { HAL_STORAGE_TIMED_CALL(IoKind::Meta, 0, exists, path); }

bool HalStorage::readCardId(uint8_t out[16]) { HAL_STORAGE_TIMED_CALL(IoKind::Meta, 0, readCardId, out); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_TIMED_CALL(IoKind::Meta, 0, remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_TIMED_CALL(IoKind::Meta, 0, rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_TIMED_CALL(IoKind::Meta, 0, rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  IoTimer ioTimer_(IoKind::Meta);  // v358
  StorageLock lock;                // ensure thread safety for the duration of this function
  ioTimer_.locked();
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  auto implPtr = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile));
  if (!implPtr) {
    // v194：Impl 配不到 → 空檔柄。close() 必須能接受 impl=nullptr（見 HalFile::close）。
    noteAllocFail("HalFile::Impl:openRead", sizeof(HalFile::Impl));
    file = HalFile();
    return false;
  }
  file = HalFile(std::move(implPtr));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  IoTimer ioTimer_(IoKind::Meta);  // v358
  StorageLock lock;                // ensure thread safety for the duration of this function
  ioTimer_.locked();
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  auto implPtr = makeUniqueNoThrow<HalFile::Impl>(std::move(fsFile));
  if (!implPtr) {
    noteAllocFail("HalFile::Impl:openWrite", sizeof(HalFile::Impl));
    file = HalFile();
    return false;
  }
  file = HalFile(std::move(implPtr));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_TIMED_CALL(IoKind::Meta, 0, removeDir, path); }

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

// v358：同 HAL_FILE_WRAPPED_CALL，外面多包一層計時（見 HalStorage::IoStats）
#define HAL_FILE_TIMED_CALL(kind, bytes, method, ...) \
  IoTimer ioTimer_(kind, bytes);                      \
  HalStorage::StorageLock lock;                       \
  ioTimer_.locked();                                  \
  assert(impl != nullptr);                            \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_TIMED_CALL(IoKind::Write, 0, flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
bool HalFile::seek(size_t pos) { HAL_FILE_TIMED_CALL(IoKind::Seek, 0, seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_TIMED_CALL(IoKind::Seek, 0, seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_TIMED_CALL(IoKind::Seek, 0, seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_TIMED_CALL(IoKind::Seek, 0, seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) {
  IoTimer ioTimer_(IoKind::Read);  // v358：bytes 記實際讀到的
  HalStorage::StorageLock lock;
  ioTimer_.locked();
  assert(impl != nullptr);
  const int n = impl->file.read(buf, count);
  ioTimer_.setBytes(n > 0 ? static_cast<size_t>(n) : 0);
  return n;
}
int HalFile::read() {
  IoTimer ioTimer_(IoKind::Read);
  HalStorage::StorageLock lock;
  ioTimer_.locked();
  assert(impl != nullptr);
  const int c = impl->file.read();
  ioTimer_.setBytes(c >= 0 ? 1 : 0);
  return c;
}
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_TIMED_CALL(IoKind::Write, count, write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_TIMED_CALL(IoKind::Write, 1, write, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_TIMED_CALL(IoKind::Meta, 0, rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_TIMED_CALL(IoKind::Meta, 0, rewindDirectory, ); }
bool HalFile::close() {
  // v194：Impl 配置失敗的空檔柄 impl=nullptr。裝置上 assert 有進出貨韌體，
  // 這裡改成 no-op 回 false，解構子（unique_ptr 對 nullptr）本來就是安全的。
  // 成功路徑（impl 非空）行為不變。
  if (!impl) return false;
  HAL_FILE_TIMED_CALL(IoKind::Meta, 0, close, );
}
HalFile HalFile::openNextFile() {
  IoTimer ioTimer_(IoKind::Meta);  // v358
  HalStorage::StorageLock lock;
  ioTimer_.locked();
  assert(impl != nullptr);
  auto next = makeUniqueNoThrow<Impl>(impl->file.openNextFile());
  if (!next) {
    HalStorage::noteAllocFail("HalFile::Impl:openNext", sizeof(Impl));
    return HalFile();
  }
  return HalFile(std::move(next));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
