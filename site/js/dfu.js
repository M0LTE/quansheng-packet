// USB DFU 1.1 with ST's DfuSe extensions, over WebUSB, for the AIOC (STM32F302).
//
// Written for this site from the DFU 1.1 specification, ST's AN3156 (DfuSe) and the behaviour
// of dfu-util, which the AIOC README uses. Two parts:
//  - detachToBootloader: the AIOC firmware has a DFU runtime interface (class FE, subclass 1,
//    protocol 1); DFU_DETACH there makes it reboot into the STM32 system bootloader, as
//    `dfu-util -d 1209:7388,0483:df11` does. The bootloader enumerates as 0483:df11, a new
//    device as far as the browser is concerned, so the page must ask for it again.
//  - DfuseDevice: upload (backup), erase, download, verify and leave on the bootloader.
//
// The USB device is anything shaped like WebUSB's USBDevice, so tests can use a fake.

export const AIOC_RUNTIME = { vendorId: 0x1209, productId: 0x7388 };
export const STM32_BOOTLOADER = { vendorId: 0x0483, productId: 0xdf11 };
export const AIOC_FLASH_START = 0x08000000;
export const AIOC_FLASH_SIZE = 128 * 1024;

export const REQ = { DETACH: 0, DNLOAD: 1, UPLOAD: 2, GETSTATUS: 3, CLRSTATUS: 4, GETSTATE: 5, ABORT: 6 };
export const STATE = {
  appIDLE: 0,
  appDETACH: 1,
  dfuIDLE: 2,
  dfuDNLOAD_SYNC: 3,
  dfuDNBUSY: 4,
  dfuDNLOAD_IDLE: 5,
  dfuMANIFEST_SYNC: 6,
  dfuMANIFEST: 7,
  dfuMANIFEST_WAIT_RESET: 8,
  dfuUPLOAD_IDLE: 9,
  dfuERROR: 10,
};
const STATUS_TEXT = [
  'OK',
  'target: wrong file for this device',
  'file: failed a check',
  'write failed',
  'erase failed',
  'erase check failed',
  'program failed',
  'verify failed',
  'address out of range',
  'data received too early',
  'firmware corrupt',
  'vendor error',
  'unexpected USB reset',
  'unexpected power-on reset',
  'unknown error',
  'stalled request',
];

export class DfuError extends Error {
  constructor(m) {
    super(m);
    this.name = 'DfuError';
  }
}

const sleepMs = (ms) => new Promise((r) => setTimeout(r, ms));

/** The first interface whose alternate 0 is DFU (class FE, subclass 1) with this protocol. */
export function findDfuInterface(device, protocol) {
  const config = device.configuration || device.configurations?.[0];
  if (!config) return null;
  for (const itf of config.interfaces) {
    for (const alt of itf.alternates) {
      if (alt.interfaceClass === 0xfe && alt.interfaceSubclass === 0x01 && (protocol === undefined || alt.interfaceProtocol === protocol)) {
        return { interfaceNumber: itf.interfaceNumber, alternates: itf.alternates };
      }
    }
  }
  return null;
}

/**
 * Sends DFU_DETACH to the AIOC's runtime DFU interface. The AIOC reboots into its bootloader
 * straight away, so errors after the request has gone out are expected and ignored.
 */
export async function detachToBootloader(device, { timeoutMs = 1000 } = {}) {
  await device.open();
  if (!device.configuration) await device.selectConfiguration(1);
  const itf = findDfuInterface(device, 1);
  if (!itf) {
    try {
      await device.close();
    } catch {}
    throw new DfuError(
      'This AIOC has no firmware update interface, so it cannot switch itself into its bootloader (firmware older than 1.2.0?). Use the manual way below.',
    );
  }
  await device.claimInterface(itf.interfaceNumber);
  try {
    await device.controlTransferOut({
      requestType: 'class',
      recipient: 'interface',
      request: REQ.DETACH,
      value: timeoutMs,
      index: itf.interfaceNumber,
    });
  } catch (e) {
    // The device may reset before the status stage completes: that is success here.
    if (!/disconnected|NetworkError|NotFoundError|transfer error|stall/i.test(`${e?.name} ${e?.message}`)) throw e;
  }
  try {
    await device.close();
  } catch {}
  return itf.interfaceNumber;
}

/**
 * Parses a DfuSe memory description such as "@Internal Flash  /0x08000000/064*0002Kg".
 * Returns { name, segments: [{ start, end, sectorSize, readable, erasable, writable }] }.
 */
