// Browser logic checks without a device: no USB output or patient text.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('src/alt_test.cpp', 'utf8');
const script = source.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
const initial = {mode: '1', downMs: '5', gapMs: '5', afterMs: '10', delaySeconds: '3', repeats: '1', text: ''};
const limits = {mode: [1, 2], downMs: [1, 100], gapMs: [1, 100], afterMs: [1, 100], delaySeconds: [0, 10], repeats: [1, 100]};
function element(id) {
  if (!elements.has(id)) {
    elements.set(id, {value: initial[id] || '', textContent: '', disabled: false,
      checkValidity() { const n = Number(this.value); const range = limits[id];
        return Number.isInteger(n) && n >= range[0] && n <= range[1]; }});
  }
  return elements.get(id);
}
let status = {firmwareVersion: '2026.10.01-alt-test1', state: 'READY', active: false,
  mode: 1, characters: 4, completedChars: 0, estimatedMs: 3000, elapsedMs: 0, numLock: true, error: ''};
const requests = [];
const context = vm.createContext({document: {getElementById: element}, URLSearchParams, TextEncoder,
  AbortSignal, setInterval: () => {},
  fetch: async (url, options) => {
    requests.push({url, options});
    let data = {};
    if (url === '/api/status') data = {...status};
    if (url === '/api/text') data = {text: '中文AB', mode: 1, downMs: 5, gapMs: 5, afterMs: 10, delayMs: 3000, repeats: 1};
    if (url === '/api/selftest') data = {text: '自检中文ABC\n'};
    if (url === '/api/start') status = {...status, state: 'DELAY', active: true};
    if (url === '/api/stop') status = {...status, state: 'STOPPED', active: false};
    return {ok: true, json: async () => data};
  }});
const tick = () => new Promise(resolve => setImmediate(resolve));
(async () => {
  vm.runInContext(script, context);
  await tick();
  assert.equal(element('text').value, '中文AB');
  assert.equal(element('count').textContent, '4字 / 8字节');
  assert.equal(element('start').disabled, false);
  // Loading the built-in test only changes the editor; never starts typing.
  const before = requests.filter(r => r.url === '/api/start').length;
  await element('selftest').onclick();
  assert.equal(element('text').value, '自检中文ABC\n');
  assert.equal(requests.filter(r => r.url === '/api/start').length, before);
  element('text').value = '中'.repeat(10923);  // 32769 bytes, must not truncate.
  await element('start').onclick();
  assert.equal(requests.filter(r => r.url === '/api/start').length, before);
  assert.match(element('error').textContent, /32 KB/);
  element('text').value = '你好AB';
  element('downMs').value = '0';
  await element('start').onclick();
  assert.equal(requests.filter(r => r.url === '/api/start').length, before);
  element('downMs').value = '1'; element('mode').value = '2';
  await element('start').onclick();
  const start = requests.find(r => r.url === '/api/start');
  assert.equal(start.options.method, 'POST');
  assert.equal(start.options.body.get('text'), '你好AB');
  assert.equal(start.options.body.get('mode'), '2');
  assert.equal(start.options.body.get('downMs'), '1');
  assert.equal(element('text').disabled, true);
  assert.equal(element('stop').disabled, false);
  await element('stop').onclick();
  assert.equal(requests.find(r => r.url === '/api/stop').options.method, 'POST');
  assert.equal(element('text').disabled, false);
  assert.equal(element('stop').disabled, true);
  console.log('PASS: webpage initialization, UTF-8 byte limits, validation, self-test no-autostart, latest-text start, stop and editor locking');
})().catch(error => { console.error(error); process.exitCode = 1; });
