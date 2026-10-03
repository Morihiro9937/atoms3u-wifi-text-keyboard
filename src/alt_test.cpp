// Isolated Alt + numeric-keypad experiment for AtomS3U.
// Stable phase_e.cpp, its settings and LittleFS text are deliberately untouched.
#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>
#include "USB.h"
#include "USBHID.h"
#include "USBHIDKeyboard.h"
#include "esp32-hal-rgb-led.h"
#include "esp_netif.h"
#include "alt_encoding.h"

namespace {
constexpr char kVersion[] = "2026.10.01-alt-test1";
constexpr size_t kMaxBytes = 32768;
constexpr uint8_t kButton = 41, kLed = 35;
constexpr char kSelfTest[] =
    "中文测试成功：你好世界。\n"
    "患者体温36.5℃，BP 120/80 mmHg，SpO2 98%，WBC 6.2×10^9/L，±μαβ。\n"
    "ABCXYZ abcxyz 0123456789 @#%&/\\()[]{} +-=.,:;!?\n";

// Only this object registers a USB interface. USBHID transport below merely
// sends reports through that SAME standard keyboard interface, with delivery
// errors checked (USBHIDKeyboard::press does not expose transfer failures).
USBHIDKeyboard keyboard;
USBHID transport;
WebServer server(80);
static_assert(sizeof(KeyReport) == 8, "Standard keyboard report must be 8 bytes");

struct Settings {
  karute_alt::Mode mode = karute_alt::Mode::GBK;
  uint16_t downMs = 5, gapMs = 5, afterMs = 10, delayMs = 3000;
  uint16_t repeats = 1;
};
Settings settings;
String loadedText;
std::vector<uint16_t> outputCodes;
enum class State { EMPTY, READY, DELAY, PRINTING, STOPPED, DONE, ERROR };
State state = State::EMPTY;
String lastError;
bool startRequested = false, stopRequested = false;
bool keyLit = false, releasePending = false;
bool buttonRaw = false, buttonStable = false, buttonLongHandled = false;
bool ignoreUntilRelease = false;
uint32_t buttonChangedAt = 0, buttonDownAt = 0;
uint32_t lastReleaseRetryAt = 0, printStartedAt = 0, elapsedMs = 0;
uint32_t completedChars = 0;
volatile bool hostLedsKnown = false;
volatile uint8_t hostLeds = 0;

const char kPage[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>karute Alt 测试</title><style>
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#f3f5f7;color:#17202a;margin:0}
main{max-width:740px;margin:auto;padding:20px 16px}h1{font-size:23px;margin:0 0 8px}
.card{background:white;border-radius:12px;padding:16px;margin:14px 0}
textarea{box-sizing:border-box;width:100%;min-height:240px;font:16px/1.5 sans-serif;border:1px solid #ccd3da;border-radius:8px;padding:10px}
label{display:flex;align-items:center;justify-content:space-between;gap:10px;margin:12px 0}
input,select{font-size:16px;padding:6px;width:145px;box-sizing:border-box}button{font-size:17px;border:0;border-radius:9px;padding:13px 16px;cursor:pointer}
.actions{display:flex;flex-wrap:wrap;gap:9px}.start{background:#137333;color:white}.stop{background:#b3261e;color:white}
button:disabled{opacity:.5}small,.meta{font-size:13px;color:#65717d}.error{color:#b3261e;white-space:pre-wrap}a{color:#246bfd}
</style></head><body><main><h1>karute · Alt测试版</h1>
<small id="version"></small><p><a href="/help">测试说明</a></p>
<section class="card"><textarea id="text" spellcheck="false"></textarea>
<div class="meta" id="count"></div>
<label>输出方式<select id="mode"><option value="1">GBK</option><option value="2">Unicode数字</option></select></label>
<label>按下时间 ms<input id="downMs" type="number" min="1" max="100" value="5"></label>
<label>按键间隔 ms<input id="gapMs" type="number" min="1" max="100" value="5"></label>
<label>每字结束等待 ms<input id="afterMs" type="number" min="1" max="100" value="10"></label>
<label>开始前秒数<input id="delaySeconds" type="number" min="0" max="10" value="3"></label>
<label>重复次数<input id="repeats" type="number" min="1" max="100" value="1"></label>
<div class="actions"><button id="selftest">载入自检文字</button><button id="load">装载</button>
<button id="start" class="start">打印</button><button id="stop" class="stop" disabled>停止</button></div>
</section><p id="status">连接中……</p><p class="meta" id="progress"></p><p class="error" id="error"></p>
</main><script>
const $=id=>document.getElementById(id);
let active=false,pending=false,connected=false;
function counts(){$('count').textContent=`${Array.from($('text').value).length}字 / ${new TextEncoder().encode($('text').value).length}字节`;}
function controls(){
  for(const id of ['load','start','selftest','mode','downMs','gapMs','afterMs','delaySeconds','repeats','text'])
    $(id).disabled=active||pending||!connected;
  $('stop').disabled=!active;
}
async function request(url,body){
  const response=await fetch(url,{method:body?'POST':'GET',body,cache:'no-store',signal:AbortSignal.timeout(10000)});
  const data=await response.json();if(!response.ok)throw Error(data.error||'设备请求失败');return data;
}
async function refresh(){
  try{
    const data=await request('/api/status');connected=true;active=data.active;
    $('version').textContent=data.firmwareVersion;
    const labels={EMPTY:'空白',READY:'待打印',DELAY:'倒计时',PRINTING:'打印中',STOPPED:'已停止',DONE:'完成',ERROR:'错误'};
    $('status').textContent=`${labels[data.state]||data.state} · ${data.mode===1?'GBK':'Unicode数字'} · Num Lock：${data.numLock===null?'未知':data.numLock?'开':'关'}`;
    $('progress').textContent=`设备已装载 ${data.characters}字；预计 ${(data.estimatedMs/1000).toFixed(1)}秒；已发送 ${data.completedChars}字；实际 ${(data.elapsedMs/1000).toFixed(1)}秒`;
    if(data.error)$('error').textContent=data.error;
  }catch(e){connected=false;$('status').textContent='设备失联';}controls();
}
function form(){
  const result=new URLSearchParams();result.set('text',$('text').value);
  for(const id of ['mode','downMs','gapMs','afterMs','delaySeconds','repeats']){
    if(!$(id).checkValidity())throw Error('请检查参数范围');result.set(id,$(id).value);
  }
  if(new TextEncoder().encode($('text').value).length>32768)throw Error('超过32 KB，未装载');
  return result;
}
async function operate(url){
  try{const body=form();pending=true;controls();await request(url,body);$('error').textContent='';}
  catch(e){$('error').textContent=e.message;}finally{pending=false;await refresh();}
}
$('load').onclick=()=>operate('/api/load');$('start').onclick=()=>operate('/api/start');
$('stop').onclick=async()=>{try{await request('/api/stop',new URLSearchParams());}catch(e){$('error').textContent=e.message;}await refresh();};
$('selftest').onclick=async()=>{try{$('text').value=(await request('/api/selftest')).text;counts();}catch(e){$('error').textContent=e.message;}};
$('text').oninput=counts;
(async()=>{
  try{const data=await request('/api/text');$('text').value=data.text;
    $('mode').value=String(data.mode);for(const id of ['downMs','gapMs','afterMs','repeats'])$(id).value=data[id];
    $('delaySeconds').value=data.delayMs/1000;counts();
  }catch(e){$('error').textContent='无法载入设备文字';}await refresh();
})();setInterval(refresh,2000);controls();
</script></body></html>
)HTML";

const char kHelp[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Alt测试说明</title><body style="font:16px/1.7 sans-serif;max-width:740px;margin:24px auto;padding:0 16px">
<h2>karute · 2026.10.01-alt-test1</h2>
<p>这是实验固件，不是已验收的正式版本。没有蓝牙、AI、鼠标、USB串口或磁盘。</p>
<ol><li>先在自己的 Windows 测试机打开空白记事本。选择纯英文 ENG，开启 Num Lock。</li>
<li>手机连接 karute，密码 autokarute，浏览器打开 192.168.4.1。</li>
<li>先选 GBK，载入自检文字，重复1次，按下/间隔/结束等待为5/5/10ms。</li>
<li>把电脑光标放回记事本，再在手机点击打印；或先装载，再短按设备按钮。默认等待3秒。</li>
<li>逐字核对。GBK不正确时，换一个空白文档，选择Unicode数字再试。Word也要独立测试。</li>
<li>第一次完全正确后，再重复10次、50次测试。然后逐步减小时间，不承诺4200字/分钟。</li></ol>
<p>所有可打印字符，包括英文和空格，都以Alt＋数字小键盘输出；换行/Tab/Backspace为正常键。
不用VUC，也不会修改电脑注册表、安装软件或自动切换输入法。GBK与Unicode数字的实际解释取决于Windows及目标软件。</p>
<p>GBK不支持全部Unicode；Unicode数字仅支持基本多文种平面。发现不支持的字或超过32KB会拒绝整篇，绝不偷偷跳过。</p>
<p>停止按钮或打印中长按实体按钮1秒：完成正在发送的这一字后停止，释放所有键，避免半个Alt码。
已输出内容无法撤回，停止后重打从头开始。USB传输失败也会停止；“完成”不代表电脑文本已核验。</p>
<p>文字和参数只在RAM，断电即消失。旧正式版的Flash正文和设置不会被本程序读取、写入或删除。
测试用虚构资料，不要上传患者身份信息。Mac不适用。</p>
<p>LED：空白白色、待机/完成绿色、倒计时绿闪、打印中数字键按下亮松开灭、停止/错误红色。</p>
<p><a href="/">返回</a></p></body></html>
)HTML";

bool active() { return state == State::DELAY || state == State::PRINTING; }
const char *stateName() {
  switch (state) {
    case State::EMPTY: return "EMPTY";
    case State::READY: return "READY";
    case State::DELAY: return "DELAY";
    case State::PRINTING: return "PRINTING";
    case State::STOPPED: return "STOPPED";
    case State::DONE: return "DONE";
    default: return "ERROR";
  }
}
void updateLed() {
  uint8_t red = 0, green = 0, blue = 0;
  if (state == State::EMPTY) red = green = blue = 10;
  else if (state == State::STOPPED || state == State::ERROR) red = 18;
  else if (state == State::DELAY) green = ((millis() / 350) % 2) ? 18 : 0;
  else if (state == State::PRINTING) green = keyLit ? 18 : 0;
  else green = 18;
  // Avoid resending WS2812 frames each loop; blink follows real keypad reports.
  static uint32_t lastColor = UINT32_MAX;
  const uint32_t color = (red << 16) | (green << 8) | blue;
  if (color != lastColor) {
    neopixelWrite(kLed, red, green, blue);
    lastColor = color;
  }
}
void updateButton() {
  const bool raw = digitalRead(kButton) == LOW;
  if (raw != buttonRaw) { buttonRaw = raw; buttonChangedAt = millis(); }
  if (raw != buttonStable && millis() - buttonChangedAt >= 35) {
    buttonStable = raw;
    if (raw) { buttonDownAt = millis(); buttonLongHandled = false; }
    else {
      if (!active() && !ignoreUntilRelease && !buttonLongHandled)
        startRequested = true;
      ignoreUntilRelease = false;
    }
  }
  if (buttonStable && active() && !ignoreUntilRelease && !buttonLongHandled &&
      millis() - buttonDownAt >= 1000) {
    stopRequested = true;
    buttonLongHandled = true;
  }
}
// Stop is observed at character boundaries, NOT halfway through an Alt code.
void serviceWait(uint32_t milliseconds) {
  const uint32_t started = millis();
  do {
    server.handleClient();
    updateButton();
    updateLed();
    delay(1);
  } while (millis() - started < milliseconds);
}

bool report(uint8_t modifiers, uint8_t usage = 0) {
  KeyReport keys = {};
  keys.modifiers = modifiers;
  keys.keys[0] = usage;
  // Check the mature TinyUSB transport completion, not just key-map acceptance.
  const bool success = transport.SendReport(HID_REPORT_ID_KEYBOARD, &keys,
                                            sizeof(keys), 100);
  keyLit = success && usage >= 0x59 && usage <= 0x62;
  updateLed();
  return success;
}
bool releaseAll() {
  const bool success = report(0);
  releasePending = !success;
  return success;
}
bool typeCode(uint16_t value) {
  return karute_alt::emitCode(value, settings.downMs, settings.gapMs,
      settings.afterMs,
      [](uint8_t modifier, uint8_t usage) { return report(modifier, usage); },
      [](uint32_t ms) { serviceWait(ms); });
}

String jsonString(const String &value) {
  String result = "\"";
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t byte = value[i];
    if (byte == '\"' || byte == '\\') { result += '\\'; result += char(byte); }
    else if (byte == '\n') result += "\\n";
    else if (byte == '\r') result += "\\r";
    else if (byte == '\t') result += "\\t";
    else if (byte < 0x20) {
      char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
      result += escaped;
    } else result += char(byte);
  }
  return result + "\"";
}
uint32_t estimateMs() {
  uint64_t total = 0;
  for (uint16_t code : outputCodes)
    total += karute_alt::characterEstimateMs(code, settings.downMs,
                                            settings.gapMs, settings.afterMs);
  total = total * settings.repeats + settings.delayMs;
  return total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
}
String settingsJson() {
  return String("\"mode\":") + static_cast<unsigned>(settings.mode) +
      ",\"downMs\":" + settings.downMs + ",\"gapMs\":" + settings.gapMs +
      ",\"afterMs\":" + settings.afterMs + ",\"delayMs\":" + settings.delayMs +
      ",\"repeats\":" + settings.repeats;
}
String statusJson() {
  String result = String("{\"firmwareVersion\":\"") + kVersion +
      "\",\"state\":\"" + stateName() + "\",\"active\":" +
      (active() ? "true" : "false") + "," + settingsJson();
  result += ",\"characters\":" + String(outputCodes.size());
  result += ",\"estimatedMs\":" + String(estimateMs());
  result += ",\"completedChars\":" + String(completedChars);
  result += ",\"elapsedMs\":" + String(state == State::PRINTING ?
                                            millis() - printStartedAt : elapsedMs);
  result += ",\"numLock\":";
  result += hostLedsKnown ? ((hostLeds & LED_NUMLOCK) ? "true" : "false") : "null";
  result += ",\"error\":" + jsonString(lastError) + "}";
  return result;
}
void respond(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", body);
}
void reject(int code, const String &message) {
  respond(code, String("{\"error\":") + jsonString(message) + "}");
}

bool unsignedArgument(const char *name, unsigned low, unsigned high,
                      uint16_t &result) {
  if (!server.hasArg(name)) return false;
  const String value = server.arg(name);
  if (value.isEmpty() || value.length() > 5) return false;
  for (size_t i = 0; i < value.length(); ++i)
    if (value[i] < '0' || value[i] > '9') return false;
  const unsigned parsed = value.toInt();
  if (parsed < low || parsed > high) return false;
  result = parsed;
  return true;
}
bool prepare(const String &text, karute_alt::Mode mode,
             std::vector<uint16_t> &codes, String &error) {
  if (text.length() > kMaxBytes) { error = "超过32 KB，未装载"; return false; }
  if (text.isEmpty()) { error = "请先放入文字"; return false; }
  codes.reserve(text.length());
  size_t index = 0, character = 0;
  while (index < text.length()) {
    uint32_t codepoint;
    if (!karute_alt::decode(text.c_str(), text.length(), index, codepoint)) {
      error = "UTF-8格式错误，未装载"; return false;
    }
    ++character;
    if (codepoint == '\r' && index < text.length() && text[index] == '\n')
      continue;  // CRLF is exactly ONE Enter.
    uint16_t code;
    if (!karute_alt::outputCode(mode, codepoint, code)) {
      char description[100];
      snprintf(description, sizeof(description),
               "第%u个字符 U+%04lX 不支持此模式；整篇未装载，也未打印",
               static_cast<unsigned>(character), static_cast<unsigned long>(codepoint));
      error = description;
      return false;
    }
    codes.push_back(code);
  }
  return true;
}
bool loadRequest() {
  if (active() || startRequested) { reject(409, "正在打印，请先停止"); return false; }
  Settings candidate;
  uint16_t mode, seconds;
  if (!unsignedArgument("mode", 1, 2, mode) ||
      !unsignedArgument("downMs", 1, 100, candidate.downMs) ||
      !unsignedArgument("gapMs", 1, 100, candidate.gapMs) ||
      !unsignedArgument("afterMs", 1, 100, candidate.afterMs) ||
      !unsignedArgument("delaySeconds", 0, 10, seconds) ||
      !unsignedArgument("repeats", 1, 100, candidate.repeats) ||
      !server.hasArg("text")) {
    reject(400, "参数无效，未装载"); return false;
  }
  candidate.mode = static_cast<karute_alt::Mode>(mode);
  candidate.delayMs = seconds * 1000;
  const String text = server.arg("text");
  std::vector<uint16_t> codes;
  String error;
  if (!prepare(text, candidate.mode, codes, error)) {
    reject(text.length() > kMaxBytes ? 413 : 400, error); return false;
  }
  // Atomic replacement only after the ENTIRE text has passed validation.
  loadedText = text;
  outputCodes.swap(codes);
  settings = candidate;
  completedChars = elapsedMs = 0;
  lastError = "";
  state = State::READY;
  updateLed();
  return true;
}

void run() {
  startRequested = false;
  if (outputCodes.empty()) { lastError = "没有已装载文字"; state = State::ERROR; return; }
  if (!releaseAll()) {
    lastError = "USB键盘未就绪，请插入Windows测试机";
    state = State::ERROR; return;
  }
  if (hostLedsKnown && !(hostLeds & LED_NUMLOCK)) {
    lastError = "请先开启电脑的Num Lock，再打印";
    state = State::ERROR; return;
  }
  // Keep a Stop that arrived while Start was queued; do not discard it.
  ignoreUntilRelease = buttonStable;
  completedChars = elapsedMs = 0;
  lastError = "";
  state = State::DELAY;
  const uint32_t countdown = millis();
  while (!stopRequested && millis() - countdown < settings.delayMs) serviceWait(1);
  bool success = true;
  printStartedAt = millis();
  if (!stopRequested) state = State::PRINTING;
  for (unsigned repeat = 0; repeat < settings.repeats && !stopRequested && success; ++repeat) {
    for (uint16_t code : outputCodes) {
      if (stopRequested) break;
      if (!typeCode(code)) { success = false; break; }
      ++completedChars;  // USB delivery only, NOT proof of text correctness.
    }
  }
  elapsedMs = millis() - printStartedAt;
  const bool released = releaseAll();
  keyLit = false;
  if (!success || !released) {
    state = State::ERROR;
    lastError = "USB发送失败，已停止；核对并清除可能的半个字符后重试";
  } else state = stopRequested ? State::STOPPED : State::DONE;
  stopRequested = false;
  updateLed();
}

bool disableRouterAdvertisement() {
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap) return false;
  esp_err_t status = esp_netif_dhcps_stop(ap);
  if (status != ESP_OK && status != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return false;
  uint8_t advertise = 0;
  status = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET,
      ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &advertise, sizeof(advertise));
  const esp_err_t restarted = esp_netif_dhcps_start(ap);
  return status == ESP_OK && (restarted == ESP_OK ||
      restarted == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED);
}
void configureServer() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kPage);
  });
  server.on("/help", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kHelp);
  });
  server.on("/api/status", HTTP_GET, []() { respond(200, statusJson()); });
  server.on("/api/text", HTTP_GET, []() {
    respond(200, String("{\"text\":") + jsonString(loadedText) + "," + settingsJson() + "}");
  });
  server.on("/api/selftest", HTTP_GET, []() {
    respond(200, String("{\"text\":") + jsonString(String(kSelfTest)) + "}");
  });
  server.on("/api/load", HTTP_POST, []() {
    if (loadRequest()) respond(200, statusJson());
  });
  server.on("/api/start", HTTP_POST, []() {
    if (!loadRequest()) return;
    if (!transport.ready() || releasePending) { reject(409, "USB键盘未就绪"); return; }
    stopRequested = false;
    startRequested = true;
    respond(202, "{\"accepted\":true}");
  });
  server.on("/api/stop", HTTP_POST, []() {
    if (active() || startRequested) stopRequested = true;
    respond(202, "{\"accepted\":true}");
  });
  server.onNotFound([]() { reject(404, "未找到页面"); });
  server.begin();
}
}  // namespace

