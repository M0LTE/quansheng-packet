// Optional end-to-end check of the page in a real (headless) Chromium, with the serial, USB and
// HID devices faked in the page by browser-sim.js. Not part of `npm test` (it needs a browser):
//
//   PLAYWRIGHT_CORE=/path/to/node_modules/playwright-core CHROME=/path/to/chrome node test/browser-e2e.js
//
// Needs site/files/ (the firmware files; see tools/build-manifest.js). Walks all four steps:
// dry run, flash and check the radio, detach, backup, flash and check the AIOC, read the EQ,
// connect, refuse and save a channel, play the level tone and read the level meter, key the
// radio with its own PTT (the page shows it and sends nothing meanwhile), a refused TX, disconnect.
import { createRequire } from 'node:module';
import { readFileSync, existsSync } from 'node:fs';
import { createServer } from 'node:http';
import { join, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../', import.meta.url));
const require = createRequire(join(process.env.PLAYWRIGHT_CORE || 'playwright-core', '..', '/'));
const { chromium } = require(process.env.PLAYWRIGHT_CORE || 'playwright-core');
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json' };
const server = createServer((req, res) => {
  const f = join(root, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!f.startsWith(root) || !existsSync(f)) return res.writeHead(404).end();
  res.writeHead(200, { 'content-type': types[extname(f)] || 'application/octet-stream' }).end(readFileSync(f));
}).listen(0, '127.0.0.1');
await new Promise((r) => server.on('listening', r));
const base = `http://127.0.0.1:${server.address().port}`;
const b = await chromium.launch({ executablePath: process.env.CHROME, args: ['--autoplay-policy=no-user-gesture-required'] });
const ctx = await b.newContext({ acceptDownloads: true, viewport: { width: 1100, height: 900 } });
const p = await ctx.newPage();
const msgs = [];
p.on('console', (m) => msgs.push(`${m.type()}: ${m.text()}`));
p.on('pageerror', (e) => msgs.push(`pageerror: ${e.message}`));
p.on('dialog', (d) => { msgs.push(`dialog: ${d.message()}`); d.accept(); });
await p.route('**/__sim.js', (r) => r.fulfill({ contentType: 'text/javascript', body: readFileSync(join(root, 'site/test/browser-sim.js'), 'utf8') }));
// load the fakes before the page's own module runs
await p.route('**/site/index.html', async (r) => {
  const res = await r.fetch();
  let html = await res.text();
  html = html.replace('<script type="module" src="js/app.js"></script>', '<script type="module">await import("/__sim.js"); await import("./js/app.js");</script>');
  r.fulfill({ response: res, body: html });
});
const text = (id) => p.textContent('#' + id);
async function waitText(id, re, ms = 20000) {
  const t0 = Date.now();
  for (;;) {
    const t = await text(id);
    if (re.test(t)) return t;
    if (Date.now() - t0 > ms) throw new Error(`#${id} never matched ${re}: "${t}"`);
    await p.waitForTimeout(100);
  }
}
await p.goto(`${base}/site/index.html`);
await waitText('k5-flash', /v1\.0\.1/);
console.log('1', await text('files-status'));
await p.click('#k5-connect');
console.log('1', await waitText('k5-status', /compatible/));
console.log('  opened with', JSON.stringify(await p.evaluate(() => [window.__openedWith, window.__signals])));
await p.click('#k5-flash');
console.log('1', await waitText('k5-status', /Flashed/, 60000));
console.log('  sim done', await p.evaluate(() => [window.__boot.done, window.__boot.flashVersion, window.__boot.blocks.length]));
await p.evaluate(() => window.__powerCycle());
console.log('1', await waitText('k5-status', /Step 1 is done|flash mode|answer/));
await p.click('#aioc-detach');
console.log('2', await waitText('aioc-status', /restarting/));
await p.click('#aioc-connect');
console.log('2', await waitText('aioc-status', /Connected to the AIOC/));
const dl = p.waitForEvent('download');
await p.click('#aioc-backup');
const d = await dl;
console.log('2', await waitText('aioc-status', /Backup saved/), '|', d.suggestedFilename());
await p.click('#aioc-flash');
console.log('2', await waitText('aioc-status', /Done|lost/, 60000));
const file = new Uint8Array(readFileSync(join(root, 'site/files', JSON.parse(readFileSync(join(root, 'site/files/manifest.json'), 'utf8')).aioc.file)));
const flashed = await p.evaluate(() => [Array.from(window.__stm.flash.subarray(0, 128000)), window.__stm.left, window.__stm.erased.length]);
console.log('  flash matches file:', flashed[0].every((x, i) => x === file[i]), 'left:', flashed[1], 'erased:', flashed[2]);
await p.click('#eq-check');
console.log('2', await waitText('eq-status', /equaliser/));
await p.click('#radio-connect');
console.log('3', await waitText('radio-status', /Connected|runs|answer/));
console.log('  fw', await text('r-fw'), '| rssi', await text('r-rssi'), '| batt', await text('r-batt'), '| freq', await p.inputValue('#f-freq'), '| dev', await text('r-dev'));
await p.fill('#f-freq', '380');
await p.click('#f-save');
console.log('3', await waitText('radio-status', /transmits only/));
await p.fill('#f-freq', '145.5');
await p.selectOption('#f-power', '2');
await p.selectOption('#f-bw', '1');
await p.click('#f-save');
console.log('3', await waitText('radio-status', /Saved/));
await p.click('#tone-start');
console.log('3', await waitText('radio-status', /tone/));
console.log('3', await waitText('meter-text', /in the green band/), '|', await text('meter-source'));
console.log('  mic opened', JSON.stringify(await p.evaluate(() => window.__micOpened)));
await p.waitForTimeout(1500);
console.log('  state', await text('r-state'));
const liveTx = await p.evaluate(() => window.__liveTx);
console.log('  subscribed with LIVE_TX:', liveTx);
const wireBefore = await p.evaluate(() => window.__simLog.length);
await p.evaluate(() => window.__sideKey(true));
console.log('3', await waitText('r-state', /^Transmitting on 145\.500 MHz, high/));
await p.fill('#f-freq', '145.6');
await p.click('#f-save');
console.log('3', await waitText('radio-status', /is transmitting/));
await p.waitForTimeout(1200); // a heartbeat or two while transmitting
console.log('  state', await text('r-state'));
const sentWhileTx = (await p.evaluate(() => window.__simLog.length)) - wireBefore;
console.log('  frames sent while transmitting:', sentWhileTx);
await p.evaluate(() => window.__sideKey(false));
console.log('3', await waitText('r-state', /^Receiving.*Last transmission 1\.5 s/));
await p.evaluate(() => window.__refuse(2));
console.log('3', await waitText('radio-status', /refused to transmit/));
if (!liveTx || sentWhileTx !== 0) process.exitCode = 1;
await p.click('#radio-disconnect');
console.log('3', await waitText('radio-status', /Disconnected/));
const micLeft = await p.evaluate(() => [document.getElementById('meter').hidden, window.__micOpened.every((o) => o.stopped)]);
console.log('  meter hidden and every input stopped after disconnect:', micLeft);
if (!micLeft.every(Boolean)) process.exitCode = 1;
console.log('  wire ids', await p.evaluate(() => window.__simLog.join(' ')));
console.log(msgs.join('\n') || 'no console messages');
if (msgs.some((m) => /^(error|pageerror)/.test(m))) process.exitCode = 1;
await b.close();
server.close();
