// A small protocol v2 client for the setup page, over a FrameLink. One command at a time, as
// the protocol requires; replies matched by id and tag; events handed to onEvent.
//
// PTT and the host-to-radio data line share one contact on the AIOC, so every frame this page
// sends ends any transmission and starts the radio's serial PTT lock. The client records when
// a key-up would be honoured again (safeAt), and asks guard() before every send so the page can
// refuse to send while the radio may be transmitting.

import { BEACON_IDS, K5ProtocolError, K5SafetyError, K5TimeoutError } from './k5link.js';
import * as v2 from './v2.js';

export class V2StatusError extends K5ProtocolError {
  constructor(cmd, header) {
    const [name, text] = v2.STATUS[header.status] || [`status 0x${header.status.toString(16)}`, 'The radio refused.'];
    super(`${text} (${name}${header.detail !== undefined ? `, detail 0x${header.detail.toString(16)}` : ''})`);
    this.status = header.status;
    this.statusName = name;
    this.detailByte = header.detail;
    this.cmd = cmd;
  }
}

export const IN_FLASH_MODE =
  'The radio is in flash mode (its bootloader). Switch it off, then on again without holding PTT.';

export class RadioClient {
  constructor(link, { replyTimeoutMs = 400, helloTimeoutMs = 600, retries = 1, sessionId, firstTag, now = () => Date.now() } = {}) {
    this.link = link;
    this.replyTimeoutMs = replyTimeoutMs;
    this.helloTimeoutMs = helloTimeoutMs;
    this.retries = retries;
    this.now = now;
    this.sessionId = sessionId ?? Math.floor(Math.random() * 0xffffffff);
    this.tag = firstTag ?? 1 + Math.floor(Math.random() * 254);
    this.pending = null;
    this.chain = Promise.resolve();
    this.safeAt = 0;
    this.serialLockMs = 20;
    this.onEvent = null;
    this.onBootloader = null;
    this.guard = null;
    this.hello = null;
    this.info = null;
    link.onFrame = (f) => this._onFrame(f);
  }

  /** ms until a PTT press would key at once (0 if now). */
  msUntilSafeToKey() {
    return Math.max(0, this.safeAt - this.now());
  }

  _onFrame(f) {
    if (f.crc === 'bad') return;
    if (BEACON_IDS.has(f.id)) {
      if (this.onBootloader) this.onBootloader();
      if (this.pending) this.pending.fail(new K5ProtocolError(IN_FLASH_MODE));
      return;
    }
    if (this.pending && this.pending.match(f)) {
      this.pending.done(f);
      return;
    }
    if (v2.isEvent(f.id) && f.crc === 'valid' && this.onEvent) {
      try {
        this.onEvent(v2.parseEvent(f.id, f.body));
      } catch {
        // a short or odd event is not worth failing over
      }
    }
  }

  _serial(fn) {
    const run = this.chain.then(fn, fn);
    this.chain = run.catch(() => {});
    return run;
  }

