// Protocol v2 message bodies (docs/protocol-v2.md): pure encoding and decoding, no I/O.
// Only what the setup page needs: identification, GET_INFO, GET_STATUS, GET/SET_PARAMS,
// SUBSCRIBE, LEVEL_TONE and the events a status display uses.

import { u16, u32 } from './k5frame.js';

export const HELLO = 0x0514;
export const HELLO_REPLY = 0x0515;
export const PKT2_MAGIC = 0x32544b50; // "PKT2"

export const CMD = {
  GET_INFO: 0x5000,
  GET_STATUS: 0x5001,
  SUBSCRIBE: 0x5002,
  GET_PARAMS: 0x5004,
  SET_PARAMS: 0x5005,
  SAVE_PARAMS: 0x5006,
  LEVEL_TONE: 0x5007,
};
export const REPLY_OFFSET = 0x80;
export const isV2 = (id) => id >= 0x5000 && id <= 0x50ff;
export const isEvent = (id) => id >= 0x50c0 && id <= 0x50df;

export const STATUS = {
  0x00: ['OK', 'OK'],
  0x01: ['UNKNOWN_CMD', 'The radio does not know this command (older firmware?).'],
  0x02: ['BAD_LENGTH', 'The radio did not understand the request (wrong length).'],
  0x03: ['BAD_PARAM', 'The radio does not know one of the settings sent.'],
  0x04: ['RANGE', 'A value is out of range.'],
  0x05: ['TX_BAND', 'The radio will not transmit on that frequency.'],
  0x06: ['STATE', 'The radio cannot do that right now (is the battery nearly flat?).'],
  0x07: ['REFUSED', 'The radio refused.'],
  0x08: ['UNSUPPORTED', 'This radio does not support that.'],
  0x09: ['EEPROM', 'The radio could not save its settings (its settings area is not ready). Switch it off and on and try again.'],
  0x0a: ['NOT_PERSISTABLE', 'That setting cannot be saved.'],
};

export const PARAM = {
  FREQ_HZ: 0x01,
  POWER: 0x02,
  BANDWIDTH: 0x03,
  DEV_WIDE: 0x04,
  DEV_NARROW: 0x05,
  RX_GAIN: 0x08,
  RX_DAC_GAIN: 0x09,
  TX_TIMEOUT_S: 0x0a,
  PTT_PRESS_MS: 0x0b,
  PTT_RELEASE_MS: 0x0c,
  PA_ENABLE_DELAY_MS: 0x0d,
  PA_BIAS_DELAY_MS: 0x0e,
  SERIAL_LOCK_MS: 0x0f,
  BUSY_SOURCE: 0x10,
  BUSY_RSSI_OPEN: 0x11,
  BUSY_RSSI_CLOSE: 0x12,
  BUSY_HANG_MS: 0x13,
  BUSY_SQL_RAW: 0x14,
  AGC_FIX: 0x15,
  AFC: 0x16,
  BACKLIGHT: 0x17,
  KEY_LOCK: 0x18,
  BUSY_SQL_LEVEL: 0x19,
};

/** Value size in bytes on the wire; undefined for unknown or retired ids. */
export function paramSize(id) {
  if (id === 0x01) return 4;
  if ([0x04, 0x05, 0x0f, 0x11, 0x12].includes(id)) return 2;
  if (id === 0x14) return 6;
  if (id >= 0x02 && id <= 0x19 && id !== 0x06 && id !== 0x07) return 1;
  return undefined;
}

export const EVENT = {
  CD: 0,
  RX_BURST: 1,
  TX_START: 2,
  TX_END: 3,
  TX_REFUSED: 4,
  RSSI_STREAM: 5,
  HEARTBEAT: 6,
  BATTERY: 7,
  PARAMS_CHANGED: 8,
  EVENTS_LOST: 9,
  TONE_END: 10,
  OVERRIDE_EXPIRED: 11,
  BOOT: 12,
};

// ---------------------------------------------------------------- identification

export function helloBody(sessionId) {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, sessionId >>> 0, true);
  return b;
}

function cstr(b) {
  let end = b.indexOf(0);
  if (end < 0) end = b.length;
  return String.fromCharCode(...b.subarray(0, end));
}

/**
 * The 0x0515 hello reply body: version char[16], then the PKT2 marker at 20 on v2 firmware.
 * kind: 'v2' (packet firmware, protocol v2), 'v1' (packet firmware without v2), 'other'.
 */
