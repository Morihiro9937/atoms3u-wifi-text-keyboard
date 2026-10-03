#include <Arduino.h>
#include <cstring>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include "esp32-hal-rgb-led.h"
#include "esp_netif.h"
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "alt_encoding.h"
#include "karute_web_assets.h"
#include "led_policy.h"
#include "typing_settings.h"

namespace {

constexpr size_t kMaxTextBytes = 32 * 1024;
constexpr char kFirmwareVersion[] = "2026.10.02-r19-progress-sync";
constexpr char kDefaultAccessPointName[] = "karute";
constexpr char kDefaultAccessPointPassword[] = "autokarute";
constexpr char kDefaultLoginHost[] = "karute";
constexpr char kBackupLoginUrl[] = "http://192.168.4.1";
constexpr char kTextPath[] = "/body.txt";
constexpr char kTempTextPath[] = "/body.tmp";
constexpr uint8_t kUserButtonPin = 41;
constexpr uint8_t kStatusLedPin = 35;

constexpr uint32_t kDebounceMs = 35;
constexpr uint32_t kStopHoldMs = 1000;
constexpr uint32_t kWifiReportHoldMs = 3000;
constexpr uint32_t kShortcutKeyDownMs = 80;
constexpr uint32_t kRunDialogOpenWaitMs = 800;
constexpr uint32_t kNotepadOpenWaitMs = 3000;
constexpr uint32_t kLinkHeartbeatTimeoutMs = 6000;


// iPhone should use this Wi-Fi only for karute's local page.  By omitting the
// DHCP Router option, iOS can keep public Internet traffic on cellular data.
bool disableSoftApRouterAdvertisement() {
  esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (apNetif == nullptr) return false;

  esp_err_t result = esp_netif_dhcps_stop(apNetif);
  if (result != ESP_OK &&
      result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
    return false;
  }

  uint8_t advertiseRouter = 0;
  result = esp_netif_dhcps_option(
      apNetif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
      &advertiseRouter, sizeof(advertiseRouter));
  if (result != ESP_OK) {
    esp_netif_dhcps_start(apNetif);  // Best effort: keep the web page usable.
    return false;
  }

  result = esp_netif_dhcps_start(apNetif);
  return result == ESP_OK ||
         result == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED;
}

enum class DeviceState { EMPTY, READY, TYPING, PAUSED, STOPPED, DONE, ERROR };
enum class LedEvent { NONE, BOOT };

USBHIDKeyboard keyboard;
USBHID altTransport;  // Uses the same single keyboard interface, no new device.
WebServer webServer(80);
TypingSettings typingSettings;

String currentText;
size_t currentCharacterCount = 0;
DeviceState deviceState = DeviceState::EMPTY;

bool stableButtonPressed = false;
bool lastRawButtonPressed = false;
uint32_t rawStateChangedAtMs = 0;
bool startRequested = false;
bool abortRequested = false;
bool pauseRequested = false;
bool resumeRequested = false;
bool credentialReportRequested = false;
bool credentialReportActive = false;
size_t typingIndex = 0;
size_t completedCharacters = 0;
uint32_t runEstimatedMs = 0;
uint32_t startDelayEndsAtMs = 0;
bool hasPrintProgress = false;
bool typingButtonArmed = false;
bool typingStopTracking = false;
uint32_t typingStopPressedAtMs = 0;
bool ignoreButtonUntilRelease = false;
uint32_t buttonPressedAtMs = 0;
bool startDelayActive = false;
bool typingIndicatorKeyDown = false;
bool altCharacterActive = false;
bool altReleasePending = false;
uint32_t altReleaseRetryAt = 0;
volatile bool hostLedsKnown = false;
volatile uint8_t hostLeds = 0;
String inputError;
bool storageReady = false;
bool storagePending = false;
uint32_t editorRevision = 0;
uint32_t savedRevision = 0;
LedEvent ledEvent = LedEvent::BOOT;
uint32_t ledEventStartedAtMs = 0;
bool wifiRestartPending = false;
uint32_t wifiRestartAtMs = 0;
String activeWifiName = kDefaultAccessPointName;
String activeWifiPassword = kDefaultAccessPointPassword;
String activeLoginHost = kDefaultLoginHost;
uint32_t activeCredentialReportCount = 0;
bool linkHeartbeatSeen = false;
uint32_t lastLinkHeartbeatAtMs = 0;

struct Utf8CountResult {
  bool valid;
  size_t characters;
};

Utf8CountResult validateAndCountUtf8(const String &text) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(text.c_str());
  const size_t length = text.length();
  size_t index = 0;
  size_t characters = 0;

  while (index < length) {
    const uint8_t first = bytes[index];
    size_t sequenceLength = 0;

    if (first <= 0x7F) {
      sequenceLength = 1;
    } else if (first >= 0xC2 && first <= 0xDF) {
      sequenceLength = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
      sequenceLength = 3;
    } else if (first >= 0xF0 && first <= 0xF4) {
      sequenceLength = 4;
    } else {
      return {false, 0};
    }

    if (index + sequenceLength > length) {
      return {false, 0};
    }

    for (size_t offset = 1; offset < sequenceLength; ++offset) {
      if ((bytes[index + offset] & 0xC0) != 0x80) {
        return {false, 0};
      }
    }

    // Reject overlong three/four-byte forms, UTF-16 surrogates, and values
    // above the Unicode maximum U+10FFFF.
    if (sequenceLength == 3 &&
        ((first == 0xE0 && bytes[index + 1] < 0xA0) ||
         (first == 0xED && bytes[index + 1] >= 0xA0))) {
      return {false, 0};
    }

    if (sequenceLength == 4 &&
        ((first == 0xF0 && bytes[index + 1] < 0x90) ||
         (first == 0xF4 && bytes[index + 1] > 0x8F))) {
      return {false, 0};
    }

    index += sequenceLength;
    ++characters;
  }

  return {true, characters};
}

// Write a replacement file first. A failed write leaves the previous body
// intact; LittleFS rename swaps the complete file into place.
bool persistText(const String &text) {
  if (!storageReady) return false;
  if (text.isEmpty()) {
    LittleFS.remove(kTempTextPath);
    return !LittleFS.exists(kTextPath) || LittleFS.remove(kTextPath);
  }
  File file = LittleFS.open(kTempTextPath, "w");
  if (!file) return false;
  const size_t written = file.write(
      reinterpret_cast<const uint8_t *>(text.c_str()), text.length());
  file.flush();
  file.close();
  if (written != text.length()) {
    LittleFS.remove(kTempTextPath);
    return false;
  }
  if (!LittleFS.rename(kTempTextPath, kTextPath)) {
    LittleFS.remove(kTempTextPath);
    return false;
  }
  return true;
}

bool loadStoredText() {
  if (!storageReady) return false;
  if (!LittleFS.exists(kTextPath)) return true;
  File file = LittleFS.open(kTextPath, "r");
  if (!file || file.size() > kMaxTextBytes) return false;
  String loaded;
  if (!loaded.reserve(file.size())) return false;
  while (file.available()) loaded += static_cast<char>(file.read());
  if (loaded.length() != file.size()) return false;
  const Utf8CountResult count = validateAndCountUtf8(loaded);
  if (!count.valid) return false;
  currentText = loaded;
  currentCharacterCount = count.characters;
  deviceState = currentText.isEmpty() ? DeviceState::EMPTY : DeviceState::READY;
  return true;
}

bool initializeStorage() {
  Preferences preferences;
  if (!preferences.begin("karute", false)) return false;
  const bool initializedBefore = preferences.getBool("fsinit", false);
  // The old RAM-only firmware never used this partition. Format it on the
  // first upgrade only; a later mount error must not erase a stored draft.
  const bool mounted = LittleFS.begin(!initializedBefore);
  if (mounted && !initializedBefore) preferences.putBool("fsinit", true);
  preferences.end();
  return mounted;
}

const char *stateName(DeviceState state) {
  switch (state) {
    case DeviceState::EMPTY:
      return "EMPTY";
    case DeviceState::READY:
      return "READY";
    case DeviceState::TYPING:
      return "TYPING";
    case DeviceState::PAUSED:
      return "PAUSED";
    case DeviceState::STOPPED:
      return "STOPPED";
    case DeviceState::DONE:
      return "DONE";
    case DeviceState::ERROR:
      return "ERROR";
  }
  return "ERROR";
}

void writeStatusLed(uint8_t red, uint8_t green, uint8_t blue) {
  static uint8_t lastRed = 255;
  static uint8_t lastGreen = 255;
  static uint8_t lastBlue = 255;
  if (red == lastRed && green == lastGreen && blue == lastBlue) {
    return;
  }
  neopixelWrite(kStatusLedPin, red, green, blue);
  lastRed = red;
  lastGreen = green;
  lastBlue = blue;
}

