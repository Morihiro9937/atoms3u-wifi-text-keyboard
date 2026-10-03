#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

namespace {

constexpr char kAccessPointName[] = "karute";
constexpr char kAccessPointPassword[] = "autokarute";
constexpr size_t kMaxTextBytes = 32 * 1024;
constexpr uint8_t kUserButtonPin = 41;

constexpr uint32_t kDebounceMs = 35;
constexpr uint32_t kKeyDownMs = 10;
constexpr uint32_t kInterKeyMs = 20;
constexpr uint32_t kUnicodeCommitWaitMs = 50;
constexpr uint32_t kStartDelayMs = 3000;
constexpr uint32_t kStopHoldMs = 1000;

enum class DeviceState { EMPTY, READY, TYPING, DONE, ERROR };

USBHIDKeyboard keyboard;
WebServer webServer(80);

String currentText;
size_t currentCharacterCount = 0;
DeviceState deviceState = DeviceState::EMPTY;

bool stableButtonPressed = false;
bool lastRawButtonPressed = false;
uint32_t rawStateChangedAtMs = 0;
bool startRequested = false;
bool abortRequested = false;
bool typingButtonArmed = false;
bool typingStopTracking = false;
uint32_t typingStopPressedAtMs = 0;
bool ignoreButtonUntilRelease = false;

const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>karute</title>
  <style>
    :root { color-scheme: light; font-family: -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; }
    body { margin: 0; background: #f3f5f7; color: #17202a; }
    main { max-width: 760px; margin: 0 auto; padding: 20px 16px 48px; }
    h1 { margin: 4px 0 6px; font-size: 25px; }
    .hint { color: #5f6b76; font-size: 14px; line-height: 1.55; margin-bottom: 16px; }
    .card { background: white; border-radius: 14px; padding: 16px; box-shadow: 0 2px 14px #17202a12; }
    textarea { box-sizing: border-box; width: 100%; min-height: 330px; resize: vertical; border: 1px solid #ccd3da; border-radius: 10px; padding: 12px; font: 16px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; }
    textarea:focus { outline: 2px solid #246bfd33; border-color: #246bfd; }
    .meta { display: flex; flex-wrap: wrap; gap: 8px 18px; margin: 10px 0 14px; color: #4d5965; font-size: 14px; }
    .status { font-weight: 650; }
    .buttons { display: flex; gap: 10px; }
    .control-buttons { margin-top: 10px; }
    button { flex: 1; min-height: 46px; border: 0; border-radius: 10px; font-size: 16px; font-weight: 650; }
    #save { color: white; background: #246bfd; }
    #clear { color: #9f2525; background: #fdecec; }
    #start { color: white; background: #137333; }
    #stop { color: white; background: #b3261e; }
    button:disabled { opacity: .55; }
    #message { min-height: 24px; margin-top: 12px; font-size: 14px; }
    .ok { color: #137333; }
    .error { color: #b3261e; }
  </style>
</head>
<body>
<main>
  <h1>karute</h1>
  <div class="hint">保存后确认电脑光标和微软拼音状态正确，再点击网页“3 秒后开始”或短按设备按钮。输入时可点击网页“立即停止”，也可长按设备按钮 1 秒停止。正文断电即消失。</div>
  <section class="card">
    <textarea id="text" placeholder="在这里粘贴文字……" spellcheck="false"></textarea>
    <div class="meta">
      <span>编辑：<b id="editChars">0</b> 字符 / <b id="editBytes">0</b> 字节</span>
      <span>设备：<b id="savedChars">0</b> 字符 / <b id="savedBytes">0</b> 字节</span>
      <span>状态：<b id="state" class="status">连接中</b></span>
    </div>
    <div class="buttons">
      <button id="save">保存到设备</button>
      <button id="clear">立即清空</button>
    </div>
    <div class="buttons control-buttons">
      <button id="start" disabled>3 秒后开始输入</button>
      <button id="stop" disabled>立即停止</button>
    </div>
    <div id="message"></div>
  </section>
</main>
<script>
const maxBytes = 32768;
const text = document.querySelector('#text');
const message = document.querySelector('#message');
const bytesOf = value => new TextEncoder().encode(value).length;
const charsOf = value => Array.from(value).length;

function updateEditorCount() {
  document.querySelector('#editChars').textContent = charsOf(text.value);
  document.querySelector('#editBytes').textContent = bytesOf(text.value);
}

function showMessage(value, kind = '') {
  message.textContent = value;
  message.className = kind;
}

function updateButtons(state) {
  const typing = state === 'TYPING';
  document.querySelector('#save').disabled = typing;
  document.querySelector('#clear').disabled = typing;
  document.querySelector('#start').disabled = !(state === 'READY' || state === 'DONE');
  document.querySelector('#stop').disabled = !typing;
}

async function refreshStatus() {
  try {
    const response = await fetch('/api/status', {cache: 'no-store'});
    const data = await response.json();
    document.querySelector('#savedChars').textContent = data.characters;
    document.querySelector('#savedBytes').textContent = data.bytes;
    document.querySelector('#state').textContent = data.state;
    updateButtons(data.state);
  } catch (_) {
    document.querySelector('#state').textContent = '连接断开';
    updateButtons('DISCONNECTED');
  }
}

document.querySelector('#save').addEventListener('click', async () => {
  const byteCount = bytesOf(text.value);
  if (byteCount > maxBytes) {
    showMessage(`内容为 ${byteCount} 字节，超过 32768 字节上限。未保存。`, 'error');
    return;
  }
  showMessage('正在保存……');
  try {
    const response = await fetch('/api/text', {
      method: 'POST',
      headers: {'Content-Type': 'text/plain; charset=utf-8'},
      body: text.value
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.message || '保存失败');
    showMessage(`已保存 ${result.characters} 个字符，共 ${result.bytes} 字节。`, 'ok');
    await refreshStatus();
  } catch (error) {
    showMessage(error.message || '无法连接设备', 'error');
  }
});

document.querySelector('#clear').addEventListener('click', async () => {
  try {
    const response = await fetch('/api/clear', {method: 'POST'});
    const result = await response.json();
    if (!response.ok) throw new Error(result.message || '清空失败');
    text.value = '';
    updateEditorCount();
    showMessage('设备中的正文已清空。', 'ok');
    await refreshStatus();
  } catch (error) {
    showMessage(error.message || '无法连接设备', 'error');
  }
});

document.querySelector('#start').addEventListener('click', async () => {
  showMessage('已发送开始指令，请保持电脑光标不动。');
  try {
    const response = await fetch('/api/start', {method: 'POST'});
    const result = await response.json();
    if (!response.ok) throw new Error(result.message || '启动失败');
    showMessage('将在 3 秒后开始输入。', 'ok');
    await refreshStatus();
  } catch (error) {
    showMessage(error.message || '无法连接设备', 'error');
  }
});

document.querySelector('#stop').addEventListener('click', async () => {
  try {
    const response = await fetch('/api/stop', {method: 'POST'});
    const result = await response.json();
    if (!response.ok) throw new Error(result.message || '停止失败');
    showMessage('停止指令已发送，正文仍会保留。', 'ok');
    await refreshStatus();
  } catch (error) {
    showMessage(error.message || '无法连接设备', 'error');
  }
});

text.addEventListener('input', updateEditorCount);

async function loadSavedText() {
  try {
    const response = await fetch('/api/text', {cache: 'no-store'});
    if (response.ok) text.value = await response.text();
  } finally {
    updateEditorCount();
    refreshStatus();
  }
}

loadSavedText();
setInterval(refreshStatus, 2000);
</script>
</body>
</html>
)HTML";

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

const char *stateName(DeviceState state) {
  switch (state) {
    case DeviceState::EMPTY:
      return "EMPTY";
    case DeviceState::READY:
      return "READY";
    case DeviceState::TYPING:
      return "TYPING";
    case DeviceState::DONE:
      return "DONE";
    case DeviceState::ERROR:
      return "ERROR";
  }
  return "ERROR";
}

String statusJson() {
  String json;
  json.reserve(100);
  json += F("{\"state\":\"");
  json += stateName(deviceState);
  json += F("\",\"bytes\":");
  json += currentText.length();
  json += F(",\"characters\":");
  json += currentCharacterCount;
  json += F(",\"maxBytes\":");
  json += kMaxTextBytes;
  json += '}';
  return json;
}

void sendNoCache(int code, const char *contentType, const String &body) {
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(code, contentType, body);
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
    if (abortRequested) {
      return false;
    }
    delay(1);
  }
  return !abortRequested;
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

bool tapAsciiKey(char character) {
  if (character < 0x20 || character > 0x7E) {
    return false;
  }
  if (keyboard.press(static_cast<uint8_t>(character)) == 0) {
    keyboard.releaseAll();
    return false;
  }
  if (!waitWhileServing(kKeyDownMs)) {
    keyboard.releaseAll();
    return false;
  }
  keyboard.releaseAll();
  return waitWhileServing(kInterKeyMs);
}

bool tapSpecialKey(uint8_t key) {
  if (keyboard.press(key) == 0) {
    keyboard.releaseAll();
    return false;
  }
  if (!waitWhileServing(kKeyDownMs)) {
    keyboard.releaseAll();
    return false;
  }
  keyboard.releaseAll();
  return waitWhileServing(kInterKeyMs);
}

bool typeUnicodeCodePoint(uint32_t codePoint) {
  constexpr char kHexDigits[] = "0123456789abcdef";
  char reversedHex[8];
  size_t hexLength = 0;

  do {
    reversedHex[hexLength++] = kHexDigits[codePoint & 0x0F];
    codePoint >>= 4;
  } while (codePoint != 0 && hexLength < sizeof(reversedHex));

  if (!tapAsciiKey('v') || !tapAsciiKey('u') || !tapAsciiKey('c')) {
    return false;
  }
  while (hexLength > 0) {
    if (!tapAsciiKey(reversedHex[--hexLength])) {
      return false;
    }
  }
  if (!tapAsciiKey(' ')) {
    return false;
  }
  return waitWhileServing(kUnicodeCommitWaitMs);
}

void finishStoppedRun() {
  keyboard.releaseAll();
  abortRequested = false;
  typingStopTracking = false;
  // A physical long press used to stop must not become a new short-press
  // start when the blocking typing loop returns.
  ignoreButtonUntilRelease = true;
  deviceState = currentText.isEmpty() ? DeviceState::EMPTY
                                      : DeviceState::READY;
}

void typeCurrentText() {
  if (currentText.isEmpty()) {
    deviceState = DeviceState::EMPTY;
    return;
  }

  abortRequested = false;
  typingStopTracking = false;
  typingButtonArmed = (digitalRead(kUserButtonPin) == HIGH);
  deviceState = DeviceState::TYPING;
  keyboard.releaseAll();
  if (!waitWhileServing(kStartDelayMs)) {
    finishStoppedRun();
    return;
  }

  size_t index = 0;
  while (index < currentText.length()) {
    uint32_t codePoint = 0;
    if (!decodeNextCodePoint(currentText, index, codePoint)) {
      deviceState = DeviceState::ERROR;
      keyboard.releaseAll();
      return;
    }

    bool success = true;
    if (codePoint == '\r') {
      // Treat Windows CRLF as one Enter; a bare CR is also Enter.
      if (index < currentText.length() && currentText[index] == '\n') {
        continue;
      }
      success = tapSpecialKey(KEY_RETURN);
    } else if (codePoint == '\n') {
      success = tapSpecialKey(KEY_RETURN);
    } else if (codePoint == '\t') {
      success = tapSpecialKey(KEY_TAB);
    } else if (codePoint == '\b') {
      success = tapSpecialKey(KEY_BACKSPACE);
    } else if (codePoint >= 0x20 && codePoint <= 0x7E) {
      success = tapAsciiKey(static_cast<char>(codePoint));
    } else if (codePoint > 0x7F) {
      success = typeUnicodeCodePoint(codePoint);
    } else {
      success = false;
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
  }

  keyboard.releaseAll();
  if (typingStopTracking || digitalRead(kUserButtonPin) == LOW) {
    ignoreButtonUntilRelease = true;
  }
  typingStopTracking = false;
  deviceState = DeviceState::DONE;
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
  if ((now - rawStateChangedAtMs) < kDebounceMs ||
      rawPressed == stableButtonPressed) {
    return;
  }

  stableButtonPressed = rawPressed;
  if (stableButtonPressed &&
      (deviceState == DeviceState::READY ||
       deviceState == DeviceState::DONE)) {
    startRequested = true;
  }
}

void configureWebServer() {
  webServer.on("/", HTTP_GET, []() {
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send_P(200, "text/html; charset=utf-8", kIndexHtml);
  });

  webServer.on("/api/status", HTTP_GET, []() {
    sendNoCache(200, "application/json; charset=utf-8", statusJson());
  });

  webServer.on("/api/text", HTTP_GET, []() {
    sendNoCache(200, "text/plain; charset=utf-8", currentText);
  });

  webServer.on("/api/start", HTTP_POST, []() {
    if (deviceState == DeviceState::TYPING || startRequested) {
      sendNoCache(409, "application/json; charset=utf-8",
                  F("{\"message\":\"设备已经准备开始或正在输入。\"}"));
      return;
    }
    if (currentText.isEmpty()) {
      sendNoCache(409, "application/json; charset=utf-8",
                  F("{\"message\":\"请先保存正文。\"}"));
      return;
    }
    if (deviceState != DeviceState::READY &&
        deviceState != DeviceState::DONE) {
      sendNoCache(409, "application/json; charset=utf-8",
                  F("{\"message\":\"当前状态不能开始输入。\"}"));
      return;
    }

    startRequested = true;
    sendNoCache(202, "application/json; charset=utf-8",
                F("{\"message\":\"将在 3 秒后开始输入。\"}"));
  });

  webServer.on("/api/stop", HTTP_POST, []() {
    if (deviceState != DeviceState::TYPING) {
      sendNoCache(409, "application/json; charset=utf-8",
                  F("{\"message\":\"设备当前没有输入。\"}"));
      return;
    }

    abortRequested = true;
    sendNoCache(202, "application/json; charset=utf-8",
                F("{\"message\":\"停止指令已发送。\"}"));
  });

  webServer.on("/api/text", HTTP_POST, []() {
    if (deviceState == DeviceState::TYPING) {
      sendNoCache(409, "application/json; charset=utf-8",
                  F("{\"message\":\"正在输入，暂不能修改正文。\"}"));
      return;
    }
    const String uploadedText = webServer.arg("plain");

    if (uploadedText.length() > kMaxTextBytes) {
      sendNoCache(413, "application/json; charset=utf-8",
                  F("{\"message\":\"正文超过 32768 字节上限，未保存。\"}"));
      return;
    }

    const Utf8CountResult result = validateAndCountUtf8(uploadedText);
    if (!result.valid) {
      sendNoCache(400, "application/json; charset=utf-8",
                  F("{\"message\":\"上传内容不是有效的 UTF-8 文本。\"}"));
      return;
    }

    currentText = uploadedText;
    currentCharacterCount = result.characters;
    deviceState = currentText.isEmpty() ? DeviceState::EMPTY
                                        : DeviceState::READY;
    sendNoCache(200, "application/json; charset=utf-8", statusJson());
  });

  webServer.on("/api/clear", HTTP_POST, []() {
    if (deviceState == DeviceState::TYPING) {
      sendNoCache(409, "application/json; charset=utf-8",
                  F("{\"message\":\"正在输入，暂不能清空正文。\"}"));
      return;
    }
    currentText = "";
    currentCharacterCount = 0;
    deviceState = DeviceState::EMPTY;
    sendNoCache(200, "application/json; charset=utf-8", statusJson());
  });

  webServer.onNotFound([]() {
    sendNoCache(404, "text/plain; charset=utf-8", F("Not found"));
  });

  webServer.begin();
}

}  // namespace

void setup() {
#if ARDUINO_USB_MODE != 0
#error "Phase D requires native USB-OTG/TinyUSB mode."
#endif

#if ARDUINO_USB_CDC_ON_BOOT != 0 || ARDUINO_USB_MSC_ON_BOOT != 0 || \
    ARDUINO_USB_DFU_ON_BOOT != 0
#error "Phase D must expose only USB HID Keyboard."
#endif

  pinMode(kUserButtonPin, INPUT_PULLUP);

  keyboard.begin();
  USB.manufacturerName("M5Stack");
  USB.productName("AtomS3U Auto Keyboard D2");
  USB.serialNumber("AK-PHASE-D2");
  USB.usbClass(0x00);
  USB.usbSubClass(0x00);
  USB.usbProtocol(0x00);
  USB.webUSB(false);
  USB.begin();

  currentText.reserve(kMaxTextBytes);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(kAccessPointName, kAccessPointPassword, 6, false, 1);
  configureWebServer();
}

void loop() {
  webServer.handleClient();
  updateButton();
  if (startRequested &&
      (deviceState == DeviceState::READY ||
       deviceState == DeviceState::DONE)) {
    startRequested = false;
    typeCurrentText();
  }
  delay(2);
}
