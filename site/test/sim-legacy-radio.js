// A UV-K5 switched on normally, answering the legacy hello (0x0514) and EEPROM reads (0x051B),
// for testing the EEPROM backup. As stock firmware: reads answer only with the hello's session,
// replies carry no CRC (0xFFFF), and frames with a bad CRC are ignored. flavour 'packet' adds the
// PKT2 marker to the hello and refuses reads over 128 bytes, as the packet firmware does.
// Nothing else is answered: v2 ids and anything unknown are recorded in unexpected.

import { FrameDecoder, encodeLegacyReply, buildPayload, u16, u32 } from '../js/k5frame.js';
import { FrameLink } from '../js/k5link.js';

/** 8 KB of plausible EEPROM: a pattern, with a calibration area that is not blank. */
export function fakeEeprom(seed = 7) {
  const b = new Uint8Array(0x2000);
  let x = seed * 2654435761;
  for (let i = 0; i < b.length; i++) {
    x = (x * 1103515245 + 12345) >>> 0;
    b[i] = x >>> 24;
  }
  return b;
}

export class SimLegacyRadio {
  /**
   * @param {object} o
   * @param {'stock'|'packet'} [o.flavour]
   * @param {string} [o.version] what the hello reply says
   * @param {Uint8Array} [o.eeprom]
   * @param {(address: number, n: number) => Uint8Array|null} [o.corrupt] replaces the data of the
   *   n-th read (from 1) of address, or leaves it with null
   * @param {Set<number>} [o.dropFirstReadOf] addresses whose first read gets no reply
   * @param {boolean} [o.lockScreen]
   * @param {boolean} [o.silent] answers nothing at all
   */
  constructor({ flavour = 'stock', version, eeprom = fakeEeprom(), corrupt = null, dropFirstReadOf = new Set(), lockScreen = false, silent = false } = {}) {
    Object.assign(this, { flavour, eeprom, corrupt, dropFirstReadOf, lockScreen, silent });
    this.version = version ?? (flavour === 'packet' ? 'PKTFW v1.0.1' : 'k5_2.01.26');
    this.decoder = new FrameDecoder({ obfuscated: true, maxPayload: 250 });
    this.session = 0;
    this.hellos = 0;
    this.reads = new Map(); // address -> times read
    this.received = []; // ids, in order
    this.unexpected = [];
    this.link = new FrameLink({ write: async (b) => this.fromHost(b) });
  }

  toHost(payload) {
    const frame = encodeLegacyReply(payload, true);
    setTimeout(() => !this.stopped && this.link.receive(frame), 0);
  }

  stop() {
    this.stopped = true;
  }

  fromHost(bytes) {
    for (const f of this.decoder.feed(bytes)) {
      if (f.crc !== 'valid') continue;
      this.received.push(f.id);
      if (this.silent) continue;
      if (f.id === 0x0514) this.hello(f.body);
      else if (f.id === 0x051b) this.read(f.body);
      else this.unexpected.push(f.id);
    }
  }

  hello(body) {
    this.hellos++;
    this.session = u32(body, 0);
    const b = new Uint8Array(36);
    b.set(new TextEncoder().encode(this.version).subarray(0, 16));
    b[17] = this.lockScreen ? 1 : 0;
    if (this.flavour === 'packet') b.set([0x50, 0x4b, 0x54, 0x32, 0x00, 0x02], 20);
    this.toHost(buildPayload(0x0515, b));
  }

  read(body) {
    if (body.length < 8 || u32(body, 4) !== this.session) return;
    const address = u16(body, 0);
    const size = body[2];
    if (this.flavour === 'packet' && size > 128) return;
    const n = (this.reads.get(address) || 0) + 1;
    this.reads.set(address, n);
    if (n === 1 && this.dropFirstReadOf.has(address)) return;
    const r = new Uint8Array(4 + size);
    r.set(body.subarray(0, 3));
    r.set(this.eeprom.subarray(address, address + size), 4);
    const bad = this.corrupt?.(address, n);
    if (bad) r.set(bad.subarray(0, size), 4);
    this.toHost(buildPayload(0x051c, r));
  }
}