void updateStatusLed() {
  static DeviceState observedState = DeviceState::EMPTY;
  static uint32_t stateChangedAtMs = 0;
  static bool linkStateKnown = false;
  static bool linkWasActive = false;
  static uint32_t linkLostAtMs = 0;
  const uint32_t now = millis();

  if (observedState != deviceState) {
    observedState = deviceState;
    stateChangedAtMs = now;
  }

  // Printing and standby lights have independent persistent switches. While
  // printing, no status color is allowed to override the sampled green key.
  if (deviceState == DeviceState::TYPING) {
    if (!typingSettings.inputLedEnabled) {
      writeStatusLed(0, 0, 0);
    } else if (startDelayActive) {
      const bool lit = ((now / 350) % 2) == 0;
      writeStatusLed(0, lit ? 14 : 2, 0);
    } else {
      writeStatusLed(0, typingIndicatorKeyDown ? 16 : 0, 0);
    }
    return;
  }

  if (!typingSettings.standbyLedEnabled) {
    writeStatusLed(0, 0, 0);
    return;
  }

  if (ledEvent == LedEvent::BOOT) {
    if ((now - ledEventStartedAtMs) < 500) {
      writeStatusLed(18, 18, 18);
      return;
    }
    ledEvent = LedEvent::NONE;
  }

  if (storagePending && deviceState != DeviceState::TYPING &&
      deviceState != DeviceState::PAUSED && deviceState != DeviceState::ERROR) {
    writeStatusLed(14, 9, 0);
    return;
  }

  const bool standbyState = deviceState == DeviceState::EMPTY ||
                            deviceState == DeviceState::READY ||
                            deviceState == DeviceState::DONE;
  if (standbyState) {
    const bool linkActive = linkHeartbeatSeen &&
        (now - lastLinkHeartbeatAtMs) <= kLinkHeartbeatTimeoutMs;
    if (!linkStateKnown || (linkWasActive && !linkActive)) {
      linkLostAtMs = now;
    }
    linkStateKnown = true;
    linkWasActive = linkActive;
    if (linkActive) {
      writeStatusLed(0, 14, 0);
    } else if (karute_led::wifiDisconnectedRed(
                   now - linkLostAtMs)) {
      writeStatusLed(18, 0, 0);
    } else {
      writeStatusLed(0, 14, 0);
    }
    return;
  }

  switch (deviceState) {
    case DeviceState::EMPTY:
    case DeviceState::READY:
      break;  // Standby states are handled by Wi-Fi connection status above.
    case DeviceState::TYPING:
      writeStatusLed(0, 0, 0);  // Handled before standby status colors.
      break;
    case DeviceState::PAUSED:
      writeStatusLed(0, 3, 16);
      break;
    case DeviceState::STOPPED:
      writeStatusLed(18, 0, 0);
      break;
    case DeviceState::DONE:
      break;  // Standby states are handled by Wi-Fi connection status above.
    case DeviceState::ERROR: {
      const uint32_t elapsed = now - stateChangedAtMs;
      const bool lit = elapsed < 180 || (elapsed >= 320 && elapsed < 500) ||
                       elapsed >= 650;
      writeStatusLed(lit ? 18 : 0, 0, 0);
      break;
    }
  }
}

uint32_t estimateCurrentTextMs();

const SpeedSettings &activeSpeed() {
  if (isAltMode(typingSettings.mode)) return typingSettings.alt;
  return typingSettings.mode == 2 ? typingSettings.eng : typingSettings.vuc;
}

bool persistTypingSettings(const TypingSettings &updated);

void loadTypingSettings() {
  Preferences preferences;
  if (!preferences.begin("karute", true)) return;

  TypingSettings loaded;
  bool migratedLegacySettings = false;
  if (preferences.getBytesLength("cfg9") == sizeof(loaded) &&
      preferences.getBytes("cfg9", &loaded, sizeof(loaded)) == sizeof(loaded) &&
      validTypingSettings(loaded)) {
    typingSettings = loaded;
  } else {
    LegacyTypingSettingsV8 legacy8{};
    LegacyTypingSettingsV7 legacy7{};
    LegacyTypingSettingsV6 legacy6{};
    LegacyTypingSettingsV5 legacy5{};
    LegacyTypingSettingsV4 legacy4{};
    LegacyTypingSettingsV3 legacy3{};
    LegacyTypingSettingsV2 legacy{};
    if (preferences.getBytesLength("cfg8") == sizeof(legacy8) &&
        preferences.getBytes("cfg8", &legacy8, sizeof(legacy8)) ==
            sizeof(legacy8) && migrateSettings(legacy8, loaded)) {
      typingSettings = loaded;
      migratedLegacySettings = true;
    } else if (preferences.getBytesLength("cfg7") == sizeof(legacy7) &&
        preferences.getBytes("cfg7", &legacy7, sizeof(legacy7)) ==
            sizeof(legacy7) && migrateSettings(legacy7, loaded)) {
      typingSettings = loaded;
      migratedLegacySettings = true;
    } else if (preferences.getBytesLength("cfg6") == sizeof(legacy6) &&
        preferences.getBytes("cfg6", &legacy6, sizeof(legacy6)) ==
            sizeof(legacy6) && migrateSettings(legacy6, loaded)) {
      typingSettings = loaded;
      migratedLegacySettings = true;
    } else if (preferences.getBytesLength("cfg5") == sizeof(legacy5) &&
        preferences.getBytes("cfg5", &legacy5, sizeof(legacy5)) ==
            sizeof(legacy5) && migrateSettings(legacy5, loaded)) {
      typingSettings = loaded;
      migratedLegacySettings = true;
    } else if (preferences.getBytesLength("cfg4") == sizeof(legacy4) &&
        preferences.getBytes("cfg4", &legacy4, sizeof(legacy4)) ==
            sizeof(legacy4) && migrateSettings(legacy4, loaded)) {
      typingSettings = loaded;
      migratedLegacySettings = true;
    } else if (preferences.getBytesLength("cfg3") == sizeof(legacy3) &&
        preferences.getBytes("cfg3", &legacy3, sizeof(legacy3)) == sizeof(legacy3) &&
        migrateSettings(legacy3, loaded)) {
      typingSettings = loaded;
      migratedLegacySettings = true;
    } else if (preferences.getBytesLength("cfg2") == sizeof(legacy) &&
        preferences.getBytes("cfg2", &legacy, sizeof(legacy)) ==
            sizeof(legacy) &&
        legacy.magic == kSettingsMagicV2 &&
        (legacy.mode == 1 || legacy.mode == 2)) {
      TypingSettings migrated;
      migrated.mode = legacy.mode;
      migrated.vuc = legacy.vuc;
      migrated.eng = legacy.eng;
      migrated.startDelayMs = legacy.startDelayMs;
      if (validTypingSettings(migrated)) {
        typingSettings = migrated;
        migratedLegacySettings = true;
      }
    }
  }
  preferences.end();

  if (migratedLegacySettings) persistTypingSettings(typingSettings);
}

bool persistTypingSettings(const TypingSettings &updated) {
  Preferences preferences;
  if (!preferences.begin("karute", false)) return false;
  const bool success = preferences.putBytes("cfg9", &updated,
                                           sizeof(updated)) == sizeof(updated);
  preferences.end();
  return success;
}

void copyCString(const char *source, char *destination, size_t capacity) {
  if (capacity == 0) return;
  size_t index = 0;
  while (index + 1 < capacity && source[index] != '\0') {
    destination[index] = source[index];
    ++index;
  }
  destination[index] = '\0';
}

bool copyValidatedWifiValue(const String &value, char *destination,
                            size_t capacity, size_t minimum,
                            size_t maximum) {
  if (value.length() < minimum || value.length() > maximum ||
      value.length() >= capacity) {
    return false;
  }
  for (size_t index = 0; index < value.length(); ++index) {
    const uint8_t character = static_cast<uint8_t>(value[index]);
    if (character < 0x20 || character > 0x7e) return false;
  }
  value.toCharArray(destination, capacity);
  return true;
}

String loginUrlForHost(const char *host) {
  String url = F("http://");
  url += host;
  url += F(".local");
  return url;
}

bool parseLocalLoginUrl(const String &value, char *destination,
                        size_t capacity) {
  constexpr char kPrefix[] = "http://";
  constexpr char kSuffix[] = ".local";
  if (!value.startsWith(kPrefix) || !value.endsWith(kSuffix)) return false;
  String host = value.substring(strlen(kPrefix),
                                value.length() - strlen(kSuffix));
  host.toLowerCase();
  if (host.length() < 1 || host.length() > 63 ||
      host.length() >= capacity) {
    return false;
  }
  host.toCharArray(destination, capacity);
  return validLocalHost(destination, capacity);
}

