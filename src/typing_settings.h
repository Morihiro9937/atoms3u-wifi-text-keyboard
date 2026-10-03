#pragma once
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

constexpr uint32_t kSettingsMagicV2 = 0x4B415232;  // KAR2
constexpr uint32_t kSettingsMagicV3 = 0x4B415233;  // KAR3
constexpr uint32_t kSettingsMagicV4 = 0x4B415234;  // KAR4
constexpr uint32_t kSettingsMagicV5 = 0x4B415235;  // KAR5
constexpr uint32_t kSettingsMagicV6 = 0x4B415236;  // KAR6
constexpr uint32_t kSettingsMagicV7 = 0x4B415237;  // KAR7
constexpr uint32_t kSettingsMagicV8 = 0x4B415238;  // KAR8
constexpr uint32_t kSettingsMagicV9 = 0x4B415239;  // KAR9

struct SpeedSettings {
  uint16_t keyDownMs, interKeyMs, beforeSpaceMs, afterSpaceMs;
};
struct LegacyTypingSettingsV2 {
  uint32_t magic;
  uint8_t mode;
  SpeedSettings vuc, eng;
  uint16_t startDelayMs;
  char unusedWifiName[33];
  char unusedWifiPassword[64];
};
// Exact NVS layout from r3; keep it for migration and rollback.
struct LegacyTypingSettingsV3 {
  uint32_t magic;
  uint8_t mode;
  SpeedSettings vuc, eng;
  uint16_t shiftBeforeMs, shiftAfterMs, startDelayMs;
  char unusedWifiName[33];
  char unusedWifiPassword[64];
};
struct LegacyTypingSettingsV4 {
  uint32_t magic = kSettingsMagicV4;
  // 1 VUC, 2 ENG, 3 ASCII hybrid, 4 Alt/GBK, 5 Alt/Unicode decimal.
  uint8_t mode = 1;
  SpeedSettings vuc = {5, 20, 20, 20};
  SpeedSettings eng = {2, 2, 0, 0};
  uint16_t shiftBeforeMs = 50, shiftAfterMs = 100, startDelayMs = 3000;
  char unusedWifiName[33] = "karute";
  char unusedWifiPassword[64] = "autokarute";
  // Shared by the two experimental Alt modes, separate from VUC/ENG speeds.
  SpeedSettings alt = {5, 5, 0, 10};
};
// Exact NVS layout from r5; keep it for migration and rollback.
struct LegacyTypingSettingsV5 {
  uint32_t magic = kSettingsMagicV5;
  uint8_t mode = 1;
  SpeedSettings vuc = {5, 20, 20, 20};
  SpeedSettings eng = {2, 2, 0, 0};
  uint16_t shiftBeforeMs = 50, shiftAfterMs = 100, startDelayMs = 3000;
  char unusedWifiName[33] = "karute";
  char unusedWifiPassword[64] = "autokarute";
  SpeedSettings alt = {5, 5, 0, 10};
  uint16_t altBeforeMs = 5, altReleaseMs = 5;
};
// Exact NVS layout from r6; the Wi-Fi bytes existed but were not user-editable.
struct LegacyTypingSettingsV6 {
  uint32_t magic = kSettingsMagicV6;
  uint8_t mode = 1;
  SpeedSettings vuc = {5, 20, 20, 20};
  SpeedSettings eng = {2, 2, 0, 0};
  uint16_t shiftBeforeMs = 50, shiftAfterMs = 100, startDelayMs = 3000;
  char unusedWifiName[33] = "karute";
  char unusedWifiPassword[64] = "autokarute";
  SpeedSettings alt = {5, 5, 0, 10};
  uint16_t altBeforeMs = 5, altReleaseMs = 5;
  uint8_t inputLedEnabled = 1, standbyLedEnabled = 1;
};
// Exact NVS layout from r7; keep it for migration and rollback.
struct LegacyTypingSettingsV7 {
  uint32_t magic = kSettingsMagicV7;
  // Stable IDs: 4 GBK, 2 ENG, 1 VUC. Removed: 3 ASCII hybrid, 5 Unicode.
  uint8_t mode = 1;
  SpeedSettings vuc = {5, 20, 20, 20};
  SpeedSettings eng = {2, 2, 0, 0};
  uint16_t shiftBeforeMs = 50, shiftAfterMs = 100, startDelayMs = 3000;
  char wifiName[33] = "karute";
  char wifiPassword[64] = "autokarute";
  SpeedSettings alt = {5, 5, 0, 10};
  uint16_t altBeforeMs = 5, altReleaseMs = 5;
  uint8_t inputLedEnabled = 1, standbyLedEnabled = 1;
};
// Exact NVS layout from r8-r17; keep it for migration and rollback.
struct LegacyTypingSettingsV8 {
  uint32_t magic = kSettingsMagicV8;
  // Stable IDs: 4 GBK, 2 ENG, 1 VUC. Removed: 3 ASCII hybrid, 5 Unicode.
  uint8_t mode = 1;
  SpeedSettings vuc = {5, 20, 20, 20};
  SpeedSettings eng = {2, 2, 0, 0};
  uint16_t shiftBeforeMs = 50, shiftAfterMs = 100, startDelayMs = 3000;
  char wifiName[33] = "karute";
  char wifiPassword[64] = "autokarute";
  SpeedSettings alt = {5, 5, 0, 10};
  uint16_t altBeforeMs = 5, altReleaseMs = 5;
  uint8_t inputLedEnabled = 1, standbyLedEnabled = 1;
  uint32_t credentialReportCount = 0;
};
struct TypingSettings {
  uint32_t magic = kSettingsMagicV9;
  // Stable IDs: 4 GBK, 2 ENG, 1 VUC. Removed: 3 ASCII hybrid, 5 Unicode.
  uint8_t mode = 1;
  SpeedSettings vuc = {5, 20, 20, 20};
  SpeedSettings eng = {2, 2, 0, 0};
  uint16_t shiftBeforeMs = 50, shiftAfterMs = 100, startDelayMs = 3000;
  char wifiName[33] = "karute";
  char wifiPassword[64] = "autokarute";
  SpeedSettings alt = {5, 5, 0, 10};
  uint16_t altBeforeMs = 5, altReleaseMs = 5;
  uint8_t inputLedEnabled = 1, standbyLedEnabled = 1;
  uint32_t credentialReportCount = 0;
  // Store only the mDNS host label. The UI adds http:// and .local.
  char loginHost[64] = "karute";
};
static_assert(sizeof(LegacyTypingSettingsV2) == 124, "Preserve cfg2 layout");
static_assert(sizeof(LegacyTypingSettingsV3) == 128, "Preserve cfg3 layout");
static_assert(sizeof(LegacyTypingSettingsV4) == 136, "Preserve cfg4 layout");
static_assert(sizeof(LegacyTypingSettingsV5) == 140, "Preserve cfg5 layout");
static_assert(sizeof(LegacyTypingSettingsV6) == 140, "Preserve cfg6 layout");
static_assert(sizeof(LegacyTypingSettingsV7) == 140, "Preserve cfg7 layout");
static_assert(sizeof(LegacyTypingSettingsV8) == 144, "Preserve cfg8 layout");
static_assert(sizeof(TypingSettings) == 208, "Versioned cfg9 layout");
static_assert(std::is_standard_layout<TypingSettings>::value,
              "NVS settings must have a stable standard layout");
