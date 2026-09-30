// A backup of the radio's 8 KB EEPROM (its settings, and its factory calibration at 0x1E00 to
// 0x1FFF), read with the radio switched on normally. The program flash cannot be read back, so
// this is the part worth keeping. A port of host/dotnet K5Radio.BackupEepromAsync and
// K5Safety.CheckBackupContent; the file is the same raw 8192-byte image.
//
// Only legacy commands are sent, so it works with whatever firmware the radio runs (stock,
// egzumer and its forks, mobilinkd, ours): a hello (0x0514, which stock firmware needs before it
// answers anything else) and EEPROM reads (0x051B, 128 bytes at a time, answered with 0x051C).
//
//   0x051B body: address u16 | length u8 | 0 | session u32 (the hello's)
//   0x051C body: address u16 | length u8 | 0 | data
//
// The image is read twice and each chunk of the second read must match the first. A chunk that
// differs is read again, up to mismatchRetries times, until two reads agree.

import { u16 } from './k5frame.js';
import { BEACON_IDS, K5ProtocolError, K5SafetyError, K5TimeoutError } from './k5link.js';
import { HELLO, HELLO_REPLY, helloBody, parseHelloReply } from './v2.js';
import { sha256Hex } from './manifest.js';

export const EEPROM_SIZE = 0x2000;
export const CALIBRATION_START = 0x1e00;
export const READ_CHUNK = 0x80;
export const EEPROM_READ = 0x051b;
export const EEPROM_READ_REPLY = 0x051c;

export const BACKUP_NO_ANSWER =
  'The radio did not answer. Check it is switched on normally (not in flash mode), the cable is pushed fully ' +
  'into the radio, and you picked the right port.';
export const BACKUP_IN_FLASH_MODE =
  'The radio is in flash mode. To back it up, switch it off, then on again without holding PTT, and press Back up again.';

const addr4 = (a) => `0x${a.toString(16).toUpperCase().padStart(4, '0')}`;

export function readRequestBody(address, length, sessionId) {
  const b = new Uint8Array(8);
  const dv = new DataView(b.buffer);
  dv.setUint16(0, address, true);
  b[2] = length;
  dv.setUint32(4, sessionId >>> 0, true);
  return b;
}

/** Sends one legacy request and waits for its reply, sending it again on a timeout. */
async function exchange(link, id, body, match, { timeoutMs, attempts, signal, what }) {
  for (let attempt = 1; ; attempt++) {
    link.drain();
    await link.send(id, body);
    let f;
    try {
      f = await link.expect({ match: (x) => match(x) || BEACON_IDS.has(x.id), timeoutMs, what, signal });
    } catch (e) {
      if (e instanceof K5TimeoutError && attempt < attempts) continue;
      throw e;
    }
    if (BEACON_IDS.has(f.id)) throw new K5ProtocolError(BACKUP_IN_FLASH_MODE);
    return f;
  }
}

/** Throws K5SafetyError if the image cannot be a real radio's EEPROM. */
export function checkBackupContent(image) {
  if (image.length !== EEPROM_SIZE) {
    throw new K5SafetyError(`A radio backup is ${EEPROM_SIZE} bytes; this is ${image.length}.`);
  }
  for (const [part, what] of [
    [image, "The radio's memory"],
    [image.subarray(CALIBRATION_START), "The radio's calibration area"],
  ]) {
    for (const blank of [0x00, 0xff]) {
      if (part.every((x) => x === blank)) {
        throw new K5SafetyError(
          `${what} read back as all 0x${blank.toString(16).toUpperCase().padStart(2, '0')}, which is not a usable backup, ` +
            'so none was saved. Switch the radio off and on again (normally) and try again.',
        );
      }
    }
  }
}

/** uvk5-eeprom-<firmware version>-<yyyymmdd>.bin, the date in local time. */
export function backupFileName(version, date = new Date()) {
  const v = String(version || '').trim().replace(/[^A-Za-z0-9._-]+/g, '_').replace(/^[_.]+|_+$/g, '') || 'unknown';
  const d = `${date.getFullYear()}${String(date.getMonth() + 1).padStart(2, '0')}${String(date.getDate()).padStart(2, '0')}`;
  return `uvk5-eeprom-${v}-${d}.bin`;
}

