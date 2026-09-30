// Protocol v2 encoding and decoding against the firmware's golden vectors, and the page's client
// against a fake radio that answers with the vectors' replies.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as v2 from '../js/v2.js';
import { RadioClient, V2StatusError, IN_FLASH_MODE } from '../js/radio.js';
import { FrameLink, K5SafetyError, K5TimeoutError } from '../js/k5link.js';
import { FrameDecoder, encodeFrame, encodePayload, encodeLegacyReply, encodeBootloaderFrame, buildPayload, hex } from '../js/k5frame.js';
import { vector, vectors } from './vectors.js';
import { BEACON_V2 } from './sim-bootloader.js';

const reqBody = (name) => {
  const v = vector(name);
  return new FrameDecoder({ obfuscated: v.requestObfuscated, maxPayload: 248 }).feed(v.request)[0].body;
};
const replyRest = (name, i = 0) => v2.parseReplyHeader(vector(name).frames[i].body).rest;
const withTag = (tag, rest) => Uint8Array.from([tag, ...rest]);

// ---------------------------------------------------------------- requests

test('hello requests match the vectors in both modes', () => {
  assert.equal(hex(encodeFrame(v2.HELLO, v2.helloBody(0x12345678), true)), hex(vector('hello_obfuscated').request));
  assert.equal(hex(encodeFrame(v2.HELLO, v2.helloBody(0x12345678), false)), hex(vector('hello_plain').request));
});

test('GET_INFO request, plain and obfuscated', () => {
  assert.equal(hex(encodeFrame(v2.CMD.GET_INFO, Uint8Array.of(1), false)), hex(vector('get_info').request));
  assert.equal(hex(encodeFrame(v2.CMD.GET_INFO, Uint8Array.of(2), true)), hex(vector('get_info_obfuscated').request));
});

test('GET_STATUS request', () => {
  assert.equal(hex(encodeFrame(v2.CMD.GET_STATUS, Uint8Array.of(0x10), false)), hex(vector('get_status').request));
});

test('GET_PARAMS requests', () => {
  assert.equal(hex(withTag(0x40, v2.encodeGetParams())), hex(reqBody('get_params_all')));
  assert.equal(hex(withTag(0x41, v2.encodeGetParams({ stored: true, ids: [0x14, 0x19, 0x08] }))), hex(reqBody('get_params_stored')));
  assert.equal(hex(withTag(0x44, v2.encodeGetParams({ ids: [6, 7] }))), hex(reqBody('get_params_retired')));
});

test('SET_PARAMS requests', () => {
  const set = v2.encodeSetParams([
    [v2.PARAM.FREQ_HZ, 433_500_000],
    [v2.PARAM.BUSY_SQL_LEVEL, 3],
    [v2.PARAM.DEV_WIDE, 0x0800],
  ]);
  assert.equal(hex(withTag(0x44, set)), hex(reqBody('set_params')));
  const range = v2.encodeSetParams([
    [v2.PARAM.BUSY_SQL_LEVEL, 5],
    [v2.PARAM.TX_TIMEOUT_S, 25],
  ]);
  assert.equal(hex(withTag(0x46, range)), hex(reqBody('set_params_range')));
  assert.equal(v2.encodeSetParams([], { persist: true, requireTxOk: true })[0], 3);
});

test('SUBSCRIBE and LEVEL_TONE requests', () => {
  assert.equal(hex(withTag(0x33, v2.encodeSubscribe({ mask: 3 }))), hex(reqBody('subscribe')));
  assert.equal(hex(withTag(0xa0, v2.encodeLevelTone({ hz: 1000, mode: 1, level: 64, durationMs: 500 }))), hex(reqBody('level_tone_raw')));
  assert.equal(
    hex(withTag(0xa9, v2.encodeLevelTone({ hz: 1000, mode: 0, level: 3000, durationMs: 1000 }))),
    hex(reqBody('level_tone_uncalibrated')),
  );
});

// ---------------------------------------------------------------- replies

