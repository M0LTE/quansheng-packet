// Which of the browser features this site needs are here.

export function checkSupport(nav = globalThis.navigator, win = globalThis) {
  const secure = !!win.isSecureContext;
  const serial = !!nav?.serial;
  const usb = !!nav?.usb;
  const hid = !!nav?.hid;
  const ua = nav?.userAgent || '';
  let browser = 'other';
  if (/Firefox\//.test(ua)) browser = 'firefox';
  else if (/Edg\//.test(ua)) browser = 'edge';
  else if (/Chrome\//.test(ua)) browser = 'chrome';
  else if (/Safari\//.test(ua)) browser = 'safari';
  const mobile = /Android|iPhone|iPad/.test(ua);
  const ok = secure && serial && usb && hid;
  let message;
  if (ok) message = 'Your browser can do everything on this page.';
  else if (!secure) message = 'This page must be opened over https (or from localhost) for the browser to allow USB and serial access.';
  else if (browser === 'firefox' || browser === 'safari') {
    message = `${browser === 'firefox' ? 'Firefox' : 'Safari'} cannot talk to USB or serial devices from a web page. Please open this page in Chrome or Edge on a computer.`;
  } else if (mobile) message = 'Phones and tablets cannot do this. Please use Chrome or Edge on a computer.';
  else {
    const missing = [!serial && 'serial ports', !usb && 'USB', !hid && 'HID'].filter(Boolean).join(', ');
    message = `This browser is missing ${missing} access. Please use a recent Chrome or Edge on a computer.`;
  }
  return { secure, serial, usb, hid, browser, mobile, ok, message };
}
