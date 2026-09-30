// The EEPROM backup against simulated radios running stock-like and packet firmware.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { backupEeprom, backupFileName, checkBackupContent, readRequestBody, BACKUP_NO_ANSWER, BACKUP_IN_FLASH_MODE } from '../js/eeprom-backup.js';
import { K5ProtocolError, K5SafetyError, K5TimeoutError } from '../js/k5link.js';
import { SimLegacyRadio, fakeEeprom } from './sim-legacy-radio.js';
import { SimBootloader } from './sim-bootloader.js';

const sha = (b) => createHash('sha256').update(b).digest('hex');
const fast = { helloTimeoutMs: 60, replyTimeoutMs: 60 };

async function withRadio(opts, fn) {
  const sim = new SimLegacyRadio(opts);
  try {
    return await fn(sim);
  } finally {
    sim.stop();
  }
}

for (const flavour of ['stock', 'packet']) {
  test(`a full backup from ${flavour} firmware`, () =>
    withRadio({ flavour }, async (sim) => {
      const progress = [];
      const r = await backupEeprom(sim.link, { ...fast, date: new Date(2026, 8, 30, 23, 59), onProgress: (p) => progress.push(p) });
      assert.deepEqual(r.data, sim.eeprom);
      assert.equal(r.sha256, sha(sim.eeprom));
      assert.equal(r.version, sim.version);
      assert.equal(r.rereads, 0);
      assert.equal(r.filename, flavour === 'stock' ? 'uvk5-eeprom-k5_2.01.26-20260930.bin' : 'uvk5-eeprom-PKTFW_v1.0.1-20260930.bin');
      // a hello first, then only EEPROM reads, each chunk twice: nothing v2
      assert.equal(sim.received[0], 0x0514);
      assert.ok(sim.received.slice(1).every((id) => id === 0x051b));
      assert.deepEqual(sim.unexpected, []);
      assert.equal(sim.reads.size, 64);
      assert.ok([...sim.reads.values()].every((n) => n === 2));
      assert.deepEqual(progress.at(-1), { stage: 'reading', done: 128, total: 128, address: 0x1f80 });
    }));
}

test('the read request carries the hello session', () => {
  assert.deepEqual(Array.from(readRequestBody(0x1e80, 0x80, 0x11223344)), [0x80, 0x1e, 0x80, 0, 0x44, 0x33, 0x22, 0x11]);
});

test('a chunk that reads differently once is read again and recovers', () =>
  withRadio({ corrupt: (a, n) => (a === 0x0400 && n === 2 ? new Uint8Array(128).fill(0x5a) : null) }, async (sim) => {
    const stages = [];
    const r = await backupEeprom(sim.link, { ...fast, onProgress: (p) => stages.push(p.stage) });
    assert.deepEqual(r.data, sim.eeprom);
    assert.equal(r.rereads, 1);
    assert.equal(sim.reads.get(0x0400), 3);
    assert.ok(stages.includes('reread'));
  }));

test('a first read that was wrong is outvoted by the two after it', () =>
  withRadio({ corrupt: (a, n) => (a === 0x1e00 && n === 1 ? new Uint8Array(128).fill(0x11) : null) }, async (sim) => {
    const r = await backupEeprom(sim.link, fast);
    assert.deepEqual(r.data, sim.eeprom);
    assert.equal(sim.reads.get(0x1e00), 3);
  }));

test('reads that keep disagreeing fail with no backup', () =>
  withRadio({ corrupt: (a, n) => (a === 0x0880 ? new Uint8Array(128).fill(n) : null) }, async (sim) => {
    await assert.rejects(backupEeprom(sim.link, fast), (e) => e instanceof K5ProtocolError && /kept disagreeing at 0x0880/.test(e.message));
    assert.equal(sim.reads.get(0x0880), 4); // two reads, then two more
  }));

test('a lost reply is asked for again', () =>
  withRadio({ dropFirstReadOf: new Set([0x0100]) }, async (sim) => {
    const r = await backupEeprom(sim.link, fast);
    assert.deepEqual(r.data, sim.eeprom);
    assert.equal(sim.reads.get(0x0100), 3);
  }));

test('an EEPROM that reads all 0xFF is refused', () =>
  withRadio({ eeprom: new Uint8Array(0x2000).fill(0xff) }, async (sim) => {
    await assert.rejects(backupEeprom(sim.link, fast), (e) => e instanceof K5SafetyError && /all 0xFF/.test(e.message));
  }));

test('an EEPROM that reads all 0x00 is refused', () =>
  withRadio({ eeprom: new Uint8Array(0x2000) }, async (sim) => {
    await assert.rejects(backupEeprom(sim.link, fast), (e) => e instanceof K5SafetyError && /all 0x00/.test(e.message));
  }));

test('a blank calibration area is refused', () => {
  const e = fakeEeprom();
  e.fill(0xff, 0x1e00);
  assert.throws(() => checkBackupContent(e), /calibration area read back as all 0xFF/);
  assert.doesNotThrow(() => checkBackupContent(fakeEeprom()));
});

test('a radio that does not answer: switched on normally?', () =>
  withRadio({ silent: true }, async (sim) => {
    await assert.rejects(backupEeprom(sim.link, fast), (e) => e instanceof K5TimeoutError && e.message === BACKUP_NO_ANSWER);
    assert.ok(sim.received.every((id) => id === 0x0514));
  }));

test('a radio in flash mode is recognised, and nothing but a hello is sent', async () => {
  const sim = new SimBootloader({ beaconIntervalMs: 20 });
  try {
    await assert.rejects(backupEeprom(sim.link, fast), (e) => e.message === BACKUP_IN_FLASH_MODE);
    assert.equal(sim.blocks.length, 0);
    assert.equal(sim.handshake, false);
  } finally {
    sim.stop();
  }
});

test('a radio on its lock screen is refused before reading', () =>
  withRadio({ lockScreen: true }, async (sim) => {
    await assert.rejects(backupEeprom(sim.link, fast), (e) => e instanceof K5SafetyError && /lock screen/.test(e.message));
    assert.equal(sim.reads.size, 0);
  }));

test('file names', () => {
  const d = new Date(2026, 0, 5);
  assert.equal(backupFileName('k5_2.01.26', d), 'uvk5-eeprom-k5_2.01.26-20260105.bin');
  assert.equal(backupFileName('PKTFW v1.0.1', d), 'uvk5-eeprom-PKTFW_v1.0.1-20260105.bin');
  assert.equal(backupFileName(' EGZUMER v0.22/x ', d), 'uvk5-eeprom-EGZUMER_v0.22_x-20260105.bin');
  assert.equal(backupFileName('', d), 'uvk5-eeprom-unknown-20260105.bin');
  assert.equal(backupFileName('../..', d), 'uvk5-eeprom-unknown-20260105.bin');
});
