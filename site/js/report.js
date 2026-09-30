// "Report a problem": a pre-filled GitHub issue with what helps diagnose a report (browser, OS,
// what the browser supports, the firmware versions this page serves, and the page's recent
// messages). Nothing is sent anywhere: the link opens GitHub's new-issue form, which the user
// can edit before submitting.

export const ISSUES_URL = 'https://github.com/M0LTE/quansheng-packet/issues/new';
const MAX_URL = 7500; // GitHub and browsers cope with about 8 KB of URL

/** The page's recent status messages, oldest first. */
export class MessageLog {
  constructor(size = 20, now = () => new Date()) {
    this.size = size;
    this.now = now;
    this.items = [];
  }

  add(where, level, text) {
    this.items.push({ time: this.now().toISOString().slice(11, 19), where, level: level || 'info', text: String(text) });
    if (this.items.length > this.size) this.items.shift();
  }

  lines() {
    return this.items.map((m) => `${m.time} [${m.where}] ${m.level}: ${m.text.replace(/\s+/g, ' ').trim()}`);
  }
}

/** The issue body as Markdown. */
export function issueBody({ userAgent = '', platform = '', support = {}, versions = {}, radio = '', log = [] } = {}) {
  const yes = (b) => (b ? 'yes' : 'no');
  const lines = [
    '**What happened, and at which step?**',
    '',
    '(Please describe what you did and what you expected.)',
    '',
    '**Details from the setup page** (filled in automatically; edit anything you like)',
    '',
    `- Browser: ${userAgent || 'unknown'}`,
    `- Platform: ${platform || 'unknown'}`,
    `- Supports: serial ${yes(support.serial)}, USB ${yes(support.usb)}, HID ${yes(support.hid)}, secure page ${yes(support.secure)}`,
    `- Radio firmware offered: ${versions.radio || 'not loaded'}`,
    `- AIOC firmware offered: ${versions.aioc || 'not loaded'}`,
  ];
  if (radio) lines.push(`- Radio reported: ${radio}`);
  lines.push('', '**Recent messages on the page**', '', '```');
  lines.push(...(log.length ? log : ['(none)']));
  lines.push('```');
  return lines.join('\n');
}

/** The new-issue URL, trimming the oldest messages if the URL would be too long. */
export function issueUrl(details, title = 'Setup page problem') {
  let log = [...(details.log || [])];
  for (;;) {
    const url = `${ISSUES_URL}?title=${encodeURIComponent(title)}&labels=setup-page&body=${encodeURIComponent(issueBody({ ...details, log }))}`;
    if (url.length <= MAX_URL || log.length === 0) return url;
    log = log.slice(1);
  }
}
