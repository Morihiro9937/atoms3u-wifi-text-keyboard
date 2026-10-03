#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

namespace {

constexpr char kAccessPointName[] = "AutoKeyboard";
constexpr char kAccessPointPassword[] = "autokeyboard";
constexpr size_t kMaxTextBytes = 32 * 1024;

USBHIDKeyboard keyboard;
WebServer webServer(80);

String currentText;
size_t currentCharacterCount = 0;

const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>AutoKeyboard</title>
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
    button { flex: 1; min-height: 46px; border: 0; border-radius: 10px; font-size: 16px; font-weight: 650; }
    #save { color: white; background: #246bfd; }
    #clear { color: #9f2525; background: #fdecec; }
    button:disabled { opacity: .55; }
    #message { min-height: 24px; margin-top: 12px; font-size: 14px; }
    .ok { color: #137333; }
    .error { color: #b3261e; }
  </style>
</head>
<body>
<main>
  <h1>AutoKeyboard</h1>
  <div class="hint">阶段 C：这里只保存手机上传的 UTF-8 原文，不会自动输入。正文仅保存在设备内存中，断电即消失。</div>
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

async function refreshStatus() {
  try {
    const response = await fetch('/api/status', {cache: 'no-store'});
    const data = await response.json();
    document.querySelector('#savedChars').textContent = data.characters;
    document.querySelector('#savedBytes').textContent = data.bytes;
    document.querySelector('#state').textContent = data.state;
  } catch (_) {
    document.querySelector('#state').textContent = '连接断开';
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
    if (!response.ok) throw new Error('清空失败');
    text.value = '';
    updateEditorCount();
    showMessage('设备中的正文已清空。', 'ok');
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

String statusJson() {
  const char *state = currentText.isEmpty() ? "EMPTY" : "READY";
  String json;
  json.reserve(100);
  json += F("{\"state\":\"");
  json += state;
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

  webServer.on("/api/text", HTTP_POST, []() {
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
    sendNoCache(200, "application/json; charset=utf-8", statusJson());
  });

  webServer.on("/api/clear", HTTP_POST, []() {
    currentText = "";
    currentCharacterCount = 0;
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
#error "Phase C requires native USB-OTG/TinyUSB mode."
#endif

#if ARDUINO_USB_CDC_ON_BOOT != 0 || ARDUINO_USB_MSC_ON_BOOT != 0 || \
    ARDUINO_USB_DFU_ON_BOOT != 0
#error "Phase C must expose only USB HID Keyboard."
#endif

  keyboard.begin();
  USB.manufacturerName("M5Stack");
  USB.productName("AtomS3U Auto Keyboard C");
  USB.serialNumber("AK-PHASE-C");
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
  delay(2);
}
