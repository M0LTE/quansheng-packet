// Web Serial for the radio: always 38400 8N1 (slower rates can key the radio), DTR and RTS
// low straight after opening (the AIOC's default serial PTT is DTR high with RTS low), never a
// BREAK. One session at a time; whoever uses it sets onData.

export const AIOC_SERIAL_FILTER = { usbVendorId: 0x1209, usbProductId: 0x7388 };

export class SerialSession {
  constructor(port) {
    this.port = port;
    this.onData = null;
    this.onClose = null;
    this.reader = null;
    this.readLoop = null;
    this.closed = false;
  }

  /** Shows the browser's port picker. Returns null if the user closed it. */
  static async choose({ showAll = false } = {}) {
    try {
      return await navigator.serial.requestPort(showAll ? {} : { filters: [AIOC_SERIAL_FILTER] });
    } catch (e) {
      if (e?.name === 'NotFoundError') return null;
      throw e;
    }
  }

  static async open(port) {
    const s = new SerialSession(port);
    await port.open({ baudRate: 38400, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none', bufferSize: 4096 });
    try {
      // one call, DTR first: never passes through DTR high with RTS low
      await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch {
      // some adapters cannot; the AIOC can
    }
    s.readLoop = s._read();
    return s;
  }

  async _read() {
    let error = null;
    while (!this.closed && this.port.readable) {
      this.reader = this.port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value && this.onData) this.onData(value);
        }
      } catch (e) {
        // BufferOverrunError, FramingError and the like are recoverable; a lost device is not
        if (e?.name === 'NetworkError' || e?.name === 'NotFoundError') error = e;
        else if (!this.closed) continue;
      } finally {
        try {
          this.reader.releaseLock();
        } catch {}
      }
      if (error || this.closed) break;
    }
    if (!this.closed) {
      this.closed = true;
      if (this.onClose) this.onClose(error || new Error('The serial port closed.'));
    }
  }

  async write(bytes) {
    if (this.closed) throw new Error('The serial port is closed.');
    const w = this.port.writable.getWriter();
    try {
      await w.write(bytes);
    } finally {
      w.releaseLock();
    }
  }

  async close() {
    if (this.closed && !this.port.readable) return;
    this.closed = true;
    try {
      await this.reader?.cancel();
    } catch {}
    try {
      await this.readLoop;
    } catch {}
    try {
      await this.port.close();
    } catch {}
  }
}

/** A plain-English message for a Web Serial error, or null for "the user cancelled". */
export function serialErrorText(e) {
  const t = `${e?.name || ''} ${e?.message || ''}`;
  if (e?.name === 'NotFoundError' && /No port selected/i.test(t)) return null;
  if (e?.name === 'InvalidStateError') return 'That port is already open on this page. Disconnect it in the other step first.';
  if (e?.name === 'NetworkError' || /Failed to open/i.test(t)) {
    return (
      'Could not open the port. Close any other program using it (CHIRP, a soundmodem, Direwolf, k5ctl) and try again. ' +
      'On Linux your user may need to be in the dialout group.'
    );
  }
  if (e?.name === 'SecurityError') return 'The browser blocked access to serial ports on this page.';
  return e?.message || String(e);
}