String jsonString(const char *value) {
  String result;
  result.reserve(strlen(value) + 4);
  result += '"';
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    if (static_cast<uint8_t>(*cursor) < 0x20) {
      char escaped[7];
      snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<uint8_t>(*cursor));
      result += escaped;
      continue;
    }
    if (*cursor == '"' || *cursor == '\\') result += '\\';
    result += *cursor;
  }
  result += '"';
  return result;
}

String wifiJson() {
  String json;
  json.reserve(260);
  json += F("{\"wifiName\":");
  json += jsonString(activeWifiName.c_str());
  json += F(",\"password\":");
  json += jsonString(activeWifiPassword.c_str());
  json += F(",\"loginUrl\":");
  const String loginUrl = loginUrlForHost(activeLoginHost.c_str());
  json += jsonString(loginUrl.c_str());
  json += F(",\"backupUrl\":\"");
  json += kBackupLoginUrl;
  json += F("\"}");
  return json;
}

bool startAccessPoint(const char *name, const char *password,
                      const char *loginHost) {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAP(name, password, 6, false, 1)) return false;
  activeWifiName = name;
  activeWifiPassword = password;
  activeLoginHost = loginHost;
  if (!disableSoftApRouterAdvertisement()) return false;
  if (MDNS.begin(loginHost)) {
    MDNS.addService("http", "tcp", 80);
  } else {
    inputError = F("Local login URL unavailable; use the backup URL.");
  }
  return true;
}

void scheduleAccessPointRestart() {
  wifiRestartPending = true;
  wifiRestartAtMs = millis() + 1500;
}

void restartConfiguredAccessPoint() {
  wifiRestartPending = false;
  const String previousName = activeWifiName;
  const String previousPassword = activeWifiPassword;
  const String previousLoginHost = activeLoginHost;
  const uint32_t previousReportCount = activeCredentialReportCount;
  MDNS.end();
  WiFi.softAPdisconnect(true);
  delay(40);
  if (startAccessPoint(typingSettings.wifiName,
                       typingSettings.wifiPassword,
                       typingSettings.loginHost)) {
    activeCredentialReportCount = typingSettings.credentialReportCount;
    return;
  }

  // A valid setting can still fail because of a transient radio error. Keep
  // the last known network reachable and make storage match what is emitted.
  WiFi.softAPdisconnect(true);
  delay(40);
  MDNS.end();
  if (startAccessPoint(previousName.c_str(), previousPassword.c_str(),
                       previousLoginHost.c_str())) {
    copyCString(previousName.c_str(), typingSettings.wifiName,
                sizeof(typingSettings.wifiName));
    copyCString(previousPassword.c_str(), typingSettings.wifiPassword,
                sizeof(typingSettings.wifiPassword));
    copyCString(previousLoginHost.c_str(), typingSettings.loginHost,
                sizeof(typingSettings.loginHost));
    typingSettings.credentialReportCount = previousReportCount;
    activeCredentialReportCount = previousReportCount;
    persistTypingSettings(typingSettings);
    inputError = F("Wi-Fi restart failed; previous settings restored.");
    return;
  }

  WiFi.softAPdisconnect(true);
  delay(40);
  MDNS.end();
  if (startAccessPoint(kDefaultAccessPointName,
                       kDefaultAccessPointPassword,
                       kDefaultLoginHost)) {
    copyCString(kDefaultAccessPointName, typingSettings.wifiName,
                sizeof(typingSettings.wifiName));
    copyCString(kDefaultAccessPointPassword, typingSettings.wifiPassword,
                sizeof(typingSettings.wifiPassword));
    copyCString(kDefaultLoginHost, typingSettings.loginHost,
                sizeof(typingSettings.loginHost));
    typingSettings.credentialReportCount =
        previousName == kDefaultAccessPointName &&
        previousPassword == kDefaultAccessPointPassword &&
        previousLoginHost == kDefaultLoginHost
            ? previousReportCount : 0;
    activeCredentialReportCount = typingSettings.credentialReportCount;
    persistTypingSettings(typingSettings);
    inputError = F("Wi-Fi restart failed; defaults restored.");
    return;
  }
  deviceState = DeviceState::ERROR;
  inputError = F("Wi-Fi access point failed to start.");
}

String settingsJson() {
  String json;
  json.reserve(400);
  json += F("{\"mode\":");
  json += typingSettings.mode;
  json += F(",\"vucDownMs\":");
  json += typingSettings.vuc.keyDownMs;
  json += F(",\"vucIntervalMs\":");
  json += typingSettings.vuc.interKeyMs;
  json += F(",\"vucBeforeSpaceMs\":");
  json += typingSettings.vuc.beforeSpaceMs;
  json += F(",\"vucAfterSpaceMs\":");
  json += typingSettings.vuc.afterSpaceMs;
  json += F(",\"engDownMs\":");
  json += typingSettings.eng.keyDownMs;
  json += F(",\"engIntervalMs\":");
  json += typingSettings.eng.interKeyMs;
  json += F(",\"altBeforeMs\":");
  json += typingSettings.altBeforeMs;
  json += F(",\"altDownMs\":");
  json += typingSettings.alt.keyDownMs;
  json += F(",\"altIntervalMs\":");
  json += typingSettings.alt.interKeyMs;
  json += F(",\"altReleaseMs\":");
  json += typingSettings.altReleaseMs;
  json += F(",\"altAfterMs\":");
  json += typingSettings.alt.afterSpaceMs;
  json += F(",\"startDelayMs\":");
  json += typingSettings.startDelayMs;
  json += F(",\"inputLedEnabled\":");
  json += typingSettings.inputLedEnabled ? F("true") : F("false");
  json += F(",\"standbyLedEnabled\":");
  json += typingSettings.standbyLedEnabled ? F("true") : F("false");
  json += '}';
  return json;
}

String statusJson(bool compact = false) {
  String json;
  json.reserve(480);
  json += F("{\"firmwareVersion\":\"");
  json += kFirmwareVersion;
  json += F("\",\"state\":\"");
  json += stateName(deviceState);
  json += F("\",\"bytes\":");
  json += currentText.length();
  json += F(",\"characters\":");
  json += currentCharacterCount;
  json += F(",\"storageReady\":");
  json += storageReady ? F("true") : F("false");
  json += F(",\"storagePending\":");
  json += storagePending ? F("true") : F("false");
  json += F(",\"revision\":");
  json += editorRevision;
  json += F(",\"savedRevision\":");
  json += savedRevision;
  json += F(",\"maxBytes\":");
  json += kMaxTextBytes;
  json += F(",\"mode\":");
  json += typingSettings.mode;
  if (!compact) {
    json += F(",\"keyDownMs\":");
    json += activeSpeed().keyDownMs;
    json += F(",\"interKeyMs\":");
    json += activeSpeed().interKeyMs;
    json += F(",\"beforeSpaceMs\":");
    json += activeSpeed().beforeSpaceMs;
    json += F(",\"afterSpaceMs\":");
    json += activeSpeed().afterSpaceMs;
    json += F(",\"vucDownMs\":");
    json += typingSettings.vuc.keyDownMs;
    json += F(",\"vucIntervalMs\":");
    json += typingSettings.vuc.interKeyMs;
    json += F(",\"vucBeforeSpaceMs\":");
    json += typingSettings.vuc.beforeSpaceMs;
    json += F(",\"vucAfterSpaceMs\":");
    json += typingSettings.vuc.afterSpaceMs;
    json += F(",\"engDownMs\":");
    json += typingSettings.eng.keyDownMs;
    json += F(",\"engIntervalMs\":");
    json += typingSettings.eng.interKeyMs;
    json += F(",\"altBeforeMs\":");
    json += typingSettings.altBeforeMs;
    json += F(",\"altDownMs\":");
    json += typingSettings.alt.keyDownMs;
    json += F(",\"altIntervalMs\":");
    json += typingSettings.alt.interKeyMs;
    json += F(",\"altReleaseMs\":");
    json += typingSettings.altReleaseMs;
    json += F(",\"altAfterMs\":");
    json += typingSettings.alt.afterSpaceMs;
    json += F(",\"inputLedEnabled\":");
    json += typingSettings.inputLedEnabled ? F("true") : F("false");
    json += F(",\"standbyLedEnabled\":");
    json += typingSettings.standbyLedEnabled ? F("true") : F("false");
  }
  // Messages are internally generated, never composed from the raw body.
  json += F(",\"inputError\":\"");
  json += inputError;
  json += '"';
  json += F(",\"startDelayMs\":");
  json += typingSettings.startDelayMs;
  if (!compact) {
    json += F(",\"estimatedMs\":");
    json += estimateCurrentTextMs();
  }
  const size_t remaining = hasPrintProgress && currentCharacterCount > completedCharacters
      ? currentCharacterCount - completedCharacters : 0;
  uint32_t remainingMs = hasPrintProgress && currentCharacterCount
      ? static_cast<uint32_t>((static_cast<uint64_t>(runEstimatedMs) * remaining +
          currentCharacterCount - 1) / currentCharacterCount) : 0;
  if (startDelayActive) {
    const int32_t delayLeft = static_cast<int32_t>(startDelayEndsAtMs - millis());
    if (delayLeft > 0) remainingMs += static_cast<uint32_t>(delayLeft);
  } else if (startRequested) remainingMs += typingSettings.startDelayMs;
  json += F(",\"hasPrintProgress\":");
  json += hasPrintProgress ? F("true") : F("false");
  json += F(",\"startPending\":");
  json += startRequested ? F("true") : F("false");
  json += F(",\"progressPercent\":");
  json += hasPrintProgress && currentCharacterCount
      ? static_cast<unsigned>((static_cast<uint64_t>(completedCharacters) * 100) /
          currentCharacterCount) : 0;
  json += F(",\"remainingCharacters\":");
  json += remaining;
  json += F(",\"remainingMs\":");
  json += remainingMs;
  json += '}';
  return json;
}