  _waitFor(match, timeoutMs, what) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending = null;
        reject(new K5TimeoutError(`No ${what} from the radio.`));
      }, timeoutMs);
      this.pending = {
        match,
        done: (f) => {
          clearTimeout(timer);
          this.pending = null;
          resolve(f);
        },
        fail: (e) => {
          clearTimeout(timer);
          this.pending = null;
          reject(e);
        },
      };
    });
  }

  async _exchange(id, body, match, timeoutMs, what) {
    const reason = this.guard ? this.guard() : null;
    if (reason) throw new K5SafetyError(reason);
    let lastError;
    for (let attempt = 0; attempt <= this.retries; attempt++) {
      const wait = this._waitFor(match, timeoutMs, what);
      try {
        await this.link.send(id, body);
      } catch (e) {
        this.pending = null;
        throw e;
      }
      try {
        return await wait;
      } catch (e) {
        lastError = e;
        if (!(e instanceof K5TimeoutError)) throw e;
      }
    }
    throw lastError;
  }

  /** Legacy hello: identifies the firmware. Throws if the radio is in flash mode. */
  sayHello() {
    return this._serial(async () => {
      const f = await this._exchange(
        v2.HELLO,
        v2.helloBody(this.sessionId),
        (f) => f.id === v2.HELLO_REPLY,
        this.helloTimeoutMs,
        'answer',
      ).catch((e) => {
        if (e instanceof K5TimeoutError) {
          throw new K5TimeoutError(
            'The radio did not answer. Check it is switched on (not in flash mode), the cable is pushed fully into ' +
              'the radio, and you picked the right port.',
          );
        }
        throw e;
      });
      this.safeAt = this.now() + this.serialLockMs + 2;
      this.hello = v2.parseHelloReply(f.body);
      return this.hello;
    });
  }

  /** A v2 command: returns the reply body after the 4-byte header; throws V2StatusError on an error status. */
  command(cmd, rest = new Uint8Array(0)) {
    return this._serial(async () => {
      this.tag = (this.tag % 255) + 1;
      const tag = this.tag;
      const body = new Uint8Array(1 + rest.length);
      body[0] = tag;
      body.set(rest, 1);
      const f = await this._exchange(
        cmd,
        body,
        (f) => f.id === cmd + v2.REPLY_OFFSET && f.crc === 'valid' && f.body[0] === tag,
        this.replyTimeoutMs,
        `reply to 0x${cmd.toString(16)}`,
      );
      const h = v2.parseReplyHeader(f.body);
      this.safeAt = this.now() + h.lockMs;
      if (h.status !== 0) throw new V2StatusError(cmd, h);
      return h.rest;
    });
  }

  async getInfo() {
    this.info = v2.parseInfo(await this.command(v2.CMD.GET_INFO));
    this.serialLockMs = this.info.serialLockMs;
    return this.info;
  }

  async getStatus() {
    return v2.parseStatus(await this.command(v2.CMD.GET_STATUS));
  }

  async getParams(ids = [], stored = false) {
    return v2.parseGetParams(await this.command(v2.CMD.GET_PARAMS, v2.encodeGetParams({ ids, stored }))).values;
  }

  async setParams(records, flags) {
    return v2.parseSetParams(await this.command(v2.CMD.SET_PARAMS, v2.encodeSetParams(records, flags)));
  }

  async subscribe(opts) {
    return v2.parseSubscribe(await this.command(v2.CMD.SUBSCRIBE, v2.encodeSubscribe(opts)));
  }

  async levelTone(opts) {
    return v2.parseLevelTone(await this.command(v2.CMD.LEVEL_TONE, v2.encodeLevelTone(opts)));
  }

  /**
   * Sets frequency, power and bandwidth and saves them in the radio. The frequency must be
   * one the radio will transmit on (checked here first, and by the radio with REQUIRE_TX_OK).
   */
  async saveChannel({ frequencyHz, power, bandwidth }) {
    const records = [];
    if (frequencyHz !== undefined) {
      if (!v2.txAllowed(frequencyHz) || frequencyHz % 10 !== 0) {
        throw new K5SafetyError('The radio transmits only from 136 to 174 MHz and from 400 to 470 MHz.');
      }
      records.push([v2.PARAM.FREQ_HZ, frequencyHz]);
    }
    if (power !== undefined) {
      if (![0, 1, 2].includes(power)) throw new K5SafetyError('Power must be low, mid or high.');
      records.push([v2.PARAM.POWER, power]);
    }
    if (bandwidth !== undefined) {
      if (![0, 1].includes(bandwidth)) throw new K5SafetyError('Bandwidth must be wide or narrow.');
      records.push([v2.PARAM.BANDWIDTH, bandwidth]);
    }
    if (!records.length) throw new K5SafetyError('Nothing to change.');
    return this.setParams(records, { persist: true, requireTxOk: frequencyHz !== undefined });
  }
}
