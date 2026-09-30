// Flashing through the UV-K5 bootloader (radio switched on with PTT held). A port of
// host/dotnet K5Bootloader, which follows the vendor updater's frames as captured by K5TOOL:
// the bootloader repeats a 0x0518 beacon until the host sends 0x0530 with the image version,
// then acknowledges each 0x0519 block with 0x051A.
//
// Quirks handled, all seen on the bench radio's bootloader 2.00.06 or in K5TOOL: the CRC field
// of bootloader frames is raw FF FF on the wire (the decoder accepts that); stale beacons are
// already buffered when flashing starts, so they are dropped first; the beacon answering 0x0530
// may be one already in flight, so the real answer and further beacons turn up while waiting
// for block acks, and up to maxStrayBeacons are skipped per block; a rejection ack carries
// chunk 0 whatever block it refers to, so any ack with a non-zero result is this block's refusal.

import { u16, u32, hex } from './k5frame.js';
import { K5ProtocolError, K5SafetyError, K5TimeoutError } from './k5link.js';
import { BLOCK_SIZE, FLASH_LIMIT } from './k5image.js';

export const BL_BEACON = 0x0518;
export const BL_BEACON_V5 = 0x057a;
export const BL_VERSION = 0x0530;
export const BL_WRITE = 0x0519;
export const BL_WRITE_ACK = 0x051a;
export const KNOWN_BOOTLOADERS = ['2.00.06'];

export function parseBeacon(frame) {
  const p = frame.payload;
  const chipId = p.length >= 20 ? hex(p.subarray(4, 20)) : '';
  let version = null;
  if (p.length > 20) {
    const v = p.subarray(20, Math.min(36, p.length));
    let end = v.indexOf(0);
    if (end < 0) end = v.length;
    version = String.fromCharCode(...v.subarray(0, end)) || null;
  }
  return { id: frame.id, version, chipId, isVersion5: frame.id === BL_BEACON_V5 };
}

/** Throws K5SafetyError with a plain-English reason if this radio must not get this image. */
export function checkCompatibility(beacon, image, allowed = KNOWN_BOOTLOADERS) {
  if (beacon.isVersion5) {
    throw new K5SafetyError(
      `This radio has bootloader ${beacon.version || '5.x'}, found on newer models (such as the UV-K5 V3 and UV-K1). ` +
        'This firmware does not run on them, so nothing was sent.',
    );
  }
  if (beacon.id !== BL_BEACON) throw new K5SafetyError(`Unexpected beacon id 0x${beacon.id.toString(16)}: nothing was sent.`);
  if (!beacon.version) {
    throw new K5SafetyError('The bootloader did not say its version (a very old radio?). Nothing was sent.');
  }
  if (!allowed.includes(beacon.version)) {
    throw new K5SafetyError(
      `This radio's bootloader is ${beacon.version}. This firmware is for the original UV-K5 hardware (bootloader ` +
        `${allowed.join(', ')}), so the flasher refuses it to be safe. It may be a newer model such as the ` +
        'UV-K5 V3 or UV-K1, which this firmware does not support. Nothing was sent.',
    );
  }
  const v = image.version;
  if (v.length > 0 && v[0] !== '*' && v[0] !== beacon.version[0]) {
    throw new K5SafetyError(`The image version "${v}" does not suit bootloader ${beacon.version}. Nothing was sent.`);
  }
}

export function makeWriteBody(seq, chunk, chunkCount, data) {
  if (data.length > BLOCK_SIZE || chunk < 0 || chunk >= chunkCount || chunkCount * BLOCK_SIZE > FLASH_LIMIT) {
    throw new K5SafetyError(`bad flash block ${chunk}/${chunkCount} length ${data.length}`);
  }
  const body = new Uint8Array(12 + BLOCK_SIZE);
  const dv = new DataView(body.buffer);
  dv.setUint32(0, seq >>> 0, true);
  dv.setUint16(4, chunk, true);
  dv.setUint16(6, chunkCount, true);
  dv.setUint16(8, data.length, true);
  body.set(data, 12);
  return body;
}