bool parseUnsignedSetting(const String &value, uint16_t minimum,
                          uint16_t maximum, uint16_t &result) {
  if (value.isEmpty()) {
    return false;
  }
  for (size_t index = 0; index < value.length(); ++index) {
    if (value[index] < '0' || value[index] > '9') {
      return false;
    }
  }
  const unsigned long parsed = value.toInt();
  if (parsed < minimum || parsed > maximum) {
    return false;
  }
  result = static_cast<uint16_t>(parsed);
  return true;
}

bool parseToggleSetting(const String &value, uint8_t &result) {
  if (value == "0") {
    result = 0;
    return true;
  }
  if (value == "1") {
    result = 1;
    return true;
  }
  return false;
}

bool parseRevision(const String &value, uint32_t &revision) {
  if (value.isEmpty()) return false;
  for (size_t index = 0; index < value.length(); ++index) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  char *end = nullptr;
  const unsigned long parsed = strtoul(value.c_str(), &end, 10);
  if (*end != '\0' || parsed == 0) return false;
  revision = static_cast<uint32_t>(parsed);
  return true;
}

void sendNoCache(int code, const char *contentType, const String &body) {
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(code, contentType, body);
}

void sendGzipPage(const uint8_t *data, size_t size) {
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.sendHeader("Content-Encoding", "gzip");
  webServer.sendHeader("Vary", "Accept-Encoding");
  webServer.send_P(200, "text/html; charset=utf-8",
                   reinterpret_cast<PGM_P>(data), size);
}

void checkPhysicalStopButton() {
  const bool pressed = (digitalRead(kUserButtonPin) == LOW);

  // A button press that started the typing must first be released. This
  // prevents holding the start press for a moment from also stopping it.
  if (!typingButtonArmed) {
    if (!pressed) {
      typingButtonArmed = true;
    }
    return;
  }

  if (!pressed) {
    typingStopTracking = false;
    return;
  }

  if (!typingStopTracking) {
    typingStopTracking = true;
    typingStopPressedAtMs = millis();
    return;
  }

  if ((millis() - typingStopPressedAtMs) >= kStopHoldMs) {
    abortRequested = true;
  }
}

// Keep the local page responsive and watch for stop requests during the
// deliberately slow keyboard output. False means the current run was stopped.
bool waitWhileServing(uint32_t milliseconds) {
  const uint32_t startedAt = millis();
  while ((millis() - startedAt) < milliseconds) {
    webServer.handleClient();
    checkPhysicalStopButton();
    updateStatusLed();
    if (abortRequested && !altCharacterActive) {
      return false;
    }
    delay(1);
  }
  return !abortRequested || altCharacterActive;
}

bool decodeNextCodePoint(const String &text, size_t &index,
                         uint32_t &codePoint) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(text.c_str());
  const size_t length = text.length();
  if (index >= length) {
    return false;
  }

  const uint8_t first = bytes[index++];
  if (first <= 0x7F) {
    codePoint = first;
    return true;
  }

  size_t continuationCount = 0;
  if (first >= 0xC2 && first <= 0xDF) {
    continuationCount = 1;
    codePoint = first & 0x1F;
  } else if (first >= 0xE0 && first <= 0xEF) {
    continuationCount = 2;
    codePoint = first & 0x0F;
  } else if (first >= 0xF0 && first <= 0xF4) {
    continuationCount = 3;
    codePoint = first & 0x07;
  } else {
    return false;
  }

  if (index + continuationCount > length) {
    return false;
  }
  for (size_t count = 0; count < continuationCount; ++count) {
    const uint8_t next = bytes[index++];
    if ((next & 0xC0) != 0x80) {
      return false;
    }
    codePoint = (codePoint << 6) | (next & 0x3F);
  }

  if ((continuationCount == 1 && codePoint < 0x80) ||
      (continuationCount == 2 && codePoint < 0x800) ||
      (continuationCount == 3 && codePoint < 0x10000) ||
      (codePoint >= 0xD800 && codePoint <= 0xDFFF) ||
      codePoint > 0x10FFFF) {
    return false;
  }
  return true;
}

// Preflight the WHOLE body before any Alt character is emitted. Unlike ENG,
// the experimental modes never silently omit an unrepresentable character.
bool validateAltText(String &error) {
  if (!isAltMode(typingSettings.mode)) return true;
  size_t index = 0, position = 0;
  while (index < currentText.length()) {
    uint32_t cp;
    if (!decodeNextCodePoint(currentText, index, cp)) {
      error = F("UTF-8格式错误，未开始打印。"); return false;
    }
    ++position;
    uint16_t encoded;
    if (!karute_alt::outputCode(karute_alt::Mode::GBK, cp, encoded)) {
      char detail[110];
      snprintf(detail, sizeof(detail),
               "第%u个字符 U+%04lX 不支持当前模式，整篇未打印。",
               static_cast<unsigned>(position), static_cast<unsigned long>(cp));
      error = detail; return false;
    }
  }
  return true;
}

bool altTransportReady(String &error) {
  if (!altTransport.ready() || altReleasePending) {
    error = F("USB键盘未就绪，请插入Windows测试机。"); return false;
  }
  if (hostLedsKnown && !(hostLeds & LED_NUMLOCK)) {
    error = F("请开启电脑的Num Lock，再打印。"); return false;
  }
  return true;
}

bool altStartReady(String &error) {
  if (!validateAltText(error)) return false;
  if (!isAltMode(typingSettings.mode)) return true;
  return altTransportReady(error);
}

bool sendAltReport(uint8_t modifier, uint8_t usage) {
  KeyReport report = {};
  static_assert(sizeof(report) == 8, "Standard keyboard report size");
  report.modifiers = modifier;
  report.keys[0] = usage;
  const bool success = altTransport.SendReport(HID_REPORT_ID_KEYBOARD,
                                               &report, sizeof(report), 100);
  if (!success) altReleasePending = true;
  else if (modifier == 0 && usage == 0) altReleasePending = false;
  const uint8_t indicatorMode = credentialReportActive ? 4
                                                       : typingSettings.mode;
  typingIndicatorKeyDown = typingSettings.inputLedEnabled && success &&
      karute_led::altIndicatorKey(indicatorMode, usage);
  updateStatusLed();
  return success;
}

bool typeAltCodePoint(uint32_t cp) {
  uint16_t value;
  if (!karute_alt::outputCode(karute_alt::Mode::GBK, cp, value)) return false;
  altCharacterActive = true;
  // While this flag is set, waitWhileServing keeps collecting Stop/Pause but
  // lets this one complete code commit. Releasing Alt halfway is NOT safe.
  const bool success = karute_alt::emitTimedCode(value,
      typingSettings.alt.keyDownMs, typingSettings.alt.interKeyMs,
      typingSettings.altBeforeMs, typingSettings.altReleaseMs,
      typingSettings.alt.afterSpaceMs,
      [](uint8_t modifier, uint8_t usage) { return sendAltReport(modifier, usage); },
      [](uint32_t ms) { waitWhileServing(ms); });
  altCharacterActive = false;
  if (!success) {
    sendAltReport(0, 0);
    inputError = F("USB发送失败，已停止；请核对可能的半个字符后重试。");
  }
  return success;
}

bool tapRawShortcut(uint8_t modifier, uint8_t usage, uint32_t afterMs) {
  if (!sendAltReport(modifier, usage)) return false;
  const bool held = waitWhileServing(kShortcutKeyDownMs);
  const bool released = sendAltReport(0, 0);
  if (!held || !released) return false;
  return waitWhileServing(afterMs);
}

