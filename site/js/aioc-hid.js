// Reading the AIOC's settings registers over WebHID, read-only: the feature report
// [control, address, d0, d1, d2, d3] with control 0 selects a register without writing it,
// and the next GET_FEATURE returns [0, address, value little-endian]. This page never sets
// the write (0x01), store (0x80), defaults (0x10), recall (0x40) or reboot (0x20) bits.

export const AIOC_HID = { vendorId: 0x1209, productId: 0x7388 };
export const REG = { MAGIC: 0x00, TXEQ_CTRL: 0xbf, INFO_TXEQ: 0xc8, INFO_TXEQPAGE: 0xc9 };
export const MAGIC_AIOC = 0x434f4941; // "AIOC"
export const MARKER_TXEQ = 0x51455854; // "TXEQ"
export const K5_RED_CTRL = 0xbb801303; // the k5-red profile: FS 48000, GEN 0x13, 3 sections

/**
 * Parses a feature report read back from the AIOC. Browsers differ on whether the report id
 * byte comes first, so both the 6-byte and the 7-byte forms are accepted.
 */
export function parseFeatureReport(bytes, addr) {
  let off = -1;
  if (bytes.length >= 7 && bytes[0] === 0 && bytes[2] === addr) off = 1;
  else if (bytes.length >= 6 && bytes[1] === addr) off = 0;
  if (off < 0) throw new Error(`unexpected reply from the AIOC reading 0x${addr.toString(16)}`);
  const b = bytes.subarray(off);
  return (b[2] | (b[3] << 8) | (b[4] << 16) | (b[5] << 24)) >>> 0;
}

export async function readRegister(device, addr) {
  if (!(addr >= 0 && addr <= 0xff)) throw new Error('bad register');
  await device.sendFeatureReport(0, Uint8Array.of(0x00, addr, 0, 0, 0, 0));
  const dv = await device.receiveFeatureReport(0);
  return parseFeatureReport(new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength), addr);
}

/** Reads the EQ registers and says in plain words what they mean. */
export async function readEqState(device) {
  const magic = await readRegister(device, REG.MAGIC);
  const ctrl = await readRegister(device, REG.TXEQ_CTRL);
  const info = await readRegister(device, REG.INFO_TXEQ);
  const marker = await readRegister(device, REG.INFO_TXEQPAGE);
  return describeEq({ magic, ctrl, info, marker });
}

export function describeEq({ magic, ctrl, info, marker }) {
  const sections = ctrl & 3;
  const fs = ctrl >>> 16;
  const running = (info & 4) !== 0;
  const clips = info >>> 16;
  const isAioc = magic === MAGIC_AIOC;
  const packetFirmware = marker === MARKER_TXEQ || ctrl !== 0;
  let summary;
  let level;
  if (!isAioc) {
    summary = 'This does not answer like an AIOC.';
    level = 'error';
  } else if (sections === 0) {
    summary = packetFirmware
      ? 'The transmit equaliser is off. Flash the recommended image in step 2 to switch it on (it resets the AIOC settings).'
      : 'The transmit equaliser is off: this AIOC does not seem to run the packet AIOC firmware. Flash it in step 2.';
    level = 'warn';
  } else if (ctrl === K5_RED_CTRL) {
    summary = 'The transmit equaliser is on, with the UV-K5 profile (k5-red). All good.';
    level = 'ok';
  } else {
    summary = `The transmit equaliser is on with a custom set (${sections} sections${fs ? `, for ${fs} Hz audio` : ''}).`;
    level = 'ok';
  }
  let detail = '';
  if (sections > 0) {
    detail = running
      ? `It is running now${clips ? `, and has clipped ${clips} samples: turn the transmit audio down a little` : ', with no clipping'}.`
      : 'It only runs while your computer plays audio to the AIOC at 48000 Hz, so it shows as idle now.';
  }
  return { isAioc, packetFirmware, on: sections > 0, k5Red: ctrl === K5_RED_CTRL, running, clips, fs, sections, summary, detail, level };
}
