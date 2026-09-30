// The receive level meter: its level maths, and how it finds the AIOC's input.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { measureLevels, levelVerdict, barFraction, toDbfs, findAiocInput, audioConstraints, LevelMeter, FLOOR_DBFS } from '../js/level-meter.js';

const near = (a, b, tol = 0.05) => assert.ok(Math.abs(a - b) <= tol, `${a} is not within ${tol} of ${b}`);

function sine(amplitude, n = 4800, hz = 1000, rate = 48000) {
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) out[i] = amplitude * Math.sin((2 * Math.PI * hz * i) / rate);
  return out;
}

test('a full-scale sine: 0 dBFS peak, -3 dBFS RMS, clipping', () => {
  const l = measureLevels(sine(1));
  near(l.peakDbfs, 0);
  near(l.rmsDbfs, -3.01);
  assert.equal(l.clipped, true);
});

test('a sine at a quarter of full scale: -12 dBFS peak, -15 RMS, no clipping', () => {
  const l = measureLevels(sine(0.25));
  near(l.peakDbfs, -12.04);
  near(l.rmsDbfs, -15.05);
  assert.equal(l.clipped, false);
});

test('a square wave has equal peak and RMS; the sign does not matter', () => {
  const sq = Float32Array.from({ length: 480 }, (_, i) => (i % 2 ? -0.5 : 0.5));
  const l = measureLevels(sq);
  near(l.peakDbfs, -6.02);
  near(l.rmsDbfs, -6.02);
  assert.equal(measureLevels(Float32Array.of(0, -0.9995, 0)).clipped, true);
});

test('silence and nothing read at the floor, not -Infinity', () => {
  assert.deepEqual(measureLevels(new Float32Array(100)), { peakDbfs: FLOOR_DBFS, rmsDbfs: FLOOR_DBFS, clipped: false });
  assert.deepEqual(measureLevels(new Float32Array(0)), { peakDbfs: FLOOR_DBFS, rmsDbfs: FLOOR_DBFS, clipped: false });
  assert.equal(toDbfs(1e-9), FLOOR_DBFS);
  near(toDbfs(0.5), -6.02);
});

test('the verdict: green band -18 to -9 dBFS on peaks, clipping first', () => {
  assert.equal(levelVerdict({ peakDbfs: -30, clipped: false }), 'low');
  assert.equal(levelVerdict({ peakDbfs: -18.01, clipped: false }), 'low');
  assert.equal(levelVerdict({ peakDbfs: -18, clipped: false }), 'ok');
  assert.equal(levelVerdict({ peakDbfs: -9, clipped: false }), 'ok');
  assert.equal(levelVerdict({ peakDbfs: -8.99, clipped: false }), 'high');
  assert.equal(levelVerdict({ peakDbfs: -12, clipped: true }), 'clip');
  assert.equal(levelVerdict(measureLevels(sine(0.25))), 'ok');
});

test('the bar runs from -60 to 0 dBFS', () => {
  assert.equal(barFraction(-120), 0);
  assert.equal(barFraction(-60), 0);
  assert.equal(barFraction(-18), 0.7);
  assert.equal(barFraction(-9), 0.85);
  assert.equal(barFraction(0), 1);
  assert.equal(barFraction(3), 1);
});

test('finding the AIOC by name, preferring the real entry over Chrome\'s "default" copy', () => {
  const devs = [
    { kind: 'audioinput', deviceId: 'default', label: 'Default - Microphone (All-In-One-Cable)' },
    { kind: 'audioinput', deviceId: 'mic', label: 'Microphone (Realtek Audio)' },
    { kind: 'audiooutput', deviceId: 'spk', label: 'Speakers (All-In-One-Cable)' },
    { kind: 'audioinput', deviceId: 'aioc', label: 'Microphone (All-In-One-Cable)' },
  ];
  assert.equal(findAiocInput(devs).deviceId, 'aioc');
  assert.equal(findAiocInput(devs.slice(0, 3)).deviceId, 'default');
  assert.equal(findAiocInput([{ kind: 'audioinput', deviceId: 'x', label: 'AIOC Mono' }]).deviceId, 'x');
  assert.equal(findAiocInput([{ kind: 'audioinput', deviceId: 'x', label: '' }]), null);
  assert.equal(findAiocInput([{ kind: 'audioinput', deviceId: 'x', label: 'Radioconnect' }]), null);
});