test('hello reply: PKT2 marker and version', () => {
  const h = v2.parseHelloReply(vector('hello_plain').frames[0].body);
  assert.equal(h.kind, 'v2');
  assert.equal(h.version, 'PKTFW test');
  assert.equal(h.protocol, 0x0200);
  assert.equal(h.protocolText, '2.0');
  const stock = new Uint8Array(36);
  stock.set(new TextEncoder().encode('k5_2.01.23'));
  assert.equal(v2.parseHelloReply(stock).kind, 'other');
  const v1 = new Uint8Array(36);
  v1.set(new TextEncoder().encode('PKTFW 2951c48'));
  assert.equal(v2.parseHelloReply(v1).kind, 'v1');
});

test('GET_INFO reply', () => {
  const h = v2.parseReplyHeader(vector('get_info').frames[0].body);
  assert.deepEqual([h.tag, h.status, h.lockMs], [1, 0, 20]);
  const i = v2.parseInfo(h.rest);
  assert.equal(i.protocol, 0x0200);
  assert.equal(i.version, 'PKTFW test');
  assert.equal(i.serialLockMs, 20);
  assert.equal(i.lateKeyMaxMs, 30);
  assert.equal(i.txPolicy, v2.TX_POLICY_FIXED);
  assert.equal(i.settingsLayout, 2);
  assert.equal(i.maxRequestBody, 120);
  assert.equal(i.ringCapacity, 20);
  assert.equal(i.canLevelToneRaw, true);
  assert.equal(i.events, 0x1fff);
});

test('GET_STATUS reply', () => {
  const s = v2.parseStatus(replyRest('get_status'));
  assert.equal(s.uptimeMs, 1000);
  assert.equal(s.frequencyHz, 144_800_000);
  assert.equal(s.state, 0);
  assert.equal(s.txAllowedHere, true);
  assert.equal(s.lockActive, true);
  assert.equal(s.power, 0);
  assert.equal(s.bandwidth, 0);
  assert.equal(s.deviation, 0x856);
  assert.equal(s.rssiRaw, 0x123);
  assert.equal(s.noise, 0x49);
  assert.equal(s.glitch, 0x5b);
  assert.equal(s.batteryLevel, 5);
  assert.equal(s.batteryMv, 7800);
  assert.equal(s.txTimeLeft100ms, 0xffff);
  assert.equal(s.txTimeoutS, 30);
});

test('GET_PARAMS replies', () => {
  const all = v2.parseGetParams(replyRest('get_params_all')).values;
  assert.equal(all.get(v2.PARAM.FREQ_HZ), 144_800_000);
  assert.equal(all.get(v2.PARAM.POWER), 0);
  assert.equal(all.get(v2.PARAM.BANDWIDTH), 0);
  assert.equal(all.get(v2.PARAM.DEV_WIDE), 0x856);
  assert.equal(all.get(v2.PARAM.DEV_NARROW), 0x756);
  assert.equal(all.get(v2.PARAM.RX_GAIN), 45);
  assert.equal(all.get(v2.PARAM.TX_TIMEOUT_S), 30);
  assert.equal(all.get(v2.PARAM.SERIAL_LOCK_MS), 20);
  assert.equal(all.get(v2.PARAM.BUSY_RSSI_OPEN), 110);
  assert.equal(all.get(v2.PARAM.BUSY_SQL_RAW).length, 6);
  assert.equal(all.get(v2.PARAM.BUSY_SQL_LEVEL), 1);
  assert.equal(all.size, 23);
  const stored = v2.parseGetParams(replyRest('get_params_stored')).values;
  assert.deepEqual([...stored.entries()], [
    [0x19, 1],
    [0x08, 45],
  ]);
});

test('SET_PARAMS replies, OK and RANGE', () => {
  const r = v2.parseSetParams(replyRest('set_params'));
  assert.equal(r.txAllowed, true);
  assert.equal(r.persistQueued, false);
  assert.equal(r.retuned, true);
  assert.equal(r.values.get(v2.PARAM.FREQ_HZ), 433_500_000);
  assert.equal(r.values.get(v2.PARAM.DEV_WIDE), 0x800);
  const h = v2.parseReplyHeader(vector('set_params_range').frames[0].body);
  assert.equal(h.statusName, 'RANGE');
  assert.equal(h.detail, 0x0a);
});

