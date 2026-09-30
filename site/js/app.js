// The page: wires the four steps to the flashers and the radio client.

import { checkSupport } from './support.js';
import { loadManifest, fetchVerified, sha256Hex } from './manifest.js';
import { loadImage } from './k5image.js';
import { FrameLink, K5CancelledError } from './k5link.js';
import { flashRadio } from './k5flasher.js';
import { RadioClient, IN_FLASH_MODE } from './radio.js';
import * as v2 from './v2.js';
import { SerialSession, serialErrorText } from './serial.js';
import { detachToBootloader, DfuseDevice, AIOC_RUNTIME, STM32_BOOTLOADER, AIOC_FLASH_START, AIOC_FLASH_SIZE, checkAiocImage } from './dfu.js';
import { AIOC_HID, readEqState } from './aioc-hid.js';

const $ = (id) => document.getElementById(id);
function show(el, text, level = '') {
  el.textContent = text;
  el.className = `status ${level}`;
}
const errText = (e) => e?.message || String(e);
let busyCount = 0; // flashes in progress, for the leave-page warning
window.addEventListener('beforeunload', (ev) => {
  if (busyCount > 0) {
    ev.preventDefault();
    ev.returnValue = '';
  }
});

// ------------------------------------------------------------------ browser support

const support = checkSupport();
$('support').textContent = support.message;
$('support').className = `note ${support.ok ? 'ok' : 'error'}`;
if (!support.serial) ['k5-connect', 'radio-connect'].forEach((id) => ($(id).disabled = true));
if (!support.usb) ['aioc-detach', 'aioc-connect'].forEach((id) => ($(id).disabled = true));
if (!support.hid) $('eq-check').disabled = true;

// ------------------------------------------------------------------ firmware files

let radioFw = null; // { image, label, detail }
let aiocFw = null; // { bytes, label, detail }

async function loadFiles() {
  const m = await loadManifest();
  if (!m) {
    $('files-status').textContent =
      'This copy of the page has no firmware files with it. Use "Use a firmware file from your computer" in steps 1 and 2.';
    return;
  }
  $('files-status').innerHTML = '';
  const p = $('files-status');
  p.append('This page flashes radio firmware ');
  p.append(link(m.radio.release, m.radio.version));
  p.append(' and AIOC firmware ');
  p.append(link(m.aioc.release, m.aioc.version));
  p.append(', straight from their releases, and checks each file against its release checksum first.');
  try {
    const { bytes } = await fetchVerified(m.radio);
    const image = loadImage(bytes);
    image.validate();
    setRadioFw({ image, label: m.radio.version, detail: `${m.radio.file}, checksum matches the release` });
  } catch (e) {
    show($('k5-status'), `The radio firmware file could not be loaded: ${errText(e)}`, 'error');
  }
  try {
    const { bytes } = await fetchVerified(m.aioc);
    checkAiocImage(bytes);
    setAiocFw({ bytes, label: `aioc-packet ${m.aioc.version}`, detail: `${m.aioc.file}, checksum matches the release` });
  } catch (e) {
    show($('aioc-status'), `The AIOC firmware file could not be loaded: ${errText(e)}`, 'error');
  }
}

function link(href, text) {
  const a = document.createElement('a');
  a.href = href;
  a.textContent = text;
  a.target = '_blank';
  a.rel = 'noopener';
  return a;
}

function setRadioFw(fw) {
  radioFw = fw;
  $('k5-flash').textContent = `Flash ${fw.label}`;
}

function setAiocFw(fw) {
  aiocFw = fw;
  $('aioc-flash').textContent = `Flash ${fw.label}`;
}

$('k5-file').addEventListener('change', async (ev) => {
  const f = ev.target.files[0];
  if (!f) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer());
    const image = loadImage(bytes);
    image.validate();
    const sha = await sha256Hex(bytes);
    setRadioFw({ image, label: image.version.replace(/^\*/, '') || f.name, detail: `${f.name}, sha256 ${sha.slice(0, 16)}...` });
    show($('k5-status'), `Using ${f.name} (version "${image.version}"). Connect to check the radio.`, 'ok');
  } catch (e) {
    show($('k5-status'), errText(e), 'error');
  }
});