static_assert(std::is_trivially_copyable<TypingSettings>::value,
              "NVS settings must be safe for raw byte storage");
inline bool isAltMode(uint8_t mode) { return mode == 4; }
inline bool isValidMode(uint8_t mode) {
  return mode == 1 || mode == 2 || mode == 4;
}
inline bool validPrintableAscii(const char *value, size_t capacity,
                                size_t minimum, size_t maximum) {
  size_t length = 0;
  while (length < capacity && value[length] != '\0') {
    const uint8_t character = static_cast<uint8_t>(value[length]);
    if (character < 0x20 || character > 0x7e) return false;
    ++length;
  }
  return length < capacity && length >= minimum && length <= maximum;
}
inline bool validLocalHost(const char *value, size_t capacity) {
  size_t length = 0;
  while (length < capacity && value[length] != '\0') {
    const char character = value[length];
    const bool valid = (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') ||
                       character == '-';
    if (!valid) return false;
    ++length;
  }
  return length >= 1 && length <= 63 && length < capacity &&
         value[0] != '-' && value[length - 1] != '-';
}
inline bool validTypingSettings(const TypingSettings &s) {
  const auto inRange = [](uint16_t n) { return n >= 1 && n <= 100; };
  return s.magic == kSettingsMagicV9 && isValidMode(s.mode) &&
      inRange(s.vuc.keyDownMs) && inRange(s.vuc.interKeyMs) &&
      inRange(s.vuc.beforeSpaceMs) && inRange(s.vuc.afterSpaceMs) &&
      inRange(s.eng.keyDownMs) && inRange(s.eng.interKeyMs) &&
      inRange(s.alt.keyDownMs) && inRange(s.alt.interKeyMs) &&
      inRange(s.alt.afterSpaceMs) && inRange(s.altBeforeMs) &&
      inRange(s.altReleaseMs) && s.shiftBeforeMs >= 1 &&
      s.shiftBeforeMs <= 1000 && s.shiftAfterMs >= 1 &&
      s.shiftAfterMs <= 1000 && s.startDelayMs <= 10000 &&
      s.inputLedEnabled <= 1 && s.standbyLedEnabled <= 1 &&
      validPrintableAscii(s.wifiName, sizeof(s.wifiName), 1, 32) &&
      validPrintableAscii(s.wifiPassword, sizeof(s.wifiPassword), 8, 63) &&
      validLocalHost(s.loginHost, sizeof(s.loginHost));
}
inline bool migrateSettings(const LegacyTypingSettingsV3 &old, TypingSettings &s) {
  if (old.magic != kSettingsMagicV3 || old.mode < 1 || old.mode > 3) return false;
  TypingSettings candidate;
  candidate.mode = old.mode == 3 ? 1 : old.mode;
  candidate.vuc = old.vuc; candidate.eng = old.eng;
  candidate.shiftBeforeMs = old.shiftBeforeMs;
  candidate.shiftAfterMs = old.shiftAfterMs;
  candidate.startDelayMs = old.startDelayMs;
  if (!validTypingSettings(candidate)) return false;
  s = candidate;
  return true;
}
inline bool migrateSettings(const LegacyTypingSettingsV4 &old, TypingSettings &s) {
  if (old.magic != kSettingsMagicV4 || old.mode < 1 || old.mode > 5) return false;
  TypingSettings candidate;
  candidate.mode = old.mode == 3 ? 1 : old.mode == 5 ? 4 : old.mode;
  candidate.vuc = old.vuc; candidate.eng = old.eng;
  candidate.shiftBeforeMs = old.shiftBeforeMs;
  candidate.shiftAfterMs = old.shiftAfterMs;
  candidate.startDelayMs = old.startDelayMs;
  for (size_t i = 0; i < sizeof(candidate.wifiName); ++i)
    candidate.wifiName[i] = old.unusedWifiName[i];
  for (size_t i = 0; i < sizeof(candidate.wifiPassword); ++i)
    candidate.wifiPassword[i] = old.unusedWifiPassword[i];
  candidate.alt = old.alt;
  // Match the old sequence: initial Alt wait was down; final digit gap was gap.
  candidate.altBeforeMs = old.alt.keyDownMs;
  candidate.altReleaseMs = old.alt.interKeyMs;
  if (!validTypingSettings(candidate)) return false;
  s = candidate;
  return true;
}
inline bool migrateSettings(const LegacyTypingSettingsV5 &old, TypingSettings &s) {
  if (old.magic != kSettingsMagicV5 || !isValidMode(old.mode)) return false;
  TypingSettings candidate;
  candidate.mode = old.mode;
  candidate.vuc = old.vuc; candidate.eng = old.eng;
  candidate.shiftBeforeMs = old.shiftBeforeMs;
  candidate.shiftAfterMs = old.shiftAfterMs;
  candidate.startDelayMs = old.startDelayMs;
  for (size_t i = 0; i < sizeof(candidate.wifiName); ++i)
    candidate.wifiName[i] = old.unusedWifiName[i];
  for (size_t i = 0; i < sizeof(candidate.wifiPassword); ++i)
    candidate.wifiPassword[i] = old.unusedWifiPassword[i];
  candidate.alt = old.alt;
  candidate.altBeforeMs = old.altBeforeMs;
  candidate.altReleaseMs = old.altReleaseMs;
  if (!validTypingSettings(candidate)) return false;
  s = candidate;
  return true;
}
inline bool migrateSettings(const LegacyTypingSettingsV6 &old, TypingSettings &s) {
  if (old.magic != kSettingsMagicV6 || !isValidMode(old.mode)) return false;
  TypingSettings candidate;
  candidate.mode = old.mode;
  candidate.vuc = old.vuc; candidate.eng = old.eng;
  candidate.shiftBeforeMs = old.shiftBeforeMs;
  candidate.shiftAfterMs = old.shiftAfterMs;
  candidate.startDelayMs = old.startDelayMs;
  for (size_t i = 0; i < sizeof(candidate.wifiName); ++i)
    candidate.wifiName[i] = old.unusedWifiName[i];
  for (size_t i = 0; i < sizeof(candidate.wifiPassword); ++i)
    candidate.wifiPassword[i] = old.unusedWifiPassword[i];
  candidate.alt = old.alt;
  candidate.altBeforeMs = old.altBeforeMs;
  candidate.altReleaseMs = old.altReleaseMs;
  candidate.inputLedEnabled = old.inputLedEnabled;
  candidate.standbyLedEnabled = old.standbyLedEnabled;
  if (!validTypingSettings(candidate)) return false;
  s = candidate;
  return true;
}
inline bool migrateSettings(const LegacyTypingSettingsV7 &old, TypingSettings &s) {
  if (old.magic != kSettingsMagicV7 || !isValidMode(old.mode)) return false;
  TypingSettings candidate;
  candidate.mode = old.mode;
  candidate.vuc = old.vuc; candidate.eng = old.eng;
  candidate.shiftBeforeMs = old.shiftBeforeMs;
  candidate.shiftAfterMs = old.shiftAfterMs;
  candidate.startDelayMs = old.startDelayMs;
  for (size_t i = 0; i < sizeof(candidate.wifiName); ++i)
    candidate.wifiName[i] = old.wifiName[i];
  for (size_t i = 0; i < sizeof(candidate.wifiPassword); ++i)
    candidate.wifiPassword[i] = old.wifiPassword[i];
  candidate.alt = old.alt;
  candidate.altBeforeMs = old.altBeforeMs;
  candidate.altReleaseMs = old.altReleaseMs;
  candidate.inputLedEnabled = old.inputLedEnabled;
  candidate.standbyLedEnabled = old.standbyLedEnabled;
  candidate.credentialReportCount = 0;
  if (!validTypingSettings(candidate)) return false;
  s = candidate;
  return true;
}
inline bool migrateSettings(const LegacyTypingSettingsV8 &old, TypingSettings &s) {
  if (old.magic != kSettingsMagicV8 || !isValidMode(old.mode)) return false;
  TypingSettings candidate;
  candidate.mode = old.mode;
  candidate.vuc = old.vuc; candidate.eng = old.eng;
  candidate.shiftBeforeMs = old.shiftBeforeMs;
  candidate.shiftAfterMs = old.shiftAfterMs;
  candidate.startDelayMs = old.startDelayMs;
  for (size_t i = 0; i < sizeof(candidate.wifiName); ++i)
    candidate.wifiName[i] = old.wifiName[i];
  for (size_t i = 0; i < sizeof(candidate.wifiPassword); ++i)
    candidate.wifiPassword[i] = old.wifiPassword[i];
  candidate.alt = old.alt;
  candidate.altBeforeMs = old.altBeforeMs;
  candidate.altReleaseMs = old.altReleaseMs;
  candidate.inputLedEnabled = old.inputLedEnabled;
  candidate.standbyLedEnabled = old.standbyLedEnabled;
  candidate.credentialReportCount = old.credentialReportCount;
  if (!validTypingSettings(candidate)) return false;
  s = candidate;
  return true;
}
