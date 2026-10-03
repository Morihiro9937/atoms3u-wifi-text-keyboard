#include <assert.h>
#include <cstring>
#include <iostream>
#include "typing_settings.h"

int main() {
  TypingSettings settings;
  assert(validTypingSettings(settings));
  for (int mode : {1, 2, 4}) {
    settings.mode = mode;
    assert(validTypingSettings(settings));
    assert(isAltMode(mode) == (mode == 4));
  }
  for (int mode : {0, 3, 5, 6}) {
    auto bad = settings; bad.mode = mode;
    assert(!validTypingSettings(bad));
  }

  LegacyTypingSettingsV3 old3{};
  old3.magic = kSettingsMagicV3;
  old3.mode = 3;
  old3.vuc = {7, 23, 21, 25}; old3.eng = {3, 4, 0, 0};
  old3.shiftBeforeMs = 71; old3.shiftAfterMs = 143; old3.startDelayMs = 5000;
  assert(migrateSettings(old3, settings));
  assert(settings.mode == 1 && settings.magic == kSettingsMagicV9);
  assert(settings.vuc.keyDownMs == 7 && settings.vuc.interKeyMs == 23);
  assert(settings.vuc.beforeSpaceMs == 21 && settings.vuc.afterSpaceMs == 25);
  assert(settings.eng.keyDownMs == 3 && settings.eng.interKeyMs == 4);
  assert(settings.startDelayMs == 5000);
  assert(settings.altBeforeMs == 5 && settings.alt.keyDownMs == 5 &&
         settings.alt.interKeyMs == 5 && settings.altReleaseMs == 5 &&
         settings.alt.afterSpaceMs == 10);

  LegacyTypingSettingsV4 old4;
  old4.mode = 5;
  old4.alt = {8, 9, 0, 11};
  assert(migrateSettings(old4, settings));
  assert(settings.mode == 4 && settings.magic == kSettingsMagicV9);
  assert(settings.altBeforeMs == 8 && settings.alt.keyDownMs == 8);
  assert(settings.alt.interKeyMs == 9 && settings.altReleaseMs == 9);
  assert(settings.alt.afterSpaceMs == 11);

  LegacyTypingSettingsV5 old5;
  old5.mode = 2;
  old5.eng = {9, 12, 0, 0};
  old5.altBeforeMs = 13;
  old5.altReleaseMs = 14;
  assert(migrateSettings(old5, settings));
  assert(settings.mode == 2 && settings.magic == kSettingsMagicV9);
  assert(settings.eng.keyDownMs == 9 && settings.eng.interKeyMs == 12);
  assert(settings.altBeforeMs == 13 && settings.altReleaseMs == 14);
  assert(settings.inputLedEnabled == 1 && settings.standbyLedEnabled == 1);

  LegacyTypingSettingsV6 old6;
  old6.mode = 4;
  old6.inputLedEnabled = 0;
  old6.standbyLedEnabled = 1;
  std::strcpy(old6.unusedWifiName, "Ward-Keyboard");
  std::strcpy(old6.unusedWifiPassword, "safe-pass-123");
  assert(migrateSettings(old6, settings));
  assert(settings.magic == kSettingsMagicV9 && settings.mode == 4);
  assert(settings.inputLedEnabled == 0 && settings.standbyLedEnabled == 1);
  assert(std::strcmp(settings.wifiName, "Ward-Keyboard") == 0);
  assert(std::strcmp(settings.wifiPassword, "safe-pass-123") == 0);
  assert(settings.credentialReportCount == 0);

  LegacyTypingSettingsV7 old7;
  old7.mode = 2;
  old7.inputLedEnabled = 1;
  old7.standbyLedEnabled = 0;
  std::strcpy(old7.wifiName, "karute-r7");
  std::strcpy(old7.wifiPassword, "r7-password");
  assert(migrateSettings(old7, settings));
  assert(settings.magic == kSettingsMagicV9 && settings.mode == 2);
  assert(std::strcmp(settings.wifiName, "karute-r7") == 0);
  assert(std::strcmp(settings.wifiPassword, "r7-password") == 0);
  assert(settings.credentialReportCount == 0);

  LegacyTypingSettingsV8 old8;
  old8.mode = 4;
  old8.credentialReportCount = 27;
  std::strcpy(old8.wifiName, "karute-r8");
  assert(migrateSettings(old8, settings));
  assert(settings.magic == kSettingsMagicV9 && settings.mode == 4);
  assert(std::strcmp(settings.wifiName, "karute-r8") == 0);
  assert(std::strcmp(settings.loginHost, "karute") == 0);
  assert(settings.credentialReportCount == 27);

  {
    auto bad = settings;
    std::strcpy(bad.wifiPassword, "short");
    assert(!validTypingSettings(bad));
    bad = settings;
    bad.wifiName[0] = '\n';
    assert(!validTypingSettings(bad));
    bad = settings;
    for (size_t i = 0; i < sizeof(bad.wifiName); ++i) bad.wifiName[i] = 'x';
    assert(!validTypingSettings(bad));
    bad = settings;
    std::strcpy(bad.loginHost, "-karute");
    assert(!validTypingSettings(bad));
    bad = settings;
    std::strcpy(bad.loginHost, "karute.local");
    assert(!validTypingSettings(bad));
    bad = settings;
    std::strcpy(bad.loginHost, "ward-keyboard-2");
    assert(validTypingSettings(bad));
  }

  for (auto invalid : {0, 101}) {
    auto bad = settings; bad.altBeforeMs = invalid;
    assert(!validTypingSettings(bad));
    bad = settings; bad.alt.keyDownMs = invalid;
    assert(!validTypingSettings(bad));
    bad = settings; bad.alt.interKeyMs = invalid;
    assert(!validTypingSettings(bad));
    bad = settings; bad.altReleaseMs = invalid;
    assert(!validTypingSettings(bad));
    bad = settings; bad.alt.afterSpaceMs = invalid;
    assert(!validTypingSettings(bad));
  }
  for (auto invalid : {2, 255}) {
    auto bad = settings; bad.inputLedEnabled = invalid;
    assert(!validTypingSettings(bad));
    bad = settings; bad.standbyLedEnabled = invalid;
    assert(!validTypingSettings(bad));
  }
  std::cout << "PASS: cfg2-cfg8 migration, local URL, Wi-Fi audit, modes, timings and LEDs\n";
}
