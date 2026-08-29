#include "BenchFlags.h"

#include <HalStorage.h>

#include <cstring>
#include <strings.h>

#include "DiagLog.h"

namespace BenchFlags {
uint8_t grayVariant = 0;
bool scrub = false;
bool wall4 = false;
bool aaDark = false;

void load() {
  if (!Storage.ready()) return;
  // 一趟掃過根目錄，而不是七次 exists()——SdFat 的 exists() 每次都是完整開檔（教訓 A-4），
  // 開機路徑（含快速喚醒）不該為預設關閉的 bench 付七次。
  HalFile dir = Storage.open("/");
  if (!dir || !dir.isDirectory()) return;
  for (HalFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    char name[24];
    const size_t n = f.getName(name, sizeof(name));
    f.close();
    if (n == 0) continue;
    if (strncasecmp(name, "gray.", 5) == 0 && name[5] >= '1' && name[5] <= '4' && name[6] == '\0') {
      if (grayVariant == 0) grayVariant = static_cast<uint8_t>(name[5] - '0');
    } else if (strcasecmp(name, "scrub.on") == 0) {
      scrub = true;
    } else if (strcasecmp(name, "wall4.on") == 0) {
      wall4 = true;
    } else if (strcasecmp(name, "aadark.on") == 0) {
      aaDark = true;
    }
  }
  dir.close();
  DiagLog::line("BENCH gray=%u scrub=%d wall4=%d aadark=%d", static_cast<unsigned>(grayVariant),
                static_cast<int>(scrub), static_cast<int>(wall4), static_cast<int>(aaDark));
}
}  // namespace BenchFlags
