// UV-K5 serial framing: CRC, obfuscation, encoding and a resynchronising decoder.
// A port of host/dotnet K5FrameCodec and K5FrameDecoder (docs/protocol-v2.md section 2).
//
//   AB CD | len u16 | payload (len bytes) + crc16 u16 | DC BA
//   payload = id u16 | body_len u16 | body
//
// crc16 is CRC-16/XMODEM over the payload. In obfuscated mode payload and CRC are XORed with a
// 16-byte key cycling from payload byte 0. Legacy replies carry 0xFFFF (obfuscated) instead of a
// CRC; the bootloader sends a raw FF FF there, not obfuscated.

export const OBFUSCATION_KEY = Uint8Array.from([
  0x16, 0x6c, 0x14, 0xe6, 0x2e, 0x91, 0x0d, 0x40, 0x21, 0x35, 0xd5, 0x40, 0x13, 0x03, 0xe9, 0x80,
]);

/** CRC-16/XMODEM (poly 0x1021, init 0). Check value for "123456789" is 0x31C3. */
export function crc16(data, crc = 0) {
  for (let i = 0; i < data.length; i++) {
    crc ^= data[i] << 8;
    for (let b = 0; b < 8; b++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

/** XORs data in place with the obfuscation key, starting at key index 0. */
export function obfuscate(data) {
  for (let i = 0; i < data.length; i++) data[i] ^= OBFUSCATION_KEY[i & 15];
  return data;
}

/** id u16 | body_len u16 | body */
export function buildPayload(id, body = new Uint8Array(0)) {
  const p = new Uint8Array(4 + body.length);
  p[0] = id & 0xff;
  p[1] = id >> 8;
  p[2] = body.length & 0xff;
  p[3] = body.length >> 8;
  p.set(body, 4);
  return p;
}

function wrap(payload, obfuscated, crc, rawCrc = false) {
  const n = payload.length;
  const f = new Uint8Array(n + 8);
  f[0] = 0xab;
  f[1] = 0xcd;
  f[2] = n & 0xff;
  f[3] = n >> 8;
  f.set(payload, 4);
  f[4 + n] = crc & 0xff;
  f[5 + n] = crc >> 8;
  if (obfuscated) obfuscate(f.subarray(4, rawCrc ? 4 + n : 6 + n));
  f[6 + n] = 0xdc;
  f[7 + n] = 0xba;
  return f;
}

/** A whole frame around a payload, with a real CRC. */
export function encodePayload(payload, obfuscated) {
  return wrap(payload, obfuscated, crc16(payload));
}

/** A whole frame for message id, with a real CRC (what a host sends). */
export function encodeFrame(id, body, obfuscated) {
  return encodePayload(buildPayload(id, body), obfuscated);
}

/** A legacy firmware reply: CRC field 0xFFFF, obfuscated with the payload. For simulators. */
export function encodeLegacyReply(payload, obfuscated) {
  return wrap(payload, obfuscated, 0xffff);
}

/** A bootloader frame: obfuscated payload, raw FF FF in the CRC field. For simulators. */
export function encodeBootloaderFrame(payload) {
  return wrap(payload, true, 0xffff, true);
}

export const u16 = (b, o) => b[o] | (b[o + 1] << 8);
export const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;

/**
 * Incremental decoder. Resynchronises on AB CD after garbage, truncated frames and unframed text
 * such as the power-on banner (docs/protocol-v2.md 2.3). Frames are
 * { id, payload, body, crc: 'valid' | 'absent' | 'bad', declaredBodyLength }.
 */
export class FrameDecoder {
  constructor({ obfuscated = true, maxPayload = 250 } = {}) {
    this.obfuscated = obfuscated;
    this.maxPayload = maxPayload;
    this.buf = new Uint8Array(1024);
    this.start = 0;
    this.count = 0;
    this.discardedBytes = 0;
    this.badCrcFrames = 0;
  }

  get hasPartialFrame() {
    return this.count > 0;
  }

  /** Feeds bytes; returns the frames they complete. Discarded bytes go to onDiscard(bytes). */
  feed(data, onDiscard) {
    this._append(data);
    const frames = [];
    while (this.count > 0) {
      const b = this.buf.subarray(this.start, this.start + this.count);
      const i = b.indexOf(0xab);
      if (i < 0) {
        this._discard(this.count, onDiscard);
        break;
      }
      if (i > 0) {
        this._discard(i, onDiscard);
        continue;
      }
      if (b.length < 2) break;
      if (b[1] !== 0xcd) {
        this._discard(1, onDiscard);
        continue;
      }
      if (b.length < 4) break;
      const n = u16(b, 2);
      if (n > this.maxPayload || n < 4) {
        this._discard(1, onDiscard);
        continue;
      }
      const total = n + 8;
      if (b.length < total) {
        // A frame cut short (the AIOC drops radio bytes while PTT is held) must not swallow a
        // complete good frame that follows it.
        const nested = this._findNested(b);
        if (nested > 0) {
          this._discard(nested, onDiscard);
          continue;
        }
        break;
      }
      if (b[total - 2] !== 0xdc || b[total - 1] !== 0xba) {
        this._discard(1, onDiscard);
        continue;
      }
      const f = this._decodeAt(b, n);
      if (f.crc === 'bad') this.badCrcFrames++;
      frames.push(f);
      this.start += total;
      this.count -= total;
    }
    if (this.count === 0) this.start = 0;
    return frames;
  }

  /** Gives up on a buffered partial frame (call after a quiet gap) and rescans what follows. */
  dropPartialFrame(onDiscard) {
    if (this.count > 0) this._discard(1, onDiscard);
    return this.feed(new Uint8Array(0), onDiscard);
  }

  reset() {
    this.start = 0;
    this.count = 0;
  }

  _decodeAt(b, n) {
    const inner = b.slice(4, 6 + n);
    const rawNone = inner[n] === 0xff && inner[n + 1] === 0xff;
    if (this.obfuscated) obfuscate(inner);
    const crc = u16(inner, n);
    const payload = inner.subarray(0, n);
    let status;
    if (crc === crc16(payload)) status = 'valid';
    else if (rawNone || crc === 0xffff) status = 'absent';
    else status = 'bad';
    return {
      id: u16(payload, 0),
      payload,
      body: payload.subarray(4),
      declaredBodyLength: u16(payload, 2),
      crc: status,
    };
  }

  _findNested(b) {
    for (let j = 1; j + 8 <= b.length; j++) {
      if (b[j] !== 0xab || b[j + 1] !== 0xcd) continue;
      const n = u16(b, j + 2);
      if (n > this.maxPayload || n < 4 || j + n + 8 > b.length) continue;
      if (b[j + n + 6] !== 0xdc || b[j + n + 7] !== 0xba) continue;
      if (this._decodeAt(b.subarray(j), n).crc !== 'bad') return j;
    }
    return -1;
  }

  _discard(n, onDiscard) {
    if (onDiscard) onDiscard(this.buf.slice(this.start, this.start + n));
    this.discardedBytes += n;
    this.start += n;
    this.count -= n;
  }

  _append(data) {
    if (this.start + this.count + data.length > this.buf.length) {
      if (this.count + data.length <= this.buf.length) {
        this.buf.copyWithin(0, this.start, this.start + this.count);
      } else {
        const bigger = new Uint8Array(Math.max(this.buf.length * 2, this.count + data.length));
        bigger.set(this.buf.subarray(this.start, this.start + this.count));
        this.buf = bigger;
      }
      this.start = 0;
    }
    this.buf.set(data, this.start + this.count);
    this.count += data.length;
  }
}

export function hex(bytes) {
  return Array.from(bytes, (x) => x.toString(16).padStart(2, '0')).join('');
}

export function fromHex(s) {
  const clean = s.replace(/\s+/g, '');
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(clean.substr(i * 2, 2), 16);
  return out;
}
