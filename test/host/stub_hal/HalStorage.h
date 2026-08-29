#pragma once
//
// Desktop stand-in for lib/hal/HalStorage.h (read that file side by side with
// this one). This exists so Task 4's src/network/SmbFileHandlers.cpp can be
// compiled, byte-for-byte unmodified, against either the real device
// HalStorage/HalFile or this POSIX-backed one -- that's the entire point of
// this test harness (see test/host/README.md).
//
// RULE: every public method HalStorage/HalFile expose on device must have an
// identically-named, identically-shaped counterpart here (same name, same
// parameter types/order, same defaults, same const-ness, same return type),
// so handler code compiles unmodified against either header. Do not add
// methods here that don't exist on device; do not drop any that do. This
// was diffed method-by-method against lib/hal/HalStorage.h -- see
// test/host/README.md and .superpowers/sdd/2026-07-28-smb2-server/task-2-report.md
// for the comparison table.
//
// What's deliberately NOT a faithful reproduction, and why:
//   - `String`  : minimal stand-in for Arduino's String (constructible from
//                 const char* / std::string, c_str(), length()) -- just
//                 enough for the signatures below to typecheck and for
//                 straightforward POSIX bodies to work. Arduino's String has
//                 dozens of other members (operator+, substring, toInt...);
//                 none of them are part of HalStorage/HalFile's own public
//                 surface, so reproducing them is out of scope here.
//   - `Print`   : minimal stand-in exposing only the two virtual `write`
//                 overloads HalFile itself overrides with `override`.
//                 Arduino's print()/println()/printf() helpers are not
//                 reproduced; nothing in HalStorage.h's own public surface
//                 depends on them.
//   - `oflag_t` : typedef'd to plain `int`, backed by real POSIX <fcntl.h>
//                 flags. This is NOT an approximation -- SdFat takes its
//                 `USE_FCNTL_H=1` path on ESP32 (see
//                 .pio/libdeps/*/SdFat/src/SdFatConfig.h:349-351), so the
//                 device build's own `oflag_t` is `typedef int oflag_t;`
//                 backed by the same real fcntl.h flag values. Same
//                 definition, not a substitute.
//   - private storage mutex (device: `SemaphoreHandle_t storageMutex`):
//                 omitted. This harness is single-threaded --
//                 `smb2_serve_port`'s blocking select() loop is the only
//                 thing running -- so there is nothing to serialize
//                 against. It is a private implementation detail, not part
//                 of the public contract this header exists to mirror.
//
#include <fcntl.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ---- oflag_t: identical definition to the device build's SdFat
// USE_FCNTL_H=1 path (real POSIX <fcntl.h> flags; see comment block above).
typedef int oflag_t;
#ifndef O_AT_END
#define O_AT_END O_NONBLOCK  // SdFat's "open at EOF" flag, reusing this bit.
#endif
#ifndef O_READ
#define O_READ O_RDONLY
#endif
#ifndef O_WRITE
#define O_WRITE O_WRONLY
#endif

// setTimestamp()'s flags. Same values as SdFat's, which the device build gets
// from common/FsApiConstants.h:79-83 via the real HalStorage.h -- handler code
// naming T_WRITE must compile identically against either header.
const uint8_t T_ACCESS = 1;
const uint8_t T_CREATE = 2;
const uint8_t T_WRITE = 4;

// ---- Arduino String stand-in (see header comment above for scope) --------
class String {
 public:
  String() = default;
  String(const char* s) : value_(s ? s : "") {}
  String(const std::string& s) : value_(s) {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  bool operator==(const String& other) const { return value_ == other.value_; }
  operator std::string() const { return value_; }

 private:
  std::string value_;
};

// ---- Arduino Print stand-in (see header comment above for scope) ---------
class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < size; ++i) n += write(buffer[i]);
    return n;
  }
};

class HalFile;

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file);
  bool removeDir(const char* path);

  static HalStorage& getInstance() { return instance; }

  class StorageLock;  // private class, used internally (unused on host; see file comment)

 private:
  static HalStorage instance;

  bool initialized = false;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  explicit HalFile(std::unique_ptr<Impl> impl);

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&);
  HalFile& operator=(HalFile&&);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  // Mirrors lib/hal/HalStorage.h's sync() -- flush() that reports whether the
  // write-back succeeded. See the .cpp for how a failure is injected here
  // (POSIX fsync() on a healthy scratch file never fails, so the failure path
  // would otherwise be present but untestable).
  bool sync();
  // Mirrors lib/hal/HalStorage.h's getModifyDateTime() -- FAT packed date/time
  // from the directory entry. See the .cpp for the two places POSIX and FAT
  // do not line up (the share root, and dates outside FAT's 1980-2107 range).
  bool getModifyDateTime(uint16_t* date, uint16_t* time);
  // Mirrors lib/hal/HalStorage.h's truncate() -- including its inability to
  // GROW a file, which POSIX ftruncate() can and SdFat cannot. See the .cpp.
  // Mirrors lib/hal/HalStorage.h's setTimestamp(), including its 1980-2099
  // range (narrower than the getter's) and its acceptance of directories.
  bool setTimestamp(uint8_t flags, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                    uint8_t second);
  bool truncate(uint64_t length);
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  // Mirrors the device header's own inline bulk-write override (see
  // lib/hal/HalStorage.h:90-93 and its v53 comment) -- routes bulk Print
  // writes (e.g. serializeJson(doc, halFile)) through write(const void*, size_t)
  // instead of Print's default one-byte-at-a-time loop.
  size_t write(const uint8_t* buf, size_t count) override { return write(static_cast<const void*>(buf), count); }
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;
};

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
