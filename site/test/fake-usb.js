// Fake WebUSB devices: the AIOC in its firmware (with a DFU runtime interface) and the STM32
// system bootloader (DfuSe), close enough to the real ones to exercise the page's sequencing.

const ok = (bytes) => ({ status: 'ok', data: new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength) });

class FakeDeviceBase {
  constructor() {
    this.opened = false;
    this.configuration = null;
    this.claimed = new Set();
    this.log = [];
  }
  async open() {
    this.opened = true;
  }
  async close() {
    this.opened = false;
  }
  async selectConfiguration(n) {
    this.configuration = this.configurations[n - 1];
  }
  async claimInterface(n) {
    if (!this.opened) throw new Error('InvalidStateError: not open');
    this.claimed.add(n);
  }
  async releaseInterface(n) {
    this.claimed.delete(n);
  }
  async selectAlternateInterface(n, alt) {
    this.alt = alt;
  }
  _checkClass(setup) {
    if (setup.recipient === 'interface' && !this.claimed.has(setup.index)) {
      throw new Error('InvalidStateError: interface not claimed');
    }
  }
}

/** The AIOC running its firmware: audio, HID, CDC and a DFU runtime interface at 6. */
export class FakeAiocRuntime extends FakeDeviceBase {
  constructor({ withDfu = true, resetDuringDetach = true } = {}) {
    super();
    this.vendorId = 0x1209;
    this.productId = 0x7388;
    const itf = (n, cls, sub, proto) => ({
      interfaceNumber: n,
      alternates: [{ alternateSetting: 0, interfaceClass: cls, interfaceSubclass: sub, interfaceProtocol: proto }],
    });
    const interfaces = [itf(0, 1, 1, 0), itf(1, 1, 2, 0), itf(2, 1, 2, 0), itf(3, 3, 0, 0), itf(4, 2, 2, 0), itf(5, 10, 0, 0)];
    if (withDfu) interfaces.push(itf(6, 0xfe, 1, 1));
    this.configurations = [{ configurationValue: 1, interfaces }];
    this.resetDuringDetach = resetDuringDetach;
    this.detached = null;
  }
  async controlTransferOut(setup) {
    this._checkClass(setup);
    this.log.push(setup);
    if (setup.requestType === 'class' && setup.request === 0) {
      this.detached = setup;
      if (this.resetDuringDetach) throw new DOMException('The device was disconnected.', 'NetworkError');
    }
    return { status: 'ok', bytesWritten: 0 };
  }
}

/** The STM32F302 system bootloader: DfuSe on interface 0, 128 KB of 2 KB pages. */
export class FakeStm32Bootloader extends FakeDeviceBase {
  constructor({ flash, failWriteAt = null, busyPolls = 1, transferSize = 2048, corruptAt = null } = {}) {
    super();
    this.vendorId = 0x0483;
    this.productId = 0xdf11;
    this.flash = flash ? flash.slice() : new Uint8Array(128 * 1024).map((_, i) => (i * 7) & 0xff);
    this.base = 0x08000000;
    this.pageSize = 2048;
    this.transferSize = transferSize;
    this.configurations = [
      {
        configurationValue: 1,
        interfaces: [
          {
            interfaceNumber: 0,
            alternates: [
              { alternateSetting: 0, interfaceClass: 0xfe, interfaceSubclass: 1, interfaceProtocol: 2, interfaceName: '@Internal Flash  /0x08000000/064*0002Kg' },
              { alternateSetting: 1, interfaceClass: 0xfe, interfaceSubclass: 1, interfaceProtocol: 2, interfaceName: '@Option Bytes  /0x1FFFF800/01*016 e' },
            ],
          },
        ],
      },
    ];
    this.state = 2; // dfuIDLE
    this.status = 0;
    this.addr = this.base;
    this.pending = null;
    this.busyPolls = busyPolls;
    this.busyLeft = 0;
    this.erased = [];
    this.writes = [];
    this.left = false;
    this.failWriteAt = failWriteAt;
    this.corruptAt = corruptAt;
  }

