#include "BookCacheUtils.h"
#include <DataDir.h>

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>

#include <functional>

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    Epub(path, DataDir::path()).clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    // XTC support was removed; still clear the leftover cache dir when the file is deleted/moved
    // via web/WebDAV. Same naming scheme as Xtc.h (kept out of lib/Xtc so the lib stays gc-able).
    const std::string cacheDir = std::string(DataDir::path()) + "/xtc_" + std::to_string(std::hash<std::string>{}(path));
    if (Storage.exists(cacheDir.c_str())) {
      Storage.removeDir(cacheDir.c_str());
    }
  } else if (FsHelpers::hasTxtExtension(path)) {
    Txt(path, DataDir::path()).clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}
