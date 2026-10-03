#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "karute_gbk_table.h"

namespace karute_alt {

enum class Mode : uint8_t { GBK = 1, UNICODE_DECIMAL = 2 };

// Strict UTF-8, with no replacement characters or truncated sequences.
inline bool decode(const char *text, size_t length, size_t &index,
                   uint32_t &codepoint) {
  if (index >= length) return false;
  const auto *bytes = reinterpret_cast<const uint8_t *>(text);
  const uint8_t first = bytes[index++];
  if (first < 0x80) {
    codepoint = first;
    return true;
  }
  unsigned count;
  uint32_t minimum;
  if (first >= 0xc2 && first <= 0xdf) {
    count = 1; minimum = 0x80; codepoint = first & 0x1f;
  } else if (first >= 0xe0 && first <= 0xef) {
    count = 2; minimum = 0x800; codepoint = first & 0x0f;
  } else if (first >= 0xf0 && first <= 0xf4) {
    count = 3; minimum = 0x10000; codepoint = first & 7;
  } else {
    return false;
  }
  if (count > length - index) return false;
  for (unsigned i = 0; i < count; ++i) {
    const uint8_t next = bytes[index++];
    if ((next & 0xc0) != 0x80) return false;
    codepoint = (codepoint << 6) | (next & 0x3f);
  }
  return codepoint >= minimum && codepoint <= 0x10ffff &&
         !(codepoint >= 0xd800 && codepoint <= 0xdfff);
}

inline bool gbkCode(uint32_t codepoint, uint16_t &value) {
  if (codepoint <= 0x7f) {
    value = static_cast<uint16_t>(codepoint);
    return true;
  }
  if (codepoint > 0xffff) return false;
  size_t low = 0, high = sizeof(kGbkPairs) / sizeof(kGbkPairs[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const uint32_t candidate = kGbkPairs[middle] >> 16;
    if (candidate < codepoint) low = middle + 1;
    else high = middle;
  }
  if (low == sizeof(kGbkPairs) / sizeof(kGbkPairs[0]) ||
      (kGbkPairs[low] >> 16) != codepoint) return false;
  value = static_cast<uint16_t>(kGbkPairs[low] & 0xffff);
  return true;
}

inline bool outputCode(Mode mode, uint32_t codepoint, uint16_t &value) {
  // Only these control keys are deliberately supported. Never turn arbitrary
  // control bytes into shortcuts or drop them without telling the user.
  if (codepoint == '\n' || codepoint == '\r' || codepoint == '\t' ||
      codepoint == '\b') {
    value = static_cast<uint16_t>(codepoint);
    return true;
  }
  if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f))
    return false;
  if (mode == Mode::GBK) return gbkCode(codepoint, value);
  // Decimal Alt input is NOT universal Unicode injection. In this experiment
  // only BMP scalars are supported; the target application decides semantics.
  if (mode != Mode::UNICODE_DECIMAL || codepoint > 0xffff ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
  value = static_cast<uint16_t>(codepoint);
  return true;
}

inline uint8_t keypadUsage(char digit) {
  if (digit == '0') return 0x62;
  if (digit >= '1' && digit <= '9') return 0x59 + (digit - '1');
  return 0;
}

inline unsigned decimalDigits(uint16_t value) {
  unsigned count = 1;
  while (value >= 10) { value /= 10; ++count; }
  return count;
}

inline uint32_t characterEstimateMs(uint16_t value, uint16_t down,
                                    uint16_t gap, uint16_t after) {
  if (value == '\n' || value == '\r' || value == '\t' || value == '\b')
    return down + after;
  const unsigned digits = decimalDigits(value);
  return (digits + 1) * down + digits * gap + after;
}

// Portable report sequence, shared by the firmware and host tests.
// send(modifier, usage) must return transport success. wait(ms) may collect a
// stop request, but must NOT cancel mid-code: releasing Alt halfway would
// commit a different character. The caller stops at the next boundary and
// always attempts a final all-keys-up report on failure or completion.
template <typename Sender, typename Waiter>
bool emitTimedCode(uint16_t value, uint16_t down, uint16_t gap,
                   uint16_t before, uint16_t release, uint16_t after,
                   Sender send, Waiter wait) {
  uint8_t special = 0;
  if (value == '\n' || value == '\r') special = 0x28;
  else if (value == '\t') special = 0x2b;
  else if (value == '\b') special = 0x2a;
  if (special) {
    if (!send(0, special)) return false;
    wait(down);
    if (!send(0, 0)) return false;
    wait(after);
    return true;
  }
  constexpr uint8_t alt = 0x04;  // Left Alt HID modifier, not a key usage.
  char decimal[6];
  snprintf(decimal, sizeof(decimal), "%u", static_cast<unsigned>(value));
  if (!send(alt, 0)) return false;
  wait(before);
  for (size_t i = 0; decimal[i]; ++i) {
    if (!send(alt, keypadUsage(decimal[i]))) return false;
    wait(down);
    // Preserve Alt, release each digit (including repeated digits).
    if (!send(alt, 0)) return false;
    wait(decimal[i + 1] ? gap : release);
  }
  if (!send(0, 0)) return false;
  wait(after);
  return true;
}

// Preserve the earlier isolated prototype's timing API.
template <typename Sender, typename Waiter>
bool emitCode(uint16_t value, uint16_t down, uint16_t gap, uint16_t after,
              Sender send, Waiter wait) {
  return emitTimedCode(value, down, gap, down, gap, after, send, wait);
}
inline uint32_t timedEstimateMs(uint16_t value, uint16_t down, uint16_t gap,
                                uint16_t before, uint16_t release, uint16_t after) {
  if (value == '\n' || value == '\r' || value == '\t' || value == '\b')
    return down + after;
  const unsigned digits = decimalDigits(value);
  return before + digits * down + (digits - 1) * gap + release + after;
}

}  // namespace karute_alt
