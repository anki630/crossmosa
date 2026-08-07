#include "util/HidReportMap.h"

namespace {
// Little-endian value of 0/1/2/4 bytes (HID short-item data).
uint32_t readUnsigned(const uint8_t* p, uint8_t n) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < n; i++) {
    v |= static_cast<uint32_t>(p[i]) << (8 * i);
  }
  return v;
}

// Little-endian bit extraction with hard bounds: bits beyond the payload read
// as 0 (a truncated notification must degrade, not index out of range).
uint32_t extractBits(const uint8_t* payload, size_t len, uint32_t bitOffset, uint8_t bitSize) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < bitSize; i++) {
    const uint32_t bit = bitOffset + i;
    const size_t byteIdx = bit >> 3;
    if (byteIdx >= len) {
      break;
    }
    if (payload[byteIdx] & (1u << (bit & 7))) {
      v |= (1u << i);
    }
  }
  return v;
}
}  // namespace

bool HidReportMap::failParse() {
  fieldCount_ = 0;
  hasReportIds_ = false;
  return false;
}

bool HidReportMap::parse(const uint8_t* desc, size_t len) {
  fieldCount_ = 0;
  hasReportIds_ = false;
  if (desc == nullptr || len == 0) {
    return false;
  }

  struct GlobalState {
    uint16_t usagePage = 0;
    uint8_t reportSize = 0;
    uint8_t reportCount = 0;
    uint8_t reportId = 0;
  };
  GlobalState g;
  GlobalState stack[4];
  uint8_t stackDepth = 0;

  // Local items — reset after every Main item per HID 1.11 §6.2.2.8.
  uint16_t usages[kMaxUsagesPerField];
  uint8_t usageCount = 0;
  uint16_t usageMin = 0;
  uint16_t usageMax = 0;
  bool haveUsageRange = false;

  // One bit cursor per report ID (ID 0 = descriptor without report IDs).
  struct Cursor {
    uint8_t id;
    uint32_t bits;
  };
  Cursor cursors[kMaxReportIds];
  uint8_t cursorCount = 0;

  size_t pos = 0;
  while (pos < len) {
    const uint8_t prefix = desc[pos++];
    if (prefix == 0xFE) {  // Long item: [0xFE][dataSize][tag][data...] — skip.
      if (pos + 2 > len) {
        return failParse();
      }
      const uint8_t longSize = desc[pos];
      pos += 2;
      if (pos + longSize > len) {
        return failParse();
      }
      pos += longSize;
      continue;
    }
    static constexpr uint8_t kSizes[4] = {0, 1, 2, 4};
    const uint8_t dataSize = kSizes[prefix & 0x03];
    const uint8_t type = (prefix >> 2) & 0x03;  // 0 = main, 1 = global, 2 = local
    const uint8_t tag = (prefix >> 4) & 0x0F;
    if (pos + dataSize > len) {
      return failParse();
    }
    const uint32_t value = readUnsigned(desc + pos, dataSize);
    pos += dataSize;

    if (type == 1) {  // Global items
      switch (tag) {
        case 0x0:
          g.usagePage = static_cast<uint16_t>(value);
          break;
        case 0x7:
          g.reportSize = value > 255 ? 255 : static_cast<uint8_t>(value);
          break;
        case 0x8:
          g.reportId = static_cast<uint8_t>(value);
          hasReportIds_ = true;
          break;
        case 0x9:
          g.reportCount = value > 255 ? 255 : static_cast<uint8_t>(value);
          break;
        case 0xA:  // Push
          if (stackDepth >= 4) {
            return failParse();
          }
          stack[stackDepth++] = g;
          break;
        case 0xB:  // Pop
          if (stackDepth == 0) {
            return failParse();
          }
          g = stack[--stackDepth];
          break;
        default:  // logical/physical min/max, unit, unit exponent — irrelevant
          break;
      }
    } else if (type == 2) {  // Local items
      switch (tag) {
        case 0x0:  // Usage
          if (usageCount < kMaxUsagesPerField) {
            usages[usageCount++] = static_cast<uint16_t>(value);
          }
          break;
        case 0x1:  // Usage Minimum
          usageMin = static_cast<uint16_t>(value);
          haveUsageRange = true;
          break;
        case 0x2:  // Usage Maximum
          usageMax = static_cast<uint16_t>(value);
          haveUsageRange = true;
          break;
        default:
          break;
      }
    } else if (type == 0) {  // Main items
      if (tag == 0x8) {  // Input
        uint32_t* cursor = nullptr;
        for (uint8_t i = 0; i < cursorCount; i++) {
          if (cursors[i].id == g.reportId) {
            cursor = &cursors[i].bits;
            break;
          }
        }
        if (cursor == nullptr) {
          if (cursorCount >= kMaxReportIds) {
            return failParse();
          }
          cursors[cursorCount] = {g.reportId, 0};
          cursor = &cursors[cursorCount++].bits;
        }
        const uint32_t fieldBits = static_cast<uint32_t>(g.reportSize) * g.reportCount;
        if (*cursor + fieldBits > kMaxReportBits) {
          return failParse();
        }
        const bool isConst = (value & 0x01) != 0;
        const bool isVariable = (value & 0x02) != 0;
        if (!isConst && g.reportSize > 0 && g.reportCount > 0 && fieldCount_ < kMaxFields) {
          Field& f = fields_[fieldCount_];
          f.reportId = g.reportId;
          f.usagePage = g.usagePage;
          f.isArray = !isVariable;
          f.bitOffset = static_cast<uint16_t>(*cursor);
          f.reportSize = g.reportSize;
          f.reportCount = g.reportCount;
          f.usageCount = usageCount;
          for (uint8_t i = 0; i < usageCount; i++) {
            f.usages[i] = usages[i];
          }
          f.usageMin = haveUsageRange ? usageMin : 0;
          f.usageMax = haveUsageRange ? usageMax : 0;
          fieldCount_++;
        }
        *cursor += fieldBits;  // const padding advances the cursor too
      }
      // Every Main item (Input/Output/Feature/Collection/End Collection)
      // resets the local-item state.
      usageCount = 0;
      usageMin = 0;
      usageMax = 0;
      haveUsageRange = false;
    }
  }
  if (fieldCount_ == 0) {
    return failParse();
  }
  return true;
}

