// DFU detach and DfuSe backup, flash, verify and leave, against fake USB devices.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  detachToBootloader,
  DfuseDevice,
  DfuError,
  parseMemoryLayout,
  transferSizeFromConfig,
  checkAiocImage,
  AIOC_FLASH_START,
  AIOC_FLASH_SIZE,
} from '../js/dfu.js';
import { FakeAiocRuntime, FakeStm32Bootloader } from './fake-usb.js';

const noSleep = async () => {};

function aiocImage(size = 128000) {
  const b = new Uint8Array(size).map((_, i) => (i * 13 + 5) & 0xff);
  const dv = new DataView(b.buffer);
  dv.setUint32(0, 0x20004000, true);
  dv.setUint32(4, 0x0800da49, true);
  b.fill(0xff, 0x1f000); // the settings page, as the full image has it
  return b;
}

test('detach: claims the DFU runtime interface and sends DFU_DETACH to it', async () => {
  const dev = new FakeAiocRuntime();
  const n = await detachToBootloader(dev);
  assert.equal(n, 6);
  assert.deepEqual(dev.detached, { requestType: 'class', recipient: 'interface', request: 0, value: 1000, index: 6 });
  assert.equal(dev.opened, false);
});

test('detach: an AIOC without the runtime interface gets the manual route', async () => {
  await assert.rejects(detachToBootloader(new FakeAiocRuntime({ withDfu: false })), (e) => e instanceof DfuError && /manual/.test(e.message));
});

test('detach: a reset during the request counts as success', async () => {
  const dev = new FakeAiocRuntime({ resetDuringDetach: true });
  await detachToBootloader(dev);
  const quiet = new FakeAiocRuntime({ resetDuringDetach: false });
  await detachToBootloader(quiet);
  assert.ok(quiet.detached);
});

test('memory layout and transfer size', () => {
  const l = parseMemoryLayout('@Internal Flash  /0x08000000/064*0002Kg');
  assert.equal(l.name, 'Internal Flash');
  assert.deepEqual(l.segments, [
    { start: 0x08000000, end: 0x08020000, sectorSize: 2048, readable: true, erasable: true, writable: true },
  ]);
  const multi = parseMemoryLayout('@Internal Flash /0x08000000/04*016Kg,01*064Kg,07*128Kg');
  assert.equal(multi.segments.length, 3);
  assert.equal(multi.segments[2].end, 0x08100000);
  assert.equal(parseMemoryLayout('nonsense'), null);
  const fake = new FakeStm32Bootloader({ transferSize: 1024 });
  assert.equal(transferSizeFromConfig(fake._configDescriptor(), 0), 1024);
});

test('open: picks the internal flash and reads the transfer size', async () => {
  const dev = new FakeStm32Bootloader();
  const d = await DfuseDevice.open(dev, { sleep: noSleep });
  assert.equal(d.interfaceNumber, 0);
  assert.equal(d.alternateSetting, 0);
  assert.equal(d.transferSize, 2048);
  assert.equal(d.layout.segments[0].sectorSize, 2048);
});

test('backup reads the whole 128 KB flash', async () => {
  const dev = new FakeStm32Bootloader();
  const d = await DfuseDevice.open(dev, { sleep: noSleep });
  let last = 0;
  const data = await d.read(AIOC_FLASH_START, AIOC_FLASH_SIZE, (done) => (last = done));
  assert.deepEqual(data, dev.flash);
  assert.equal(last, AIOC_FLASH_SIZE);
  // DfuSe: set the address pointer, abort to idle, then UPLOAD from block 2
  const uploads = dev.log.filter((l) => l.dir === 'in' && l.request === 2);
  assert.equal(uploads.length, 64);
  assert.equal(uploads[0].value, 2);
  assert.equal(uploads.at(-1).value, 65);
});

test('flash: erases the pages the image needs, writes, verifies and leaves', async () => {
  const dev = new FakeStm32Bootloader();
  const d = await DfuseDevice.open(dev, { sleep: noSleep });
  const img = aiocImage();
  const stages = new Set();
  await d.write(AIOC_FLASH_START, img, (s) => stages.add(s));
  assert.equal(dev.erased.length, 63); // 128000 bytes over 2 KB pages
  assert.equal(dev.erased[0], 0x08000000);
  assert.equal(dev.erased.at(-1), 0x0801f000);
  assert.deepEqual(dev.flash.subarray(0, img.length), img);
  assert.equal(dev.writes.length, Math.ceil(img.length / 2048));
  await d.verify(AIOC_FLASH_START, img, (s) => stages.add(s));
  assert.deepEqual([...stages], ['erase', 'write', 'verify']);
  await d.leave();
  assert.equal(dev.left, true);
  // leave is: set the address to the start of flash, then a zero-length download
  const outs = dev.log.filter((l) => l.dir === 'out' && l.request === 1);
  const setAddr = outs.at(-2);
  assert.deepEqual([...setAddr.data], [0x21, 0x00, 0x00, 0x00, 0x08]);
  assert.equal(outs.at(-1).data.length, 0);
  assert.equal(outs.at(-1).value, 2);
});

test('a write error from the bootloader is reported', async () => {
  const dev = new FakeStm32Bootloader({ failWriteAt: 0x08001000 });
  const d = await DfuseDevice.open(dev, { sleep: noSleep });
  await assert.rejects(d.write(AIOC_FLASH_START, aiocImage()), (e) => e instanceof DfuError && /write failed/.test(e.message));
});

test('verify catches a bad byte', async () => {
  const dev = new FakeStm32Bootloader({ corruptAt: 0x4321 });
  const d = await DfuseDevice.open(dev, { sleep: noSleep });
  const img = aiocImage();
  await d.write(AIOC_FLASH_START, img);
  await assert.rejects(d.verify(AIOC_FLASH_START, img), /0x8004321/);
});

test('a bootloader left in an error state is cleared first', async () => {
  const dev = new FakeStm32Bootloader();
  dev.state = 10;
  dev.status = 3;
  const d = await DfuseDevice.open(dev, { sleep: noSleep });
  const data = await d.read(AIOC_FLASH_START, 4096);
  assert.equal(data.length, 4096);
  assert.ok(dev.log.some((l) => l.dir === 'out' && l.request === 4));
});

test('AIOC image check', () => {
  checkAiocImage(aiocImage());
  assert.throws(() => checkAiocImage(new Uint8Array(200000)), /at most/);
  const bad = aiocImage();
  bad[3] = 0x10;
  assert.throws(() => checkAiocImage(bad), /does not look like AIOC firmware/);
});