$('aioc-file').addEventListener('change', async (ev) => {
  const f = ev.target.files[0];
  if (!f) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer());
    checkAiocImage(bytes);
    const sha = await sha256Hex(bytes);
    setAiocFw({ bytes, label: f.name, detail: `${f.name}, sha256 ${sha.slice(0, 16)}...` });
    show($('aioc-status'), `Using ${f.name} (${bytes.length} bytes).`, 'ok');
    $('aioc-flash').disabled = !dfu;
  } catch (e) {
    show($('aioc-status'), errText(e), 'error');
  }
});

// ------------------------------------------------------------------ the serial port (steps 1 and 3)

let serial = null;
const serialUsers = { flash: null, radio: null };

/** The open serial session, or a new one after the port picker; null if the user cancelled. */
async function getSerial(owner, showAll = false) {
  if (!serial) {
    const port = await SerialSession.choose({ showAll });
    if (!port) return null;
    const s = await SerialSession.open(port);
    s.onClose = (e) => {
      if (serial !== s) return;
      serial = null;
      for (const k of Object.keys(serialUsers)) serialUsers[k]?.(e);
    };
    serial = s;
  }
  return serial;
}

async function closeSerial() {
  const s = serial;
  serial = null;
  if (s) await s.close();
}

function newLink(s) {
  const l = new FrameLink({ write: (b) => s.write(b) });
  s.onData = (b) => l.receive(b);
  return l;
}

// ------------------------------------------------------------------ step 1: flash the radio

const k5 = { link: null, abort: null, flashed: false, checking: false, bannerTimer: null, fallbackTimer: null };

serialUsers.flash = (e) => {
  if (!k5.link) return;
  k5.link.close(e);
  k5.link = null;
  $('k5-flash').disabled = true;
  if (!k5.abort) show($('k5-status'), 'The serial port closed (was the cable unplugged?). Press Connect to start again.', 'warn');
};

function k5Busy(on) {
  $('k5-connect').disabled = on || !support.serial;
  $('k5-cancel').hidden = !on;
}

$('k5-connect').addEventListener('click', async () => {
  if (!radioFw) {
    show($('k5-status'), 'No firmware loaded yet. Wait a moment, or pick a file under "Use a firmware file from your computer".', 'warn');
    return;
  }
  stopRadio('The radio connection in step 3 was closed so step 1 could use the port.');
  $('k5-after').hidden = true;
  $('k5-flash').disabled = true;
  let s;
  try {
    s = await getSerial('flash', $('k5-showall').checked);
  } catch (e) {
    const t = serialErrorText(e);
    if (t) show($('k5-status'), t, 'error');
    return;
  }
  if (!s) return;
  k5.link = newLink(s);
  k5.abort = new AbortController();
  k5Busy(true);
  show($('k5-status'), 'Listening for the radio in flash mode...');
  try {
    const r = await flashRadio(k5.link, radioFw.image, { reallyFlash: false, signal: k5.abort.signal });
    show(
      $('k5-status'),
      `Found the radio's bootloader ${r.beacon.version}: compatible.\n` +
        `Ready to flash ${radioFw.label} (${radioFw.image.raw.length} bytes, ${r.blocks} blocks; ${radioFw.detail}).\n` +
        'Nothing has been written yet. Press Flash to go ahead.',
      'ok',
    );
    $('k5-flash').disabled = false;
  } catch (e) {
    if (!(e instanceof K5CancelledError)) show($('k5-status'), errText(e), 'error');
  } finally {
    k5.abort = null;
    k5Busy(false);
  }
});

$('k5-cancel').addEventListener('click', () => {
  if (busyCount > 0 && !confirm('Stop flashing? The radio will need flashing again (it stays safe in flash mode).')) return;
  k5.abort?.abort();
  show($('k5-status'), 'Cancelled.', 'warn');
});

