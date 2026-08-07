// Host-side unit test for HidReportMap + HidUsageMap (pure logic, no HAL).
// Pattern follows fat_time_test.cpp: plain main(), gFailures counter.
#include "util/HidReportMap.h"
#include "util/HidUsageMap.h"

#include <cstdio>
#include <cstring>

namespace {
int gFailures = 0;

void expectEq(const char* what, long long got, long long want) {
  if (got == want) {
    std::printf("PASS (%s): %lld\n", what, got);
    return;
  }
  std::printf("FAIL (%s): got %lld, want %lld\n", what, got, want);
  gFailures++;
}

void expectTrue(const char* what, bool v) { expectEq(what, v ? 1 : 0, 1); }

// Sample A — consumer control, report ID 1, one 16-bit array element
// (the shape used by "AB Shutter"-style selfie/page-turner remotes).
const uint8_t kConsumerArray[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x10,        //   Report Size (16)
    0x81, 0x00,        //   Input (Data, Array)
    0xC0,              // End Collection
};

// Sample B — boot-style keyboard, report ID 2: 8x1-bit modifiers (E0-E7,
// variable via Usage Min/Max), 8-bit const padding, 6x8-bit key array.
const uint8_t kKeyboard[] = {
    0x05, 0x07, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
    0xC0,
};

// Sample C — consumer bitmap (variable) style, report ID 3: six named 1-bit
// usages + 2 bits padding.
const uint8_t kConsumerBitmap[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03,
    0x09, 0xCD, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xE9, 0x09, 0xEA, 0x09, 0xE2,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x06, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x01,
    0xC0,
};

// Sample D — no report IDs at all (single unnumbered consumer array element).
const uint8_t kNoReportId[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x19, 0x00, 0x2A, 0xFF, 0x03, 0x15, 0x00, 0x26, 0xFF, 0x03,
    0x95, 0x01, 0x75, 0x10, 0x81, 0x00,
    0xC0,
};

uint8_t decodeOne(const HidReportMap& map, uint8_t reportId, const uint8_t* payload, size_t len,
                  HidReportMap::Usage* out) {
  return map.decode(reportId, payload, len, out, HidReportMap::kMaxActiveUsages);
}
}  // namespace

