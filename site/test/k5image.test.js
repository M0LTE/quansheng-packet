// Firmware image packing and the bootloader write frames, against host/dotnet BootloaderTests
// (vendor updater frames captured by K5TOOL).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FirmwareImage, loadImage, pack, unpack, FLASH_LIMIT } from '../js/k5image.js';
import { makeWriteBody } from '../js/k5flasher.js';
import { buildPayload, fromHex, hex, u32 } from '../js/k5frame.js';
import { K5SafetyError } from '../js/k5link.js';
import { fakeImage } from './helpers.js';

// K5TOOL Packet2FlashWriteReq: vendor updater, first and last block of a 0xE6-block image.
const FLASH_FIRST = fromHex(
  '19050c01945d6a2c0000e6000001000088130020d5000000d9000000db000000000000000000000000000000' +
    '00000000000000000000000000000000dd0000000000000000000000df00000025c40000e3000000e5000000' +
    'e7000000e9000000eb000000ed000000ef000000f1000000f3000000f5000000f7000000f9000000fb000000' +
    'fd000000ff00000001010000030100000501000007010000090100000b0100000d0100000f01000011010000' +
    '130100001501000017010000190100001b0100001d0100001f010000210100000348854600f058fb00480047' +
    'edd100008813002013480047fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7fee7' +
    'fee7fee7fee7fee7',
);
const FLASH_LAST = fromHex(
  '19050c01272f5d07e500e600f4000000fff74eff2e4948602c480830fff748ff2b49886029480c30fff742' +
    'ff2849c860f920c000fff736ff2549403988632548fff730ff22494039c86322482030fff729ff214908623f' +
    '204001fff723ff1e494862fb20c000fff71dff1b49086319481030fff717ff184948637d200001fff711ff15' +
    '49886313480830fff70bff1249c863f720c000fff705ff104908600d480838fff7fffe044601208007806801' +
    '214906084349018860e0b209490863a001800d48630020fff713ff10bd18f0000080000040c40700000008' +
    '004078040020c0a00b40e6006cdc023093e8034201021c014201021801021001d2ff53ffff04ebff00ff00' +
    '0000000000000000000000',
);

test('write frame matches the vendor first block', () => {
  const seq = u32(FLASH_FIRST, 4);
  const body = makeWriteBody(seq, 0, 0xe6, FLASH_FIRST.subarray(16));
  assert.equal(hex(buildPayload(0x0519, body)), hex(FLASH_FIRST));
});

test('write frame matches the vendor last block', () => {
  const seq = u32(FLASH_LAST, 4);
  const body = makeWriteBody(seq, 0xe5, 0xe6, FLASH_LAST.subarray(16, 16 + 0xf4));
  assert.equal(hex(buildPayload(0x0519, body)), hex(FLASH_LAST));
});

test('write frame refuses the bootloader area', () => {
  assert.throws(() => makeWriteBody(1, 0xf0, 0xf1, new Uint8Array(256)), K5SafetyError);
});

test('pack and unpack round trip', () => {
  const raw = fakeImage();
  const packed = pack(raw, '*EGZUMER test');
  assert.equal(packed.length, raw.length + 18);
  const { raw: back, version } = unpack(packed);
  assert.deepEqual(back, raw);
  assert.equal(String.fromCharCode(...version).replace(/\0+$/, ''), '*EGZUMER test');
});

test('load detects packed and raw images', () => {
  const raw = fakeImage();
  const r = loadImage(raw);
  const p = loadImage(pack(raw, '*X'));
  assert.equal(r.wasPacked, false);
  assert.equal(r.version, '*');
  assert.equal(p.wasPacked, true);
  assert.equal(p.version, '*X');
  assert.deepEqual(p.raw, raw);
});

test('a packed file with a bad CRC is not mistaken for a raw image', () => {
  const bad = pack(fakeImage(), '*X');
  bad[bad.length - 1] ^= 0xff;
  assert.throws(() => loadImage(bad), /does not look like UV-K5 firmware/);
});

test('an oversized image is refused', () => {
  assert.throws(() => new FirmwareImage(fakeImage(FLASH_LIMIT + 1), '*').validate(), K5SafetyError);
  new FirmwareImage(fakeImage(FLASH_LIMIT), '*').validate();
});

test('the version the release workflow checks for is read from a packed image', () => {
  const img = loadImage(pack(fakeImage(0x8000), '*PKTFW v1.0.1'));
  assert.equal(img.version, '*PKTFW v1.0.1');
  assert.equal(img.blockCount, 0x80);
});
