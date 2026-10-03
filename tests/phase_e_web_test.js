// Runs the real embedded scripts without a browser, device or USB output.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/phase_e.cpp', 'utf8');
const webPages = ['index', 'settings', 'wifi', 'help'].map(name =>
  fs.readFileSync(`web/${name}.html`, 'utf8'));
assert.match(webPages[2], /id="wifiPassword" type="text"/);
assert.doesNotMatch(webPages[2], /id="wifiPassword" type="password"/);
assert.match(webPages[2], /id="loginUrl" type="url"/);
assert.match(webPages[2], /id="backupUrl" class="fixed">http:\/\/192\.168\.4\.1/);
assert.match(webPages[1], /<button id="save">保存设置<\/button>\s*<div class="utility-row"><a class="utility-link" href="\/settings\/wifi">Wi-Fi 设置<\/a>/);
assert.doesNotMatch(webPages[1], /class="subpage-link"/);
assert.doesNotMatch(webPages[0], /<h1>karute<\/h1>/);
assert.match(webPages[0], /viewport-fit=cover/);
assert.match(webPages[0], /body \{ position: fixed; inset: 0;/);
assert.match(webPages[0], /height: 100dvh;/);
assert.match(webPages[0], /textarea \{[^}]*flex: 1 1 auto;[^}]*overflow-y: auto;/);
assert.match(webPages[0], /const statusIntervalMs = 1000;/);
assert.match(webPages[0], /setInterval\(refreshStatus, statusIntervalMs\);/);
assert.match(webPages[0], /#action:disabled \{ color: white; background: #9aa4ae; opacity: 1; \}/);
assert.match(webPages[3], /红0\.4秒、绿1\.6秒循环/);
assert.match(webPages[3], /约6秒未收到网页互联心跳/);
const webSource = webPages.join('\n');
const scripts = webPages.map(page =>
  page.match(/<script>([\s\S]*?)<\/script>/)[1]);
const defaults = {mode: 4, vucDownMs: 5, vucIntervalMs: 20, vucBeforeSpaceMs: 20,
  vucAfterSpaceMs: 20, engDownMs: 2, engIntervalMs: 2, altBeforeMs: 5,
  altDownMs: 5, altIntervalMs: 5, altReleaseMs: 5, altAfterMs: 10,
  startDelayMs: 3000, inputLedEnabled: true, standbyLedEnabled: true};
const tick = () => new Promise(resolve => setImmediate(resolve));
function harness(script, initialSettings, initialWifi) {
  const elements = new Map(), requests = [];
  let status = {...defaults, ...initialSettings, state: 'READY', revision: 1,
    storageReady: true, storagePending: false, inputError: '', linked: true, commandCode: 200, text: '中文AB', hasPrintProgress: false};
  let wifi = {wifiName: 'karute', password: 'autokarute',
    loginUrl: 'http://karute.local', backupUrl: 'http://192.168.4.1',
    ...initialWifi};
  function element(selector) {
    if (!elements.has(selector)) elements.set(selector, {
      value: '', checked: false, hidden: false, textContent: '', listeners: {},
      addEventListener(type, fn) { this.listeners[type] = fn; },
      style: {}, setAttribute() {}, classList: {add() {}, toggle() {}}});
    return elements.get(selector);
  }
  const context = vm.createContext({document: {querySelector: element},
    TextEncoder, URLSearchParams, AbortController, confirm: () => true,
    setTimeout: () => 1, clearTimeout() {}, setInterval() {},
    fetch: async (url, options) => {
      requests.push({url, options});
      if (url === '/api/wifi' && options?.method === 'POST') {
        wifi.wifiName = options.body.get('wifiName');
        wifi.password = options.body.get('password');
        wifi.loginUrl = options.body.get('loginUrl');
      }
      if (url === '/api/settings' && options?.method === 'POST') {
        for (const [key, value] of options.body) status[key] = Number(value);
        status.startDelayMs = Number(options.body.get('startDelaySeconds')) * 1000;
      }
      if (url === '/api/settings/reset') status = {...status, ...defaults};
      return {ok: true,
        json: async () => url === '/api/wifi' ? ({...wifi}) : ({...status}),
        text: async () => '中文AB'};
    }});
  vm.runInContext(script, context);
  return {context, element, requests, setStatus: value => {status = {...status, ...value};}};
}
(async () => {
  assert(webSource.indexOf('<option value="4">GBK</option>') < webSource.indexOf('<option value="2">ENG</option>'));
  assert(webSource.indexOf('<option value="2">ENG</option>') < webSource.indexOf('<option value="1">VUC</option>'));
  assert.doesNotMatch(webSource, /<option[^>]*>[^<]*(Unicode|ASCII 混合|GBK\+ASCII)/);
  assert.match(webSource, /href="\/settings\/wifi">Wi-Fi 设置<\/a>/);
  assert.match(webSource, /<div class="top"><h1>输入设置<\/h1><a href="\/">返回<\/a>/);
  assert.match(webSource, /<div class="top"><h1>Wi-Fi 设置<\/h1><a href="\/settings">返回<\/a>/);

  const settings = harness(scripts[1], {mode: 4, vucDownMs: 7});
  await tick();
  assert.equal(settings.element('#vucDown').value, 7);
  assert.equal(settings.element('#inputLed').checked, true);
  assert.equal(settings.element('#standbyLed').checked, true);

  const wifi = harness(scripts[2], {}, {wifiName: 'Ward-Keyboard',
    password: 'safe-pass-123', loginUrl: 'http://ward-keyboard.local'});
  await tick();
  assert.equal(wifi.element('#wifiName').value, 'Ward-Keyboard');
  assert.equal(wifi.element('#wifiPassword').value, 'safe-pass-123');
  assert.equal(wifi.element('#loginUrl').value, 'http://ward-keyboard.local');
  assert.equal(wifi.element('#backupUrl').textContent, 'http://192.168.4.1');
  wifi.element('#wifiName').value = 'karute-test';
  wifi.element('#wifiPassword').value = 'new-password';
  wifi.element('#loginUrl').value = 'http://karute-test.local';
  await wifi.element('#saveWifi').listeners.click();
  const wifiPost = wifi.requests.find(r => r.url === '/api/wifi' &&
    r.options?.method === 'POST');
  assert.equal(wifiPost.options.body.get('wifiName'), 'karute-test');
  assert.equal(wifiPost.options.body.get('password'), 'new-password');
  assert.equal(wifiPost.options.body.get('loginUrl'), 'http://karute-test.local');
  for (const mode of [4, 2, 1]) {
    settings.element('#mode').value = String(mode);
    vm.runInContext('showMode()', settings.context);
    assert.equal(settings.element('#vucFields').hidden, mode !== 1);
    assert.equal(settings.element('#engFields').hidden, mode !== 2);
    assert.equal(settings.element('#altFields').hidden, mode !== 4);
  }
  settings.element('#mode').value = '4';
  settings.element('#altBefore').value = '7';
  settings.element('#altDown').value = '8';
  settings.element('#altInterval').value = '9';
  settings.element('#altRelease').value = '10';
  settings.element('#altAfter').value = '11';
  settings.element('#inputLed').checked = false;
  settings.element('#standbyLed').checked = true;
  await settings.element('#save').listeners.click();
  const post = settings.requests.find(r => r.options?.method === 'POST');
  assert.equal(post.options.body.get('mode'), '4');
  assert.equal(post.options.body.get('vucDownMs'), '7');
  assert.equal(post.options.body.get('altBeforeMs'), '7');
  assert.equal(post.options.body.get('altDownMs'), '8');
  assert.equal(post.options.body.get('altIntervalMs'), '9');
  assert.equal(post.options.body.get('altReleaseMs'), '10');
  assert.equal(post.options.body.get('altAfterMs'), '11');
  assert.equal(post.options.body.get('inputLedEnabled'), '0');
  assert.equal(post.options.body.get('standbyLedEnabled'), '1');
  await settings.element('#reset').listeners.click();
  assert.equal(settings.element('#altBefore').value, 5);
  assert.equal(settings.element('#altDown').value, 5);
  assert.equal(settings.element('#altInterval').value, 5);
  assert.equal(settings.element('#altRelease').value, 5);
  assert.equal(settings.element('#altAfter').value, 10);
  assert.equal(settings.element('#inputLed').checked, true);
  assert.equal(settings.element('#standbyLed').checked, true);

  const main = harness(scripts[0]);
  await tick();
  assert.equal(main.element('#summaryChars').textContent, '4字');
  for (const [mode, body, ms] of [[1, '中', 3200], [2, 'Ab', 3008],
      [4, '中', 3065], [4, 'A', 3035], [4, '℃', 3065], [4, '\r\n', 3015]]) {
    main.setStatus({mode}); await vm.runInContext('refreshStatus()', main.context);
    main.element('#text').value = body;
    assert.equal(vm.runInContext('estimateEditorMs()', main.context), ms);
  }
  main.setStatus({mode: 4, inputError: '第1个字符不支持，整篇未打印。'});
  await vm.runInContext('refreshStatus()', main.context);
  assert.match(main.element('#errorToast').textContent, /整篇未打印/);
  main.setStatus({state: 'TYPING'});
  await vm.runInContext('refreshStatus()', main.context);
  assert.equal(main.element('#actionLabel').textContent, '暂停');
  assert.equal(main.element('#text').readOnly, true);
  main.setStatus({state: 'PAUSED'});
  await vm.runInContext('refreshStatus()', main.context);
  assert.equal(main.element('#actionLabel').textContent, '继续');
  main.setStatus({state: 'STOPPED'});
  await vm.runInContext('refreshStatus()', main.context);
  assert.equal(main.element('#actionLabel').textContent, '打印');
  assert.match(webSource, />输入指示灯</);
  assert.match(webSource, />待机指示灯</);
  assert.match(source, /constexpr uint32_t kWifiReportHoldMs = 3000/);
  assert.match(source, /F\("SSID\\n"\)/);
  assert.doesNotMatch(source, /Wi-Fi network name/);
  assert.match(source, /\\nPassword\\n/);
  assert.match(source, /\\nLogin URL\\n/);
  assert.match(source, /\\nBackup URL\\n/);
  assert.match(source, /kBackupLoginUrl\[\] = "http:\/\/192\.168\.4\.1"/);
  assert.match(source, /MDNS\.begin\(loginHost\)/);
  assert.match(source, /parseLocalLoginUrl\(webServer\.arg\("loginUrl"\)/);
  assert.match(source, /These credentials have now been printed /);
  assert.match(source, /success = typeAltCodePoint\(static_cast<uint8_t>\(report\[index\]\)\)/);
  assert.match(source, /if \(!altTransportReady\(inputError\)\)/);
  assert.doesNotMatch(source, /softAPgetStationNum/);
  assert.match(source, /kLinkHeartbeatTimeoutMs = 6000/);
  assert.match(source, /linkHeartbeatSeen = true/);
  assert.match(source, /lastLinkHeartbeatAtMs = millis\(\)/);
  assert.match(source, /karute_led::wifiDisconnectedRed/);
  assert.match(source, /tapRawShortcut\(0x08, 0x15, kRunDialogOpenWaitMs\)/);
  assert.match(source, /constexpr char kCommand\[\] = "notepad"/);
  assert.match(source, /return waitWhileServing\(kNotepadOpenWaitMs\)/);
  assert.match(source, /if \(changed\) updated\.credentialReportCount = 0/);
  assert.match(source, /Record the disclosure attempt before sending any credential keystrokes/);
  assert.match(source, /Content-Encoding", "gzip"/);
  assert.match(source, /karute_web::kIndexHtmlGz/);
  assert.doesNotMatch(source, /<!doctype html>/);
  console.log('PASS: modes, Wi-Fi page, audited recovery report, LEDs, estimates and controls');
})().catch(error => {console.error(error); process.exitCode = 1;});
