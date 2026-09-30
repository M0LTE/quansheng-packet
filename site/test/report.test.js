import { test } from 'node:test';
import assert from 'node:assert/strict';
import { MessageLog, issueBody, issueUrl, ISSUES_URL } from '../js/report.js';

test('the message log keeps the newest entries, oldest first', () => {
  let n = 0;
  const log = new MessageLog(3, () => new Date(Date.UTC(2026, 8, 30, 12, 0, n++)));
  for (const t of ['a', 'b', 'c', 'd']) log.add('k5-status', 'error', t);
  assert.deepEqual(log.lines().map((l) => l.slice(-1)), ['b', 'c', 'd']);
  assert.match(log.lines()[0], /^12:00:01 \[k5-status\] error: b$/);
});

test('the issue body carries browser, support, versions and messages', () => {
  const body = issueBody({
    userAgent: 'Mozilla/5.0 X11 Linux Chrome/153',
    platform: 'Linux',
    support: { serial: true, usb: true, hid: false, secure: true },
    versions: { radio: 'PKTFW v1.0.1', aioc: 'aioc-packet 1.4.1-packet.2' },
    radio: 'PKTFW v1.0.1',
    log: ['12:00:00 [aioc-status] error: Must be handling a user gesture'],
  });
  assert.match(body, /Chrome\/153/);
  assert.match(body, /serial yes, USB yes, HID no, secure page yes/);
  assert.match(body, /PKTFW v1\.0\.1/);
  assert.match(body, /user gesture/);
  assert.doesNotMatch(body, /[\u2013\u2014]/);
});

test('the URL targets the issue form and trims old messages to stay short', () => {
  const log = Array.from({ length: 200 }, (_, i) => `12:00:00 [x] info: message ${i} ${'y'.repeat(60)}`);
  const url = issueUrl({ userAgent: 'ua', log });
  assert.ok(url.startsWith(`${ISSUES_URL}?title=`));
  assert.ok(url.length <= 7500);
  assert.match(decodeURIComponent(url), /message 199/);
  assert.doesNotMatch(decodeURIComponent(url), /message 0 /);
});