export function parseMemoryLayout(desc) {
  const m = /^@([^/]*)\/(0x[0-9a-fA-F]+)\/(.*)$/.exec((desc || '').trim());
  if (!m) return null;
  const name = m[1].trim();
  let addr = parseInt(m[2], 16);
  const segments = [];
  for (const part of m[3].split(',')) {
    const s = /^\s*(\d+)\s*\*\s*(\d+)\s*([ BKM])\s*([a-g])\s*$/.exec(part);
    if (!s) continue;
    const count = parseInt(s[1], 10);
    const mult = { ' ': 1, B: 1, K: 1024, M: 1024 * 1024 }[s[3]];
    const size = parseInt(s[2], 10) * mult;
    const flags = s[4].charCodeAt(0) - 0x60; // a=1 readable, b=2 erasable, d=4 writable
    segments.push({
      start: addr,
      end: addr + count * size,
      sectorSize: size,
      readable: (flags & 1) !== 0,
      erasable: (flags & 2) !== 0,
      writable: (flags & 4) !== 0,
    });
    addr += count * size;
  }
  return { name, segments };
}

/** wTransferSize from the DFU functional descriptor in the raw configuration descriptor. */
export function transferSizeFromConfig(bytes, interfaceNumber) {
  let i = 0;
  let inTarget = false;
  while (i + 1 < bytes.length) {
    const len = bytes[i];
    const type = bytes[i + 1];
    if (len < 2) break;
    if (type === 0x04) inTarget = bytes[i + 2] === interfaceNumber;
    if (type === 0x21 && inTarget && len >= 7) return bytes[i + 5] | (bytes[i + 6] << 8);
    i += len;
  }
  return null;
}

async function readConfigDescriptor(device) {
  const head = await device.controlTransferIn(
    { requestType: 'standard', recipient: 'device', request: 6, value: 0x0200, index: 0 },
    9,
  );
  if (head.status !== 'ok' || head.data.byteLength < 4) return null;
  const total = head.data.getUint16(2, true);
  const all = await device.controlTransferIn(
    { requestType: 'standard', recipient: 'device', request: 6, value: 0x0200, index: 0 },
    total,
  );
  if (all.status !== 'ok') return null;
  return new Uint8Array(all.data.buffer, all.data.byteOffset, all.data.byteLength);
}

export class DfuseDevice {
  constructor(device, { interfaceNumber, alternateSetting = 0, transferSize = 2048, layout = null, sleep = sleepMs } = {}) {
    Object.assign(this, { device, interfaceNumber, alternateSetting, transferSize, layout, sleep });
  }

  /** Opens the bootloader, claims its DFU interface and picks the internal flash. */
  static async open(device, { sleep } = {}) {
    await device.open();
    if (!device.configuration) await device.selectConfiguration(1);
    const itf = findDfuInterface(device, 2);
    if (!itf) throw new DfuError('This device has no DFU interface. Is it the AIOC in bootloader mode ("STM32 BOOTLOADER")?');
    let alt = itf.alternates.find((a) => /internal flash/i.test(a.interfaceName || '')) || itf.alternates[0];
    await device.claimInterface(itf.interfaceNumber);
    if (itf.alternates.length > 1) await device.selectAlternateInterface(itf.interfaceNumber, alt.alternateSetting);
    let transferSize = 2048;
    try {
      const cfg = await readConfigDescriptor(device);
      const t = cfg && transferSizeFromConfig(cfg, itf.interfaceNumber);
      if (t) transferSize = t;
    } catch {
      // keep the STM32 bootloader's 2048
    }
    const layout = parseMemoryLayout(alt.interfaceName);
    return new DfuseDevice(device, {
      interfaceNumber: itf.interfaceNumber,
      alternateSetting: alt.alternateSetting,
      transferSize,
      layout,
      sleep,
    });
  }

  _setup(request, value = 0) {
    return { requestType: 'class', recipient: 'interface', request, value, index: this.interfaceNumber };
  }

  async _out(request, value, data) {
    const r = await this.device.controlTransferOut(this._setup(request, value), data);
    if (r.status !== 'ok') throw new DfuError(`USB request ${request} failed (${r.status})`);
    return r;
  }

  async _in(request, value, length) {
    const r = await this.device.controlTransferIn(this._setup(request, value), length);
    if (r.status !== 'ok') throw new DfuError(`USB request ${request} failed (${r.status})`);
    return new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
  }

  async getStatus() {
    const d = await this._in(REQ.GETSTATUS, 0, 6);
    return { status: d[0], pollTimeout: d[1] | (d[2] << 8) | (d[3] << 16), state: d[4] };
  }

  clearStatus() {
    return this._out(REQ.CLRSTATUS, 0);
  }

  abort() {
    return this._out(REQ.ABORT, 0);
  }

  /** Back to dfuIDLE from wherever the bootloader is (clearing an error first). */
  async toIdle() {
    let s = await this.getStatus();
    if (s.state === STATE.dfuERROR) {
      await this.clearStatus();
      s = await this.getStatus();
    }
    if (s.state !== STATE.dfuIDLE) {
      await this.abort();
      s = await this.getStatus();
    }
    if (s.state !== STATE.dfuIDLE) throw new DfuError(`The bootloader would not get ready (state ${s.state}).`);
  }