$('k5-flash').addEventListener('click', async () => {
  if (!k5.link) return;
  $('k5-flash').disabled = true;
  const bar = $('k5-progress');
  bar.hidden = false;
  bar.value = 0;
  k5.abort = new AbortController();
  k5Busy(true);
  busyCount++;
  show($('k5-status'), 'Flashing. Keep the radio and the cable still until it finishes.');
  try {
    const r = await flashRadio(k5.link, radioFw.image, {
      reallyFlash: true,
      signal: k5.abort.signal,
      onProgress: (p) => {
        if (p.stage === 'writing') {
          bar.max = p.total;
          bar.value = p.done;
          show($('k5-status'), `Flashing: block ${p.done} of ${p.total}.`);
        } else if (p.stage === 'retry') {
          show($('k5-status'), `Block ${p.chunk + 1}: no answer, sending it again.`, 'warn');
        }
      },
    });
    k5.flashed = true;
    show($('k5-status'), `Flashed ${radioFw.label}: all ${r.blocks} blocks written and confirmed by the radio. It is restarting; checking the new firmware in a few seconds.`, 'ok');
    $('k5-after').hidden = false;
    listenForBanner();
    // If the start-up text is missed, check anyway once the welcome screen must be over.
    k5.fallbackTimer = setTimeout(() => { k5.fallbackTimer = null; checkNewFirmware({ auto: true }); }, 6000);
  } catch (e) {
    if (!(e instanceof K5CancelledError)) show($('k5-status'), errText(e), 'error');
  } finally {
    busyCount--;
    k5.abort = null;
    k5Busy(false);
  }
});

