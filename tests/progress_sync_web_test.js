const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync('web/index.html', 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map(), requests = [], timers = [];
let now = 10000, fail = false, hold = null;
let status = {linked: true, commandCode: 200, state: 'READY', mode: 4,
  revision: 1, savedRevision: 1, storageReady: true, storagePending: false,
  text: '中文AB', hasPrintProgress: false, progressPercent: 0, remainingMs: 0,
  altBeforeMs: 5, altDownMs: 5, altIntervalMs: 5, altReleaseMs: 5, altAfterMs: 10,
  startDelayMs: 3000};
function el(selector) {
  if (!elements.has(selector)) elements.set(selector, {value: '', hidden: false,
    textContent: '', style: {}, attrs: {}, listeners: {}, classes: new Set(),
    addEventListener(k, fn) {this.listeners[k] = fn;},
    setAttribute(k, v) {this.attrs[k] = v;},
    classList: {add(k) {el(selector).classes.add(k);}, toggle(k, v) {
      if (v) el(selector).classes.add(k); else el(selector).classes.delete(k);
    }}});
  return elements.get(selector);
}
const ctx = vm.createContext({document: {querySelector: el}, TextEncoder,
  URLSearchParams, AbortController, Date: {now: () => now},
  setTimeout: () => 1, clearTimeout() {}, setInterval(fn, ms) {timers.push({fn, ms});},
  fetch: async (url, options) => {
    requests.push({url, options});
    if (hold) await hold;
    if (fail) throw Error('offline');
    const query = new URLSearchParams(url.split('?')[1]);
    if (query.get('save') === '1') {
      status = {...status, savedRevision: Number(query.get('revision')),
        revision: Number(query.get('revision')), storagePending: false};
    }
    return {ok: true, json: async () => ({...status})};
  }});
const run = expression => vm.runInContext(expression, ctx);
const tick = () => new Promise(resolve => setImmediate(resolve));
(async () => {
  vm.runInContext(script, ctx);
  await tick();
  assert.equal(timers.length, 1); assert.equal(timers[0].ms, 1000);
  assert.equal(requests.length, 1);
  assert.equal(el('#text').value, '中文AB');
  assert.equal(el('#progress').hidden, true);
  assert.equal(el('#summaryChars').hidden, false);

  status = {...status, state: 'TYPING', hasPrintProgress: true,
    progressPercent: 17, remainingMs: 89901};
  await run('refreshStatus()');
  assert.equal(el('#progressFill').style.width, '17%');
  assert.equal(el('#summaryTime').textContent, '1分30秒');
  assert.equal(el('#summaryChars').hidden, true);
  assert.equal(el('#actionLabel').textContent, '暂停');
  const count = requests.length;
  fail = true; await run('refreshStatus()');
  assert.equal(requests.length, count + 1);
  assert.equal(el('#progressFill').style.width, '17%');
  assert.equal(el('#summaryTime').textContent, '1分30秒');
  assert.equal(el('#progress').classes.has('offline'), true);
  assert.equal(el('#action').disabled, true);
  fail = false;
  status = {...status, state: 'PAUSED', progressPercent: 23, remainingMs: 61000};
  await run('refreshStatus()');
  assert.equal(el('#progress').classes.has('offline'), false);
  assert.equal(el('#actionLabel').textContent, '继续');
  assert.equal(el('#summaryTime').textContent, '1分1秒');

  status = {...status, state: 'DONE', progressPercent: 100, remainingMs: 0};
  await run('refreshStatus()');
  assert.equal(el('#progressFill').style.width, '100%');
  assert.equal(el('#progress').hidden, false);
  assert.equal(el('#summaryTime').textContent, '0秒');

  // Click waits for the next heartbeat; saving and starting share that request.
  el('#text').value = '新正文\r\nABC'; el('#text').listeners.input();
  assert.equal(el('#progress').hidden, true);
  const beforeClick = requests.length;
  const click = el('#action').listeners.click();
  assert.equal(requests.length, beforeClick);
  status = {...status, state: 'TYPING', startPending: true,
    hasPrintProgress: true, progressPercent: 0, remainingMs: 3500};
  await run('refreshStatus()'); await click;
  const start = requests.at(-1);
  assert.match(start.url, /command=start/); assert.match(start.url, /save=1/);
  assert.equal(start.options.body, '新正文\r\nABC');
  assert.equal(start.options.headers['Content-Type'], 'text/plain; charset=utf-8');
  assert.equal(requests.length, beforeClick + 1);
  assert.equal(el('#summaryTime').textContent, '4秒');

  status = {...status, state: 'READY', startPending: false, hasPrintProgress: false};
  await run('refreshStatus()');
  el('#text').value = '自动保存'; el('#text').listeners.input();
  await run('refreshStatus()'); // Dirty notice shares heartbeat.
  assert.match(requests.at(-1).url, /dirty=1/);
  assert.equal(requests.at(-1).options.body, '');
  now += 1500; await run('refreshStatus()');
  assert.match(requests.at(-1).url, /save=1/);
  assert.equal(requests.at(-1).options.body, '自动保存');

  // A firmware command rejection remains connected and is not retried.
  const stop = run("performCommand('stop')");
  status = {...status, commandCode: 409, commandResult: {message: '当前没有输入。'}};
  await run('refreshStatus()'); await stop;
  assert.equal(run('connected'), true);
  await run('refreshStatus()');
  assert.doesNotMatch(requests.at(-1).url, /command=stop/);
  status = {...status, commandCode: 200, commandResult: {}};

  // Lost save/start acknowledgement: first poll state, recover the saved
  // revision, and never replay the start or rewrite text during printing.
  el('#text').value = '断联测试'; el('#text').listeners.input();
  const lostStart = run("performCommand('start')");
  fail = true; await run('refreshStatus()'); await lostStart;
  const sentRevision = Number(new URLSearchParams(requests.at(-1).url.split('?')[1]).get('revision'));
  status = {...status, state: 'TYPING', startPending: false, hasPrintProgress: true,
    progressPercent: 8, remainingMs: 2000, revision: sentRevision,
    savedRevision: sentRevision, storagePending: false};
  fail = false; await run('refreshStatus()');
  assert.doesNotMatch(requests.at(-1).url, /save=1|command=start/);
  assert.equal(run('localDirty()'), false);
  assert.equal(el('#progressFill').style.width, '8%');
  assert.equal(el('#progress').hidden, false);

  // No overlapping heartbeat request, even when the previous one stalls.
  let release; hold = new Promise(resolve => {release = resolve;});
  const first = run('refreshStatus()'), inFlightCount = requests.length;
  await run('refreshStatus()'); assert.equal(requests.length, inFlightCount);
  release(); await first; hold = null;
  assert(requests.every(r => r.url.startsWith('/api/sync?')));
  assert.equal((script.match(/await fetch\(/g) || []).length, 1);
  console.log('PASS: one-second unified transport, grouped save/start, progress, countdown, pause, completion, freeze/reconnect, rejection and overlap');
})().catch(error => {console.error(error); process.exitCode = 1;});
