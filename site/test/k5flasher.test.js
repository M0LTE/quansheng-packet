// The radio flasher against a simulated bootloader, after host/dotnet BootloaderTests.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { flashRadio, checkCompatibility } from '../js/k5flasher.js';
import { FirmwareImage } from '../js/k5image.js';
import { K5ProtocolError, K5SafetyError, K5TimeoutError, K5CancelledError } from '../js/k5link.js';
import { SimBootloader, beaconWithVersion } from './sim-bootloader.js';
import { fakeImage } from './helpers.js';

async function withSim(opts, fn) {
  const sim = new SimBootloader(opts);
  try {
    return await fn(sim);
  } finally {
    sim.stop();
  }
}

test('dry run listens and sends nothing', () =>
  withSim({}, async (sim) => {
    const r = await flashRadio(sim.link, new FirmwareImage(fakeImage(), '*PKTFW test'));
    assert.equal(r.flashed, false);
    assert.equal(r.beacon.version, '2.00.06');
    assert.equal(r.blocks, 0x35);
    assert.equal(sim.hostBytes, 0);
    assert.equal(sim.flashVersion, null);
  }));

test('a real flash writes the image', () =>
  withSim({}, async (sim) => {
    const raw = fakeImage(0x1234);
    const progress = [];
    const r = await flashRadio(sim.link, new FirmwareImage(raw, '*PKTFW test'), {
      reallyFlash: true,
      seq: 0x11223344,
      onProgress: (p) => progress.push(p),
    });
    assert.equal(r.flashed, true);
    assert.equal(sim.done, true);
    assert.equal(sim.flashVersion, '*PKTFW test');
    assert.deepEqual(sim.flash.subarray(0, raw.length), raw);
    assert.equal(sim.blocks.length, 0x13);
    assert.ok(sim.blocks.every((b) => b.seq === 0x11223344));
    assert.deepEqual(sim.violations, []);
    assert.equal(progress.at(-1).done, 0x13);
    assert.equal(progress.at(-1).stage, 'writing');
  }));

test('stray beacons during block writes are tolerated', () =>
  withSim({ keepBeaconing: true, beaconIntervalMs: 2 }, async (sim) => {
    const r = await flashRadio(sim.link, new FirmwareImage(fakeImage(0x800), '*T'), { reallyFlash: true });
    assert.equal(r.flashed, true);
    assert.equal(sim.done, true);
  }));

test('a delayed answer to the version message is handled', () =>
  withSim({ versionReplyDelayMs: 60, beaconIntervalMs: 15 }, async (sim) => {
    const r = await flashRadio(sim.link, new FirmwareImage(fakeImage(0x800), '*T'), { reallyFlash: true });
    assert.equal(r.flashed, true);
  }));

test('a refused block is reported, although the ack names chunk 0', () =>
  withSim({ refuseBlock: 3 }, async (sim) => {
    await assert.rejects(
      flashRadio(sim.link, new FirmwareImage(fakeImage(0x800), '*T'), { reallyFlash: true }),
      (e) => e instanceof K5ProtocolError && /refused block 4 of 8/.test(e.message) && /names chunk 0/.test(e.message),
    );
    assert.equal(sim.done, false);
  }));

test('a lost ack is resent', () =>
  withSim({ dropAckForBlock: 2 }, async (sim) => {
    const retries = [];
    const r = await flashRadio(sim.link, new FirmwareImage(fakeImage(0x800), '*T'), {
      reallyFlash: true,
      ackTimeoutMs: 150,
      onProgress: (p) => p.stage === 'retry' && retries.push(p.chunk),
    });
    assert.equal(r.flashed, true);
    assert.equal(sim.blocks.filter((b) => b.chunk === 2).length, 2);
    assert.deepEqual(retries, [2]);
  }));

test('a radio that stops answering fails with a clear message', () =>
  withSim({ dropAckForBlock: 1 }, async (sim) => {
    sim.dropAckForBlock = 1;
    // drop every ack for block 1
    const handle = sim.handle.bind(sim);
    sim.handle = (f) => {
      if (f.id === 0x0519 && (f.payload[8] | (f.payload[9] << 8)) === 1) return;
      handle(f);
    };
    await assert.rejects(
      flashRadio(sim.link, new FirmwareImage(fakeImage(0x800), '*T'), { reallyFlash: true, ackTimeoutMs: 50, retries: 1 }),
      (e) => e instanceof K5TimeoutError && /stopped answering at block 2 of 8/.test(e.message),
    );
  }));

test('bootloader 5 is refused before anything is sent', () =>
  withSim({ version5: true }, async (sim) => {
    await assert.rejects(
      flashRadio(sim.link, new FirmwareImage(fakeImage(0x800), '*T'), { reallyFlash: true }),
      (e) => e instanceof K5SafetyError && /newer models/.test(e.message),
    );
    assert.equal(sim.hostBytes, 0);
    assert.equal(sim.blocks.length, 0);
  }));

test('a wrong bootloader version is refused before anything is sent', () =>
  withSim({ beaconPayload: beaconWithVersion('2.00.99') }, async (sim) => {
    await assert.rejects(
      flashRadio(sim.link, new FirmwareImage(fakeImage(0x800), '*T'), { reallyFlash: true }),
      (e) => e instanceof K5SafetyError && /bootloader is 2\.00\.99/.test(e.message),
    );
    assert.equal(sim.hostBytes, 0);
  }));

test('no beacon at all explains flash mode', () =>
  withSim({ silent: true }, async (sim) => {
    await assert.rejects(
      flashRadio(sim.link, new FirmwareImage(fakeImage(0x800), '*T'), { beaconTimeoutMs: 50 }),
      (e) => e instanceof K5TimeoutError && /hold PTT/.test(e.message),
    );
  }));

test('cancel stops the wait', () =>
  withSim({ silent: true }, async (sim) => {
    const ac = new AbortController();
    const p = flashRadio(sim.link, new FirmwareImage(fakeImage(0x800), '*T'), { signal: ac.signal });
    setTimeout(() => ac.abort(), 10);
    await assert.rejects(p, K5CancelledError);
  }));

test('compatibility rules', () => {
  const img = new FirmwareImage(fakeImage(), '*EGZUMER v1');
  const ok = { id: 0x0518, version: '2.00.06', isVersion5: false };
  checkCompatibility(ok, img);
  assert.throws(() => checkCompatibility({ id: 0x057a, version: '5.00.01', isVersion5: true }, img), K5SafetyError);
  assert.throws(() => checkCompatibility({ ...ok, version: '2.00.99' }, img), K5SafetyError);
  checkCompatibility({ ...ok, version: '2.00.99' }, img, ['2.00.99']);
  assert.throws(() => checkCompatibility(ok, new FirmwareImage(fakeImage(), '3.01.23')), K5SafetyError);
  assert.throws(() => checkCompatibility({ ...ok, version: null }, img), K5SafetyError);
});