function listenForBanner() {
  if (!k5.link) return;
  let text = '';
  k5.link.onText = (t) => {
    text = (text + t).slice(-200);
    if (/packet firmware/i.test(text) && !k5.bannerTimer) {
      text = '';
      if (k5.fallbackTimer) { clearTimeout(k5.fallbackTimer); k5.fallbackTimer = null; }
      // The start-up text comes about 0.3 s after a restart, but the firmware only answers once its
      // 2.5 s welcome screen is over (measured: first reply 2.86 s after a restart).
      k5.bannerTimer = setTimeout(() => {
        k5.bannerTimer = null;
        checkNewFirmware({ auto: true });
      }, 2800);
    }
  };
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function checkNewFirmware({ auto = false } = {}) {
  if (!k5.link || k5.checking) {
    if (!k5.link) show($('k5-status'), 'Not connected. Press Connect in step 3 to check the radio instead.', 'warn');
    return;
  }
  k5.checking = true;
  const l = k5.link;
  const client = new RadioClient(l);
  try {
    // An automatic check retries for a few seconds: the radio may still be on its welcome screen.
    let h;
    for (let attempt = 1; ; attempt++) {
      try {
        h = await client.sayHello();
        break;
      } catch (e) {
        if (!auto || attempt >= 6 || e.message === IN_FLASH_MODE) throw e;
        await sleep(1000);
      }
    }
    const want = radioFw.image.version.replace(/^\*/, '');
    if (h.version === want) {
      show($('k5-status'), `The radio runs ${h.version}${h.pkt2 ? ` (protocol ${h.protocolText})` : ''}. Step 1 is done.`, 'ok');
      $('k5-after').hidden = true;
      $('k5-progress').hidden = true;
      l.onText = null;
    } else if (h.kind === 'other' || h.kind === 'v1') {
      show($('k5-status'), `The radio answers with "${h.version}", not ${want}. Did the flash finish? Try flashing again.`, 'warn');
    } else {
      show($('k5-status'), `The radio runs ${h.version} (this page flashed ${want}).`, 'warn');
    }
  } catch (e) {
    if (e.message === IN_FLASH_MODE) show($('k5-status'), 'The radio is still in flash mode. Switch it off, then on again without holding PTT.', 'warn');
    else show($('k5-status'), `${errText(e)} If you have just switched it on, wait a second and press "Check the radio now".`, 'warn');
  } finally {
    l.onFrame = null;
    k5.checking = false;
  }
}

$('k5-check').addEventListener('click', () => checkNewFirmware());

// ------------------------------------------------------------------ step 2: flash the AIOC

let dfu = null;
let backupSaved = false;

function aiocButtons() {
  $('aioc-backup').disabled = !dfu;
  $('aioc-flash').disabled = !dfu || !aiocFw;
}

function usbCancelled(e) {
  return e?.name === 'NotFoundError' && /No device selected/i.test(e.message);
}

function usbErrorText(e) {
  if (e?.name === 'SecurityError' || e?.name === 'NotAllowedError' || /Access denied/i.test(e?.message)) {
    return 'The browser was not allowed to use the device. On Windows it needs the WinUSB driver (see the notes below); on Linux, the udev rule; or another program has it open.';
  }
  return errText(e);
}

$('aioc-detach').addEventListener('click', async () => {
  let dev;
  try {
    dev = await navigator.usb.requestDevice({ filters: [AIOC_RUNTIME] });
  } catch (e) {
    if (!usbCancelled(e)) show($('aioc-status'), usbErrorText(e), 'error');
    return;
  }
  try {
    await detachToBootloader(dev);
    show($('aioc-status'), 'The AIOC is restarting into its bootloader. Now press "Connect to bootloader" and pick "STM32 BOOTLOADER".', 'ok');
    // If the browser already has permission for the bootloader (from an earlier visit), use it.
    setTimeout(async () => {
      if (dfu) return;
      const known = (await navigator.usb.getDevices()).filter((d) => d.vendorId === STM32_BOOTLOADER.vendorId && d.productId === STM32_BOOTLOADER.productId);
      if (known.length === 1) connectBootloader(known[0]);
    }, 2500);
  } catch (e) {
    show($('aioc-status'), usbErrorText(e), 'error');
  }
});

async function connectBootloader(dev) {
  try {
    if (!dev) dev = await navigator.usb.requestDevice({ filters: [STM32_BOOTLOADER] });
  } catch (e) {
    if (!usbCancelled(e)) show($('aioc-status'), usbErrorText(e), 'error');
    return;
  }
  try {
    dfu = await DfuseDevice.open(dev);
    backupSaved = false;
    const seg = dfu.layout?.segments[0];
    const size = seg ? `${(seg.end - seg.start) / 1024} KB of flash` : 'flash';
    show($('aioc-status'), `Connected to the AIOC's bootloader (${size}).${aiocFw ? ` Ready to flash ${aiocFw.label} (${aiocFw.detail}).` : ''} Save a backup first if you like.`, 'ok');
  } catch (e) {
    dfu = null;
    show($('aioc-status'), usbErrorText(e), 'error');
  }
  aiocButtons();
}

$('aioc-connect').addEventListener('click', () => connectBootloader());

if (support.usb) {
  navigator.usb.addEventListener('disconnect', (ev) => {
    if (dfu && ev.device === dfu.device) {
      dfu = null;
      aiocButtons();
    }
  });
}

function aiocProgress(label) {
  const bar = $('aioc-progress');
  bar.hidden = false;
  return (stageOrDone, a, b) => {
    const [stage, done, total] = typeof stageOrDone === 'string' ? [stageOrDone, a, b] : [label, stageOrDone, a];
    bar.max = total;
    bar.value = done;
    const words = { erase: 'Erasing', write: 'Writing', verify: 'Checking', read: 'Reading' }[stage] || stage;
    show($('aioc-status'), `${words}: ${Math.round((100 * done) / total)}%`);
  };
}

$('aioc-backup').addEventListener('click', async () => {
  if (!dfu) return;
  $('aioc-backup').disabled = $('aioc-flash').disabled = true;
  busyCount++;
  try {
    const data = await dfu.read(AIOC_FLASH_START, AIOC_FLASH_SIZE, aiocProgress('read'));
    const d = new Date();
    const stamp = `${d.getFullYear()}${String(d.getMonth() + 1).padStart(2, '0')}${String(d.getDate()).padStart(2, '0')}-${String(d.getHours()).padStart(2, '0')}${String(d.getMinutes()).padStart(2, '0')}`;
    const name = `aioc-backup-${stamp}.bin`;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([data], { type: 'application/octet-stream' }));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    backupSaved = true;
    show($('aioc-status'), `Backup saved as ${name} (128 KB: the firmware and the AIOC's stored settings). Flashing it back here restores this AIOC exactly.`, 'ok');
  } catch (e) {
    show($('aioc-status'), `Could not read the AIOC (${errText(e)}). It may be read-protected. You can still flash; stock firmware is on the AIOC project's releases page.`, 'warn');
  } finally {
    busyCount--;
    $('aioc-progress').hidden = true;
    aiocButtons();
  }
});

