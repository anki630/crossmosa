#pragma once

#include <cstdint>

// Maps HID usages from a page-turner remote to X3 physical-button indices.
// Values mirror HalGPIO::BTN_* (BACK=0 CONFIRM=1 LEFT=2 RIGHT=3 UP=4 DOWN=5);
// BleRemoteManager.cpp static_asserts the mirror so drift cannot silently
// remap every remote key. Header-only and Arduino-free for the host tests.
//
// BTN_POWER (6) is deliberately unreachable: a remote must never be able to
// drive the sleep/wake power-button paths.
inline int hidUsageToButtonIndex(uint16_t page, uint16_t usage) {
  if (page == 0x0C) {  // Consumer page
    switch (usage) {
      case 0x00E9: return 4;  // Volume Up      -> BTN_UP   (previous page)
      case 0x00EA: return 5;  // Volume Down    -> BTN_DOWN (next page)
      case 0x00CD: return 1;  // Play/Pause     -> BTN_CONFIRM
      case 0x00B5: return 3;  // Scan Next      -> BTN_RIGHT
      case 0x00B6: return 2;  // Scan Previous  -> BTN_LEFT
      case 0x0041: return 1;  // Menu Pick      -> BTN_CONFIRM
      case 0x0042: return 4;  // Menu Up        -> BTN_UP
      case 0x0043: return 5;  // Menu Down      -> BTN_DOWN
      case 0x0044: return 2;  // Menu Left      -> BTN_LEFT
      case 0x0045: return 3;  // Menu Right     -> BTN_RIGHT
      case 0x0224: return 0;  // AC Back        -> BTN_BACK
      default: return -1;
    }
  }
  if (page == 0x07) {  // Keyboard/Keypad page
    switch (usage) {
      case 0x28: return 1;  // Enter          -> BTN_CONFIRM
      case 0x58: return 1;  // Keypad Enter   -> BTN_CONFIRM
      case 0x29: return 0;  // Escape         -> BTN_BACK
      case 0x2C: return 5;  // Space          -> BTN_DOWN (next page)
      case 0x4F: return 3;  // Right Arrow    -> BTN_RIGHT
      case 0x50: return 2;  // Left Arrow     -> BTN_LEFT
      case 0x51: return 5;  // Down Arrow     -> BTN_DOWN
      case 0x52: return 4;  // Up Arrow       -> BTN_UP
      case 0x4B: return 4;  // Page Up        -> BTN_UP
      case 0x4E: return 5;  // Page Down      -> BTN_DOWN
      case 0x80: return 4;  // Volume Up      -> BTN_UP
      case 0x81: return 5;  // Volume Down    -> BTN_DOWN
      default: return -1;
    }
  }
  return -1;
}