bool openNotepadForCredentialReport() {
  // Standard keyboard report: Left GUI modifier + HID usage 0x15 (R).
  // This is deliberately confined to the physical 3-second recovery action.
  if (!tapRawShortcut(0x08, 0x15, kRunDialogOpenWaitMs)) return false;

  constexpr char kCommand[] = "notepad";
  for (const char *cursor = kCommand; *cursor != '\0'; ++cursor) {
    // Alt+numpad ASCII avoids the active Chinese IME while filling Run.
    if (!typeAltCodePoint(static_cast<uint8_t>(*cursor)) ||
        abortRequested) {
      return false;
    }
  }
  if (!typeAltCodePoint('\n') || abortRequested) return false;
  return waitWhileServing(kNotepadOpenWaitMs);
}

size_t vucHexDigitCount(uint32_t codePoint) {
  size_t digits = 1;
  while (codePoint >= 16) {
    codePoint >>= 4;
    ++digits;
  }
  return digits < 4 ? 4 : digits;
}

uint64_t estimateVucCodePointMs(uint32_t codePoint) {
  const SpeedSettings &speed = typingSettings.vuc;
  const size_t sequenceKeys = 3 + vucHexDigitCount(codePoint);
  uint64_t total = static_cast<uint64_t>(sequenceKeys + 1) *
                   speed.keyDownMs;
  total += static_cast<uint64_t>(sequenceKeys - 1) * speed.interKeyMs;
  total += speed.beforeSpaceMs + speed.afterSpaceMs;
  return total;
}

uint32_t estimateCurrentTextMs() {
  if (currentText.isEmpty()) {
    return 0;
  }

  uint64_t total = typingSettings.startDelayMs;
  const uint32_t vucTapTime = typingSettings.vuc.keyDownMs +
                              typingSettings.vuc.interKeyMs;
  const uint32_t engTapTime = typingSettings.eng.keyDownMs +
                              typingSettings.eng.interKeyMs;
  size_t index = 0;
  while (index < currentText.length()) {
    uint32_t codePoint = 0;
    if (!decodeNextCodePoint(currentText, index, codePoint)) {
      break;
    }
    if (codePoint == '\r' && index < currentText.length() &&
        currentText[index] == '\n') {
      continue;
    }
    if (isAltMode(typingSettings.mode)) {
      uint16_t value;
      if (karute_alt::outputCode(karute_alt::Mode::GBK, codePoint, value))
        total += karute_alt::timedEstimateMs(value,
            typingSettings.alt.keyDownMs, typingSettings.alt.interKeyMs,
            typingSettings.altBeforeMs, typingSettings.altReleaseMs,
            typingSettings.alt.afterSpaceMs);
      continue;
    }
    if (codePoint == '\r' || codePoint == '\n' || codePoint == '\t' ||
        codePoint == '\b' || codePoint == ' ') {
      total += typingSettings.mode == 2 ? engTapTime : vucTapTime;
    } else if (typingSettings.mode == 2 &&
               codePoint >= 0x20 && codePoint <= 0x7E) {
      total += engTapTime;
    } else if (typingSettings.mode == 1 && codePoint >= 0x20) {
      total += estimateVucCodePointMs(codePoint);
    }
  }
  return total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
}

bool tapAsciiKey(char character, const SpeedSettings &speed,
                 uint16_t afterMs, uint8_t indicatorMode = 0) {
  if (character < 0x20 || character > 0x7E) {
    return false;
  }
  const uint8_t effectiveIndicatorMode =
      indicatorMode == 0 ? typingSettings.mode : indicatorMode;
  const bool flashForThisKey = typingSettings.inputLedEnabled &&
      karute_led::asciiIndicatorKey(effectiveIndicatorMode, character);
  if (flashForThisKey) {
    typingIndicatorKeyDown = true;
    updateStatusLed();
  }
  if (keyboard.press(static_cast<uint8_t>(character)) == 0) {
    keyboard.releaseAll();
    if (flashForThisKey) {
      typingIndicatorKeyDown = false;
      updateStatusLed();
    }
    return false;
  }
  const bool keyDownCompleted = waitWhileServing(speed.keyDownMs);
  keyboard.releaseAll();
  if (flashForThisKey) {
    typingIndicatorKeyDown = false;
    updateStatusLed();
  }
  if (!keyDownCompleted) return false;
  return waitWhileServing(afterMs);
}

bool tapSpecialKey(uint8_t key, const SpeedSettings &speed,
                   uint16_t afterMs) {
  if (keyboard.press(key) == 0) {
    keyboard.releaseAll();
    return false;
  }
  if (!waitWhileServing(speed.keyDownMs)) {
    keyboard.releaseAll();
    return false;
  }
  keyboard.releaseAll();
  return waitWhileServing(afterMs);
}

bool typeUnicodeCodePoint(uint32_t codePoint) {
  constexpr char kHexDigits[] = "0123456789abcdef";
  char reversedHex[8];
  size_t hexLength = 0;

  do {
    reversedHex[hexLength++] = kHexDigits[codePoint & 0x0F];
    codePoint >>= 4;
  } while (codePoint != 0 && hexLength < sizeof(reversedHex));

  // Four digits make ASCII code points explicit (for example, S = U+0053)
  // while supplementary-plane characters naturally use five or six digits.
  while (hexLength < 4) {
    reversedHex[hexLength++] = '0';
  }

  const SpeedSettings &speed = typingSettings.vuc;
  if (!tapAsciiKey('v', speed, speed.interKeyMs) ||
      !tapAsciiKey('u', speed, speed.interKeyMs) ||
      !tapAsciiKey('c', speed, speed.interKeyMs)) {
    return false;
  }
  while (hexLength > 0) {
    const char digit = reversedHex[--hexLength];
    const uint16_t waitMs = hexLength == 0 ? speed.beforeSpaceMs
                                           : speed.interKeyMs;
    if (!tapAsciiKey(digit, speed, waitMs)) {
      return false;
    }
  }
  return tapAsciiKey(' ', speed, speed.afterSpaceMs);
}

void finishStoppedRun() {
  keyboard.releaseAll();
  typingIndicatorKeyDown = false;
  abortRequested = false;
  pauseRequested = false;
  startDelayActive = false;
  typingStopTracking = false;
  // A physical long press used to stop must not become a new short-press
  // start when the blocking typing loop returns.
  ignoreButtonUntilRelease = true;
  typingIndex = 0;
  deviceState = currentText.isEmpty() ? DeviceState::EMPTY
                                      : DeviceState::STOPPED;
}

void finishPausedRun() {
  keyboard.releaseAll();
  typingIndicatorKeyDown = false;
  pauseRequested = false;
  startDelayActive = false;
  typingStopTracking = false;
  if (digitalRead(kUserButtonPin) == LOW) ignoreButtonUntilRelease = true;
  deviceState = DeviceState::PAUSED;
}