$('aioc-flash').addEventListener('click', async () => {
  if (!dfu || !aiocFw) return;
  if (!backupSaved && !confirm('Flash without saving a backup first?')) return;
  $('aioc-backup').disabled = $('aioc-flash').disabled = true;
  busyCount++;
  const progress = aiocProgress('write');
  try {
    checkAiocImage(aiocFw.bytes);
    await dfu.write(AIOC_FLASH_START, aiocFw.bytes, progress);
    await dfu.verify(AIOC_FLASH_START, aiocFw.bytes, progress);
    const d = dfu;
    dfu = null;
    await d.leave(AIOC_FLASH_START);
    show(
      $('aioc-status'),
      `Done: ${aiocFw.label} is written and checked, and the AIOC is restarting with it. Its settings are back to the defaults.\n` +
        'If you shorted the pins, unplug it, remove the short and plug it back in. Then check the equaliser below.',
      'ok',
    );
  } catch (e) {
    show($('aioc-status'), `${errText(e)} Nothing is lost: connect to the bootloader again and flash again.`, 'error');
  } finally {
    busyCount--;
    $('aioc-progress').hidden = true;
    aiocButtons();
  }
});

$('eq-check').addEventListener('click', async () => {
  let dev;
  try {
    [dev] = await navigator.hid.requestDevice({ filters: [AIOC_HID] });
  } catch (e) {
    show($('eq-status'), errText(e), 'error');
    return;
  }
  if (!dev) return;
  try {
    if (!dev.opened) await dev.open();
    const s = await readEqState(dev);
    show($('eq-status'), `${s.summary}${s.detail ? `\n${s.detail}` : ''}`, s.level);
  } catch (e) {
    show(
      $('eq-status'),
      `Could not read the AIOC: ${errText(e)}. On Linux this needs the udev rule in the notes above; elsewhere, unplug and replug the AIOC and try again.`,
      'error',
    );
  } finally {
    try {
      await dev.close();
    } catch {}
  }
});

// ------------------------------------------------------------------ step 3: set up the radio

const r = { client: null, lastHeartbeat: 0, subscribed: false, timer: null, info: null };
const QUIET_MS = 2500;

serialUsers.radio = (e) => {
  if (!r.client) return;
  stopRadio('The connection closed (was the cable unplugged, or the radio switched off?). Press Connect to try again.');
};

function stopRadio(message) {
  if (!r.client) return;
  r.client.link.close();
  r.client = null;
  r.subscribed = false;
  clearInterval(r.timer);
  $('radio-panel').hidden = true;
  $('radio-connect').disabled = !support.serial;
  $('radio-disconnect').disabled = true;
  if (message) show($('radio-status'), message, 'warn');
}

function quiet() {
  return r.subscribed && Date.now() - r.lastHeartbeat > QUIET_MS;
}

