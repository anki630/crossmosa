#pragma once

#include <cstddef>
#include <cstdint>

// Parsed layout of the INPUT reports described by a HID report descriptor
// (USB HID 1.11 short items). Pure logic — no Arduino/NimBLE/HAL includes —
// so the host test suite compiles this file verbatim (test/host convention,
// same as FatTimestamp). Sized for page-turner remotes and small keyboards.
//
// The descriptor bytes come from an external device over the air: every parse
// step bounds-checks, and any malformed input empties the map (decode then
// yields zero usages) instead of trusting attacker-controlled lengths.
class HidReportMap {
 public:
  static constexpr uint8_t kMaxFields = 16;         // input-report fields tracked
  static constexpr uint8_t kMaxUsagesPerField = 8;  // discrete usages per variable field
  static constexpr uint8_t kMaxActiveUsages = 8;    // decode output cap
  static constexpr uint8_t kMaxReportIds = 8;       // distinct report IDs tracked
  static constexpr uint16_t kMaxReportBits = 64 * 8;  // sanity cap on one report's size

  struct Field {
    uint8_t reportId;     // 0 when the descriptor declares no report IDs
    uint16_t usagePage;
    bool isArray;         // array item: payload elements carry usage IDs
    uint16_t bitOffset;   // within the report payload (report-ID byte excluded)
    uint8_t reportSize;   // bits per element
    uint8_t reportCount;  // number of elements
    // Variable fields: usages[i] belongs to element i; when the element index
    // exceeds usageCount, Usage Minimum + index applies (Usage Min/Max form).
    // Array fields: element values are usage IDs, bounded by usageMin..usageMax
    // when a range was declared (usageMax > 0).
    uint16_t usages[kMaxUsagesPerField];
    uint8_t usageCount;
    uint16_t usageMin;
    uint16_t usageMax;
  };

  struct Usage {
    uint16_t page;
    uint16_t usage;
  };

  // Returns false and empties the map on malformed input (truncated item,
  // push/pop imbalance, report growing past kMaxReportBits, too many report
  // IDs) or when no input field survives. Fields beyond the caps are dropped,
  // not an error.
  bool parse(const uint8_t* desc, size_t len);

  bool hasReportIds() const { return hasReportIds_; }
  uint8_t fieldCount() const { return fieldCount_; }
  const Field& field(uint8_t i) const { return fields_[i]; }

  // Decodes one notification payload into the set of currently-active usages.
  // GATT report payloads do NOT carry the report ID (it lives in the Report
  // Reference descriptor), so the caller passes it. Truncated payloads read
  // missing bits as 0. Returns the number of usages written (<= outCap).
  uint8_t decode(uint8_t reportId, const uint8_t* payload, size_t len, Usage* out, uint8_t outCap) const;

 private:
  bool failParse();

  Field fields_[kMaxFields] = {};
  uint8_t fieldCount_ = 0;
  bool hasReportIds_ = false;
};
