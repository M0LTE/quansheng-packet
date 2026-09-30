// UV-K5 firmware images: the raw DP32G030 image plus the 16-byte version the bootloader is told.
// Packed images (fw-pack.py: version inserted at 0x2000, XOR-obfuscated, CRC appended) are what
// the releases publish. A port of host/dotnet FirmwareImage.

import { crc16, u16, u32 } from './k5frame.js';
import { K5SafetyError } from './k5link.js';

export const FLASH_LIMIT = 0xf000; // 60 KiB from 0; the bootloader lives above
export const BLOCK_SIZE = 0x100;
export const VERSION_OFFSET = 0x2000;
const RAM_START = 0x20000000;
const RAM_END = 0x20004000;

const FW_KEY = Uint8Array.from([
  0x47, 0x22, 0xc0, 0x52, 0x5d, 0x57, 0x48, 0x94, 0xb1, 0x60, 0x60, 0xdb, 0x6f, 0xe3, 0x4c, 0x7c,
  0xd8, 0x4a, 0xd6, 0x8b, 0x30, 0xec, 0x25, 0xe0, 0x4c, 0xd9, 0x00, 0x7f, 0xbf, 0xe3, 0x54, 0x05,
  0xe9, 0x3a, 0x97, 0x6b, 0xb0, 0x6e, 0x0c, 0xfb, 0xb1, 0x1a, 0xe2, 0xc9, 0xc1, 0x56, 0x47, 0xe9,
  0xba, 0xf1, 0x42, 0xb6, 0x67, 0x5f, 0x0f, 0x96, 0xf7, 0xc9, 0x3c, 0x84, 0x1b, 0x26, 0xe1, 0x4e,
  0x3b, 0x6f, 0x66, 0xe6, 0xa0, 0x6a, 0xb0, 0xbf, 0xc6, 0xa5, 0x70, 0x3a, 0xba, 0x18, 0x9e, 0x27,
  0x1a, 0x53, 0x5b, 0x71, 0xb1, 0x94, 0x1e, 0x18, 0xf2, 0xd6, 0x81, 0x02, 0x22, 0xfd, 0x5a, 0x28,
  0x91, 0xdb, 0xba, 0x5d, 0x64, 0xc6, 0xfe, 0x86, 0x83, 0x9c, 0x50, 0x1c, 0x73, 0x03, 0x11, 0xd6,
  0xaf, 0x30, 0xf4, 0x2c, 0x77, 0xb2, 0x7d, 0xbb, 0x3f, 0x29, 0x28, 0x57, 0x22, 0xd6, 0x92, 0x8b,
]);

export class InvalidImageError extends Error {
  constructor(m) {
    super(m);
    this.name = 'InvalidImageError';
  }
}

function fwXor(data) {
  for (let i = 0; i < data.length; i++) data[i] ^= FW_KEY[i & 127];
  return data;
}

export function versionBytes(version) {
  const v = new Uint8Array(16);
  for (let i = 0; i < Math.min(16, version.length); i++) v[i] = version.charCodeAt(i) & 0x7f;
  return v;
}

export class FirmwareImage {
  constructor(raw, version, wasPacked = false) {
    this.raw = raw;
    this.versionField = typeof version === 'string' ? versionBytes(version) : version;
    this.wasPacked = wasPacked;
  }

  /** For example "*PKTFW v1.0.1". A leading * is a wildcard for the bootloader's major version. */
  get version() {
    let end = this.versionField.indexOf(0);
    if (end < 0) end = 16;
    return String.fromCharCode(...this.versionField.subarray(0, end));
  }

  get blockCount() {
    return Math.ceil(this.raw.length / BLOCK_SIZE);
  }

  block(i) {
    return this.raw.subarray(i * BLOCK_SIZE, Math.min(this.raw.length, (i + 1) * BLOCK_SIZE));
  }

  /** Throws unless this looks like a UV-K5 image that stays below the bootloader. */
  validate() {
    checkVectorTable(this.raw);
    if (this.blockCount * BLOCK_SIZE > FLASH_LIMIT) {
      throw new K5SafetyError(
        `This image is ${this.raw.length} bytes, too big: it would write into the radio's bootloader.`,
      );
    }
  }
}

function packedCrcOk(data) {
  return data.length > 2 && crc16(data.subarray(0, data.length - 2)) === u16(data, data.length - 2);
}

export function pack(raw, version) {
  const ver = versionBytes(version);
  const split = Math.min(VERSION_OFFSET, raw.length);
  const plain = new Uint8Array(raw.length + 16);
  plain.set(raw.subarray(0, split));
  plain.set(ver, split);
  plain.set(raw.subarray(split), split + 16);
  fwXor(plain);
  const out = new Uint8Array(plain.length + 2);
  out.set(plain);
  const c = crc16(plain);
  out[plain.length] = c & 0xff;
  out[plain.length + 1] = c >> 8;
  return out;
}

export function unpack(packed) {
  if (!packedCrcOk(packed)) throw new InvalidImageError('packed image CRC mismatch');
  const plain = fwXor(packed.slice(0, packed.length - 2));
  if (plain.length < VERSION_OFFSET + 16) throw new InvalidImageError('packed image too short');
  const version = plain.slice(VERSION_OFFSET, VERSION_OFFSET + 16);
  const raw = new Uint8Array(plain.length - 16);
  raw.set(plain.subarray(0, VERSION_OFFSET));
  raw.set(plain.subarray(VERSION_OFFSET + 16), VERSION_OFFSET);
  return { raw, version };
}

function checkVectorTable(raw) {
  if (raw.length < 8) throw new InvalidImageError('This file is too short to be UV-K5 firmware.');
  const sp = u32(raw, 0);
  const reset = u32(raw, 4);
  if (!(sp > RAM_START && sp <= RAM_END)) {
    throw new InvalidImageError('This file does not look like UV-K5 firmware (or it is a packed file that is damaged).');
  }
  if ((reset & 1) === 0 || (reset & ~1) >>> 0 >= raw.length) {
    throw new InvalidImageError('This file does not look like UV-K5 firmware (bad reset vector).');
  }
}

/** Loads a packed or raw image. Raw images get the version "*" unless one is given. */
export function loadImage(file, versionOverride) {
  if (packedCrcOk(file)) {
    try {
      const { raw, version } = unpack(file);
      checkVectorTable(raw);
      return new FirmwareImage(raw, versionOverride ?? version, true);
    } catch (e) {
      if (!(e instanceof InvalidImageError)) throw e;
      // maybe a raw image whose tail happens to look like a CRC
    }
  }
  checkVectorTable(file);
  return new FirmwareImage(file.slice(), versionOverride ?? '*', false);
}