function fillParams(p) {
  if (document.activeElement?.closest?.('#radio-form')) return; // the user is editing
  if (p.has(v2.PARAM.FREQ_HZ)) $('f-freq').value = v2.formatMHz(p.get(v2.PARAM.FREQ_HZ));
  if (p.has(v2.PARAM.POWER)) $('f-power').value = String(p.get(v2.PARAM.POWER));
  if (p.has(v2.PARAM.BANDWIDTH)) $('f-bw').value = String(p.get(v2.PARAM.BANDWIDTH));
  if (p.has(v2.PARAM.DEV_WIDE) && p.has(v2.PARAM.DEV_NARROW)) {
    const w = p.get(v2.PARAM.DEV_WIDE);
    const n = p.get(v2.PARAM.DEV_NARROW);
    $('r-dev').textContent =
      `wide 0x${w.toString(16).toUpperCase()} (about ${v2.deviationKhz(w).toFixed(1)} kHz), ` +
      `narrow 0x${n.toString(16).toUpperCase()} (about ${v2.deviationKhz(n).toFixed(1)} kHz), at full-scale audio through the packet AIOC`;
  }
}

async function readParams() {
  fillParams(await r.client.getParams([v2.PARAM.FREQ_HZ, v2.PARAM.POWER, v2.PARAM.BANDWIDTH, v2.PARAM.DEV_WIDE, v2.PARAM.DEV_NARROW]));
}

function showSignal(rssiRaw) {
  const dbm = v2.rssiDbm(rssiRaw);
  $('r-rssi').textContent = `${dbm.toFixed(0)} dBm (${v2.sPoint(dbm)})`;
}

function showBattery(mv) {
  $('r-batt').textContent = `${(mv / 1000).toFixed(2)} V`;
}

function tick() {
  if (!r.client) return;
  if (quiet()) {
    $('r-state').textContent = 'quiet: probably transmitting (commands wait until it is back)';
    return;
  }
  const lock = r.client.msUntilSafeToKey();
  if (lock > 0) $('r-state').textContent = `${$('r-state').dataset.base || 'receiving'}; PTT is ignored for ${lock} ms after the page's last command`;
  else $('r-state').textContent = $('r-state').dataset.base || 'receiving';
}

function onRadioEvent(e) {
  switch (e.type) {
    case 'heartbeat':
      r.lastHeartbeat = Date.now();
      if (e.state !== 2) showSignal(e.rssiRaw);
      showBattery(e.batteryMv);
      $('r-state').dataset.base = (v2.STATE_NAMES[e.state] || 'receiving').toLowerCase();
      break;
    case 'tx_refused':
      show($('radio-status'), `The radio refused to transmit: ${v2.TX_REFUSED_REASONS[e.reason] || `reason ${e.reason}`}.`, 'warn');
      break;
    case 'battery':
      showBattery(e.mv);
      break;
    case 'params_changed':
      if (e.source === 0 && !quiet()) readParams().catch(() => {});
      break;
    case 'tone_end':
      show($('radio-status'), 'The level tone has finished; the radio is back to received audio.', 'ok');
      break;
    default:
  }
}