void typeCurrentText(bool resume) {
  if (currentText.isEmpty()) {
    deviceState = DeviceState::EMPTY;
    return;
  }
  inputError = "";
  if (!altStartReady(inputError)) {
    deviceState = DeviceState::STOPPED;
    return;
  }

  abortRequested = false;
  pauseRequested = false;
  typingStopTracking = false;
  typingButtonArmed = (digitalRead(kUserButtonPin) == HIGH);
  deviceState = DeviceState::TYPING;
  typingIndicatorKeyDown = false;
  keyboard.releaseAll();
  if (!resume) {
    typingIndex = 0;
    completedCharacters = 0;
    hasPrintProgress = true;
    runEstimatedMs = estimateCurrentTextMs() - typingSettings.startDelayMs;
    startDelayEndsAtMs = millis() + typingSettings.startDelayMs;
    startDelayActive = typingSettings.startDelayMs > 0;
    if (!waitWhileServing(typingSettings.startDelayMs)) {
      startDelayActive = false;
      finishStoppedRun();
      return;
    }
    startDelayActive = false;
    if (pauseRequested && typingIndex < currentText.length()) {
      finishPausedRun();
      return;
    }
  }
  updateStatusLed();
  while (typingIndex < currentText.length()) {
    if (abortRequested) {
      finishStoppedRun();
      return;
    }
    uint32_t codePoint = 0;
    if (!decodeNextCodePoint(currentText, typingIndex, codePoint)) {
      deviceState = DeviceState::ERROR;
      keyboard.releaseAll();
      return;
    }

    // Treat Windows CRLF as one Enter; a bare CR is also Enter.
    if (codePoint == '\r' && typingIndex < currentText.length() &&
        currentText[typingIndex] == '\n') {
      ++completedCharacters;
      continue;
    }

    bool success = true;
    {
      if (isAltMode(typingSettings.mode)) {
        success = typeAltCodePoint(codePoint);
      } else {
        const SpeedSettings &controlSpeed = activeSpeed();
        if (codePoint == '\r' || codePoint == '\n') {
          success = tapSpecialKey(KEY_RETURN, controlSpeed,
                                  controlSpeed.interKeyMs);
        } else if (codePoint == '\t') {
          success = tapSpecialKey(KEY_TAB, controlSpeed,
                                  controlSpeed.interKeyMs);
        } else if (codePoint == '\b') {
          success = tapSpecialKey(KEY_BACKSPACE, controlSpeed,
                                  controlSpeed.interKeyMs);
        } else if (codePoint == ' ') {
          success = tapAsciiKey(' ', controlSpeed,
                                controlSpeed.interKeyMs);
        } else if (typingSettings.mode == 2 &&
                   codePoint >= 0x20 && codePoint <= 0x7E) {
          success = tapAsciiKey(static_cast<char>(codePoint),
                                typingSettings.eng,
                                typingSettings.eng.interKeyMs);
        } else if (typingSettings.mode == 1 && codePoint >= 0x20) {
          success = typeUnicodeCodePoint(codePoint);
        } else {
          // ENG skips non-ASCII text. Unsupported control codes are skipped.
          success = typingSettings.mode == 2;
        }
      }
    }

    if (!success) {
      if (abortRequested) {
        finishStoppedRun();
      } else {
        deviceState = DeviceState::ERROR;
        keyboard.releaseAll();
      }
      return;
    }
    ++completedCharacters;
    if (abortRequested) {
      finishStoppedRun();
      return;
    }
    if (pauseRequested && typingIndex < currentText.length()) {
      finishPausedRun();
      return;
    }
  }
  keyboard.releaseAll();
  typingIndicatorKeyDown = false;
  if (typingStopTracking || digitalRead(kUserButtonPin) == LOW) {
    ignoreButtonUntilRelease = true;
  }
  typingStopTracking = false;
  deviceState = DeviceState::DONE;
  typingIndex = 0;
}

String wifiCredentialReport() {
  String report;
  report.reserve(activeWifiName.length() + activeWifiPassword.length() +
                 activeLoginHost.length() + 190);
  report += F("SSID\n");
  report += activeWifiName;
  report += F("\nPassword\n");
  report += activeWifiPassword;
  report += F("\nLogin URL\n");
  report += loginUrlForHost(activeLoginHost.c_str());
  report += F("\nBackup URL\n");
  report += kBackupLoginUrl;
  report += F("\nThese credentials have now been printed ");
  report += static_cast<unsigned long>(activeCredentialReportCount);
  report += activeCredentialReportCount == 1 ? F(" time.") : F(" times.");
  return report;
}

void typeWifiCredentialReport() {
  const DeviceState previousState = deviceState;
  inputError = "";
  if (!altTransportReady(inputError)) {
    deviceState = DeviceState::ERROR;
    keyboard.releaseAll();
    return;
  }
  TypingSettings updated = typingSettings;
  if (updated.credentialReportCount < UINT32_MAX) {
    ++updated.credentialReportCount;
  }
  // Record the disclosure attempt before sending any credential keystrokes.
  // A power loss or USB failure therefore cannot erase the audit clue.
  if (!persistTypingSettings(updated)) {
    deviceState = DeviceState::ERROR;
    inputError = F("Credential print count could not be saved.");
    keyboard.releaseAll();
    return;
  }
  typingSettings = updated;
  activeCredentialReportCount = updated.credentialReportCount;
  const String report = wifiCredentialReport();
  credentialReportActive = true;
  abortRequested = false;
  pauseRequested = false;
  typingStopTracking = false;
  typingButtonArmed = false;  // Ignore the held button until it is released.
  typingIndicatorKeyDown = false;
  startDelayActive = false;
  deviceState = DeviceState::TYPING;
  keyboard.releaseAll();

  bool success = openNotepadForCredentialReport();
  for (size_t index = 0;
       index < report.length() && success && !abortRequested; ++index) {
    // Wi-Fi fields and labels are printable ASCII. Sending their GBK/ASCII
    // code values through Alt+numpad avoids dependence on the active IME.
    success = typeAltCodePoint(static_cast<uint8_t>(report[index]));
  }

  keyboard.releaseAll();
  typingIndicatorKeyDown = false;
  credentialReportActive = false;
  typingStopTracking = false;
  if (digitalRead(kUserButtonPin) == LOW) ignoreButtonUntilRelease = true;
  if (!success && !abortRequested) {
    deviceState = DeviceState::ERROR;
    inputError = F("USB failed while typing Wi-Fi information.");
  } else {
    abortRequested = false;
    deviceState = previousState;
  }
}

void updateButton() {
  const bool rawPressed = (digitalRead(kUserButtonPin) == LOW);
  const uint32_t now = millis();

  if (ignoreButtonUntilRelease) {
    lastRawButtonPressed = rawPressed;
    stableButtonPressed = rawPressed;
    rawStateChangedAtMs = now;
    if (!rawPressed) {
      ignoreButtonUntilRelease = false;
    }
    return;
  }

  if (rawPressed != lastRawButtonPressed) {
    lastRawButtonPressed = rawPressed;
    rawStateChangedAtMs = now;
  }
  if (stableButtonPressed && rawPressed &&
      deviceState == DeviceState::PAUSED &&
      (now - buttonPressedAtMs) >= kStopHoldMs) {
    finishStoppedRun();
    return;
  }
  if (stableButtonPressed && rawPressed &&
      deviceState != DeviceState::TYPING &&
      deviceState != DeviceState::PAUSED && !startRequested &&
      !credentialReportRequested && !wifiRestartPending &&
      (now - buttonPressedAtMs) >= kWifiReportHoldMs) {
    credentialReportRequested = true;
    ignoreButtonUntilRelease = true;
    return;
  }
  if ((now - rawStateChangedAtMs) < kDebounceMs ||
      rawPressed == stableButtonPressed) {
    return;
  }

  stableButtonPressed = rawPressed;
  if (stableButtonPressed) {
    buttonPressedAtMs = now;
    return;
  }
  if ((now - buttonPressedAtMs) >= kStopHoldMs ||
      storagePending || !storageReady) return;
  if (deviceState == DeviceState::READY ||
      deviceState == DeviceState::DONE ||
      deviceState == DeviceState::STOPPED ||
      deviceState == DeviceState::PAUSED) {
    if (deviceState == DeviceState::PAUSED) resumeRequested = true;
    else startRequested = true;
  }
}

struct CommandReply { int code; String body; };

CommandReply commandStart() {
  if (deviceState == DeviceState::TYPING || startRequested) {
    return {409, F("{\"message\":\"设备已经准备开始或正在输入。\"}")};
  }
  if (currentText.isEmpty()) {
    return {409, F("{\"message\":\"断电存储中没有正文。\"}")};
  }
  if (!storageReady || storagePending) {
    return {409, F("{\"message\":\"正文尚未完成断电保存，不能打印。\"}")};
  }
  if (deviceState != DeviceState::READY &&
      deviceState != DeviceState::DONE &&
      deviceState != DeviceState::STOPPED) {
    return {409, F("{\"message\":\"当前状态不能开始输入。\"}")};
  }

  inputError = "";
  if (!altStartReady(inputError)) {
    return {400, String("{\"message\":\"") + inputError + "\"}"};
  }
  completedCharacters = 0;
  hasPrintProgress = true;
  runEstimatedMs = estimateCurrentTextMs() - typingSettings.startDelayMs;
  startRequested = true;
  String response = F("{\"message\":\"");
  if (typingSettings.startDelayMs == 0) {
    response += F("将立即开始输入。");
  } else {
    response += F("将在 ");
    response += typingSettings.startDelayMs / 1000;
    response += F(" 秒后开始输入。");
  }
  response += F("\"}");
  return {202, response};
}

CommandReply commandPause() {
  if (deviceState != DeviceState::TYPING || credentialReportActive) {
    return {409, F("{\"message\":\"当前没有正在打印的内容。\"}")};
  }
  pauseRequested = true;
  return {202, F("{\"message\":\"将在当前字符结束后暂停。\"}")};
}

CommandReply commandResume() {
  if (deviceState != DeviceState::PAUSED) {
    return {409, F("{\"message\":\"当前没有暂停的任务。\"}")};
  }
  resumeRequested = true;
  return {202, F("{\"message\":\"将从暂停位置继续。\"}")};
}

CommandReply commandStop() {
  if (deviceState != DeviceState::TYPING &&
      deviceState != DeviceState::PAUSED) {
    return {409, F("{\"message\":\"设备当前没有输入。\"}")};
  }

  if (deviceState == DeviceState::PAUSED) finishStoppedRun();
  else abortRequested = true;
  return {202, F("{\"message\":\"停止指令已发送。\"}")};
}

