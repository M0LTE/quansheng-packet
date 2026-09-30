// The firmware's golden vectors (tests/vectors/protocol-v2.json at the repo root).
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { fromHex } from '../js/k5frame.js';

const path = fileURLToPath(new URL('../../tests/vectors/protocol-v2.json', import.meta.url));

export const vectors = JSON.parse(readFileSync(path, 'utf8')).vectors.map((v) => ({
  name: v.name,
  state: v.state,
  requestObfuscated: v.request_mode === 'obfuscated',
  request: fromHex(v.request),
  responseObfuscated: v.response_mode === 'obfuscated',
  response: fromHex(v.response),
  frames: v.frames.map((f) => ({ id: parseInt(f.id, 16), crc: f.crc, body: fromHex(f.body) })),
}));

export const vector = (name) => {
  const v = vectors.find((x) => x.name === name);
  if (!v) throw new Error(`no vector ${name}`);
  return v;
};
