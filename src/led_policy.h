#pragma once
#include <stdint.h>

namespace karute_led {

constexpr uint32_t kWifiDisconnectedCycleMs = 2000;
constexpr uint32_t kWifiDisconnectedRedMs = 400;

constexpr bool wifiDisconnectedRed(uint32_t elapsedMs) {
  return (elapsedMs % kWifiDisconnectedCycleMs) < kWifiDisconnectedRedMs;
}

// The input indicator deliberately samples one key per mode. This keeps LED
// traffic and duty cycle low during very long runs without changing HID data.
constexpr bool asciiIndicatorKey(uint8_t mode, char character) {
  return (mode == 1 && character == 'c') ||
         (mode == 2 && (character == 'e' || character == 'E'));
}

constexpr bool altIndicatorKey(uint8_t mode, uint8_t usage) {
  return mode == 4 && usage == 0x59;  // HID usage for keypad 1.
}

}  // namespace karute_led