CommandReply commandDirty() {
  if (deviceState == DeviceState::TYPING ||
      deviceState == DeviceState::PAUSED) {
    return {409, F("{\"message\":\"打印期间不能修改正文。\"}")};
  }
  uint32_t revision = 0;
  if (!parseRevision(webServer.arg("revision"), revision)) {
    return {400, F("{\"message\":\"编辑版本无效。\"}")};
  }
  if (revision > editorRevision) editorRevision = revision;
  storagePending = editorRevision > savedRevision;
  if (storagePending) startRequested = false;
  return {200, "{}"};
}

CommandReply commandSave() {
  if (deviceState == DeviceState::TYPING ||
      deviceState == DeviceState::PAUSED || startRequested) {
    return {409, F("{\"message\":\"正在打印或已暂停，暂不能修改正文。\"}")};
  }
  uint32_t revision = 0;
  if (!parseRevision(webServer.arg("revision"), revision) ||
      revision < editorRevision || revision <= savedRevision) {
    return {409, F("{\"message\":\"这是一份较旧的编辑内容，请重新同步。\"}")};
  }
  const String uploadedText = webServer.hasArg("text") ? webServer.arg("text") : webServer.arg("plain");

  if (uploadedText.length() > kMaxTextBytes) {
    return {413, F("{\"message\":\"正文超过 32768 字节上限，未保存。\"}")};
  }

  const Utf8CountResult result = validateAndCountUtf8(uploadedText);
  if (!result.valid) {
    return {400, F("{\"message\":\"上传内容不是有效的 UTF-8 文本。\"}")};
  }

  const bool changed = uploadedText != currentText;
  if (changed && !persistText(uploadedText)) {
    storagePending = true;
    return {507, F("{\"message\":\"写入断电存储失败，旧正文仍保留。\"}")};
  }
  currentText = uploadedText;
  inputError = "";
  currentCharacterCount = result.characters;
  editorRevision = revision;
  savedRevision = revision;
  storagePending = false;
  if (changed) { hasPrintProgress = false; completedCharacters = 0; }
  if (changed) deviceState = currentText.isEmpty() ? DeviceState::EMPTY
                                                  : DeviceState::READY;
  return {200, "{}"};
}

CommandReply commandClear() {
  if (deviceState == DeviceState::TYPING ||
      deviceState == DeviceState::PAUSED || startRequested) {
    return {409, F("{\"message\":\"正在打印或已暂停，暂不能清空正文。\"}")};
  }
  if (!persistText("")) {
    return {507, F("{\"message\":\"清空断电存储失败。\"}")};
  }
  hasPrintProgress = false;
  completedCharacters = 0;
  currentText = "";
  inputError = "";
  currentCharacterCount = 0;
  ++editorRevision;
  savedRevision = editorRevision;
  storagePending = false;
  deviceState = DeviceState::EMPTY;
  return {200, "{}"};
}

void syncHome() {
  linkHeartbeatSeen = true;
  lastLinkHeartbeatAtMs = millis();
  CommandReply reply{200, "{}"};
  const String command = webServer.arg("command");
  const bool known = command.isEmpty() || command == "load" || command == "save" ||
      command == "start" || command == "pause" || command == "resume" ||
      command == "stop" || command == "clear";
  if (!known) reply = {400, F("{\"message\":\"无效指令。\"}")};
  if (known && webServer.arg("save") == "1") reply = commandSave();
  else if (known && webServer.arg("dirty") == "1") reply = commandDirty();
  if (reply.code < 400) {
    if (command == "start") reply = commandStart();
    else if (command == "pause") reply = commandPause();
    else if (command == "resume") reply = commandResume();
    else if (command == "stop") reply = commandStop();
    else if (command == "clear") reply = commandClear();
  }
  String json = statusJson(command != "load");
  json.remove(json.length() - 1);
  json += F(",\"linked\":true,\"commandCode\":");
  json += reply.code;
  // Success needs only an acknowledgement; errors retain their message.
  json += F(",\"commandResult\":");
  json += reply.code >= 400 ? reply.body : String("{}");
  if (command == "load") {
    json += F(",\"text\":");
    json += jsonString(currentText.c_str());
  }
  json += '}';
  sendNoCache(200, "application/json; charset=utf-8", json);
}

