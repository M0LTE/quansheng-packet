// A frame link over any byte transport: feeds received bytes through the decoder and lets a
// caller wait for a frame, or hands every frame to a listener. The transport is two functions:
// write(bytes) (async) and whoever owns the transport calls link.receive(bytes).

import { FrameDecoder, encodeFrame } from './k5frame.js';

export class K5Error extends Error {
  constructor(message, detail) {
    super(message);
    this.name = this.constructor.name;
    if (detail) this.detail = detail;
  }
}
/** Nothing arrived in time. */
export class K5TimeoutError extends K5Error {}
/** The other end said or did something unexpected. */
export class K5ProtocolError extends K5Error {}
/** Refused on purpose, before anything risky was sent. */
export class K5SafetyError extends K5Error {}
/** Cancelled by the user. */
export class K5CancelledError extends K5Error {}

export const BEACON_IDS = new Set([0x0518, 0x057a]);

export class FrameLink {
  /**
   * @param {object} o
   * @param {(bytes: Uint8Array) => Promise<void>} o.write
   * @param {boolean} [o.obfuscated]
   * @param {number} [o.maxPayload]
   */
  constructor({ write, obfuscated = true, maxPayload = 250 }) {
    this.write = write;
    this.obfuscated = obfuscated;
    this.decoder = new FrameDecoder({ obfuscated, maxPayload });
    this.queue = [];
    this.waiter = null;
    this.onFrame = null; // if set, every frame goes here instead of the queue
    this.onText = null; // unframed bytes (the power-on banner), as text
    this.closedError = null;
    this.lastWriteAt = 0;
    this.lastReadAt = 0;
  }

  /** Bytes from the transport. */
  receive(bytes) {
    this.lastReadAt = Date.now();
    const frames = this.decoder.feed(bytes, (d) => {
      if (this.onText) this.onText(new TextDecoder('latin1').decode(d));
    });
    for (const f of frames) {
      if (this.onFrame) {
        this.onFrame(f);
        continue;
      }
      this.queue.push(f);
      if (this.queue.length > 256) this.queue.shift();
    }
    if (this.waiter) this.waiter();
  }

  /** The transport has gone (unplugged, closed). Pending and later waits fail with err. */
  close(err) {
    this.closedError = err || new K5ProtocolError('The connection was closed.');
    if (this.waiter) this.waiter();
  }

  /** Drops frames already received (stale beacons, say). */
  drain() {
    this.queue.length = 0;
  }

  async send(id, body) {
    await this.sendRaw(encodeFrame(id, body, this.obfuscated));
  }

  async sendRaw(frame) {
    if (this.closedError) throw this.closedError;
    await this.write(frame);
    this.lastWriteAt = Date.now();
  }

  /**
   * Waits for a frame that match(frame) accepts. Frames with a bad CRC are skipped, and so are
   * other frames, except that bootloader beacons are counted: more than ignoreBeacons of them
   * fail the wait (the radio is in, or has fallen back to, its bootloader).
   */
  expect({ match, timeoutMs, ignoreBeacons = 0, what = 'a reply', signal, onBeacon } = {}) {
    return new Promise((resolve, reject) => {
      let beacons = 0;
      let timer = null;
      const finish = (fn, v) => {
        clearTimeout(timer);
        this.waiter = null;
        if (signal) signal.removeEventListener('abort', onAbort);
        fn(v);
      };
      const onAbort = () => finish(reject, new K5CancelledError('Cancelled.'));
      const pump = () => {
        while (this.queue.length) {
          const f = this.queue.shift();
          if (f.crc === 'bad') continue;
          if (match(f)) return finish(resolve, f);
          if (BEACON_IDS.has(f.id)) {
            beacons++;
            if (onBeacon) onBeacon(f);
            if (beacons > ignoreBeacons) {
              return finish(
                reject,
                new K5ProtocolError(
                  ignoreBeacons > 0
                    ? `More than ${ignoreBeacons} bootloader beacons while waiting for ${what}.`
                    : `A bootloader beacon arrived while waiting for ${what}.`,
                ),
              );
            }
          }
        }
        if (this.closedError) finish(reject, this.closedError);
      };
      if (signal?.aborted) return onAbort();
      if (signal) signal.addEventListener('abort', onAbort);
      timer = setTimeout(
        () => finish(reject, new K5TimeoutError(`No ${what} within ${(timeoutMs / 1000).toFixed(1)} s.`)),
        timeoutMs,
      );
      this.waiter = pump;
      pump();
    });
  }
}