test('error statuses', () => {
  assert.equal(v2.parseReplyHeader(vector('unknown_command').frames[0].body).statusName, 'UNKNOWN_CMD');
  assert.equal(v2.parseReplyHeader(vector('bad_length').frames[0].body).statusName, 'BAD_LENGTH');
  const retired = v2.parseReplyHeader(vector('get_params_retired').frames[0].body);
  assert.equal(retired.statusName, 'UNSUPPORTED');
  assert.equal(retired.detail, 6);
  assert.equal(v2.parseReplyHeader(vector('level_tone_uncalibrated').frames[0].body).statusName, 'UNSUPPORTED');
});

test('SUBSCRIBE and LEVEL_TONE replies', () => {
  assert.deepEqual(v2.parseSubscribe(replyRest('subscribe')), { nextSeq: 0, oldestSeq: 0, tMs: 1000 });
  assert.deepEqual(v2.parseLevelTone(replyRest('level_tone_raw')), { gainCode: 64, word: 0x2854 });
});

test('events', () => {
  const ev = (name, i = 0) => {
    const f = vector(name).frames[i];
    return v2.parseEvent(f.id, f.body);
  };
  const hb = ev('event_heartbeat');
  assert.equal(hb.type, 'heartbeat');
  assert.equal(hb.rssiRaw, 0x77);
  assert.equal(hb.batteryMv, 7800);
  assert.equal(hb.lockMs, 0);
  assert.equal(ev('event_cd_open').busy, true);
  assert.equal(ev('event_cd_open').rssiRaw, 200);
  const start = ev('event_tx_start_and_end', 0);
  assert.equal(start.type, 'tx_start');
  assert.equal(start.frequencyHz, 144_800_000);
  assert.equal(start.lockDelayMs, 12);
  assert.equal(start.lateKey, true);
  assert.equal(ev('event_tx_start_and_end', 1).reason, 0);
  const refused = ev('event_tx_refused');
  assert.deepEqual([refused.reason, refused.detail], [1, 1234]);
  assert.equal(ev('event_boot').type, 'boot');
  assert.equal(ev('event_boot').protocol, 0x0200);
});

// ---------------------------------------------------------------- frequency rules

test('frequency parsing refuses what the radio would refuse', () => {
  assert.deepEqual(v2.parseFrequencyMHz('144.800'), { hz: 144_800_000 });
  assert.deepEqual(v2.parseFrequencyMHz(' 433,5 MHz'), { hz: 433_500_000 });
  assert.deepEqual(v2.parseFrequencyMHz('145.01251'), { hz: 145_012_510 });
  assert.deepEqual(v2.parseFrequencyMHz('136'), { hz: 136_000_000 });
  assert.match(v2.parseFrequencyMHz('174').error, /136 to 174/);
  assert.match(v2.parseFrequencyMHz('173.99999').error ?? 'ok', /ok/);
  assert.match(v2.parseFrequencyMHz('470.000').error, /400 to 470/);
  assert.match(v2.parseFrequencyMHz('380').error, /transmits only/);
  assert.match(v2.parseFrequencyMHz('145.012501').error, /10 Hz/);
  assert.match(v2.parseFrequencyMHz('abc').error, /MHz/);
  assert.equal(v2.formatMHz(144_800_000), '144.800');
  assert.equal(v2.formatMHz(144_812_500), '144.8125');
});

test("signal level as the radio's screen shows it", () => {
  assert.equal(v2.rssiDbm(0x77), -100.5);
  // Tom's bench radio on 144.800 MHz into a terminated load: the page said -107, the screen -127.
  assert.equal(v2.signalDbm(106, 144_800_000), -127);
  assert.equal(v2.signalDbm(107, 144_800_000), -127); // the screen halves the raw value rounding down
  // band edges (frequencies.c): each band starts at its lower edge; below 108 MHz is band 1
  const edges = [
    [0, -15], [49_999_990, -15], [50_000_000, -15], [107_999_990, -15],
    [108_000_000, -25], [136_000_000, -25], [136_999_990, -25],
    [137_000_000, -20], [173_999_990, -20],
    [174_000_000, -4], [349_999_990, -4],
    [350_000_000, -7], [399_999_990, -7],
    [400_000_000, -6], [469_999_990, -6],
    [470_000_000, -1], [600_000_000, -1], [1_300_000_000, -1],
  ];
  for (const [hz, db] of edges) assert.equal(v2.rssiCorrectionDb(hz), db, `${hz} Hz`);
  assert.equal(v2.signalDbm(200, 433_500_000), 200 / 2 - 160 - 6);
});