export function parseHelloReply(body) {
  if (body.length < 20) throw new Error('short hello reply');
  const version = cstr(body.subarray(0, 16));
  const pkt2 = body.length >= 26 && u32(body, 20) === PKT2_MAGIC;
  const protocol = pkt2 ? u16(body, 24) : 0;
  const kind = pkt2 ? 'v2' : version.startsWith('PKTFW') ? 'v1' : 'other';
  return { version, pkt2, protocol, protocolText: pkt2 ? `${protocol >> 8}.${protocol & 0xff}` : '', kind };
}

// ---------------------------------------------------------------- reply header

export function parseReplyHeader(body) {
  if (body.length < 4) throw new Error('short v2 reply');
  const status = body[1];
  return {
    tag: body[0],
    status,
    statusName: (STATUS[status] || [`0x${status.toString(16)}`])[0],
    lockMs: u16(body, 2),
    rest: body.subarray(4),
    detail: status !== 0 && body.length > 4 ? body[4] : undefined,
  };
}

// ---------------------------------------------------------------- GET_INFO

export const TX_POLICY_FIXED = 8;

export function parseInfo(r) {
  return {
    protocol: u16(r, 0),
    version: cstr(r.subarray(2, 18)),
    caps: u32(r, 18),
    params: u32(r, 22),
    events: u32(r, 26),
    maxRequestBody: r[30],
    ringCapacity: r[31],
    serialLockMs: u16(r, 32),
    lateKeyMaxMs: r[34],
    txPolicy: r[35],
    settingsLayout: r[37],
    v2BlockLayout: r[38],
    burstPeriodMs: r[39],
    canLevelToneRaw: (u32(r, 18) & (1 << 2)) !== 0,
    canPersist: (u32(r, 18) & (1 << 9)) !== 0,
  };
}

// ---------------------------------------------------------------- GET_STATUS

export const STATE_NAMES = ['Receiving', 'Receiving, signal present', 'Transmitting', 'Receiving', 'Reduced service (battery)'];

export function parseStatus(r) {
  return {
    uptimeMs: u32(r, 0),
    frequencyHz: u32(r, 4),
    state: r[8],
    flags1: r[9],
    busy: (r[9] & 0x02) !== 0,
    pttPressed: (r[9] & 0x04) !== 0,
    lockActive: (r[9] & 0x08) !== 0,
    txAllowedHere: (r[9] & 0x10) !== 0,
    txLatched: (r[9] & 0x20) !== 0,
    toneRunning: (r[9] & 0x40) !== 0,
    flags2: r[10],
    liveDiffersFromStored: (r[10] & 0x01) !== 0,
    power: r[11],
    bandwidth: r[12],
    busyLevel: r[13],
    deviation: u16(r, 14),
    rssiRaw: u16(r, 16),
    noise: r[18],
    glitch: r[19],
    agc: r[20],
    batteryLevel: r[21],
    batteryMv: u16(r, 22),
    lockRemainingMs: u16(r, 24),
    txTimeLeft100ms: u16(r, 26),
    busyAgeMs: u16(r, 28),
    nextSeq: u16(r, 30),
    txTimeoutS: r[33],
  };
}

// ---------------------------------------------------------------- params

function putValue(out, id, value) {
  const n = paramSize(id);
  if (n === undefined) throw new Error(`unknown parameter 0x${id.toString(16)}`);
  if (n === 6) {
    if (!(value instanceof Uint8Array) || value.length !== 6) throw new Error('BUSY_SQL_RAW takes 6 bytes');
    out.push(...value);
    return;
  }
  for (let i = 0; i < n; i++) out.push(Math.floor(value / 2 ** (8 * i)) & 0xff);
}

/** GET_PARAMS body after the tag: flags (bit 0 STORED) then ids (none = all). */
export function encodeGetParams({ stored = false, ids = [] } = {}) {
  return Uint8Array.from([stored ? 1 : 0, ...ids]);
}

/** Parses (id, value) records; returns a Map id -> number (or Uint8Array for BUSY_SQL_RAW). */
export function parseRecords(b) {
  const values = new Map();
  let i = 0;
  while (i < b.length) {
    const id = b[i++];
    const n = paramSize(id);
    if (n === undefined || i + n > b.length) throw new Error(`cannot parse parameter 0x${id.toString(16)}`);
    values.set(id, n === 6 ? b.slice(i, i + 6) : n === 4 ? u32(b, i) : n === 2 ? u16(b, i) : b[i]);
    i += n;
  }
  return values;
}