test('the input is opened with no processing, at 48 kHz if possible', () => {
  const c = audioConstraints('aioc');
  assert.equal(c.echoCancellation, false);
  assert.equal(c.noiseSuppression, false);
  assert.equal(c.autoGainControl, false);
  assert.deepEqual(c.sampleRate, { ideal: 48000 });
  assert.deepEqual(c.deviceId, { exact: 'aioc' });
  assert.equal('deviceId' in audioConstraints(null), false);
});

// ---- LevelMeter with fake media devices and a fake AudioContext

function fakeMedia({ aioc = true } = {}) {
  let granted = false;
  const opened = [];
  const devices = [
    { kind: 'audioinput', deviceId: 'default', label: 'Default - Microphone (Realtek Audio)' },
    { kind: 'audioinput', deviceId: 'mic', label: 'Microphone (Realtek Audio)' },
    ...(aioc ? [{ kind: 'audioinput', deviceId: 'aioc', label: 'Microphone (All-In-One-Cable)' }] : []),
  ];
  const labelOf = (id) => devices.find((d) => d.deviceId === id)?.label;
  return {
    opened,
    async enumerateDevices() {
      return devices.map((d) => ({ ...d, label: granted ? d.label : '' }));
    },
    async getUserMedia({ audio }) {
      granted = true;
      const id = audio.deviceId?.exact || 'mic';
      const track = { stopped: false, label: labelOf(id), getSettings: () => ({ deviceId: id, sampleRate: 48000 }), stop() { this.stopped = true; } };
      opened.push(track);
      return { getAudioTracks: () => [track], getTracks: () => [track] };
    },
  };
}

class FakeAudioContext {
  constructor({ sampleRate = 44100 } = {}) {
    this.sampleRate = sampleRate;
    this.closed = false;
  }
  async resume() {}
  async close() { this.closed = true; }
  createAnalyser() {
    return { fftSize: 2048, getFloatTimeDomainData: (buf) => buf.set(sine(0.25, buf.length)) };
  }
  createMediaStreamSource() {
    return { connect() {} };
  }
}

test('LevelMeter finds the AIOC once permission makes the names readable, and reports levels', async () => {
  const media = fakeMedia();
  const levels = [];
  const m = new LevelMeter({ media, AudioCtx: FakeAudioContext, intervalMs: 5, onLevel: (l) => levels.push(l) });
  const r = await m.start();
  assert.equal(r.isAioc, true);
  assert.equal(r.deviceId, 'aioc');
  assert.equal(r.sampleRate, 48000);
  assert.equal(media.opened.length, 2, 'first the default input for permission, then the AIOC');
  assert.equal(media.opened[0].stopped, true);
  assert.equal(m.ctx.sampleRate, 48000);
  await new Promise((res) => setTimeout(res, 30));
  assert.ok(levels.length > 0);
  near(levels[0].peakDbfs, -12.04);
  m.stop();
  assert.equal(media.opened[1].stopped, true);
  assert.equal(m.running, false);
  // a second start already has permission, so it goes straight to the AIOC
  await m.start();
  assert.equal(media.opened.length, 3);
  assert.equal(media.opened[2].label, 'Microphone (All-In-One-Cable)');
  m.stop();
});

test('LevelMeter without an AIOC says so, so the page can offer a picker', async () => {
  const media = fakeMedia({ aioc: false });
  const m = new LevelMeter({ media, AudioCtx: FakeAudioContext, onLevel() {} });
  const r = await m.start();
  assert.equal(r.isAioc, false);
  assert.deepEqual((await m.inputs()).map((d) => d.deviceId), ['default', 'mic']);
  m.stop();
});

test('LevelMeter stopped while opening lets the device go', async () => {
  const media = fakeMedia();
  const m = new LevelMeter({ media, AudioCtx: FakeAudioContext, onLevel() {} });
  const p = m.start();
  m.stop();
  assert.equal(await p, null);
  assert.ok(media.opened.every((t) => t.stopped));
  assert.equal(m.running, false);
});