void setup() {
#if ARDUINO_USB_MODE != 0 || ARDUINO_USB_CDC_ON_BOOT != 0 || \
    ARDUINO_USB_MSC_ON_BOOT != 0 || ARDUINO_USB_DFU_ON_BOOT != 0
#error "Alt test requires USB-OTG with HID keyboard ONLY."
#endif
  pinMode(kButton, INPUT_PULLUP);
  pinMode(kLed, OUTPUT);
  // Booting while holding the physical button must never trigger typing.
  buttonRaw = buttonStable = digitalRead(kButton) == LOW;
  ignoreUntilRelease = buttonStable;
  keyboard.onEvent(ARDUINO_USB_HID_KEYBOARD_LED_EVENT,
      [](void *, esp_event_base_t, int32_t, void *data) {
        hostLeds = static_cast<arduino_usb_hid_keyboard_event_data_t *>(data)->leds;
        hostLedsKnown = true;
      });
  keyboard.begin();
  USB.manufacturerName("M5Stack");
  USB.productName("karute Alt Test Keyboard");
  USB.serialNumber("AK-ALT-TEST1");
  USB.usbClass(0); USB.usbSubClass(0); USB.usbProtocol(0);
  USB.webUSB(false);
  USB.begin();
  String error;
  if (prepare(String(kSelfTest), settings.mode, outputCodes, error)) {
    loadedText = kSelfTest;
    state = State::READY;
  } else { state = State::ERROR; lastError = error; }
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  const bool started = WiFi.softAP("karute", "autokarute", 6, false, 1);
  if (!started || !disableRouterAdvertisement()) {
    state = State::ERROR; lastError = "Wi-Fi启动或本地路由配置失败";
  }
  configureServer();
  updateLed();
}

void loop() {
  server.handleClient();
  updateButton();
  if (releasePending && !active() && millis() - lastReleaseRetryAt >= 250) {
    lastReleaseRetryAt = millis();
    // A host reconnection after a failed send must receive an empty report
    // before any new input. Do not restart the interrupted text automatically.
    if (transport.ready()) releaseAll();
  }
  if (startRequested && !active()) run();
  updateLed();
  delay(1);
}
