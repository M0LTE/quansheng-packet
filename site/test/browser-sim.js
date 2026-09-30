// In-page fakes for navigator.serial, navigator.usb, navigator.hid and navigator.mediaDevices, for
// browser-e2e.js only: the radio (stock firmware for the backup, its bootloader, then the
// firmware answering protocol v2), the
// AIOC runtime and bootloader over WebUSB, the AIOC HID settings interface with the k5-red
// profile, and the AIOC's sound card input.
import { encodeLegacyReply, encodePayload, buildPayload, FrameDecoder, fromHex } from '/site/js/k5frame.js';
import { SimBootloader } from '/site/test/sim-bootloader.js';
import { SimLegacyRadio } from '/site/test/sim-legacy-radio.js';
import { FakeAiocRuntime, FakeStm32Bootloader } from '/site/test/fake-usb.js';

const vectors = (await (await fetch('/tests/vectors/protocol-v2.json')).json()).vectors;
const vbody = (name) => fromHex(vectors.find((v) => v.name === name).frames[0].body);

let push = null;
const toHost = (bytes) => push && push(bytes);
const log = (window.__simLog = []);

// ---- radio: stock firmware first, the bootloader after __intoFlashMode(), then the packet
// firmware after __powerCycle()
let mode = 'stock';
const stock = new SimLegacyRadio({ flavour: 'stock', version: 'k5_2.01.26' });
stock.link = { receive: (f) => toHost(f) };
window.__stock = stock;
let boot = null;
window.__intoFlashMode = () => {
  mode = 'boot';
  boot = new SimBootloader({ beaconIntervalMs: 50 });
  boot.link = { receive: (f) => toHost(f) };
  window.__boot = boot;
};
const params = new Map([[1, 144800000], [2, 0], [3, 0], [4, 0x856], [5, 0x756]]);
const dec = new FrameDecoder({ obfuscated: true, maxPayload: 248 });
let hb = null;
function reply(id, tag, rest) {
  toHost(encodePayload(buildPayload(id + 0x80, Uint8Array.from([tag, 0, 20, 0, ...rest])), true));
}
// Events: stored ones take the next seq, heartbeats carry it and the radio's state.
let seq = 0;
let txState = 0;
function event(n, payload, flags = 0, s = seq) {
  const b = new Uint8Array(7 + payload.length);
  new DataView(b.buffer).setUint16(0, s, true);
  new DataView(b.buffer).setUint32(2, performance.now() >>> 0, true);
  b[6] = flags;
  b.set(payload, 7);
  toHost(encodePayload(buildPayload(0x50c0 + n, b), true));
}
function stored(n, payload) {
  event(n, payload);
  seq = (seq + 1) & 0xffff;
}
function heartbeat() {
  const p = vbody('event_heartbeat').slice(7);
  p[3] = txState;
  if (txState === 2) p[2] |= 0x04; // PTT pressed
  event(6, p, 0x18);
}
// The radio's own PTT key: TX_START at 145.5 MHz, high power, then TX_END 1.5 s of RF later.
window.__sideKey = (down) => {
  const p = new Uint8Array(down ? 15 : 19);
  const dv = new DataView(p.buffer);
  if (down) {
    dv.setUint32(4, params.get(1), true);
    p[8] = params.get(2);
    p[9] = params.get(3);
    txState = 2;
    stored(2, p);
  } else {
    txState = 0;
    dv.setUint32(0, (performance.now() - 1500) >>> 0, true);
    stored(3, p);
  }
};
window.__refuse = (reason) => stored(4, [0, 0, 0, 0, reason, 0, 0]);
function firmware(bytes) {
  for (const f of dec.feed(bytes)) {
    log.push(f.id.toString(16));
    if (f.id === 0x0514) {
      const b = new Uint8Array(36);
      b.set(new TextEncoder().encode('PKTFW v1.0.1'));
      b.set([0x50, 0x4b, 0x54, 0x32, 0x00, 0x02], 20);
      toHost(encodeLegacyReply(buildPayload(0x0515, b), true));
      continue;
    }
    const tag = f.body[0];
    const rest = f.body.subarray(1);
    switch (f.id) {
      case 0x5000: {
        const r = vbody('get_info').slice(4);
        r.fill(0, 2, 18);
        r.set(new TextEncoder().encode('PKTFW v1.0.1'), 2);
        reply(f.id, tag, r);
        break;
      }
      case 0x5001: {
        const r = vbody('get_status').slice(4);
        new DataView(r.buffer).setUint32(4, params.get(1), true);
        reply(f.id, tag, r);
        break;
      }
      case 0x5004: {
        const out = [0];
        for (const id of rest.subarray(1)) {
          out.push(id);
          const v = params.get(id);
          const n = id === 1 ? 4 : id === 4 || id === 5 ? 2 : 1;
          for (let i = 0; i < n; i++) out.push(Math.floor(v / 2 ** (8 * i)) & 0xff);
        }
        reply(f.id, tag, out);
        break;
      }
      case 0x5005: {
        const rec = rest.subarray(1);
        for (let i = 0; i < rec.length; ) {
          const id = rec[i];
          const n = id === 1 ? 4 : 1;
          let v = 0;
          for (let k = 0; k < n; k++) v += rec[i + 1 + k] * 2 ** (8 * k);
          params.set(id, v);
          i += 1 + n;
        }
        reply(f.id, tag, [1 | (rest[0] & 1 ? 2 : 0) | 4, ...rec]);
        break;
      }
      case 0x5002: {
        reply(f.id, tag, [seq & 0xff, seq >> 8, seq & 0xff, seq >> 8, 0, 0, 0, 0]);
        clearInterval(hb);
        const mask = rest[0] | (rest[1] << 8);
        window.__liveTx = (rest[4] & 1) === 1;
        if (mask & (1 << 6)) hb = setInterval(heartbeat, 1000);
        break;
      }
      case 0x5007:
        reply(f.id, tag, [64, 0x54, 0x28]);
        break;
      default:
    }
  }
}
window.__powerCycle = () => {
  boot.stop();
  mode = 'fw';
  toHost(new TextEncoder().encode('UV-K5 packet firmware, PKTFW v1.0.1\r\n'));
};

