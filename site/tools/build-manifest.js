#!/usr/bin/env node
// Picks the firmware files out of two downloaded releases, checks them against their
// SHA256SUMS, copies them into the site and writes files/manifest.json.
//
//   node site/tools/build-manifest.js --out site/files \
//     --radio-dir DIR --radio-tag v1.0.1 --aioc-dir DIR --aioc-tag v1.4.1-packet.2
//
// Each DIR holds that release's assets (at least the image and SHA256SUMS).

import { readFileSync, writeFileSync, mkdirSync, copyFileSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const RADIO_REPO = 'M0LTE/quansheng-packet';
export const AIOC_REPO = 'M0LTE/aioc-packet';

/** "hash  name" lines (sha256sum output, text or binary mode) to a Map name -> hash. */
export function parseSha256Sums(text) {
  const m = new Map();
  for (const line of text.split(/\r?\n/)) {
    const r = /^([0-9a-fA-F]{64})\s+\*?(.+)$/.exec(line.trim());
    if (r) m.set(r[2].trim(), r[1].toLowerCase());
  }
  return m;
}

export function chooseRadioImage(names, tag) {
  const want = `quansheng-packet-${tag}.bin`;
  if (!names.includes(want)) throw new Error(`${RADIO_REPO} ${tag} has no ${want}`);
  return want;
}

/**
 * The recommended full AIOC image. From 1.4.1-packet.2 that is aioc-packet-<ver>.bin (the K5
 * equaliser on by default). In 1.4.1-packet.1 the plain image had the equaliser off and the
 * equivalent was aioc-packet-<ver>-k5-red.bin, so that name wins where it exists.
 */
export function chooseAiocImage(names, tag) {
  const ver = tag.replace(/^v/, '');
  for (const want of [`aioc-packet-${ver}-k5-red.bin`, `aioc-packet-${ver}.bin`]) {
    if (names.includes(want)) return want;
  }
  throw new Error(`${AIOC_REPO} ${tag} has neither aioc-packet-${ver}.bin nor aioc-packet-${ver}-k5-red.bin`);
}

function entryFor(repo, tag, dir, file, outDir) {
  const sums = parseSha256Sums(readFileSync(join(dir, 'SHA256SUMS'), 'utf8'));
  const expected = sums.get(file);
  if (!expected) throw new Error(`${repo} ${tag}: SHA256SUMS does not list ${file}`);
  const bytes = readFileSync(join(dir, file));
  const got = createHash('sha256').update(bytes).digest('hex');
  if (got !== expected) throw new Error(`${repo} ${tag}: ${file} has sha256 ${got}, SHA256SUMS says ${expected}`);
  copyFileSync(join(dir, file), join(outDir, file));
  return {
    repo,
    tag,
    version: tag.replace(/^v/, ''),
    file,
    sha256: got,
    size: bytes.length,
    release: `https://github.com/${repo}/releases/tag/${tag}`,
    download: `https://github.com/${repo}/releases/download/${tag}/${file}`,
  };
}

export function buildManifest({ out, radioDir, radioTag, aiocDir, aiocTag, now = new Date() }) {
  mkdirSync(out, { recursive: true });
  const radioFile = chooseRadioImage(readdirSync(radioDir), radioTag);
  const aiocFile = chooseAiocImage(readdirSync(aiocDir), aiocTag);
  const manifest = {
    generated: now.toISOString(),
    radio: { ...entryFor(RADIO_REPO, radioTag, radioDir, radioFile, out), version: radioTag },
    aioc: entryFor(AIOC_REPO, aiocTag, aiocDir, aiocFile, out),
  };
  writeFileSync(join(out, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
  return manifest;
}

function args(argv) {
  const a = {};
  for (let i = 0; i < argv.length; i += 2) a[argv[i].replace(/^--/, '').replace(/-(\w)/g, (_, c) => c.toUpperCase())] = argv[i + 1];
  return a;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const a = args(process.argv.slice(2));
  for (const k of ['out', 'radioDir', 'radioTag', 'aiocDir', 'aiocTag']) {
    if (!a[k]) {
      console.error(`missing --${k.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase())}`);
      process.exit(2);
    }
  }
  const m = buildManifest(a);
  console.log(JSON.stringify(m, null, 2));
}