$('radio-connect').addEventListener('click', async () => {
  if (k5.abort) return;
  let s;
  try {
    s = await getSerial('radio', $('k5-showall').checked);
  } catch (e) {
    const t = serialErrorText(e);
    if (t) show($('radio-status'), t, 'error');
    return;
  }
  if (!s) return;
  k5.link = null; // step 1 no longer owns the port
  $('k5-flash').disabled = true;
  const client = new RadioClient(newLink(s));
  r.client = client;
  $('radio-connect').disabled = true;
  $('radio-disconnect').disabled = false;
  show($('radio-status'), 'Talking to the radio...');
  try {
    client.onBootloader = () => show($('radio-status'), IN_FLASH_MODE, 'warn');
    client.onEvent = onRadioEvent;
    const h = await client.sayHello();
    if (h.kind !== 'v2') {
      throw new Error(
        h.kind === 'v1'
          ? `The radio runs an older packet firmware (${h.version}). Flash the latest in step 1 first.`
          : `The radio runs other firmware ("${h.version}"). Flash the packet firmware in step 1 first.`,
      );
    }
    r.info = await client.getInfo();
    const st = await client.getStatus();
    await readParams();
    $('r-fw').textContent = `${r.info.version}, protocol ${h.protocolText}`;
    showSignal(st.rssiRaw);
    showBattery(st.batteryMv);
    $('r-state').dataset.base = (v2.STATE_NAMES[st.state] || 'receiving').toLowerCase();
    $('tone-start').disabled = $('tone-stop').disabled = !r.info.canLevelToneRaw;
    await client.subscribe({
      mask: (1 << v2.EVENT.TX_REFUSED) | (1 << v2.EVENT.HEARTBEAT) | (1 << v2.EVENT.BATTERY) | (1 << v2.EVENT.PARAMS_CHANGED) | (1 << v2.EVENT.TONE_END),
      heartbeatMs: 1000,
    });
    r.subscribed = true;
    r.lastHeartbeat = Date.now();
    client.guard = () => (quiet() ? 'The radio has gone quiet for a moment (it may be transmitting). Try again when it is back.' : null);
    r.timer = setInterval(tick, 250);
    tick();
    $('radio-panel').hidden = false;
    const txNote = r.info.txPolicy === v2.TX_POLICY_FIXED ? ' It transmits only from 136 to 174 MHz and from 400 to 470 MHz.' : '';
    show($('radio-status'), `Connected.${txNote}`, 'ok');
  } catch (e) {
    const msg = errText(e);
    stopRadio();
    await closeSerial();
    show($('radio-status'), msg, 'error');
  }
});

$('radio-disconnect').addEventListener('click', async () => {
  const c = r.client;
  if (c && r.subscribed && !quiet()) {
    try {
      await c.subscribe({ mask: 0 });
    } catch {}
  }
  stopRadio();
  await closeSerial();
  show($('radio-status'), 'Disconnected. You can start your soundmodem now.', 'ok');
});

$('radio-form').addEventListener('submit', async (ev) => {
  ev.preventDefault();
  if (!r.client) return;
  const f = v2.parseFrequencyMHz($('f-freq').value);
  if (f.error) {
    show($('radio-status'), f.error, 'error');
    return;
  }
  const power = Number($('f-power').value);
  const bandwidth = Number($('f-bw').value);
  $('f-save').disabled = true;
  try {
    const res = await r.client.saveChannel({ frequencyHz: f.hz, power, bandwidth });
    const back = res.values;
    show(
      $('radio-status'),
      `Saved: ${v2.formatMHz(back.get(v2.PARAM.FREQ_HZ))} MHz, ${v2.POWER_NAMES[back.get(v2.PARAM.POWER)]}, ${v2.BANDWIDTH_NAMES[back.get(v2.PARAM.BANDWIDTH)]}.` +
        (res.persistQueued ? ' The radio remembers it.' : ''),
      'ok',
    );
    $('f-freq').value = v2.formatMHz(back.get(v2.PARAM.FREQ_HZ));
  } catch (e) {
    show($('radio-status'), errText(e), 'error');
  } finally {
    $('f-save').disabled = false;
  }
});

$('tone-start').addEventListener('click', async () => {
  if (!r.client) return;
  try {
    await r.client.levelTone({ hz: 1000, mode: 1, level: 64, durationMs: 10000 });
    show($('radio-status'), 'Playing a 1 kHz tone for 10 seconds. Turn the volume knob so your soundmodem shows it just below clipping, then back off a little.', 'ok');
  } catch (e) {
    show($('radio-status'), errText(e), 'error');
  }
});

$('tone-stop').addEventListener('click', async () => {
  if (!r.client) return;
  try {
    await r.client.levelTone({ hz: 1000, mode: 1, level: 0, durationMs: 0 });
  } catch (e) {
    show($('radio-status'), errText(e), 'error');
  }
});

loadFiles();
