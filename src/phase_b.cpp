#include <Arduino.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

namespace {

// M5Stack AtomS3U (K125) official user-button pin.
constexpr uint8_t kUserButtonPin = 41;

constexpr uint32_t kDebounceMs = 35;
constexpr uint32_t kKeyDownMs = 10;
constexpr uint32_t kInterKeyMs = 20;
constexpr uint32_t kUnicodeCommitWaitMs = 50;

// Expected Windows output with Microsoft Pinyin in Chinese mode:
// 中文输入测试成功：你好世界，℃±μαβ。
constexpr uint32_t kPhaseBCodePoints[] = {
    0x4E2D,  // 中
    0x6587,  // 文
    0x8F93,  // 输
    0x5165,  // 入
    0x6D4B,  // 测
    0x8BD5,  // 试
    0x6210,  // 成
    0x529F,  // 功
    0xFF1A,  // ：
    0x4F60,  // 你
    0x597D,  // 好
    0x4E16,  // 世
    0x754C,  // 界
    0xFF0C,  // ，
    0x2103,  // ℃
    0x00B1,  // ±
    0x03BC,  // μ
    0x03B1,  // α
    0x03B2,  // β
    0x3002,  // 。
};

USBHIDKeyboard keyboard;

bool stableButtonPressed = false;
bool lastRawButtonPressed = false;
uint32_t rawStateChangedAtMs = 0;

// Convert only the lowercase letters, hexadecimal digits, and Space used by
// a Microsoft Pinyin VUC sequence to their physical US/QWERTY HID Usage IDs.
bool asciiToUsUsageId(char character, uint8_t &usageId) {
  if (character >= 'a' && character <= 'z') {
    usageId = static_cast<uint8_t>(0x04 + character - 'a');
    return true;
  }

  if (character >= '1' && character <= '9') {
    usageId = static_cast<uint8_t>(0x1E + character - '1');
    return true;
  }

  if (character == '0') {
    usageId = 0x27;
    return true;
  }

  if (character == ' ') {
    usageId = 0x2C;
    return true;
  }

  return false;
}

bool tapAsciiKey(char character) {
  uint8_t usageId = 0;
  if (!asciiToUsUsageId(character, usageId)) {
    return false;
  }

  keyboard.pressRaw(usageId);
  delay(kKeyDownMs);
  keyboard.releaseAll();
  delay(kInterKeyMs);
  return true;
}

void typeUnicodeCodePoint(uint32_t codePoint) {
  constexpr char kHexDigits[] = "0123456789abcdef";
  char reversedHex[8];
  size_t hexLength = 0;

  // Unicode currently ends at U+10FFFF, so eight slots are comfortably safe.
  do {
    reversedHex[hexLength++] = kHexDigits[codePoint & 0x0F];
    codePoint >>= 4;
  } while (codePoint != 0 && hexLength < sizeof(reversedHex));

  tapAsciiKey('v');
  tapAsciiKey('u');
  tapAsciiKey('c');

  while (hexLength > 0) {
    tapAsciiKey(reversedHex[--hexLength]);
  }

  tapAsciiKey(' ');
  delay(kUnicodeCommitWaitMs);
}

void typePhaseBTest() {
  keyboard.releaseAll();
  delay(kInterKeyMs);

  for (const uint32_t codePoint : kPhaseBCodePoints) {
    typeUnicodeCodePoint(codePoint);
  }
}

void updateButton() {
  const bool rawPressed = (digitalRead(kUserButtonPin) == LOW);
  const uint32_t now = millis();

  if (rawPressed != lastRawButtonPressed) {
    lastRawButtonPressed = rawPressed;
    rawStateChangedAtMs = now;
  }

  if ((now - rawStateChangedAtMs) < kDebounceMs ||
      rawPressed == stableButtonPressed) {
    return;
  }

  stableButtonPressed = rawPressed;
  if (stableButtonPressed) {
    typePhaseBTest();
  }
}

}  // namespace

void setup() {
#if ARDUINO_USB_MODE != 0
#error "Phase B requires native USB-OTG/TinyUSB mode."
#endif

#if ARDUINO_USB_CDC_ON_BOOT != 0 || ARDUINO_USB_MSC_ON_BOOT != 0 || \
    ARDUINO_USB_DFU_ON_BOOT != 0
#error "Phase B must expose only USB HID Keyboard."
#endif

  pinMode(kUserButtonPin, INPUT_PULLUP);

  keyboard.begin();
  USB.manufacturerName("M5Stack");
  USB.productName("AtomS3U Auto Keyboard B");
  USB.serialNumber("AK-PHASE-B");
  USB.usbClass(0x00);
  USB.usbSubClass(0x00);
  USB.usbProtocol(0x00);
  USB.webUSB(false);
  USB.begin();
}

void loop() {
  updateButton();
  delay(1);
}
