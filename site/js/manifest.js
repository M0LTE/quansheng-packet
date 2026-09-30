// The firmware files this site serves: files/manifest.json, written by the Pages workflow from
// the latest releases, and a SHA-256 check of each file against it before anything is flashed.

export const FILES_BASE = 'files/';

export async function sha256Hex(bytes) {
  const d = await globalThis.crypto.subtle.digest('SHA-256', bytes);
  return Array.from(new Uint8Array(d), (x) => x.toString(16).padStart(2, '0')).join('');
}

/** Checks bytes against a manifest entry ({ file, sha256, size }). Throws with a plain message. */
export async function verifyEntry(entry, bytes) {
  if (entry.size !== undefined && bytes.length !== entry.size) {
    throw new Error(`${entry.file} is ${bytes.length} bytes, expected ${entry.size}. Reload the page and try again.`);
  }
  const got = await sha256Hex(bytes);
  if (got !== entry.sha256.toLowerCase()) {
    throw new Error(`${entry.file} does not match its release checksum. Reload the page and try again; if it keeps happening, tell us.`);
  }
  return got;
}

/** The manifest, or null if this copy of the site has no firmware files (a local checkout). */
export async function loadManifest(base = FILES_BASE, fetchFn = globalThis.fetch) {
  let r;
  try {
    r = await fetchFn(`${base}manifest.json`, { cache: 'no-cache' });
  } catch {
    return null;
  }
  if (!r.ok) return null;
  return r.json();
}

/** Downloads a manifest entry's file from this site and checks it. Returns { bytes, sha256 }. */
export async function fetchVerified(entry, base = FILES_BASE, fetchFn = globalThis.fetch) {
  const r = await fetchFn(`${base}${entry.file}`, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`Could not download ${entry.file} (HTTP ${r.status}).`);
  const bytes = new Uint8Array(await r.arrayBuffer());
  const sha256 = await verifyEntry(entry, bytes);
  return { bytes, sha256 };
}
