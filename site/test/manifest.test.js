// The release picking and checksum checks, in the workflow's tool and in the page. If this copy
// of the site has its firmware files (the workflow runs the tests again after downloading them),
// they are checked against the manifest too.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, readFileSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseSha256Sums, chooseAiocImage, chooseRadioImage, buildManifest } from '../tools/build-manifest.js';
import { verifyEntry, fetchVerified, loadManifest, sha256Hex } from '../js/manifest.js';
import { loadImage } from '../js/k5image.js';
import { checkAiocImage } from '../js/dfu.js';

const sha = (b) => createHash('sha256').update(b).digest('hex');

function release(files) {
  const dir = mkdtempSync(join(tmpdir(), 'rel-'));
  const lines = [];
  for (const [name, bytes] of Object.entries(files)) {
    writeFileSync(join(dir, name), bytes);
    lines.push(`${sha(bytes)}  ${name}`);
  }
  writeFileSync(join(dir, 'SHA256SUMS'), lines.join('\n') + '\n');
  return dir;
}

test('SHA256SUMS parsing', () => {
  const m = parseSha256Sums(`${'a'.repeat(64)}  one.bin\n${'B'.repeat(64)} *two.bin\n\njunk\n`);
  assert.equal(m.get('one.bin'), 'a'.repeat(64));
  assert.equal(m.get('two.bin'), 'b'.repeat(64));
  assert.equal(m.size, 2);
});

test('AIOC image choice: 1.4.1-packet.1 naming and the naming from 1.4.1-packet.2', () => {
  const p1 = [
    'aioc-packet-1.4.1-packet.1-k5-red.bin',
    'aioc-packet-1.4.1-packet.1-keep-settings.bin',
    'aioc-packet-1.4.1-packet.1.bin',
    'aioc-packet-1.4.1-packet.1.hex',
    'SHA256SUMS',
  ];
  assert.equal(chooseAiocImage(p1, 'v1.4.1-packet.1'), 'aioc-packet-1.4.1-packet.1-k5-red.bin');
  const p2 = ['aioc-packet-1.4.1-packet.2-keep-settings.bin', 'aioc-packet-1.4.1-packet.2.bin', 'aioc-packet-1.4.1-packet.2.hex', 'SHA256SUMS'];
  assert.equal(chooseAiocImage(p2, 'v1.4.1-packet.2'), 'aioc-packet-1.4.1-packet.2.bin');
  assert.throws(() => chooseAiocImage(['aioc-packet-1.4.1-packet.2-keep-settings.bin'], 'v1.4.1-packet.2'));
  assert.equal(chooseRadioImage(['quansheng-packet-v1.0.1-raw.bin', 'quansheng-packet-v1.0.1.bin'], 'v1.0.1'), 'quansheng-packet-v1.0.1.bin');
});

test('buildManifest copies and checks the files', () => {
  const radioDir = release({ 'quansheng-packet-v1.0.1.bin': Buffer.from('radio'), 'quansheng-packet-v1.0.1-raw.bin': Buffer.from('raw') });
  const aiocDir = release({ 'aioc-packet-1.4.1-packet.2.bin': Buffer.from('aioc'), 'aioc-packet-1.4.1-packet.2-keep-settings.bin': Buffer.from('keep') });
  const out = mkdtempSync(join(tmpdir(), 'site-'));
  const m = buildManifest({ out, radioDir, radioTag: 'v1.0.1', aiocDir, aiocTag: 'v1.4.1-packet.2', now: new Date(0) });
  assert.equal(m.radio.file, 'quansheng-packet-v1.0.1.bin');
  assert.equal(m.radio.version, 'v1.0.1');
  assert.equal(m.radio.sha256, sha(Buffer.from('radio')));
  assert.equal(m.aioc.file, 'aioc-packet-1.4.1-packet.2.bin');
  assert.equal(m.aioc.version, '1.4.1-packet.2');
  assert.equal(m.aioc.size, 4);
  assert.equal(readFileSync(join(out, 'aioc-packet-1.4.1-packet.2.bin'), 'utf8'), 'aioc');
  assert.deepEqual(JSON.parse(readFileSync(join(out, 'manifest.json'), 'utf8')), m);
});

test('buildManifest refuses a file that does not match SHA256SUMS', () => {
  const radioDir = release({ 'quansheng-packet-v1.0.1.bin': Buffer.from('radio') });
  writeFileSync(join(radioDir, 'quansheng-packet-v1.0.1.bin'), 'tampered');
  const aiocDir = release({ 'aioc-packet-1.4.1-packet.2.bin': Buffer.from('aioc') });
  const out = mkdtempSync(join(tmpdir(), 'site-'));
  assert.throws(() => buildManifest({ out, radioDir, radioTag: 'v1.0.1', aiocDir, aiocTag: 'v1.4.1-packet.2' }), /SHA256SUMS says/);
});

test('the page checks a download against the manifest', async () => {
  const bytes = new TextEncoder().encode('firmware');
  const entry = { file: 'x.bin', sha256: sha(bytes), size: bytes.length };
  assert.equal(await sha256Hex(bytes), entry.sha256);
  await verifyEntry(entry, bytes);
  await assert.rejects(verifyEntry(entry, new TextEncoder().encode('firmwarE')), /checksum/);
  await assert.rejects(verifyEntry(entry, new TextEncoder().encode('firmware!')), /expected 8/);
  const fakeFetch = async (url) =>
    url.endsWith('manifest.json')
      ? { ok: true, json: async () => ({ radio: entry }) }
      : url.endsWith('x.bin')
        ? { ok: true, arrayBuffer: async () => bytes.buffer }
        : { ok: false, status: 404 };
  const m = await loadManifest('files/', fakeFetch);
  assert.deepEqual(m.radio, entry);
  const got = await fetchVerified(entry, 'files/', fakeFetch);
  assert.deepEqual(got.bytes, bytes);
  assert.equal(await loadManifest('files/', async () => ({ ok: false, status: 404 })), null);
  await assert.rejects(fetchVerified({ ...entry, file: 'missing.bin' }, 'files/', fakeFetch), /HTTP 404/);
});

const siteFiles = fileURLToPath(new URL('../files/', import.meta.url));
test('the downloaded firmware files match the manifest and look right', { skip: !existsSync(join(siteFiles, 'manifest.json')) }, async () => {
  const m = JSON.parse(readFileSync(join(siteFiles, 'manifest.json'), 'utf8'));
  for (const key of ['radio', 'aioc']) {
    const bytes = new Uint8Array(readFileSync(join(siteFiles, m[key].file)));
    await verifyEntry(m[key], bytes);
  }
  const radio = loadImage(new Uint8Array(readFileSync(join(siteFiles, m.radio.file))));
  assert.equal(radio.wasPacked, true);
  assert.equal(radio.version, `*PKTFW ${m.radio.version}`);
  radio.validate();
  checkAiocImage(new Uint8Array(readFileSync(join(siteFiles, m.aioc.file))));
});
