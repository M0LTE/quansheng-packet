// The receive level meter: the AIOC's sound card input, read with the Web Audio API, as peak and
// RMS in dBFS. The maths is pure (and tested in node); LevelMeter is the browser glue.
//
// The target band is pdn-soundmodem's receive target on peaks, -18 to -9 dBFS: there most modes
// lose nothing, with room above for a louder station before the converter clips.

export const FLOOR_DBFS = -120;
export const TARGET_LOW_DBFS = -18;
export const TARGET_HIGH_DBFS = -9;
export const SCALE_MIN_DBFS = -60;
/** A float sample at or above this (1.0 is the converter's full scale) counts as clipping. */
export const CLIP_LEVEL = 0.999;

/** An amplitude (1.0 = full scale) in dBFS, never below FLOOR_DBFS. */
export function toDbfs(amplitude) {
  return amplitude > 0 ? Math.max(FLOOR_DBFS, 20 * Math.log10(amplitude)) : FLOOR_DBFS;
}

/** Peak and RMS of float samples in dBFS (a full-scale sine is 0 peak, -3 RMS), and whether any sample clipped. */
export function measureLevels(samples) {
  let peak = 0;
  let sum = 0;
  for (let i = 0; i < samples.length; i++) {
    const s = samples[i];
    const a = Math.abs(s);
    if (a > peak) peak = a;
    sum += s * s;
  }
  const rms = samples.length ? Math.sqrt(sum / samples.length) : 0;
  return { peakDbfs: toDbfs(peak), rmsDbfs: toDbfs(rms), clipped: peak >= CLIP_LEVEL };
}

/** 'clip', 'high' (above the green band), 'ok' (in it) or 'low' (below it), judged on the peak. */
export function levelVerdict({ peakDbfs, clipped }) {
  if (clipped) return 'clip';
  if (peakDbfs > TARGET_HIGH_DBFS) return 'high';
  if (peakDbfs >= TARGET_LOW_DBFS) return 'ok';
  return 'low';
}

/** Where a level sits along a bar running from SCALE_MIN_DBFS to 0 dBFS, 0 to 1. */
export function barFraction(dbfs) {
  return Math.min(1, Math.max(0, (dbfs - SCALE_MIN_DBFS) / -SCALE_MIN_DBFS));
}

const AIOC_LABEL = /all-in-one-cable|\baioc\b/i;

/**
 * The AIOC's input among enumerateDevices() results, by label (labels are empty until the page
 * has permission). Chrome also lists the default device again as "default" or "communications";
 * the real entry is preferred.
 */
export function findAiocInput(devices) {
  const aioc = devices.filter((d) => d.kind === 'audioinput' && AIOC_LABEL.test(d.label || ''));
  return aioc.find((d) => d.deviceId !== 'default' && d.deviceId !== 'communications') || aioc[0] || null;
}

/** Input with no processing: the browser's echo cancelling, noise suppression and gain control would all change the level. */
export function audioConstraints(deviceId) {
  return {
    echoCancellation: false,
    noiseSuppression: false,
    autoGainControl: false,
    sampleRate: { ideal: 48000 },
    channelCount: { ideal: 1 },
    ...(deviceId ? { deviceId: { exact: deviceId } } : {}),
  };
}

/** Reads a sound card input and reports its level every intervalMs (windows of about 100 ms, overlapping a little). */
export class LevelMeter {
  constructor({ onLevel, onEnded = null, intervalMs = 80, media = globalThis.navigator?.mediaDevices, AudioCtx = globalThis.AudioContext } = {}) {
    this.onLevel = onLevel;
    this.onEnded = onEnded;
    this.intervalMs = intervalMs;
    this.media = media;
    this.AudioCtx = AudioCtx;
    this.stream = null;
    this.ctx = null;
    this.timer = null;
    this.gen = 0; // bumped by stop(), so a start still opening the device gives up
  }

  static supported(media = globalThis.navigator?.mediaDevices) {
    return !!media?.getUserMedia && !!globalThis.AudioContext;
  }

  get running() {
    return !!this.stream;
  }

  /**
   * Starts on deviceId, or on the AIOC if it can be found by name, else on the browser's default
   * input. Returns { isAioc, label, deviceId, sampleRate }; isAioc is false when the input in use
   * is not named as an AIOC (then offer a picker). Returns null if stop() was called meanwhile.
   */
  async start(deviceId = null) {
    this.stop();
    const gen = this.gen;
    const release = (st) => st.getTracks().forEach((t) => t.stop());
    let id = deviceId || findAiocInput(await this.media.enumerateDevices())?.deviceId || null;
    let stream = await this.media.getUserMedia({ audio: audioConstraints(id) });
    if (!id && gen === this.gen) {
      // The first permission prompt is what makes the names readable: look again.
      const aioc = findAiocInput(await this.media.enumerateDevices());
      if (aioc) {
        id = aioc.deviceId;
        if (stream.getAudioTracks()[0]?.getSettings?.().deviceId !== id) {
          release(stream);
          stream = await this.media.getUserMedia({ audio: audioConstraints(id) });
        }
      }
    }
    if (gen !== this.gen) {
      release(stream);
      return null;
    }
    const track = stream.getAudioTracks()[0];
    const settings = track?.getSettings?.() || {};
    let ctx;
    try {
      ctx = new this.AudioCtx({ sampleRate: settings.sampleRate || 48000 });
    } catch {
      ctx = new this.AudioCtx();
    }
    await ctx.resume();
    if (gen !== this.gen) {
      release(stream);
      ctx.close().catch(() => {});
      return null;
    }
    const analyser = ctx.createAnalyser();
    analyser.fftSize = 2 ** Math.round(Math.log2(ctx.sampleRate * 0.1)); // about 100 ms
    ctx.createMediaStreamSource(stream).connect(analyser);
    const buf = new Float32Array(analyser.fftSize);
    this.stream = stream;
    this.ctx = ctx;
    this.timer = setInterval(() => {
      analyser.getFloatTimeDomainData(buf);
      this.onLevel?.(measureLevels(buf));
    }, this.intervalMs);
    if (track) {
      track.onended = () => {
        this.stop();
        this.onEnded?.();
      };
    }
    const label = track?.label || '';
    return { isAioc: AIOC_LABEL.test(label), label, deviceId: settings.deviceId || id, sampleRate: ctx.sampleRate };
  }

  stop() {
    this.gen++;
    clearInterval(this.timer);
    this.timer = null;
    this.stream?.getTracks().forEach((t) => t.stop());
    this.stream = null;
    this.ctx?.close().catch(() => {});
    this.ctx = null;
  }

  /** The audio inputs, for a picker when the AIOC could not be found by name (needs permission first). */
  async inputs() {
    return (await this.media.enumerateDevices()).filter((d) => d.kind === 'audioinput' && d.deviceId !== 'communications');
  }
}