int main() {
  HidReportMap::Usage u[HidReportMap::kMaxActiveUsages];

  {  // Sample A: parse + Volume Up press/release
    HidReportMap m;
    expectTrue("A parse", m.parse(kConsumerArray, sizeof(kConsumerArray)));
    expectTrue("A hasReportIds", m.hasReportIds());
    const uint8_t volUp[] = {0xE9, 0x00};
    expectEq("A volup count", decodeOne(m, 1, volUp, sizeof(volUp), u), 1);
    expectEq("A volup page", u[0].page, 0x0C);
    expectEq("A volup usage", u[0].usage, 0xE9);
    expectEq("A volup button", hidUsageToButtonIndex(u[0].page, u[0].usage), 4);
    const uint8_t idle[] = {0x00, 0x00};
    expectEq("A idle count", decodeOne(m, 1, idle, sizeof(idle), u), 0);
    const uint8_t wrongReport[] = {0xE9, 0x00};
    expectEq("A wrong report id", decodeOne(m, 2, wrongReport, sizeof(wrongReport), u), 0);
  }

  {  // Sample B: keyboard — arrow key in the array, modifier bit, truncation
    HidReportMap m;
    expectTrue("B parse", m.parse(kKeyboard, sizeof(kKeyboard)));
    const uint8_t down[] = {0x00, 0x00, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00};
    expectEq("B down count", decodeOne(m, 2, down, sizeof(down), u), 1);
    expectEq("B down usage", u[0].usage, 0x51);
    expectEq("B down button", hidUsageToButtonIndex(0x07, u[0].usage), 5);
    // Left-Shift modifier (bit 1 of byte 0) => usage E1 via Usage Min + index.
    const uint8_t shiftOnly[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    expectEq("B shift count", decodeOne(m, 2, shiftOnly, sizeof(shiftOnly), u), 1);
    expectEq("B shift usage", u[0].usage, 0xE1);
    expectEq("B shift unmapped", hidUsageToButtonIndex(0x07, 0xE1), -1);
    // Truncated payload: missing bytes read as zero, no out-of-bounds.
    const uint8_t truncated[] = {0x00, 0x00, 0x52};
    expectEq("B truncated count", decodeOne(m, 2, truncated, sizeof(truncated), u), 1);
    expectEq("B truncated usage", u[0].usage, 0x52);
    // Two keys at once.
    const uint8_t two[] = {0x00, 0x00, 0x4B, 0x28, 0x00, 0x00, 0x00, 0x00};
    expectEq("B two keys", decodeOne(m, 2, two, sizeof(two), u), 2);
  }

  {  // Sample C: bitmap — named variable usages
    HidReportMap m;
    expectTrue("C parse", m.parse(kConsumerBitmap, sizeof(kConsumerBitmap)));
    const uint8_t playAndNext[] = {0x03};  // bit0 Play/Pause + bit1 ScanNext
    expectEq("C count", decodeOne(m, 3, playAndNext, sizeof(playAndNext), u), 2);
    expectEq("C usage0", u[0].usage, 0xCD);
    expectEq("C usage1", u[1].usage, 0xB5);
    const uint8_t paddingOnly[] = {0xC0};  // only the 2 padding bits set
    expectEq("C padding ignored", decodeOne(m, 3, paddingOnly, sizeof(paddingOnly), u), 0);
  }

  {  // Sample D: descriptor without report IDs decodes under reportId 0
    HidReportMap m;
    expectTrue("D parse", m.parse(kNoReportId, sizeof(kNoReportId)));
    expectEq("D hasReportIds", m.hasReportIds() ? 1 : 0, 0);
    const uint8_t volDown[] = {0xEA, 0x00};
    expectEq("D voldown count", decodeOne(m, 0, volDown, sizeof(volDown), u), 1);
    expectEq("D voldown button", hidUsageToButtonIndex(0x0C, u[0].usage), 5);
  }

  {  // Malformed inputs must fail parse (empty map), never crash or overflow.
    HidReportMap m;
    const uint8_t truncatedItem[] = {0x05, 0x0C, 0x26, 0xFF};  // 2-byte item, 1 byte present
    expectEq("M truncated item", m.parse(truncatedItem, sizeof(truncatedItem)) ? 1 : 0, 0);
    expectEq("M truncated fields", m.fieldCount(), 0);

    const uint8_t popUnderflow[] = {0xB4, 0xC0};  // Pop with empty stack
    expectEq("M pop underflow", m.parse(popUnderflow, sizeof(popUnderflow)) ? 1 : 0, 0);

    // Report grows past kMaxReportBits: size 255 x count 255.
    const uint8_t hugeReport[] = {0x05, 0x0C, 0x75, 0xFF, 0x95, 0xFF, 0x81, 0x00, 0xC0};
    expectEq("M huge report", m.parse(hugeReport, sizeof(hugeReport)) ? 1 : 0, 0);

    expectEq("M empty", m.parse(nullptr, 0) ? 1 : 0, 0);

    // Long item is skipped, not fatal; descriptor still has a valid field after.
    const uint8_t withLongItem[] = {0xFE, 0x02, 0x00, 0xAA, 0xBB,
                                    0x05, 0x0C, 0x19, 0x00, 0x2A, 0xFF, 0x03,
                                    0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xC0};
    expectTrue("M long item skipped", m.parse(withLongItem, sizeof(withLongItem)));

    // Array element outside the declared usage range is dropped.
    HidReportMap a;
    expectTrue("M range parse", a.parse(kConsumerArray, sizeof(kConsumerArray)));
    const uint8_t outOfRange[] = {0xFF, 0x07};  // 0x7FF > usageMax 0x3FF
    expectEq("M out of range", decodeOne(a, 1, outOfRange, sizeof(outOfRange), u), 0);
  }

  {  // Mapping table safety: no usage on any page may reach BTN_POWER (6).
    int hits = 0;
    for (uint32_t page = 0; page <= 0xFF; page++) {
      for (uint32_t usage = 0; usage <= 0x3FF; usage++) {
        if (hidUsageToButtonIndex(static_cast<uint16_t>(page), static_cast<uint16_t>(usage)) == 6) hits++;
      }
    }
    expectEq("Map never BTN_POWER", hits, 0);
  }

  std::printf(gFailures ? "hid_report_map_test: %d FAILURE(S)\n" : "hid_report_map_test: all passed\n", gFailures);
  return gFailures ? 1 : 0;
}
