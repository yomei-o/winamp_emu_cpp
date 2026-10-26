// Opens the page in headless Chrome, waits for Winamp to come up, plays the
// sample MP3 from the file panel and checks that the sound really goes out
// (the AudioWorklet consumes frames at the sample rate).  Screenshots in tmp/.
//   node tools/web_test.mjs
// Serves the repository root with the COOP/COEP headers itself (coi-sw.js is
// what does that on GitHub Pages).  CHROME=... picks the browser.
import http from 'http';
import fs from 'fs';
import path from 'path';
import os from 'os';
import cp from 'child_process';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Z]:)/, '$1')), '..');
const chrome = process.env.CHROME || ['C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', '/usr/bin/google-chrome'].find((p) => fs.existsSync(p));
if (!chrome) { console.log('SKIP no Chrome or Edge'); process.exit(0); }
const OUT = path.join(root, 'tmp');
fs.mkdirSync(OUT, {recursive: true});
const PORT = process.env.CDP_PORT || '9344';
const types = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm', '.data': 'application/octet-stream'};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0;
const check = (c, m) => { console.log((c ? 'ok   ' : 'BAD  ') + m); if (!c) fails++; };

const server = http.createServer((req, res) => {
  let p = decodeURIComponent(req.url.split('?')[0]);
  if (p === '/') p = '/index.html';
  const f = path.join(root, p);
  if (!f.startsWith(root) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); return res.end(); }
  res.writeHead(200, {'content-type': types[path.extname(f)] || 'application/octet-stream',
    'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp'});
  fs.createReadStream(f).pipe(res);
});

await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'waemu-chrome-'));
const proc = cp.spawn(chrome, ['--headless=new', '--remote-debugging-port=' + PORT, '--user-data-dir=' + prof,
  '--no-first-run', '--disable-gpu', '--autoplay-policy=no-user-gesture-required', '--window-size=1400,1500', 'about:blank'], {stdio: 'ignore'});
let targets = [];
for (let i = 0; i < 50; i++) {
  try { targets = await (await fetch('http://127.0.0.1:' + PORT + '/json')).json(); if (targets.length) break; } catch (e) {}
  await sleep(200);
}
const pageT = targets.find((t) => t.type === 'page');
const ws = new WebSocket(pageT.webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0;
const waiting = new Map();
const logs = [];
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  if (m.method === 'Runtime.consoleAPICalled') logs.push(m.params.args.map((a) => a.value).join(' '));
  if (m.method === 'Runtime.exceptionThrown') logs.push('EXC ' + JSON.stringify(m.params.exceptionDetails.text));
  if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
};
const send = (method, params = {}) => new Promise((r) => { const n = ++id; waiting.set(n, r); ws.send(JSON.stringify({id: n, method, params})); });
const ev = async (expr) => (await send('Runtime.evaluate', {expression: expr, returnByValue: true})).result.result.value;
const shot = async (name) => {
  const d = (await send('Page.captureScreenshot', {format: 'png'})).result.data;
  fs.writeFileSync(path.join(OUT, name), Buffer.from(d, 'base64'));
};
const mouse = (type, x, y) => send('Input.dispatchMouseEvent', {type, x, y, button: 'left', buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1});

try {
  await send('Page.enable');
  await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', {width: 1400, height: 1500, deviceScaleFactor: 1, mobile: false});
  await send('Page.navigate', {url: process.env.URL || ('http://127.0.0.1:' + port + '/')});
  // On GitHub Pages the first load installs coi-sw.js and reloads once.
  let isolated = false;
  for (let i = 0; i < 40 && !isolated; i++) { await sleep(500); isolated = !!(await ev('window.crossOriginIsolated')); }
  check(isolated, 'cross-origin isolated');
  await sleep(10000);
  await sleep(8000);
  await shot('web_start.png');
  console.log('status: ' + await ev("document.getElementById('status').textContent"));
  check((await ev("document.getElementById('status').textContent")).includes('動作中'), 'Winamp is on the screen');
  await ev("document.getElementById('sound').click()");
  await sleep(1000);
  // ▶ on doremi.mp3 in the panel.
  console.log('play: ' + await ev("(() => { const r = [...document.querySelectorAll('#files tr')].find((r) => r.textContent.includes('doremi.mp3')); if (!r) return 'no row'; r.querySelector('button').click(); return r.querySelector('button').textContent; })()"));
  await sleep(2000);
  const f0 = await ev('Atomics.load(window.waAudioHeader, 1)');
  const t0 = Date.now();
  await sleep(3000);
  const f1 = await ev('Atomics.load(window.waAudioHeader, 1)');
  const rate = (f1 - f0) / ((Date.now() - t0) / 1000);
  console.log('frames played per second: ' + rate.toFixed(0));
  check(rate > 40000 && rate < 48000, 'sound plays at the sample rate (44100 frames/s)');

  await shot('web_play.png');
  console.log(logs.slice(-15).join('\n'));
} finally {
  ws.close();
  proc.kill();
  server.close();
}
process.exit(fails ? 1 : 0);