  _configDescriptor() {
    const t = this.transferSize;
    const d = [
      9, 2, 0, 0, 1, 1, 0, 0x80, 50, // configuration (total length patched below)
      9, 4, 0, 0, 0, 0xfe, 1, 2, 4, // interface 0 alt 0
      9, 4, 0, 1, 0, 0xfe, 1, 2, 5, // interface 0 alt 1
      9, 0x21, 0x0b, 0xff, 0x00, t & 0xff, t >> 8, 0x1a, 0x01, // DFU functional
    ];
    d[2] = d.length;
    return Uint8Array.from(d);
  }

  async controlTransferIn(setup, length) {
    if (setup.requestType === 'standard' && setup.request === 6) {
      return ok(this._configDescriptor().subarray(0, length));
    }
    this._checkClass(setup);
    this.log.push({ ...setup, length, dir: 'in' });
    switch (setup.request) {
      case 3: {
        // GETSTATUS: the first one after a download reports busy, the next executes it (DfuSe)
        if (this.pending) {
          if (this.busyLeft > 0) {
            this.busyLeft--;
            this.state = 4;
            return ok(Uint8Array.of(0, 1, 0, 0, 4, 0));
          }
          this._execute(this.pending);
          this.pending = null;
        }
        return ok(Uint8Array.of(this.status, 0, 0, 0, this.state, 0));
      }
      case 2: {
        if (this.state !== 2 && this.state !== 9) return { status: 'stall' };
        const block = setup.value;
        if (block < 2) return { status: 'stall' };
        const off = this.addr - this.base + (block - 2) * this.transferSize;
        this.state = 9;
        return ok(this.flash.slice(off, off + length));
      }
      default:
        return { status: 'stall' };
    }
  }

  async controlTransferOut(setup, data = new Uint8Array(0)) {
    this._checkClass(setup);
    const bytes = new Uint8Array(data);
    this.log.push({ ...setup, data: bytes, dir: 'out' });
    switch (setup.request) {
      case 1: // DNLOAD
        if (setup.value === 0 && bytes.length === 0) return { status: 'stall' };
        if (setup.value !== 0 && setup.value < 2) return { status: 'stall' };
        if (bytes.length === 0) {
          this.left = true;
          this.state = 7;
          return { status: 'ok', bytesWritten: 0 };
        }
        this.pending = { block: setup.value, bytes };
        this.busyLeft = this.busyPolls;
        this.state = 3;
        return { status: 'ok', bytesWritten: bytes.length };
      case 4:
        this.status = 0;
        this.state = 2;
        return { status: 'ok', bytesWritten: 0 };
      case 6:
        this.state = 2;
        this.pending = null;
        return { status: 'ok', bytesWritten: 0 };
      default:
        return { status: 'stall' };
    }
  }

  _addrOf(b) {
    return (b[1] | (b[2] << 8) | (b[3] << 16) | (b[4] << 24)) >>> 0;
  }

  _execute({ block, bytes }) {
    this.state = 5; // dfuDNLOAD_IDLE
    if (block === 0) {
      if (bytes[0] === 0x21) this.addr = this._addrOf(bytes);
      else if (bytes[0] === 0x41 && bytes.length === 5) {
        const a = this._addrOf(bytes);
        const off = a - this.base;
        this.flash.fill(0xff, off, off + this.pageSize);
        this.erased.push(a);
      }
      return;
    }
    const a = this.addr + (block - 2) * this.transferSize;
    if (this.failWriteAt !== null && a === this.failWriteAt) {
      this.status = 3; // errWRITE
      this.state = 10;
      return;
    }
    const off = a - this.base;
    for (let i = 0; i < bytes.length; i++) {
      if (this.flash[off + i] !== 0xff) {
        this.status = 6; // errPROG: not erased
        this.state = 10;
        return;
      }
      this.flash[off + i] = bytes[i];
    }
    if (this.corruptAt !== null && this.corruptAt >= off && this.corruptAt < off + bytes.length) this.flash[this.corruptAt] ^= 1;
    this.writes.push({ addr: a, length: bytes.length });
  }
}