export async function waitForBeacon(link, timeoutMs = 10000, signal) {
  try {
    const f = await link.expect({
      match: (f) => f.id === BL_BEACON || f.id === BL_BEACON_V5,
      timeoutMs,
      what: 'bootloader beacon',
      signal,
    });
    return parseBeacon(f);
  } catch (e) {
    if (e instanceof K5TimeoutError) {
      throw new K5TimeoutError(
        'The radio did not answer. Check it is in flash mode (switch it off, hold PTT and switch it on: the torch LED ' +
          'lights and the screen stays blank), that the cable is pushed fully into the radio, and that you picked the ' +
          "cable's port.",
      );
    }
    throw e;
  }
}

function randomSeq() {
  const a = new Uint32Array(1);
  globalThis.crypto.getRandomValues(a);
  return a[0];
}

/**
 * Checks the image and the bootloader and, only with reallyFlash, writes the image.
 * onProgress({ stage, done, total }) reports what is happening.
 * Returns { flashed, beacon, blocks }.
 */
export async function flashRadio(link, image, opts = {}) {
  const {
    reallyFlash = false,
    allowed = KNOWN_BOOTLOADERS,
    beaconTimeoutMs = 10000,
    ackTimeoutMs = 3000,
    retries = 2,
    maxStrayBeacons = 10,
    seq = randomSeq(),
    onProgress = () => {},
    signal,
  } = opts;
  image.validate();
  const n = image.blockCount;
  onProgress({ stage: 'waiting', done: 0, total: n });
  const beacon = await waitForBeacon(link, beaconTimeoutMs, signal);
  checkCompatibility(beacon, image, allowed);
  if (!reallyFlash) {
    onProgress({ stage: 'checked', done: 0, total: n });
    return { flashed: false, beacon, blocks: n };
  }

  // Stale beacons are buffered: drop them, then say which firmware is coming.
  link.drain();
  await link.send(BL_VERSION, image.versionField);
  await link.expect({ match: (f) => f.id === BL_BEACON, timeoutMs: ackTimeoutMs, what: 'the answer to the version message', signal });

  for (let chunk = 0; chunk < n; chunk++) {
    const body = makeWriteBody(seq, chunk, n, image.block(chunk));
    let ack;
    for (let attempt = 0; ; attempt++) {
      await link.send(BL_WRITE, body);
      try {
        ack = await link.expect({
          match: (f) => f.id === BL_WRITE_ACK && f.body.length >= 7 && (u16(f.body, 4) === chunk || f.body[6] !== 0),
          timeoutMs: ackTimeoutMs,
          ignoreBeacons: maxStrayBeacons,
          what: `the answer for block ${chunk + 1} of ${n}`,
          signal,
        });
        break;
      } catch (e) {
        if (!(e instanceof K5TimeoutError)) throw e;
        if (attempt >= retries) {
          throw new K5TimeoutError(
            `The radio stopped answering at block ${chunk + 1} of ${n}. Keep the radio in flash mode (switch it off, ` +
              'hold PTT, switch it on) and flash again: a part-written radio still starts its bootloader.',
          );
        }
        onProgress({ stage: 'retry', done: chunk, total: n, chunk });
      }
    }
    const a = ack.body;
    const rseq = u32(a, 0);
    const rchunk = u16(a, 4);
    const result = a[6];
    if (result !== 0) {
      throw new K5ProtocolError(
        `The radio refused block ${chunk + 1} of ${n} (bootloader result ${result}, ack names chunk ${rchunk}). ` +
          'Switch it off, put it back in flash mode and try again. If it keeps refusing, the bootloader may not accept this image.',
      );
    }
    if (rseq !== seq >>> 0) {
      throw new K5ProtocolError(
        `The answer for block ${chunk + 1} carries another session (0x${rseq.toString(16)}, sent 0x${(seq >>> 0).toString(16)}). ` +
          'Is another program talking to the radio? Close it and flash again.',
      );
    }
    onProgress({ stage: 'writing', done: chunk + 1, total: n });
  }
  return { flashed: true, beacon, blocks: n };
}