export function parseGetParams(r) {
  return { flags: r[0], values: parseRecords(r.subarray(1)) };
}

export const SET_FLAGS = { PERSIST: 1, REQUIRE_TX_OK: 2, DRY_RUN: 4 };

/** SET_PARAMS body after the tag. records: [[id, value], ...] */
export function encodeSetParams(records, { persist = false, requireTxOk = false, dryRun = false } = {}) {
  const out = [(persist ? 1 : 0) | (requireTxOk ? 2 : 0) | (dryRun ? 4 : 0)];
  for (const [id, value] of records) {
    out.push(id);
    putValue(out, id, value);
  }
  return Uint8Array.from(out);
}

export function parseSetParams(r) {
  return {
    result: r[0],
    txAllowed: (r[0] & 1) !== 0,
    persistQueued: (r[0] & 2) !== 0,
    retuned: (r[0] & 4) !== 0,
    values: parseRecords(r.subarray(1)),
  };
}

// ---------------------------------------------------------------- SUBSCRIBE and LEVEL_TONE

export function encodeSubscribe({ mask = 0, liveTx = false, persist = false, heartbeatMs = 0, streamPeriodMs = 0, streamBatch = 0, burstPeriodMs = 0 } = {}) {
  const b = new Uint8Array(10);
  const dv = new DataView(b.buffer);
  dv.setUint32(0, mask >>> 0, true);
  b[4] = (liveTx ? 1 : 0) | (persist ? 2 : 0);
  dv.setUint16(5, heartbeatMs, true);
  b[7] = streamPeriodMs;
  b[8] = streamBatch;
  b[9] = burstPeriodMs;
  return b;
}

export function parseSubscribe(r) {
  return { nextSeq: u16(r, 0), oldestSeq: u16(r, 2), tMs: u32(r, 4) };
}

/** mode 0 deviation-equivalent (level in Hz), mode 1 raw gain code 0..127. durationMs 0 stops. */
export function encodeLevelTone({ hz = 1000, mode = 1, level = 64, durationMs = 10000 } = {}) {
  const b = new Uint8Array(7);
  const dv = new DataView(b.buffer);
  dv.setUint16(0, hz, true);
  b[2] = mode;
  dv.setUint16(3, level, true);
  dv.setUint16(5, durationMs, true);
  return b;
}

export function parseLevelTone(r) {
  return { gainCode: r[0], word: u16(r, 1) };
}

// ---------------------------------------------------------------- events

export function parseEvent(id, body) {
  const n = id - 0x50c0;
  const head = { event: n, seq: u16(body, 0), tMs: u32(body, 2), flags: body[6] };
  const e = body.subarray(7);
  switch (n) {
    case EVENT.CD:
      return { ...head, type: 'cd', busy: e[0] === 1, rssiRaw: u16(e, 3) };
    case EVENT.TX_START:
      return { ...head, type: 'tx_start', frequencyHz: u32(e, 4), power: e[8], lockDelayMs: u16(e, 12), lateKey: (e[14] & 2) !== 0 };
    case EVENT.TX_END:
      return { ...head, type: 'tx_end', reason: e[12] };
    case EVENT.TX_REFUSED:
      return { ...head, type: 'tx_refused', reason: e[4], detail: u16(e, 5) };
    case EVENT.HEARTBEAT:
      return { ...head, type: 'heartbeat', flags1: e[2], state: e[3], rssiRaw: u16(e, 4), batteryMv: u16(e, 6), lockMs: u16(e, 10) };
    case EVENT.BATTERY:
      return { ...head, type: 'battery', batteryClass: e[0], level: e[1], mv: u16(e, 2) };
    case EVENT.PARAMS_CHANGED:
      return { ...head, type: 'params_changed', source: e[0], mask: u32(e, 1) };
    case EVENT.TONE_END:
      return { ...head, type: 'tone_end', reason: e[0] };
    case EVENT.BOOT:
      return { ...head, type: 'boot', protocol: u16(e, 0) };
    default:
      return { ...head, type: 'other' };
  }
}

export const TX_REFUSED_REASONS = {
  1: 'the serial lock was running (the page had just sent something)',
  2: 'the frequency is outside the transmit bands',
  3: 'the battery is flat',
  4: 'the supply voltage is too high',
  5: 'the battery is nearly flat',
  6: 'the radio could not key in time',
};

// ---------------------------------------------------------------- frequency and units