uint8_t HidReportMap::decode(uint8_t reportId, const uint8_t* payload, size_t len, Usage* out,
                             uint8_t outCap) const {
  if (payload == nullptr || out == nullptr || outCap == 0) {
    return 0;
  }
  uint8_t n = 0;
  for (uint8_t fi = 0; fi < fieldCount_; fi++) {
    const Field& f = fields_[fi];
    if (f.reportId != reportId) {
      continue;
    }
    if (f.reportSize == 0 || f.reportSize > 16) {
      continue;  // wider elements are axes/vendor data, never buttons
    }
    for (uint8_t e = 0; e < f.reportCount; e++) {
      const uint32_t v =
          extractBits(payload, len, f.bitOffset + static_cast<uint32_t>(e) * f.reportSize, f.reportSize);
      if (v == 0) {
        continue;
      }
      uint16_t usage;
      if (f.isArray) {
        usage = static_cast<uint16_t>(v);
        if (f.usageMax > 0 && (usage < f.usageMin || usage > f.usageMax)) {
          continue;
        }
      } else {
        if (e < f.usageCount) {
          usage = f.usages[e];
        } else if (f.usageMax > 0 && static_cast<uint32_t>(f.usageMin) + e <= f.usageMax) {
          usage = static_cast<uint16_t>(f.usageMin + e);
        } else {
          continue;
        }
      }
      if (n < outCap) {
        out[n].page = f.usagePage;
        out[n].usage = usage;
        n++;
      }
    }
  }
  return n;
}