const port = {
  readable: null,
  writable: null,
  async open(o) {
    window.__openedWith = o;
    this.readable = new ReadableStream({ start: (c) => (push = (b) => c.enqueue(b)) });
    this.writable = new WritableStream({ write: (b) => (mode === 'stock' ? stock.fromHost(b) : mode === 'boot' ? boot.fromHost(b) : firmware(b)) });
  },
  async setSignals(s) {
    window.__signals = s;
  },
  async close() {},
  getInfo: () => ({ usbVendorId: 0x1209, usbProductId: 0x7388 }),
};
Object.defineProperty(navigator, 'serial', { value: { requestPort: async () => port, addEventListener() {} } });

// ---- USB: the AIOC runtime, then the bootloader after DFU_DETACH
const runtime = new FakeAiocRuntime();
const stm = new FakeStm32Bootloader();
window.__stm = stm;
Object.defineProperty(navigator, 'usb', {
  value: {
    requestDevice: async ({ filters }) => (filters[0].vendorId === 0x1209 ? runtime : stm),
    getDevices: async () => [],
    addEventListener() {},
  },
});

// ---- HID: an AIOC with the k5-red profile
const regs = { 0x00: 0x434f4941, 0xbf: 0xbb801303, 0xc8: 0, 0xc9: 0x51455854 };
let cur = 0;
const hid = {
  opened: false,
  async open() { this.opened = true; },
  async close() { this.opened = false; },
  async sendFeatureReport(id, d) { if (d[0]) throw new Error('write attempted'); cur = d[1]; },
  async receiveFeatureReport() { const v = regs[cur] ?? 0; return new DataView(Uint8Array.of(0, cur, v & 255, (v >> 8) & 255, (v >> 16) & 255, v >>> 24).buffer); },
};
Object.defineProperty(navigator, 'hid', { value: { requestDevice: async () => [hid] } });
// ---- audio input: the AIOC's sound card, carrying a 1 kHz tone at a quarter of full scale
// (-12 dBFS peak). Its name is hidden until the page has asked for permission, as in a browser.
let micGranted = false;
const micDevices = [
  { kind: 'audioinput', deviceId: 'default', groupId: 'g1', label: 'Default - Built-in Microphone' },
  { kind: 'audioinput', deviceId: 'builtin', groupId: 'g1', label: 'Built-in Microphone' },
  { kind: 'audioinput', deviceId: 'aioc', groupId: 'g2', label: 'All-In-One-Cable Mono' },
];
window.__micOpened = [];
Object.defineProperty(navigator, 'mediaDevices', {
  value: {
    async enumerateDevices() {
      return micDevices.map((d) => ({ ...d, label: micGranted ? d.label : '' }));
    },
    async getUserMedia({ audio }) {
      micGranted = true;
      const id = audio.deviceId?.exact || 'builtin';
      window.__micOpened.push({ id, echoCancellation: audio.echoCancellation, noiseSuppression: audio.noiseSuppression, autoGainControl: audio.autoGainControl });
      const ac = new AudioContext({ sampleRate: 48000 });
      const osc = ac.createOscillator();
      osc.frequency.value = 1000;
      const gain = ac.createGain();
      gain.gain.value = id === 'aioc' ? 0.25 : 0.01;
      const dest = ac.createMediaStreamDestination();
      osc.connect(gain).connect(dest);
      osc.start();
      const [track] = dest.stream.getAudioTracks();
      Object.defineProperty(track, 'label', { value: micDevices.find((d) => d.deviceId === id).label });
      Object.defineProperty(track, 'getSettings', { value: () => ({ deviceId: id, sampleRate: 48000 }) });
      const stop = track.stop.bind(track);
      track.stop = () => { stop(); ac.close(); window.__micOpened.find((o) => o.id === id && !o.stopped).stopped = true; };
      return dest.stream;
    },
  },
});

window.__simReady = true;
