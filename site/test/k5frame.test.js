// Framing, CRC and obfuscation against frames captured from other tools and from the bench radio
// (the same captures as host/dotnet CodecTests), and against the firmware's golden vectors.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  crc16,
  encodeFrame,
  encodePayload,
  encodeLegacyReply,
  buildPayload,
  FrameDecoder,
  OBFUSCATION_KEY,
  fromHex,
  hex,
} from '../js/k5frame.js';
import { parseBeacon } from '../js/k5flasher.js';
import { vectors } from './vectors.js';

// amnemonic/Quansheng_UV-K5_Firmware docs/communication.md, "Get firmware version"
const HELLO_TX = fromHex('abcd0800026910e6b1dd58242bdfdcba');
const HELLO_RX = fromHex('abcd2800036930e645a452720f05e46e2130e9802a8e14e62e910d4066c929359d488b9884eba7b453e58337decadcba');
// Captured from the bench K5's bootloader 2.00.06 on 28 September 2026: CRC field raw FF FF.
export const REAL_BEACON_WIRE = fromHex(
  'abcd24000e6934e62f930e422d669f735e401697696c9be61cbf3d700f05e3402709e980166c14c6ffffdcba',
);

const ascii = (b) => String.fromCharCode(...b).split('\0')[0];

test('CRC-16/XMODEM check value', () => {
  assert.equal(crc16(new TextEncoder().encode('123456789')), 0x31c3);
});

test('hello frame matches the capture', () => {
  assert.deepEqual(encodeFrame(0x0514, fromHex('9f4c5564'), true), HELLO_TX);
});

test('captured hello request decodes with a valid CRC', () => {
  const [f] = new FrameDecoder().feed(HELLO_TX);
  assert.equal(f.id, 0x0514);
  assert.equal(f.crc, 'valid');
});

test('captured stock hello reply decodes, CRC absent', () => {
  const frames = new FrameDecoder().feed(HELLO_RX);
  assert.equal(frames.length, 1);
  assert.equal(frames[0].id, 0x0515);
  assert.equal(frames[0].crc, 'absent');
  assert.equal(ascii(frames[0].body.subarray(0, 16)), 'k5_2.01.23');
});

test('real bootloader beacon with a raw FF FF CRC', () => {
  const frames = new FrameDecoder().feed(REAL_BEACON_WIRE);
  assert.equal(frames.length, 1);
  assert.equal(frames[0].id, 0x0518);
  assert.equal(frames[0].crc, 'absent');
  const b = parseBeacon(frames[0]);
  assert.equal(b.version, '2.00.06');
  assert.equal(b.isVersion5, false);
});

test('obfuscated hello has raw id 0x6902', () => {
  const f = encodeFrame(0x0514, new Uint8Array(4), true);
  assert.equal(f[4] | (f[5] << 8), 0x6902);
});

test('plain frames round trip with the id visible', () => {
  const frame = encodeFrame(0x0601, Uint8Array.of(0x7d), false);
  assert.deepEqual([...frame.subarray(4, 6)], [0x01, 0x06]);
  const [f] = new FrameDecoder({ obfuscated: false }).feed(frame);
  assert.equal(f.crc, 'valid');
  assert.equal(f.body[0], 0x7d);
});

test('resynchronises on garbage and byte-at-a-time input', () => {
  const frames = [0, 1, 2, 3, 4].map((r) => encodeFrame(0x0601, Uint8Array.of(r), true));
  const stream = Uint8Array.from([0x00, 0xab, 0x12, ...frames[0], 0xab, 0xcd, 0xff, ...frames.slice(1).flatMap((x) => [...x])]);
  const dec = new FrameDecoder();
  const got = [];
  for (const b of stream) got.push(...dec.feed(Uint8Array.of(b)));
  assert.deepEqual(
    got.map((f) => f.body[0]),
    [0, 1, 2, 3, 4],
  );
});