test("S-meter as the radio's screen shows it", () => {
  // S0 at -130 dBm, S9 at -76 dBm, 6 dB per S-unit, then dB over S9 from S9+10
  assert.equal(v2.sPoint(-160), 'S0');
  assert.equal(v2.sPoint(-127), 'S0');
  assert.equal(v2.sPoint(-125), 'S0');
  assert.equal(v2.sPoint(-124), 'S1');
  assert.equal(v2.sPoint(-107), 'S3');
  assert.equal(v2.sPoint(-83), 'S7');
  assert.equal(v2.sPoint(-82), 'S8');
  assert.equal(v2.sPoint(-77), 'S8');
  assert.equal(v2.sPoint(-76), 'S9');
  assert.equal(v2.sPoint(-67), 'S9');
  assert.equal(v2.sPoint(-66), 'S9+10');
  assert.equal(v2.sPoint(-43), 'S9+33');
  assert.equal(v2.sPoint(0), 'S9+76');
  assert.equal(v2.sPoint(40), 'S9+99');
});

test('units', () => {
  assert.ok(Math.abs(v2.deviationKhz(0x856) - 2.8) < 1e-9);
  assert.ok(Math.abs(v2.deviationKhz(0x756) - 1.4) < 1e-9);
});

// ---------------------------------------------------------------- the client

/**
 * A radio that answers obfuscated like the real one after an obfuscated hello: with the reply
 * frames of the vector whose request matches (the tag rewritten to the one asked).
 */
class FakeRadio {
  constructor({ silent = false, bootloader = false, dropFirst = 0 } = {}) {
    this.decoder = new FrameDecoder({ obfuscated: true, maxPayload: 248 });
    this.link = new FrameLink({ write: async (b) => this.fromHost(b) });
    this.requests = [];
    Object.assign(this, { silent, bootloader, dropFirst });
    this.params = new Map([
      [1, 144_800_000],
      [2, 0],
      [3, 0],
    ]);
  }

  send(frame) {
    setTimeout(() => this.link.receive(frame), 1);
  }

  fromHost(bytes) {
    for (const f of this.decoder.feed(bytes)) {
      this.requests.push(f);
      if (this.bootloader) {
        this.send(encodeBootloaderFrame(BEACON_V2));
        continue;
      }
      if (this.silent) continue;
      if (this.dropFirst > 0) {
        this.dropFirst--;
        continue;
      }
      this.answer(f);
    }
  }

  answer(f) {
    if (f.id === v2.HELLO) {
      const body = vector('hello_obfuscated').frames[0].body;
      this.send(encodeLegacyReply(buildPayload(0x0515, body), true));
      return;
    }
    const tag = f.body[0];
    if (f.id === v2.CMD.SET_PARAMS) {
      // echo the records back, TX allowed, persist queued if asked
      const flags = f.body[1];
      const records = f.body.subarray(2);
      const reply = Uint8Array.from([tag, 0, 20, 0, 1 | (flags & 1 ? 2 : 0) | 4, ...records]);
      this.send(encodeFrame(v2.CMD.SET_PARAMS + 0x80, reply, true));
      return;
    }
    for (const v of vectors) {
      if (!v.request.length) continue;
      const [req] = new FrameDecoder({ obfuscated: v.requestObfuscated, maxPayload: 248 }).feed(v.request);
      if (req.id !== f.id || hex(req.body.subarray(1)) !== hex(f.body.subarray(1))) continue;
      for (const fr of v.frames) {
        const body = fr.body.slice();
        if (!v2.isEvent(fr.id)) body[0] = tag;
        this.send(encodePayload(buildPayload(fr.id, body), true));
      }
      return;
    }
  }
}

