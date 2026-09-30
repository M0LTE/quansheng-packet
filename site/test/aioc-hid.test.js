// The read-only AIOC register reader, against a fake WebHID device built from usb_hid.c.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseFeatureReport, readEqState, describeEq, K5_RED_CTRL, MAGIC_AIOC, MARKER_TXEQ } from '../js/aioc-hid.js';

class FakeHid {
  constructor(regs, { withReportId = false } = {}) {
    this.regs = regs;
    this.withReportId = withReportId;
    this.current = 0;
    this.writes = [];
  }
  async sendFeatureReport(id, data) {
    assert.equal(id, 0);
    assert.equal(data.length, 6);
    if (data[0] & 0xf1) this.writes.push([...data]); // any write, store, defaults, recall or reboot bit
    this.current = data[1];
  }
  async receiveFeatureReport(id) {
    const v = this.regs[this.current] ?? 0;
    const body = [0, this.current, v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >>> 24];
    const bytes = Uint8Array.from(this.withReportId ? [id, ...body] : body);
    return new DataView(bytes.buffer);
  }
}

test('feature reports parse with and without the report id byte', () => {
  assert.equal(parseFeatureReport(Uint8Array.of(0, 0xbf, 3, 0x13, 0x80, 0xbb), 0xbf), K5_RED_CTRL);
  assert.equal(parseFeatureReport(Uint8Array.of(0, 0, 0xbf, 3, 0x13, 0x80, 0xbb), 0xbf), K5_RED_CTRL);
  assert.throws(() => parseFeatureReport(Uint8Array.of(0, 0xc8, 0, 0, 0, 0), 0xbf));
});

for (const withReportId of [false, true]) {
  test(`k5-red on, playing, read only (report id ${withReportId})`, async () => {
    const dev = new FakeHid({ 0x00: MAGIC_AIOC, 0xbf: K5_RED_CTRL, 0xc8: 0x1307, 0xc9: MARKER_TXEQ }, { withReportId });
    const s = await readEqState(dev);
    assert.equal(s.on, true);
    assert.equal(s.k5Red, true);
    assert.equal(s.running, true);
    assert.equal(s.clips, 0);
    assert.equal(s.level, 'ok');
    assert.deepEqual(dev.writes, []);
  });
}

test('EQ off, and stock firmware', () => {
  assert.equal(describeEq({ magic: MAGIC_AIOC, ctrl: 0, info: 0, marker: MARKER_TXEQ }).level, 'warn');
  const stock = describeEq({ magic: MAGIC_AIOC, ctrl: 0, info: 0, marker: 0 });
  assert.equal(stock.packetFirmware, false);
  assert.match(stock.summary, /does not seem to run the packet AIOC firmware/);
  assert.equal(describeEq({ magic: 0, ctrl: 0, info: 0, marker: 0 }).level, 'error');
});

test('idle and clipping details', () => {
  assert.match(describeEq({ magic: MAGIC_AIOC, ctrl: K5_RED_CTRL, info: 0, marker: MARKER_TXEQ }).detail, /48000 Hz/);
  assert.match(describeEq({ magic: MAGIC_AIOC, ctrl: K5_RED_CTRL, info: (12 << 16) | 0x1307, marker: MARKER_TXEQ }).detail, /clipped 12/);
});