test('flags a bad CRC', () => {
  const frame = encodeFrame(0x0601, Uint8Array.of(0x10), true);
  frame[5] ^= 1;
  assert.equal(new FrameDecoder().feed(frame)[0].crc, 'bad');
});

test('legacy reply footer is an obfuscated FFFF', () => {
  const payload = buildPayload(0x0601, Uint8Array.of(0x7d, 0x40, 0xe9));
  const frame = encodeLegacyReply(payload, true);
  const size = payload.length;
  assert.equal(frame[4 + size], OBFUSCATION_KEY[size % 16] ^ 0xff);
  assert.equal(frame[5 + size], OBFUSCATION_KEY[(size + 1) % 16] ^ 0xff);
  const [f] = new FrameDecoder().feed(frame);
  assert.equal(f.crc, 'absent');
  assert.deepEqual(f.payload, payload);
});

test('a truncated frame followed by a good one is recovered at once', () => {
  const cut = encodeFrame(0x50c1, new Uint8Array(36), true).subarray(0, 20);
  const good = encodeFrame(0x50c0, Uint8Array.of(1, 2, 3, 4, 5, 6, 7, 1, 1, 0, 0x70, 0, 10, 2), true);
  const dec = new FrameDecoder();
  const frames = dec.feed(Uint8Array.from([...cut, ...good]));
  assert.equal(frames.length, 1);
  assert.equal(frames[0].id, 0x50c0);
  assert.equal(frames[0].crc, 'valid');
  assert.equal(dec.hasPartialFrame, false);
});

test('dropPartialFrame gives up on a cut frame', () => {
  const dec = new FrameDecoder();
  assert.equal(dec.feed(encodeFrame(0x50c0, new Uint8Array(14), true).subarray(0, 10)).length, 0);
  assert.equal(dec.hasPartialFrame, true);
  assert.equal(dec.dropPartialFrame().length, 0);
  assert.equal(dec.hasPartialFrame, false);
  assert.equal(dec.feed(encodeFrame(0x50c6, new Uint8Array(19), true)).length, 1);
});

test('an oversize length is garbage', () => {
  const junk = [0xab, 0xcd, 0xfb, 0x00, 1, 2, 3];
  const good = encodeFrame(0x0528, Uint8Array.of(0x7a, 0, 0x3b, 0x29), true);
  assert.equal(new FrameDecoder().feed(Uint8Array.from([...junk, ...good])).length, 1);
});

test('the power-on banner goes to the discard callback', () => {
  const banner = new TextEncoder().encode('UV-K5 packet firmware, PKTFW 2951c48\r\n');
  const good = encodeFrame(0x0528, Uint8Array.of(0x7a, 0, 0x3b, 0x29), true);
  const text = [];
  const frames = new FrameDecoder().feed(Uint8Array.from([...banner, ...good]), (d) => text.push(...d));
  assert.equal(frames.length, 1);
  assert.deepEqual(Uint8Array.from(text), banner);
});

for (const v of vectors) {
  test(`golden vector ${v.name}: response decodes to the listed frames`, () => {
    const frames = new FrameDecoder({ obfuscated: v.responseObfuscated }).feed(v.response);
    assert.equal(frames.length, v.frames.length);
    frames.forEach((f, i) => {
      assert.equal(f.id, v.frames[i].id);
      assert.equal(f.crc, v.frames[i].crc === 'real' ? 'valid' : 'absent');
      assert.equal(hex(f.body), hex(v.frames[i].body));
      assert.equal(f.declaredBodyLength, f.body.length);
    });
  });

  if (v.request.length) {
    test(`golden vector ${v.name}: request re-encodes identically`, () => {
      const [f] = new FrameDecoder({ obfuscated: v.requestObfuscated, maxPayload: 248 }).feed(v.request);
      assert.equal(f.crc, 'valid');
      assert.equal(hex(encodePayload(f.payload, v.requestObfuscated)), hex(v.request));
    });
  }
}