test('client: hello, GET_INFO, GET_STATUS and GET_PARAMS against the vectors', async () => {
  const radio = new FakeRadio();
  const c = new RadioClient(radio.link);
  const h = await c.sayHello();
  assert.equal(h.kind, 'v2');
  const info = await c.getInfo();
  assert.equal(info.txPolicy, 8);
  const s = await c.getStatus();
  assert.equal(s.frequencyHz, 144_800_000);
  const p = await c.getParams();
  assert.equal(p.get(v2.PARAM.DEV_WIDE), 0x856);
  assert.ok(c.msUntilSafeToKey() <= 20);
  // obfuscated on the wire, like the reference client
  assert.equal(radio.requests[0].id, v2.HELLO);
  assert.ok(radio.requests.every((r) => r.crc === 'valid'));
});

test('client: error statuses become V2StatusError', async () => {
  const radio = new FakeRadio();
  const c = new RadioClient(radio.link);
  await assert.rejects(
    c.levelTone({ hz: 1000, mode: 0, level: 3000, durationMs: 1000 }),
    (e) => e instanceof V2StatusError && e.statusName === 'UNSUPPORTED',
  );
  await assert.rejects(c.getParams([6, 7]), (e) => e instanceof V2StatusError && e.detailByte === 6);
});

test('client: saveChannel persists with REQUIRE_TX_OK and refuses non-TX frequencies locally', async () => {
  const radio = new FakeRadio();
  const c = new RadioClient(radio.link);
  const r = await c.saveChannel({ frequencyHz: 145_500_000, power: 2, bandwidth: 1 });
  assert.equal(r.persistQueued, true);
  assert.equal(r.values.get(v2.PARAM.FREQ_HZ), 145_500_000);
  const sent = radio.requests.at(-1);
  assert.equal(sent.id, v2.CMD.SET_PARAMS);
  assert.equal(sent.body[1], 3); // PERSIST | REQUIRE_TX_OK
  const before = radio.requests.length;
  await assert.rejects(c.saveChannel({ frequencyHz: 380_000_000 }), K5SafetyError);
  await assert.rejects(c.saveChannel({ power: 3 }), K5SafetyError);
  assert.equal(radio.requests.length, before);
});

test('client: the guard stops a send', async () => {
  const radio = new FakeRadio();
  const c = new RadioClient(radio.link);
  c.guard = () => 'The radio may be transmitting.';
  await assert.rejects(c.getStatus(), /may be transmitting/);
  assert.equal(radio.requests.length, 0);
});

test('client: a lost reply is retried once, then a clear timeout', async () => {
  const radio = new FakeRadio({ dropFirst: 1 });
  const c = new RadioClient(radio.link, { replyTimeoutMs: 50 });
  assert.equal((await c.getStatus()).batteryMv, 7800);
  assert.equal(radio.requests.length, 2);
  const quiet = new FakeRadio({ silent: true });
  const q = new RadioClient(quiet.link, { replyTimeoutMs: 30, helloTimeoutMs: 30 });
  await assert.rejects(q.sayHello(), (e) => e instanceof K5TimeoutError && /did not answer/.test(e.message));
  assert.equal(quiet.requests.length, 2);
});

test('client: a radio in flash mode is recognised', async () => {
  const radio = new FakeRadio({ bootloader: true });
  const c = new RadioClient(radio.link);
  let told = false;
  c.onBootloader = () => (told = true);
  await assert.rejects(c.sayHello(), (e) => e.message === IN_FLASH_MODE);
  assert.equal(told, true);
});

test('client: events reach onEvent', async () => {
  const radio = new FakeRadio();
  const c = new RadioClient(radio.link);
  const got = [];
  c.onEvent = (e) => got.push(e.type);
  radio.link.receive(vector('event_heartbeat').response.map((x) => x)); // plain frames: not decodable obfuscated
  const hb = vector('event_heartbeat').frames[0];
  radio.link.receive(encodePayload(buildPayload(hb.id, hb.body), true));
  assert.deepEqual(got, ['heartbeat']);
});