  /** Polls GET_STATUS until the bootloader is no longer busy; throws on an error status. */
  async _waitIdle(what) {
    for (let i = 0; i < 1000; i++) {
      const s = await this.getStatus();
      if (s.status !== 0) {
        throw new DfuError(`The bootloader reported "${STATUS_TEXT[s.status] || s.status}" during ${what}.`);
      }
      if (s.state === STATE.dfuDNLOAD_IDLE || s.state === STATE.dfuIDLE) return s;
      if (s.state === STATE.dfuMANIFEST || s.state === STATE.dfuMANIFEST_WAIT_RESET) return s;
      if (s.state !== STATE.dfuDNBUSY && s.state !== STATE.dfuDNLOAD_SYNC) {
        throw new DfuError(`Unexpected bootloader state ${s.state} during ${what}.`);
      }
      await this.sleep(Math.max(1, s.pollTimeout));
    }
    throw new DfuError(`The bootloader stayed busy during ${what}.`);
  }

  async _command(bytes, what) {
    await this._out(REQ.DNLOAD, 0, Uint8Array.from(bytes));
    await this._waitIdle(what);
  }

  setAddress(addr) {
    return this._command([0x21, addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >>> 24) & 0xff], 'set address');
  }

  erasePage(addr) {
    return this._command([0x41, addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff, (addr >>> 24) & 0xff], `erase at 0x${addr.toString(16)}`);
  }

  _segment(addr) {
    return this.layout?.segments.find((s) => addr >= s.start && addr < s.end) || null;
  }

  /** Reads length bytes from start (DfuSe upload). onProgress(done, total). */
  async read(start, length, onProgress = () => {}) {
    await this.toIdle();
    await this.setAddress(start);
    await this.abort(); // back to dfuIDLE; the address pointer stays
    const out = new Uint8Array(length);
    const n = this.transferSize;
    for (let block = 0, off = 0; off < length; block++, off += n) {
      const want = Math.min(n, length - off);
      const d = await this._in(REQ.UPLOAD, block + 2, want);
      if (d.length < want) throw new DfuError(`The bootloader sent less than asked while reading (${d.length} of ${want}).`);
      out.set(d.subarray(0, want), off);
      onProgress(off + want, length);
    }
    await this.abort();
    return out;
  }

  /** Erases the pages data needs, then writes it from start. onProgress(stage, done, total). */
  async write(start, data, onProgress = () => {}) {
    await this.toIdle();
    const pages = [];
    for (let addr = start; addr < start + data.length; ) {
      const seg = this._segment(addr);
      const size = seg ? seg.sectorSize : 2048;
      if (seg && !seg.erasable) throw new DfuError(`The memory at 0x${addr.toString(16)} cannot be erased.`);
      const page = seg ? seg.start + Math.floor((addr - seg.start) / size) * size : addr - (addr % size);
      pages.push(page);
      addr = page + size;
    }
    const end = start + data.length;
    if (this.layout && !this._segment(end - 1)) throw new DfuError('The image is bigger than the AIOC\'s flash.');
    for (let i = 0; i < pages.length; i++) {
      await this.erasePage(pages[i]);
      onProgress('erase', i + 1, pages.length);
    }
    const n = this.transferSize;
    for (let off = 0; off < data.length; off += n) {
      const chunk = data.subarray(off, Math.min(data.length, off + n));
      await this.setAddress(start + off);
      await this._out(REQ.DNLOAD, 2, chunk);
      await this._waitIdle(`writing at 0x${(start + off).toString(16)}`);
      onProgress('write', off + chunk.length, data.length);
    }
  }

  /** Reads back and compares; throws if different. */
  async verify(start, data, onProgress = () => {}) {
    const back = await this.read(start, data.length, (d, t) => onProgress('verify', d, t));
    for (let i = 0; i < data.length; i++) {
      if (back[i] !== data[i]) throw new DfuError(`Check after writing failed at 0x${(start + i).toString(16)}. Try flashing again.`);
    }
  }

  /** Leaves the bootloader and starts the firmware at start (dfu-util's ":leave"). */
  async leave(start = AIOC_FLASH_START) {
    await this.toIdle();
    await this.setAddress(start);
    try {
      await this._out(REQ.DNLOAD, 2, new Uint8Array(0));
      await this.getStatus(); // the bootloader jumps to the firmware here
    } catch {
      // expected: the device goes away
    }
    try {
      await this.device.close();
    } catch {}
  }

  async close() {
    try {
      await this.device.releaseInterface(this.interfaceNumber);
    } catch {}
    try {
      await this.device.close();
    } catch {}
  }
}

/** Checks a file looks like AIOC firmware (STM32F302 vector table) and fits the flash. */
export function checkAiocImage(bytes) {
  if (bytes.length < 8 || bytes.length > AIOC_FLASH_SIZE) {
    throw new DfuError(`This file is ${bytes.length} bytes: not an AIOC firmware image (at most ${AIOC_FLASH_SIZE}).`);
  }
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const sp = dv.getUint32(0, true);
  const reset = dv.getUint32(4, true);
  if (sp < 0x20000000 || sp > 0x2000a000 || (reset & 1) === 0 || reset < AIOC_FLASH_START || reset >= AIOC_FLASH_START + bytes.length) {
    throw new DfuError('This file does not look like AIOC firmware.');
  }
}