/** Transmit is allowed from 136 up to 174 MHz and from 400 up to 470 MHz (upper edges excluded). */
export const TX_BANDS = [
  [136_000_000, 174_000_000],
  [400_000_000, 470_000_000],
];

export function txAllowed(hz) {
  return TX_BANDS.some(([lo, hi]) => hz >= lo && hz < hi);
}

/**
 * Parses a frequency typed in MHz ("144.800", "144,8", "433.5 MHz"). Returns { hz } or
 * { error } with a message for the user. Refuses what the radio would refuse with
 * REQUIRE_TX_OK: anything outside the transmit bands or not a multiple of 10 Hz.
 */
export function parseFrequencyMHz(text) {
  const t = String(text).trim().toLowerCase().replace(/mhz$/, '').trim().replace(',', '.');
  if (!/^\d{1,3}(\.\d*)?$/.test(t)) return { error: 'Type the frequency in MHz, for example 144.800.' };
  const [whole, frac = ''] = t.split('.');
  if (frac.replace(/0+$/, '').length > 5) return { error: 'The radio tunes in 10 Hz steps: use at most five decimal places.' };
  const hz = Number(whole) * 1_000_000 + Number(frac.slice(0, 6).padEnd(6, '0'));
  if (!txAllowed(hz)) {
    return { error: 'The radio transmits only from 136 to 174 MHz and from 400 to 470 MHz. Pick a frequency in one of those.' };
  }
  return { hz };
}

/** 144800000 -> "144.800", 144812500 -> "144.8125", 433500010 -> "433.50001" */
export function formatMHz(hz) {
  const s = (hz / 1e6).toFixed(5);
  return s.replace(/0{1,2}$/, '');
}

/** The chip's own reading, uncorrected: dBm = raw / 2 - 160 (0.5 dB steps). */
export function rssiDbm(raw) {
  return raw / 2 - 160;
}

/**
 * The radio's screen corrects the chip's reading per band (ui/main.c dBmCorrTable), with the
 * bands of frequencies.c: a frequency belongs to the highest band whose lower edge it reaches,
 * and anything below 108 MHz to the first. Upstream's empirical table, not yet checked against
 * a calibrated signal generator.
 */
export const RSSI_BANDS = [
  { fromHz: 50_000_000, correctionDb: -15 },
  { fromHz: 108_000_000, correctionDb: -25 },
  { fromHz: 137_000_000, correctionDb: -20 },
  { fromHz: 174_000_000, correctionDb: -4 },
  { fromHz: 350_000_000, correctionDb: -7 },
  { fromHz: 400_000_000, correctionDb: -6 },
  { fromHz: 470_000_000, correctionDb: -1 },
];

/** The screen's correction in dB for a receive frequency in Hz. */
export function rssiCorrectionDb(hz) {
  for (let i = RSSI_BANDS.length - 1; i > 0; i--) if (hz >= RSSI_BANDS[i].fromHz) return RSSI_BANDS[i].correctionDb;
  return RSSI_BANDS[0].correctionDb;
}

/** The whole-dB level the radio's screen shows for a raw reading at a frequency: raw / 2 rounded down, - 160, plus the band's correction. */
export function signalDbm(raw, hz) {
  return Math.floor(raw / 2) - 160 + rssiCorrectionDb(hz);
}

/** The radio's S-meter (ui/main.c): S0 at -130 dBm, S9 at -76 dBm, 6 dB per S-unit; from S9+10 it shows dB over S9. */
export const S0_DBM = -130;
export const S9_DBM = -76;

export function sPoint(dbm) {
  const d = Math.floor(dbm);
  const over = Math.min(Math.max(d - S9_DBM, 0), 99);
  if (over >= 10) return `S9+${over}`;
  return `S${Math.min(Math.max(Math.floor((d - S0_DBM) / ((S9_DBM - S0_DBM) / 9)), 0), 9)}`;
}

/**
 * Approximate peak deviation at full-scale audio through the packet AIOC firmware for a
 * deviation register value: 0x856 is about 2.8 kHz, and 256 higher doubles it (README).
 */
export function deviationKhz(reg) {
  return 2.8 * 2 ** ((reg - 0x856) / 256);
}

export const POWER_NAMES = ['low (~0.5 W)', 'mid (~2 W)', 'high (~5 W)'];
export const BANDWIDTH_NAMES = ['wide (25 kHz)', 'narrow (12.5 kHz)'];
