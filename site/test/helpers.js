// Shared test helpers.

/** A random image with a plausible vector table. */
export function fakeImage(size = 0x3456, seed = 1) {
  const b = new Uint8Array(size);
  let x = seed * 2654435761;
  for (let i = 0; i < size; i++) {
    x = (x * 1103515245 + 12345) >>> 0;
    b[i] = x >>> 24;
  }
  new DataView(b.buffer).setUint32(0, 0x20004000, true);
  new DataView(b.buffer).setUint32(4, 0xd5, true);
  return b;
}
