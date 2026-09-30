// An in-process UV-K5 bootloader 2.00.06 (or 5.x) for testing the flasher, after host/dotnet
// SimulatedBootloader: beacons until 0x0530, then acknowledges 0x0519 blocks with 0x051A. Frames
// carry a raw FF FF CRC, as the real one does.

import { FrameDecoder, encodeBootloaderFrame, buildPayload, fromHex, u16, u32 } from '../js/k5frame.js';
import { FrameLink } from '../js/k5link.js';

export const BEACON_V2 = fromHex('18052000010202061c53504a3747ff0f8c005300322e30302e303600340a000000000020');
export const BEACON_V5 = fromHex('7a052000010202061c53504a3747ff1093008900352e30302e303100280c000000000020');

export function beaconWithVersion(v) {
  const p = BEACON_V2.slice();
  p.fill(0, 20, 28);
  for (let i = 0; i < v.length; i++) p[20 + i] = v.charCodeAt(i);
  return p;
}

export class SimBootloader {
  constructor({
    beaconIntervalMs = 20,
    version5 = false,
    beaconPayload = null,
    refuseBlock = null,
    dropAckForBlock = null,
    keepBeaconing = false,
    versionReplyDelayMs = 0,
    silent = false,
  } = {}) {
    Object.assign(this, { beaconIntervalMs, version5, refuseBlock, dropAckForBlock, keepBeaconing, versionReplyDelayMs, silent });
    this.beaconPayload = beaconPayload || (version5 ? BEACON_V5 : BEACON_V2);
    this.decoder = new FrameDecoder({ obfuscated: true, maxPayload: 0x200 });
    this.flash = new Uint8Array(0xf000).fill(0xff);
    this.blocks = [];
    this.violations = [];
    this.handshake = false;
    this.done = false;
    this.dropped = false;
    this.flashVersion = null;
    this.hostBytes = 0;
    this.link = new FrameLink({ write: async (b) => this.fromHost(b) });
    if (!silent) this.timer = setInterval(() => this.beacon(true), beaconIntervalMs);
    queueMicrotask(() => !silent && this.beacon(true));
  }

  stop() {
    clearInterval(this.timer);
    this.stopped = true;
  }

  toHost(payload) {
    const frame = encodeBootloaderFrame(payload);
    // deliver asynchronously, as a serial port would
    setTimeout(() => !this.stopped && this.link.receive(frame), 0);
  }

  beacon(periodic) {
    if (this.stopped) return;
    if (periodic && this.handshake && !(this.keepBeaconing && !this.done)) return;
    this.toHost(this.beaconPayload);
  }

  fromHost(bytes) {
    this.hostBytes += bytes.length;
    for (const f of this.decoder.feed(bytes)) {
      if (f.crc !== 'valid') continue;
      this.handle(f);
    }
  }

  handle(f) {
    const p = f.payload;
    if (f.id === 0x0530) {
      let end = p.subarray(4, 20).indexOf(0);
      if (end < 0) end = 16;
      this.flashVersion = String.fromCharCode(...p.subarray(4, 4 + end));
      this.handshake = true;
      if (this.versionReplyDelayMs > 0) setTimeout(() => this.beacon(false), this.versionReplyDelayMs);
      else this.toHost(this.beaconPayload);
      return;
    }
    if (f.id !== 0x0519) return;
    if (!this.handshake) {
      this.violations.push('flash block before the 0x0530 version message');
      return;
    }
    const seq = u32(p, 4);
    const chunk = u16(p, 8);
    const count = u16(p, 10);
    const length = u16(p, 12);
    const data = p.subarray(16, 16 + Math.min(256, p.length - 16));
    this.blocks.push({ seq, chunk, count, length });
    if (chunk === this.dropAckForBlock && !this.dropped) {
      this.dropped = true;
      return;
    }
    const refuse = chunk === this.refuseBlock;
    if (!refuse) {
      if ((chunk + 1) * 256 > 0xf000) {
        this.violations.push('write into the bootloader area');
        return;
      }
      this.flash.set(data.subarray(0, length), chunk * 256);
    }
    const ack = new Uint8Array(8);
    const dv = new DataView(ack.buffer);
    dv.setUint32(0, seq, true);
    dv.setUint16(4, refuse ? 0 : chunk, true);
    ack[6] = refuse ? 1 : 0;
    this.toHost(buildPayload(0x051a, ack));
    if (!refuse && chunk === count - 1) this.done = true;
  }
}