function randomSession() {
  const a = new Uint32Array(1);
  globalThis.crypto.getRandomValues(a);
  return a[0];
}

const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

/**
 * Says hello, then reads the whole EEPROM twice. onProgress({ stage, done, total, address })
 * reports 'reading' (done counts chunks over both reads) and 'reread' (a chunk that differed).
 * Returns { data, sha256, filename, version, hello, rereads }.
 */
export async function backupEeprom(link, opts = {}) {
  const {
    sessionId = randomSession(),
    helloTimeoutMs = 800,
    helloAttempts = 4,
    replyTimeoutMs = 500,
    attempts = 3,
    mismatchRetries = 2,
    onProgress = () => {},
    signal,
    date = new Date(),
  } = opts;
  link.onFrame = null; // replies are awaited from the link's queue

  const helloFrame = await exchange(link, HELLO, helloBody(sessionId), (f) => f.id === HELLO_REPLY && f.body.length >= 20, {
    timeoutMs: helloTimeoutMs,
    attempts: helloAttempts,
    signal,
    what: 'answer from the radio',
  }).catch((e) => {
    throw e instanceof K5TimeoutError ? new K5TimeoutError(BACKUP_NO_ANSWER) : e;
  });
  const hello = parseHelloReply(helloFrame.body);
  if (hello.lockScreen) {
    throw new K5SafetyError('The radio is showing its lock screen. Unlock it (or switch it off and on) and press Back up again.');
  }

  const chunks = EEPROM_SIZE / READ_CHUNK;
  const total = chunks * 2;
  let done = 0;
  onProgress({ stage: 'reading', done, total });

  const readChunk = async (address) => {
    const f = await exchange(
      link,
      EEPROM_READ,
      readRequestBody(address, READ_CHUNK, sessionId),
      (x) => x.id === EEPROM_READ_REPLY && x.body.length >= 4 && u16(x.body, 0) === address,
      { timeoutMs: replyTimeoutMs, attempts, signal, what: `memory at ${addr4(address)}` },
    ).catch((e) => {
      if (!(e instanceof K5TimeoutError)) throw e;
      throw new K5TimeoutError(
        `The radio stopped answering while its memory was being read (at ${addr4(address)}). Check it is still switched ` +
          'on normally (not in flash mode) and the cable is pushed fully in, then press Back up again.',
      );
    });
    const n = f.body[2];
    if (n !== READ_CHUNK || f.body.length < 4 + READ_CHUNK) {
      throw new K5ProtocolError(
        `The radio answered with ${Math.min(n, f.body.length - 4)} bytes at ${addr4(address)}, not ${READ_CHUNK}. ` +
          'Its firmware may not support backups from this page.',
      );
    }
    return f.body.slice(4, 4 + READ_CHUNK);
  };

  const first = new Uint8Array(EEPROM_SIZE);
  for (let a = 0; a < EEPROM_SIZE; a += READ_CHUNK) {
    first.set(await readChunk(a), a);
    onProgress({ stage: 'reading', done: ++done, total, address: a });
  }

  const data = new Uint8Array(EEPROM_SIZE);
  let rereads = 0;
  for (let a = 0; a < EEPROM_SIZE; a += READ_CHUNK) {
    const seen = [first.subarray(a, a + READ_CHUNK)];
    let agreed = null;
    for (let k = 0; !agreed; k++) {
      if (k > 0) {
        if (k > mismatchRetries) {
          throw new K5ProtocolError(
            `The two reads of the radio's memory kept disagreeing at ${addr4(a)}, so no backup was saved. ` +
              'Check the cable is pushed fully into the radio and press Back up again.',
          );
        }
        rereads++;
        onProgress({ stage: 'reread', done, total, address: a });
      }
      const c = await readChunk(a);
      if (seen.some((s) => same(s, c))) agreed = c;
      seen.push(c);
    }
    data.set(agreed, a);
    onProgress({ stage: 'reading', done: ++done, total, address: a });
  }

  checkBackupContent(data);
  return {
    data,
    sha256: await sha256Hex(data),
    filename: backupFileName(hello.version, date),
    version: hello.version,
    hello,
    rereads,
  };
}