void configureWebServer() {
  webServer.on("/api/sync", HTTP_POST, syncHome);
  webServer.on("/", HTTP_GET, []() {
    sendGzipPage(karute_web::kIndexHtmlGz,
                 karute_web::kIndexHtmlGzSize);
  });

  webServer.on("/settings", HTTP_GET, []() {
    sendGzipPage(karute_web::kSettingsHtmlGz,
                 karute_web::kSettingsHtmlGzSize);
  });

  webServer.on("/settings/wifi", HTTP_GET, []() {
    sendGzipPage(karute_web::kWifiSettingsHtmlGz,
                 karute_web::kWifiSettingsHtmlGzSize);
  });

  webServer.on("/help", HTTP_GET, []() {
    sendGzipPage(karute_web::kHelpHtmlGz,
                 karute_web::kHelpHtmlGzSize);
  });

  webServer.on("/api/status", HTTP_GET, []() {
    linkHeartbeatSeen = true;
    lastLinkHeartbeatAtMs = millis();
    sendNoCache(200, "application/json; charset=utf-8", statusJson());
  });

  webServer.on("/api/text", HTTP_GET, []() {
    sendNoCache(200, "text/plain; charset=utf-8", currentText);
  });

  webServer.on("/api/settings", HTTP_GET, []() {
    sendNoCache(200, "application/json; charset=utf-8", settingsJson());
  });

  webServer.on("/api/wifi", HTTP_GET, []() {
    sendNoCache(200, "application/json; charset=utf-8", wifiJson());
  });

  webServer.on("/api/wifi", HTTP_POST, []() {
    if (deviceState == DeviceState::TYPING ||
        deviceState == DeviceState::PAUSED || startRequested ||
        credentialReportRequested || credentialReportActive ||
        wifiRestartPending) {
      sendNoCache(409, "application/json; charset=utf-8",
                  F("{\"message\":\"设备忙，暂不能修改 Wi-Fi。\"}"));
      return;
    }

    TypingSettings updated = typingSettings;
    if (!copyValidatedWifiValue(webServer.arg("wifiName"),
                                updated.wifiName,
                                sizeof(updated.wifiName), 1, 32) ||
        !copyValidatedWifiValue(webServer.arg("password"),
                                updated.wifiPassword,
                                sizeof(updated.wifiPassword), 8, 63) ||
        !parseLocalLoginUrl(webServer.arg("loginUrl"),
                            updated.loginHost,
                            sizeof(updated.loginHost)) ||
        !validTypingSettings(updated)) {
      sendNoCache(400, "application/json; charset=utf-8",
                  F("{\"message\":\"Wi-Fi 名称须为1～32个可打印英文字符，密码须为8～63个；登录地址须为 http://名称.local。\"}"));
      return;
    }

    const bool changed = strcmp(updated.wifiName, typingSettings.wifiName) != 0 ||
                         strcmp(updated.wifiPassword,
                                typingSettings.wifiPassword) != 0 ||
                         strcmp(updated.loginHost,
                                typingSettings.loginHost) != 0;
    if (changed) updated.credentialReportCount = 0;
    if (!persistTypingSettings(updated)) {
      sendNoCache(507, "application/json; charset=utf-8",
                  F("{\"message\":\"Wi-Fi 设置写入断电存储失败。\"}"));
      return;
    }
    typingSettings = updated;
    inputError = "";
    if (changed) scheduleAccessPointRestart();
    sendNoCache(200, "application/json; charset=utf-8", wifiJson());
  });

  webServer.on("/api/settings", HTTP_POST, []() {
    if (deviceState == DeviceState::TYPING ||
        deviceState == DeviceState::PAUSED || startRequested) {
      sendNoCache(409, "application/json; charset=utf-8",
                  F("{\"message\":\"正在输入或即将开始，暂不能修改设置。\"}"));
      return;
    }

    TypingSettings updated = typingSettings;
    uint16_t startDelaySeconds = 0;
    uint16_t requestedMode = 0;
    if (!parseUnsignedSetting(webServer.arg("mode"), 1, 4,
                              requestedMode) ||
        !isValidMode(static_cast<uint8_t>(requestedMode)) ||
        !parseUnsignedSetting(webServer.arg("vucDownMs"), 1, 100,
                              updated.vuc.keyDownMs) ||
        !parseUnsignedSetting(webServer.arg("vucIntervalMs"), 1, 100,
                              updated.vuc.interKeyMs) ||
        !parseUnsignedSetting(webServer.arg("vucBeforeSpaceMs"), 1, 100,
                              updated.vuc.beforeSpaceMs) ||
        !parseUnsignedSetting(webServer.arg("vucAfterSpaceMs"), 1, 100,
                              updated.vuc.afterSpaceMs) ||
        !parseUnsignedSetting(webServer.arg("engDownMs"), 1, 100,
                              updated.eng.keyDownMs) ||
        !parseUnsignedSetting(webServer.arg("engIntervalMs"), 1, 100,
                              updated.eng.interKeyMs) ||
        !parseUnsignedSetting(webServer.arg("altBeforeMs"), 1, 100,
                              updated.altBeforeMs) ||
        !parseUnsignedSetting(webServer.arg("altDownMs"), 1, 100,
                              updated.alt.keyDownMs) ||
        !parseUnsignedSetting(webServer.arg("altIntervalMs"), 1, 100,
                              updated.alt.interKeyMs) ||
        !parseUnsignedSetting(webServer.arg("altReleaseMs"), 1, 100,
                              updated.altReleaseMs) ||
        !parseUnsignedSetting(webServer.arg("altAfterMs"), 1, 100,
                              updated.alt.afterSpaceMs) ||
        !parseUnsignedSetting(webServer.arg("startDelaySeconds"), 0, 10,
                              startDelaySeconds) ||
        !parseToggleSetting(webServer.arg("inputLedEnabled"),
                            updated.inputLedEnabled) ||
        !parseToggleSetting(webServer.arg("standbyLedEnabled"),
                            updated.standbyLedEnabled)) {
      sendNoCache(400, "application/json; charset=utf-8",
                  F("{\"message\":\"设置无效：所有按键与间隔须为 1～100 ms。\"}"));
      return;
    }

    updated.startDelayMs = startDelaySeconds * 1000U;
    updated.mode = requestedMode;
    if (!persistTypingSettings(updated)) {
      sendNoCache(507, "application/json; charset=utf-8",
                  F("{\"message\":\"设置写入断电存储失败。\"}"));
      return;
    }
    typingSettings = updated;
    inputError = "";
    updateStatusLed();
    sendNoCache(200, "application/json; charset=utf-8", settingsJson());
  });

  webServer.on("/api/settings/reset", HTTP_POST, []() {
    if (deviceState == DeviceState::TYPING ||
        deviceState == DeviceState::PAUSED || startRequested) {
      sendNoCache(409, "application/json; charset=utf-8",
                  F("{\"message\":\"打印期间不能恢复设置。\"}"));
      return;
    }
    TypingSettings defaults;
    const bool wifiChanged =
        strcmp(defaults.wifiName, typingSettings.wifiName) != 0 ||
        strcmp(defaults.wifiPassword, typingSettings.wifiPassword) != 0 ||
        strcmp(defaults.loginHost, typingSettings.loginHost) != 0;
    if (!wifiChanged) {
      defaults.credentialReportCount = typingSettings.credentialReportCount;
    }
    if (!persistTypingSettings(defaults)) {
      sendNoCache(507, "application/json; charset=utf-8",
                  F("{\"message\":\"恢复默认设置失败。\"}"));
      return;
    }
    typingSettings = defaults;
    inputError = "";
    if (wifiChanged) scheduleAccessPointRestart();
    updateStatusLed();
    sendNoCache(200, "application/json; charset=utf-8", settingsJson());
  });

  webServer.on("/api/start", HTTP_POST, []() {
    const CommandReply reply = commandStart();
    sendNoCache(reply.code, "application/json; charset=utf-8", reply.body);
  });

  webServer.on("/api/pause", HTTP_POST, []() {
    const CommandReply reply = commandPause();
    sendNoCache(reply.code, "application/json; charset=utf-8", reply.body);
  });

  webServer.on("/api/resume", HTTP_POST, []() {
    const CommandReply reply = commandResume();
    sendNoCache(reply.code, "application/json; charset=utf-8", reply.body);
  });

  webServer.on("/api/stop", HTTP_POST, []() {
    const CommandReply reply = commandStop();
    sendNoCache(reply.code, "application/json; charset=utf-8", reply.body);
  });

  webServer.on("/api/dirty", HTTP_POST, []() {
    const CommandReply reply = commandDirty();
    sendNoCache(reply.code, "application/json; charset=utf-8",
                reply.code < 400 ? statusJson() : reply.body);
  });

  webServer.on("/api/text", HTTP_POST, []() {
    const CommandReply reply = commandSave();
    sendNoCache(reply.code, "application/json; charset=utf-8",
                reply.code < 400 ? statusJson() : reply.body);
  });

  webServer.on("/api/clear", HTTP_POST, []() {
    const CommandReply reply = commandClear();
    sendNoCache(reply.code, "application/json; charset=utf-8",
                reply.code < 400 ? statusJson() : reply.body);
  });

  webServer.onNotFound([]() {
    sendNoCache(404, "text/plain; charset=utf-8", F("Not found"));
  });

  webServer.begin();
}

}  // namespace

void setup() {
#if ARDUINO_USB_MODE != 0
#error "Phase E requires native USB-OTG/TinyUSB mode."
#endif

#if ARDUINO_USB_CDC_ON_BOOT != 0 || ARDUINO_USB_MSC_ON_BOOT != 0 || \
    ARDUINO_USB_DFU_ON_BOOT != 0
#error "Phase E must expose only USB HID Keyboard."
#endif

  pinMode(kUserButtonPin, INPUT_PULLUP);
  pinMode(kStatusLedPin, OUTPUT);
  ledEventStartedAtMs = millis();
  loadTypingSettings();
  updateStatusLed();

  keyboard.onEvent(ARDUINO_USB_HID_KEYBOARD_LED_EVENT,
      [](void *, esp_event_base_t, int32_t, void *data) {
        hostLeds = static_cast<arduino_usb_hid_keyboard_event_data_t *>(data)->leds;
        hostLedsKnown = true;
      });
  keyboard.begin();
  USB.manufacturerName("M5Stack");
  USB.productName("AtomS3U Auto Keyboard E");
  USB.serialNumber("AK-PHASE-E");
  USB.usbClass(0x00);
  USB.usbSubClass(0x00);
  USB.usbProtocol(0x00);
  USB.webUSB(false);
  USB.begin();

  currentText.reserve(kMaxTextBytes);
  storageReady = initializeStorage();
  if (!storageReady || !loadStoredText()) deviceState = DeviceState::ERROR;

  if (!startAccessPoint(typingSettings.wifiName,
                        typingSettings.wifiPassword,
                        typingSettings.loginHost)) {
    WiFi.softAPdisconnect(true);
    delay(40);
    if (startAccessPoint(kDefaultAccessPointName,
                         kDefaultAccessPointPassword,
                         kDefaultLoginHost)) {
      copyCString(kDefaultAccessPointName, typingSettings.wifiName,
                  sizeof(typingSettings.wifiName));
      copyCString(kDefaultAccessPointPassword, typingSettings.wifiPassword,
                  sizeof(typingSettings.wifiPassword));
      copyCString(kDefaultLoginHost, typingSettings.loginHost,
                  sizeof(typingSettings.loginHost));
      typingSettings.credentialReportCount = 0;
      activeCredentialReportCount = 0;
      persistTypingSettings(typingSettings);
      inputError = F("Saved Wi-Fi failed; defaults restored.");
    } else {
      deviceState = DeviceState::ERROR;
      inputError = F("Wi-Fi access point failed to start.");
    }
  } else {
    activeCredentialReportCount = typingSettings.credentialReportCount;
  }
  configureWebServer();
}

void loop() {
  webServer.handleClient();
  updateButton();
  updateStatusLed();
  if (wifiRestartPending &&
      static_cast<int32_t>(millis() - wifiRestartAtMs) >= 0 &&
      deviceState != DeviceState::TYPING &&
      deviceState != DeviceState::PAUSED) {
    restartConfiguredAccessPoint();
  }
  if (altReleasePending && deviceState != DeviceState::TYPING &&
      millis() - altReleaseRetryAt >= 250 && altTransport.ready()) {
    altReleaseRetryAt = millis();
    sendAltReport(0, 0);  // Never resume interrupted text on reconnection.
  }
  if (credentialReportRequested &&
      deviceState != DeviceState::TYPING &&
      deviceState != DeviceState::PAUSED) {
    credentialReportRequested = false;
    typeWifiCredentialReport();
  }
  if (startRequested &&
      (deviceState == DeviceState::READY ||
       deviceState == DeviceState::DONE ||
       deviceState == DeviceState::STOPPED) &&
      !storagePending && storageReady) {
    startRequested = false;
    typeCurrentText(false);
  }
  if (resumeRequested && deviceState == DeviceState::PAUSED) {
    resumeRequested = false;
    typeCurrentText(true);
  }
  delay(2);
}
