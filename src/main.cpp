#include <Arduino.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

namespace {

// M5Stack AtomS3U (K125) official pin map.
constexpr uint8_t kUserButtonPin = 41;

// Conservative timings for the first hardware test.
constexpr uint32_t kDebounceMs = 35;
constexpr uint32_t kKeyDownMs = 10;
constexpr uint32_t kInterKeyMs = 20;

// USB HID Usage IDs for the physical US/QWERTY keys a, b, c, 1, 2, 3.
// Raw usage IDs avoid any framework ASCII-map ambiguity during phase A.
constexpr uint8_t kPhaseATestKeys[] = {0x04, 0x05, 0x06, 0x1E, 0x1F, 0x20};

USBHIDKeyboard keyboard;

bool stableButtonPressed = false;
bool lastRawButtonPressed = false;
uint32_t rawStateChangedAtMs = 0;

void typePhaseATestSlowly() {
  // Begin from an explicitly released state and allow the host one quiet
  // interval before the first key.
  keyboard.releaseAll();
  delay(kInterKeyMs);

  for (const uint8_t usageId : kPhaseATestKeys) {
    keyboard.pressRaw(usageId);
    delay(kKeyDownMs);
    keyboard.releaseAll();
    delay(kInterKeyMs);
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
    typePhaseATestSlowly();
  }
}

}  // namespace

void setup() {
#if ARDUINO_USB_MODE != 0
#error "Phase A requires native USB-OTG/TinyUSB mode (ARDUINO_USB_MODE=0)."
#endif

#if ARDUINO_USB_CDC_ON_BOOT != 0
#error "Phase A must not expose a USB CDC serial interface."
#endif

#if ARDUINO_USB_MSC_ON_BOOT != 0
#error "Phase A must not expose USB Mass Storage."
#endif

#if ARDUINO_USB_DFU_ON_BOOT != 0
#error "Phase A must not expose runtime USB DFU."
#endif

  pinMode(kUserButtonPin, INPUT_PULLUP);

  // Register only the keyboard HID interface before starting USB.
  // Arduino-ESP32 2.0.17 uses its built-in US/QWERTY ASCII map.
  keyboard.begin();
  USB.manufacturerName("M5Stack");
  USB.productName("AtomS3U Auto Keyboard A");
  USB.serialNumber("AK-PHASE-A");

  // Arduino-ESP32 defaults the outer device descriptor to the IAD/composite
  // class. This firmware has exactly one interface, so advertise class 0 at
  // device level and let the single interface identify itself as HID Keyboard.
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
