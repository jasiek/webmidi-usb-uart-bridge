// midi-bridge-serial.js — generated file, do not edit.
//
// A Web Serial-shaped provider over the Web MIDI API, for talking to the
// webmidi-usb-uart-bridge (a Raspberry Pi Pico that tunnels a UART through
// USB MIDI SysEx). Built from host/src/ of that repository by
// host/bin/build-webserial.js; edit the sources there and rebuild with
// `npm run build:webserial`.
//
// Usage:
//   import { createMidiBridgeSerial } from './midi-bridge-serial.js';
//   const port = await createMidiBridgeSerial().requestPort();
//   await port.open({ baudRate: 115200 });
//   // then port.readable / port.writable / port.setSignals, as Web Serial.

// ------------------------------ src/constants.js ------------------------------

// Wire protocol constants — the host-side mirror of lib/bridge_proto/bridge_proto.h.
// PROTOCOL.md is the specification both are written against.
//
// Plain ES module with no Node built-ins, so a browser page using the Web MIDI
// API can import it unchanged. See DECISIONS.md D3.

export const SYSEX_START = 0xf0;
export const SYSEX_END = 0xf7;
export const MANUFACTURER_ID = 0x7d; // non-commercial / educational
export const MAGIC = 0x55; // 'U' — this product
export const PROTOCOL_VERSION = 0x01;

export const HEADER_LEN = 5; // F0 7D 55 01 <cmd>
export const FRAME_OVERHEAD = HEADER_LEN + 1;

export const MAX_DATA_RAW = 128;
export const RX_BUFFER_SIZE = 2048;
export const SEQ_MASK = 0x7f;

/** Host → device. */
export const Cmd = Object.freeze({
  HELLO: 0x01,
  OPEN: 0x02,
  CLOSE: 0x03,
  DATA: 0x04,
  SET_LINES: 0x05,
  FLUSH: 0x06,
  CREDIT: 0x07,
  PING: 0x08,
  GET_STATUS: 0x09,
  RESET: 0x0a,
  REBOOT: 0x0b,
});

/** Device → host. */
export const Rsp = Object.freeze({
  INFO: 0x41,
  STATUS: 0x42,
  DATA: 0x44,
  CREDIT: 0x47,
  PONG: 0x48,
  ERROR: 0x4e,
  EVENT: 0x4f,
});

export const Err = Object.freeze({
  VERSION: 0x01,
  BAD_CMD: 0x02,
  BAD_LENGTH: 0x03,
  BAD_ENCODING: 0x04,
  NOT_OPEN: 0x05,
  BAD_PARAM: 0x06,
  NO_CREDIT: 0x07,
  SEQ: 0x08,
  OVERFLOW: 0x09,
  BACKEND: 0x0a,
});

export const ERR_NAMES = Object.freeze({
  [Err.VERSION]: 'unsupported protocol version',
  [Err.BAD_CMD]: 'unknown or wrong-direction command',
  [Err.BAD_LENGTH]: 'payload length wrong for the command',
  [Err.BAD_ENCODING]: '7-in-8 unpacking failed',
  [Err.NOT_OPEN]: 'port is not open',
  [Err.BAD_PARAM]: 'unsupported parameter',
  [Err.NO_CREDIT]: 'sender exceeded its window — data was dropped',
  [Err.SEQ]: 'sequence gap',
  [Err.OVERFLOW]: 'receive buffer overran',
  [Err.BACKEND]: 'backend failure',
});

export const Evt = Object.freeze({
  LINES: 0x01,
  BREAK: 0x02,
  OVERRUN: 0x03,
  ATTACH: 0x04,
  DETACH: 0x05,
  REBOOTING: 0x06,
});

/** Output control lines (SET_LINES, STATUS.outLines). */
export const Line = Object.freeze({
  DTR: 1 << 0,
  RTS: 1 << 1,
  BREAK: 1 << 2,
});

/** Input control lines (STATUS.inLines, EVT_LINES). */
export const InLine = Object.freeze({
  CTS: 1 << 0,
  DSR: 1 << 1,
  DCD: 1 << 2,
  RI: 1 << 3,
});

export const Cap = Object.freeze({
  DTR: 1 << 0,
  RTS: 1 << 1,
  BREAK: 1 << 2,
  CTS: 1 << 3,
  DSR: 1 << 4,
  DCD: 1 << 5,
  RI: 1 << 6,
  FLOW_RTSCTS: 1 << 7,
  HOTPLUG: 1 << 8,
});

export const ErrFlag = Object.freeze({
  OVERRUN: 1 << 0,
  FRAMING: 1 << 1,
  PARITY: 1 << 2,
  BREAK: 1 << 3,
  HOST_OVERFLOW: 1 << 4,
});

export const Flush = Object.freeze({
  DRAIN_TX: 1 << 0,
  DISCARD_TX: 1 << 1,
  DISCARD_RX: 1 << 2,
});

export const PortState = Object.freeze({
  CLOSED: 0,
  OPEN: 1,
  FAULT: 2,
});

export const Parity = Object.freeze({
  NONE: 0,
  ODD: 1,
  EVEN: 2,
});

export const BackendId = Object.freeze({
  HARDWARE_UART: 0,
  PIO_USB_CDC: 1,
});

export const OPEN_FLAG_RTSCTS = 1 << 0;

/** Milliseconds of quiet before a below-threshold credit balance is returned. */
export const CREDIT_IDLE_MS = 10;

// ------------------------------ src/sysex7.js ------------------------------

// 7-in-8 MSB packing — PROTOCOL.md §2.
//
// Byte-for-byte equivalent to lib/bridge_proto/sysex7.cpp. The two are
// independent implementations on purpose: host/test/roundtrip.test.js checks
// them against the same spec vector, so a change to one that the other does
// not follow shows up as a test failure rather than as corrupt serial data.

/** Bytes on the wire needed to carry `n` raw bytes. */
export function packedLen(n) {
  return n + Math.ceil(n / 7);
}

/** Largest raw length whose packed form fits in `capacity` bytes. */
export function maxRawFor(capacity) {
  const rem = capacity % 8;
  return Math.floor(capacity / 8) * 7 + (rem === 0 ? 0 : rem - 1);
}

/**
 * @param {Uint8Array} raw
 * @returns {Uint8Array} 7-bit-safe bytes
 */
export function pack(raw) {
  const out = new Uint8Array(packedLen(raw.length));
  let w = 0;
  for (let i = 0; i < raw.length; i += 7) {
    const k = Math.min(7, raw.length - i);
    const msbAt = w++;
    let msb = 0;
    for (let j = 0; j < k; j++) {
      const b = raw[i + j];
      msb |= ((b >> 7) & 1) << j;
      out[w++] = b & 0x7f;
    }
    out[msbAt] = msb;
  }
  return out;
}

export class UnpackError extends Error {
  constructor(message) {
    super(message);
    this.name = 'UnpackError';
  }
}

/**
 * Decodes 7-in-8 packed bytes. Throws UnpackError on malformed input rather
 * than guessing — a corrupt stream must be visible, not reinterpreted.
 *
 * @param {Uint8Array} packedBytes
 * @returns {Uint8Array}
 */
export function unpack(packedBytes) {
  const n = packedBytes.length;
  const out = new Uint8Array(n); // an upper bound; sliced at the end
  let w = 0;
  let i = 0;

  while (i < n) {
    const msb = packedBytes[i++];
    if (msb & 0x80) throw new UnpackError(`MSB byte 0x${msb.toString(16)} has bit 7 set`);

    const remaining = n - i;
    if (remaining === 0) throw new UnpackError('trailing MSB byte with no data');
    const k = Math.min(7, remaining);

    // The MSB byte may only claim high bits for bytes that actually follow.
    if (k < 7 && msb >> k !== 0) {
      throw new UnpackError(`MSB byte 0x${msb.toString(16)} claims more than ${k} bytes`);
    }

    for (let j = 0; j < k; j++) {
      const b = packedBytes[i + j];
      if (b & 0x80) throw new UnpackError(`data byte 0x${b.toString(16)} has bit 7 set`);
      out[w++] = b | (((msb >> j) & 1) << 7);
    }
    i += k;
  }

  return out.subarray(0, w);
}

// ------------------------------ src/emitter.js ------------------------------

// A 30-line event emitter, so the client stays importable from a browser
// without pulling in node:events. See DECISIONS.md D3.

export class Emitter {
  #listeners = new Map();

  on(event, fn) {
    if (!this.#listeners.has(event)) this.#listeners.set(event, new Set());
    this.#listeners.get(event).add(fn);
    return this;
  }

  off(event, fn) {
    this.#listeners.get(event)?.delete(fn);
    return this;
  }

  once(event, fn) {
    const wrapper = (...args) => {
      this.off(event, wrapper);
      fn(...args);
    };
    return this.on(event, wrapper);
  }

  emit(event, ...args) {
    const set = this.#listeners.get(event);
    if (!set || set.size === 0) return false;
    // Copy first: a listener that removes itself must not disturb the walk.
    for (const fn of [...set]) fn(...args);
    return true;
  }
}

// ------------------------------ src/frame.js ------------------------------

// Frame construction and parsing — PROTOCOL.md §1, §3.
// Host-side counterpart of lib/bridge_proto/frame.cpp.

export class FrameBuilder {
  /** @param {number} cmd */
  constructor(cmd) {
    this.bytes = [SYSEX_START, MANUFACTURER_ID, MAGIC, PROTOCOL_VERSION, cmd];
  }

  u7(v) {
    this.bytes.push(v & 0x7f);
    return this;
  }

  u14(v) {
    this.bytes.push(v & 0x7f, (v >>> 7) & 0x7f);
    return this;
  }

  u21(v) {
    this.bytes.push(v & 0x7f, (v >>> 7) & 0x7f, (v >>> 14) & 0x7f);
    return this;
  }

  u32(v) {
    // >>> keeps the shift unsigned; v may legitimately have bit 31 set.
    for (let i = 0; i < 5; i++) this.bytes.push((v / 2 ** (7 * i)) & 0x7f);
    return this;
  }

  /** Appends bytes that are already 7-bit safe. */
  raw(arr) {
    for (const b of arr) {
      if (b & 0x80) throw new RangeError(`0x${b.toString(16)} is not 7-bit safe`);
      this.bytes.push(b);
    }
    return this;
  }

  /** Appends bytes through 7-in-8 packing. */
  packed(arr) {
    for (const b of pack(Uint8Array.from(arr))) this.bytes.push(b);
    return this;
  }

  /** @returns {Uint8Array} the complete SysEx message, F0 … F7 inclusive. */
  build() {
    return Uint8Array.from([...this.bytes, SYSEX_END]);
  }
}

export const ParseStatus = Object.freeze({
  OK: 'ok',
  NOT_SYSEX: 'not-sysex',
  NOT_OURS: 'not-ours',
  BAD_VERSION: 'bad-version',
  TOO_SHORT: 'too-short',
});

/**
 * Validates the header of a complete SysEx message.
 * @param {Uint8Array} msg
 * @returns {{status: string, cmd?: number, cursor?: FrameCursor}}
 */
export function parseFrame(msg) {
  const n = msg.length;
  if (n < 2 || msg[0] !== SYSEX_START || msg[n - 1] !== SYSEX_END) {
    return { status: ParseStatus.NOT_SYSEX };
  }
  if (n < HEADER_LEN + 1) {
    if (n >= 2 && msg[1] !== MANUFACTURER_ID) return { status: ParseStatus.NOT_OURS };
    if (n >= 3 && msg[2] !== MAGIC) return { status: ParseStatus.NOT_OURS };
    return { status: ParseStatus.TOO_SHORT };
  }
  if (msg[1] !== MANUFACTURER_ID || msg[2] !== MAGIC) {
    return { status: ParseStatus.NOT_OURS };
  }
  if (msg[3] !== PROTOCOL_VERSION) return { status: ParseStatus.BAD_VERSION };

  return {
    status: ParseStatus.OK,
    cmd: msg[4],
    cursor: new FrameCursor(msg.subarray(HEADER_LEN, n - 1)),
  };
}

/** Reads typed fields out of a frame payload. Throws on underrun. */
export class FrameCursor {
  /** @param {Uint8Array} payload */
  constructor(payload) {
    this.payload = payload;
    this.pos = 0;
  }

  get remaining() {
    return this.payload.length - this.pos;
  }

  #need(n) {
    if (this.remaining < n) {
      throw new RangeError(`frame underrun: wanted ${n}, have ${this.remaining}`);
    }
  }

  u7() {
    this.#need(1);
    return this.payload[this.pos++] & 0x7f;
  }

  u14() {
    this.#need(2);
    const v = (this.payload[this.pos] & 0x7f) | ((this.payload[this.pos + 1] & 0x7f) << 7);
    this.pos += 2;
    return v;
  }

  u21() {
    this.#need(3);
    const p = this.payload;
    const v =
      (p[this.pos] & 0x7f) |
      ((p[this.pos + 1] & 0x7f) << 7) |
      ((p[this.pos + 2] & 0x7f) << 14);
    this.pos += 3;
    return v;
  }

  u32() {
    this.#need(5);
    let v = 0;
    // Accumulate with multiplication: bit 31 would make | produce a negative.
    for (let i = 0; i < 5; i++) v += (this.payload[this.pos + i] & 0x7f) * 2 ** (7 * i);
    this.pos += 5;
    return v >>> 0;
  }

  /** The rest of the payload, verbatim (still 7-bit). */
  rest() {
    const v = this.payload.subarray(this.pos);
    this.pos = this.payload.length;
    return v;
  }

  /** The rest of the payload, unpacked from 7-in-8. */
  unpackRest() {
    return unpack(this.rest());
  }
}

// ------------------------------ src/client.js ------------------------------

// Host-side protocol client — PROTOCOL.md, from the other end.
//
// Takes a transport that can send and receive complete SysEx messages, and
// presents a serial-port-shaped API: open, write, read, control lines. The
// credit windowing in §6 is handled here, so callers can write more than the
// device's buffer holds and simply wait.
//
// No Node built-ins: transports live behind an interface so the same client
// works over the Web MIDI API. See DECISIONS.md D3.

export class ProtocolError extends Error {
  constructor(code, detail) {
    super(`${ERR_NAMES[code] ?? `error 0x${code.toString(16)}`} (detail ${detail})`);
    this.name = 'ProtocolError';
    this.code = code;
    this.detail = detail;
  }
}

/**
 * @typedef {object} Transport
 * @property {(msg: Uint8Array) => void} send  sends one complete SysEx message
 * @property {(cb: (msg: Uint8Array) => void) => void} onSysEx
 * @property {() => void} close
 */

export class BridgeClient extends Emitter {
  /**
   * @param {Transport} transport
   * @param {{rxBuffer?: number, maxRaw?: number, timeoutMs?: number}} [options]
   */
  constructor(transport, options = {}) {
    super();
    this.transport = transport;
    this.rxBuffer = options.rxBuffer ?? RX_BUFFER_SIZE;
    this.maxRaw = Math.min(options.maxRaw ?? MAX_DATA_RAW, MAX_DATA_RAW);
    this.timeoutMs = options.timeoutMs ?? 2000;

    /** Device capabilities, populated by hello(). */
    this.info = null;
    this.state = PortState.CLOSED;
    // Whether a far end is attached. True until a device says otherwise,
    // because a backend that cannot be unplugged never will. §5.8.
    this.attached = true;

    // Flow control, §6. `credit` is what we may still send; `freed` is what we
    // have consumed and not yet handed back.
    this.credit = 0;
    this.deviceMaxRaw = MAX_DATA_RAW;
    this.freed = 0;
    this.lastCreditAt = 0;

    this.txSeq = 0;
    this.rxSeq = 0;

    this.pendingWrites = []; // {bytes, offset, resolve, reject}
    this.rxQueue = [];       // Uint8Array chunks awaiting the application
    this.rxLength = 0;
    this.waiters = new Map(); // response cmd -> [{resolve, reject, timer}]
    this.creditTimer = null;

    transport.onSysEx((msg) => this.#onSysEx(msg));
  }

  // ---- handshake -----------------------------------------------------------

  /** Learns what the device is and what it can do. Must precede open(). */
  async hello() {
    const frame = new FrameBuilder(Cmd.HELLO).u14(this.rxBuffer).u14(this.maxRaw).build();
    const cursor = await this.#request(frame, Rsp.INFO);
    this.info = {
      protocol: cursor.u7(),
      firmware: [cursor.u7(), cursor.u7(), cursor.u7()].join('.'),
      backend: cursor.u7(),
      caps: cursor.u14(),
      maxRaw: cursor.u14(),
      rxBuffer: cursor.u14(),
      maxBaud: cursor.u32(),
    };
    this.deviceMaxRaw = Math.min(this.info.maxRaw, MAX_DATA_RAW);
    return this.info;
  }

  /**
   * @param {{baud?: number, databits?: number, parity?: number,
   *          stopbits?: number, flags?: number}} [config]
   */
  async open(config = {}) {
    if (!this.info) await this.hello();

    const baud = config.baud ?? 115200;
    const frame = new FrameBuilder(Cmd.OPEN)
      .u32(baud)
      .u7(config.databits ?? 8)
      .u7(config.parity ?? Parity.NONE)
      .u7(config.stopbits ?? 1)
      .u7(config.flags ?? 0)
      .u14(this.rxBuffer)
      .build();

    // OPEN resets both windows and both sequence counters on the device, so
    // the host has to restart from the same place. PROTOCOL.md §5.1.
    //
    // Deliberately after the reply rather than before sending the request.
    // The device may still be delivering the tail of the previous session
    // right up to the moment it processes this OPEN (§5.9), and SysEx is
    // ordered, so everything arriving before the STATUS belongs to the old
    // session. Resetting first would count those frames against the new
    // sequence and report them as a gap; resetting here discards them, which
    // is what a session boundary means.
    const status = await this.#request(frame, Rsp.STATUS);
    this.#resetSession();
    this.credit = this.info.rxBuffer;
    return this.#readStatus(status);
  }

  async close() {
    const status = await this.#request(new FrameBuilder(Cmd.CLOSE).build(), Rsp.STATUS);
    return this.#readStatus(status);
  }

  async reset() {
    const status = await this.#request(new FrameBuilder(Cmd.RESET).build(), Rsp.STATUS);
    this.#resetSession();
    return this.#readStatus(status);
  }

  // Ask the device to reset itself, and resolve when it says it will.
  //
  // Deliberately not a #request: the reply is an EVENT rather than a STATUS,
  // and what follows it is the device leaving the bus — so there is no reply
  // to wait for beyond the acknowledgement, and waiting for one would always
  // time out. The caller is reconnecting afterwards either way; this exists so
  // that "did it hear me?" has an answer. PROTOCOL.md §5.10.
  reboot({ timeoutMs = 2000 } = {}) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.off('event', onEvent);
        reject(new Error('no EVT_REBOOTING; the device did not acknowledge the reboot'));
      }, timeoutMs);
      const onEvent = ({ event, arg }) => {
        if (event !== Evt.REBOOTING) return;
        clearTimeout(timer);
        this.off('event', onEvent);
        resolve(arg);
      };
      this.on('event', onEvent);
      this.transport.send(new FrameBuilder(Cmd.REBOOT).build());
    });
  }

  async getStatus() {
    const status = await this.#request(new FrameBuilder(Cmd.GET_STATUS).build(), Rsp.STATUS);
    return this.#readStatus(status);
  }

  /**
   * Resolves once a far end is attached, immediately if one already is.
   * On a backend that cannot be unplugged this is always a no-op, so it is
   * safe to call unconditionally before open(). §5.8.
   *
   * @param {number} [timeoutMs] rejects after this long with nothing attached
   */
  async waitForAttach(timeoutMs = 30000) {
    if (!this.info) await this.hello();
    if ((this.info.caps & Cap.HOTPLUG) === 0) return;
    // Ask rather than assume: the adapter may have been plugged in before we
    // connected, in which case its EVT_ATTACH was emitted to nobody.
    await this.getStatus();
    if (this.attached) return;

    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.off('attach', onAttach);
        reject(new Error(`nothing attached to the device after ${timeoutMs} ms`));
      }, timeoutMs);
      const onAttach = () => {
        clearTimeout(timer);
        this.off('attach', onAttach);
        resolve();
      };
      this.on('attach', onAttach);
    });
  }

  /** Round-trips an opaque cookie; resolves with the latency in ms. */
  async ping() {
    const cookie = Uint8Array.from([0x01, 0x02, 0x03, 0x04]);
    const started = now();
    const cursor = await this.#request(
      new FrameBuilder(Cmd.PING).raw(cookie).build(),
      Rsp.PONG,
    );
    const echoed = cursor.rest();
    if (echoed.length !== cookie.length || echoed.some((b, i) => b !== cookie[i])) {
      throw new Error('PONG did not echo the cookie');
    }
    return now() - started;
  }

  // ---- data ----------------------------------------------------------------

  /**
   * Queues bytes for the far end. Resolves once every byte has been handed to
   * the device — which may take a while, because the window throttles us to
   * whatever the far end can actually drain.
   *
   * @param {Uint8Array | number[]} data
   * @returns {Promise<void>}
   */
  write(data) {
    if (this.state !== PortState.OPEN) {
      // Queueing here would wait on credit that only OPEN can grant, so the
      // write would hang rather than fail. Say so immediately instead.
      return Promise.reject(new Error('port is not open — call open() first'));
    }
    const bytes = data instanceof Uint8Array ? data : Uint8Array.from(data);
    return new Promise((resolve, reject) => {
      this.pendingWrites.push({ bytes, offset: 0, resolve, reject });
      this.#pumpWrites();
    });
  }

  /** Bytes received and not yet taken by read(). */
  get available() {
    return this.rxLength;
  }

  /**
   * Takes up to `max` buffered bytes (all of them by default). Returns an
   * empty array when nothing has arrived.
   * @param {number} [max]
   * @returns {Uint8Array}
   */
  read(max = Infinity) {
    const want = Math.min(max, this.rxLength);
    const out = new Uint8Array(want);
    let written = 0;
    while (written < want) {
      const head = this.rxQueue[0];
      const take = Math.min(head.length, want - written);
      out.set(head.subarray(0, take), written);
      written += take;
      if (take === head.length) this.rxQueue.shift();
      else this.rxQueue[0] = head.subarray(take);
    }
    this.rxLength -= want;
    return out;
  }

  /**
   * Waits until `count` bytes have arrived, then returns them.
   * @param {number} count
   * @param {number} [timeoutMs]
   */
  readExactly(count, timeoutMs = this.timeoutMs) {
    if (this.rxLength >= count) return Promise.resolve(this.read(count));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.off('data', check);
        reject(new Error(`timed out waiting for ${count} bytes, got ${this.rxLength}`));
      }, timeoutMs);
      const check = () => {
        if (this.rxLength < count) return;
        clearTimeout(timer);
        this.off('data', check);
        resolve(this.read(count));
      };
      this.on('data', check);
    });
  }

  // ---- control -------------------------------------------------------------

  setLines(mask, values) {
    this.transport.send(new FrameBuilder(Cmd.SET_LINES).u7(mask).u7(values).build());
  }

  async flush(what = Flush.DRAIN_TX) {
    const status = await this.#request(new FrameBuilder(Cmd.FLUSH).u7(what).build(), Rsp.STATUS);
    return this.#readStatus(status);
  }

  destroy() {
    if (this.creditTimer) clearTimeout(this.creditTimer);
    this.creditTimer = null;
    for (const list of this.waiters.values()) {
      for (const w of list) {
        clearTimeout(w.timer);
        w.reject(new Error('client destroyed'));
      }
    }
    this.waiters.clear();
    for (const w of this.pendingWrites) w.reject(new Error('client destroyed'));
    this.pendingWrites = [];
    this.transport.close();
  }

  // ---- internals -----------------------------------------------------------

  #resetSession() {
    this.txSeq = 0;
    this.rxSeq = 0;
    this.credit = 0;
    this.freed = 0;
    this.lastCreditAt = now();
    this.rxQueue = [];
    this.rxLength = 0;
  }

  #readStatus(cursor) {
    const status = {
      state: cursor.u7(),
      outLines: cursor.u7(),
      inLines: cursor.u7(),
      errFlags: cursor.u7(),
      rxCount: cursor.u21(),
      txCount: cursor.u21(),
      credit: cursor.u14(),
      // Added after the first release. A device that does not send it cannot
      // have a far end that comes and goes, so "attached" is the right answer.
      // PROTOCOL.md §5.4.
      present: cursor.remaining > 0 ? cursor.u7() === 1 : true,
    };
    this.state = status.state;
    this.attached = status.present;
    return status;
  }

  #request(frame, expect) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#dropWaiter(expect, entry);
        reject(new Error(`timed out waiting for response 0x${expect.toString(16)}`));
      }, this.timeoutMs);
      const entry = { resolve, reject, timer };
      if (!this.waiters.has(expect)) this.waiters.set(expect, []);
      this.waiters.get(expect).push(entry);
      this.transport.send(frame);
    });
  }

  #dropWaiter(cmd, entry) {
    const list = this.waiters.get(cmd);
    if (!list) return;
    const i = list.indexOf(entry);
    if (i >= 0) list.splice(i, 1);
  }

  #resolveWaiter(cmd, cursor) {
    const list = this.waiters.get(cmd);
    if (!list || list.length === 0) return false;
    const entry = list.shift();
    clearTimeout(entry.timer);
    entry.resolve(cursor);
    return true;
  }

  /** Fails every queued write — the bytes are not going anywhere. */
  #failWrites(error) {
    const jobs = this.pendingWrites;
    this.pendingWrites = [];
    for (const job of jobs) job.reject(error);
  }

  /** Fails every outstanding request — used when the device reports an error. */
  #failWaiters(error) {
    for (const list of this.waiters.values()) {
      while (list.length) {
        const entry = list.shift();
        clearTimeout(entry.timer);
        entry.reject(error);
      }
    }
  }

  #onSysEx(msg) {
    const parsed = parseFrame(msg);
    if (parsed.status !== ParseStatus.OK) {
      // Other devices share the MIDI bus; only our own malformed frames are
      // worth reporting.
      if (parsed.status !== ParseStatus.NOT_OURS && parsed.status !== ParseStatus.NOT_SYSEX) {
        this.emit('warning', `ignoring frame: ${parsed.status}`);
      }
      return;
    }

    const { cmd, cursor } = parsed;
    switch (cmd) {
      case Rsp.INFO:
      case Rsp.STATUS:
      case Rsp.PONG:
        this.#resolveWaiter(cmd, cursor);
        break;

      case Rsp.DATA:
        this.#onData(cursor);
        break;

      case Rsp.CREDIT:
        this.credit += cursor.u14();
        this.#pumpWrites();
        break;

      case Rsp.ERROR: {
        const error = new ProtocolError(cursor.u7(), cursor.u7());
        this.emit('error', error);
        // A sequence gap is reported and recovered from; anything else means a
        // caller is waiting on something that will not arrive.
        if (error.code !== Err.SEQ) this.#failWaiters(error);
        // These say the write path itself is dead. Without this a write to a
        // closed port would sit in the queue for ever, waiting on credit that
        // is never coming.
        if (error.code === Err.NOT_OPEN || error.code === Err.BACKEND) {
          this.#failWrites(error);
        }
        break;
      }

      case Rsp.EVENT: {
        const event = cursor.u7();
        const arg = cursor.u7();
        this.emit('event', { event, arg });
        if (event === Evt.LINES) this.emit('lines', arg);
        if (event === Evt.ATTACH) {
          this.attached = true;
          // The device clears its own fault when a far end reappears (§5.8),
          // so a client left at FAULT disagrees with a device already back at
          // CLOSED — and goes on refusing writes on the strength of a fault
          // that is over. Closed, not open: attaching means there is a port to
          // open, not that one has been opened.
          if (this.state === PortState.FAULT) this.state = PortState.CLOSED;
          this.emit('attach', arg);
        }
        if (event === Evt.REBOOTING) {
          // The device is about to leave the bus, so everything this client
          // believes about a port is about to stop being true. Say so now
          // rather than let callers discover it as a string of timeouts.
          this.attached = false;
          this.state = PortState.FAULT;
          this.emit('rebooting', arg);
        }
        if (event === Evt.DETACH) {
          this.attached = false;
          this.state = PortState.FAULT;
          // The device faults the port on detach, so nothing queued for the
          // far end will ever be granted credit. Same reasoning as ERR_NOT_OPEN
          // above: fail it now rather than leave a caller waiting for ever.
          this.#failWrites(new ProtocolError(Err.BACKEND, arg));
          this.emit('detach', arg);
        }
        break;
      }

      default:
        this.emit('warning', `unexpected response 0x${cmd.toString(16)}`);
    }
  }

  #onData(cursor) {
    const seq = cursor.u7();
    if (seq !== this.rxSeq) {
      // Report and resynchronise, mirroring the device's behaviour in §7 —
      // the payload is still good, only our count of what came before is not.
      this.emit('warning', `DATA sequence gap: expected ${this.rxSeq}, got ${seq}`);
      this.emit('gap', { expected: this.rxSeq, got: seq });
    }
    this.rxSeq = (seq + 1) & SEQ_MASK;

    const bytes = cursor.unpackRest();
    if (bytes.length === 0) return;
    this.rxQueue.push(bytes);
    this.rxLength += bytes.length;

    // We buffer in JS, so the bytes are "drained" the moment they arrive; the
    // batching rule below is what keeps CREDIT from flooding the bus.
    this.freed += bytes.length;
    this.#maybeReturnCredit();
    this.emit('data', bytes);
  }

  #maybeReturnCredit() {
    if (this.freed === 0) return;
    if (this.freed >= this.rxBuffer / 2) {
      this.#returnCredit();
      return;
    }
    if (this.creditTimer) return;
    // Deliberately not unref'd. This timer carries the CREDIT that unblocks
    // the device's next DATA frame, so it is protocol traffic, not
    // housekeeping: letting Node exit while it is pending strands a transfer
    // with both ends waiting on the other. destroy() is what clears it.
    this.creditTimer = setTimeout(() => {
      this.creditTimer = null;
      if (this.freed > 0) this.#returnCredit();
    }, CREDIT_IDLE_MS);
  }

  #returnCredit() {
    const delta = this.freed;
    this.freed = 0;
    this.lastCreditAt = now();
    if (this.creditTimer) {
      clearTimeout(this.creditTimer);
      this.creditTimer = null;
    }
    this.transport.send(new FrameBuilder(Cmd.CREDIT).u14(delta).build());
  }

  #pumpWrites() {
    while (this.pendingWrites.length > 0) {
      const job = this.pendingWrites[0];
      const left = job.bytes.length - job.offset;
      const chunk = Math.min(left, this.deviceMaxRaw, this.credit);
      if (chunk <= 0) return; // out of window; a CREDIT will restart us

      const slice = job.bytes.subarray(job.offset, job.offset + chunk);
      this.transport.send(new FrameBuilder(Cmd.DATA).u7(this.txSeq).packed(slice).build());
      this.txSeq = (this.txSeq + 1) & SEQ_MASK;
      this.credit -= chunk;
      job.offset += chunk;

      if (job.offset >= job.bytes.length) {
        this.pendingWrites.shift();
        job.resolve();
      }
    }
  }
}

function now() {
  return typeof performance !== 'undefined' ? performance.now() : Date.now();
}

// ------------------------------ src/transport-webmidi.js ------------------------------

// Browser transport: the Web MIDI API (navigator.requestMIDIAccess).
//
// The browser counterpart of transport-node.js: turns a MIDIInput/MIDIOutput
// pair into the send/onSysEx/close interface BridgeClient expects. Also owns
// port discovery, which Web MIDI makes harder than CoreMIDI does: there is no
// per-device chooser — permission covers the whole MIDI system — so the right
// port has to be *found*, not picked. Name matching narrows the candidates and
// a HELLO probe confirms one, because port names cannot be trusted (CoreMIDI
// caches them by VID/PID; see FINDINGS.md).
//
// Plain ES module with no Node built-ins, same rule as the rest of src/
// (DECISIONS.md D3). Nothing here touches `navigator` — callers hand in the
// MIDIAccess, so the module also loads (and its discovery logic tests) in Node.

/** Same candidates, same order, as transport-node.js — and for the same
 *  CoreMIDI-name-cache reason. Kept as a separate constant because a browser
 *  bundle must not import the RtMidi module just to share a string. */
export const DEFAULT_MIDI_PORT_MATCH = 'uart bridge,bridge,pico,rp2040';

// A SysEx message the spec says arrives in one MIDIMessageEvent, but that some
// implementations have split across events. Reassembly is bounded so a lost F7
// cannot grow a buffer for ever; our own frames top out at 154 bytes.
const MAX_SYSEX_BYTES = 64 * 1024;

/** Swallows the rejection of a MIDIPort open/close promise — the port may
 *  already be gone, which is exactly when we are closing it. */
function settle(maybePromise) {
  if (maybePromise && typeof maybePromise.catch === 'function') {
    maybePromise.catch(() => {});
  }
}

export class WebMidiTransport {
  /**
   * @param {MIDIInput} input
   * @param {MIDIOutput} output
   */
  constructor(input, output) {
    this.input = input;
    this.output = output;
    this.handler = null;
    this.disconnectHandler = null;
    this.partial = null; // reassembly buffer for a SysEx split across events

    // Attaching the handler implicitly opens the input; the first send()
    // implicitly opens the output. No explicit open() round trip needed.
    input.onmidimessage = (event) => this.#onMessage(event.data);
    input.onstatechange = () => this.#checkAlive();
    output.onstatechange = () => this.#checkAlive();
  }

  /** @param {Uint8Array} msg one complete SysEx message, F0…F7 inclusive */
  send(msg) {
    this.output.send(msg);
  }

  /** @param {(msg: Uint8Array) => void} cb */
  onSysEx(cb) {
    this.handler = cb;
  }

  /** Called at most once, when either MIDI port physically disappears. The
   *  Transport interface does not require this; the Web Serial wrapper checks
   *  for it, so a yanked cable errors the stream instead of going silent. */
  onDisconnect(cb) {
    this.disconnectHandler = cb;
  }

  close() {
    this.handler = null;
    this.disconnectHandler = null;
    this.partial = null;
    this.input.onmidimessage = null;
    this.input.onstatechange = null;
    this.output.onstatechange = null;
    // Release the device for the next user; harmless if already closed.
    try {
      settle(this.input.close());
    } catch {
      // Port already invalid.
    }
    try {
      settle(this.output.close());
    } catch {
      // Port already invalid.
    }
  }

  #checkAlive() {
    if (this.input.state !== 'disconnected' && this.output.state !== 'disconnected') return;
    const cb = this.disconnectHandler;
    this.disconnectHandler = null;
    cb?.(new Error('MIDI device disconnected'));
  }

  #onMessage(data) {
    if (!data || data.length === 0) return;

    if (this.partial) {
      // Real-time messages (F8–FF) may legally interleave a SysEx transfer.
      if (data[0] >= 0xf8) return;
      this.#append(data);
      return;
    }

    if (data[0] !== 0xf0) return; // only SysEx carries the protocol
    if (data[data.length - 1] === 0xf7) {
      this.handler?.(data instanceof Uint8Array ? data : Uint8Array.from(data));
      return;
    }
    // Per spec one MIDIMessageEvent is one complete message, but a SysEx that
    // arrives split is cheap to stitch and expensive to debug.
    this.partial = data instanceof Uint8Array ? data.slice() : Uint8Array.from(data);
  }

  #append(chunk) {
    const end = chunk.indexOf(0xf7);
    const take = end < 0 ? chunk.length : end + 1;
    if (this.partial.length + take > MAX_SYSEX_BYTES) {
      this.partial = null; // corrupt stream — drop it rather than grow for ever
      return;
    }
    const joined = new Uint8Array(this.partial.length + take);
    joined.set(this.partial, 0);
    joined.set(chunk.subarray(0, take), this.partial.length);
    if (end < 0) {
      this.partial = joined;
      return;
    }
    this.partial = null;
    this.handler?.(joined);
  }
}

/** @returns {{inputs: string[], outputs: string[]}} port names, for error text */
export function listMidiPorts(access) {
  return {
    inputs: [...access.inputs.values()].map((p) => p.name || p.id || '(unnamed)'),
    outputs: [...access.outputs.values()].map((p) => p.name || p.id || '(unnamed)'),
  };
}

/**
 * Asks one input/output pair whether it speaks the protocol: sends HELLO,
 * waits for INFO. Anything else on that pair — a synth's own SysEx included —
 * is ignored until the timeout.
 *
 * @returns {Promise<object|null>} the parsed INFO, or null on timeout
 */
export function probeBridge(input, output, timeoutMs = 500) {
  return new Promise((resolve) => {
    const transport = new WebMidiTransport(input, output);
    const finish = (result) => {
      clearTimeout(timer);
      transport.close();
      resolve(result);
    };
    const timer = setTimeout(() => finish(null), timeoutMs);
    transport.onSysEx((msg) => {
      const parsed = parseFrame(msg);
      if (parsed.status !== ParseStatus.OK || parsed.cmd !== Rsp.INFO) return;
      try {
        finish(readInfo(parsed.cursor));
      } catch {
        finish(null); // an INFO that does not parse is not our device
      }
    });
    try {
      transport.send(
        new FrameBuilder(Cmd.HELLO).u14(RX_BUFFER_SIZE).u14(MAX_DATA_RAW).build(),
      );
    } catch {
      finish(null); // the port refused the send — it cannot be the bridge
    }
  });
}

/** INFO payload → object, PROTOCOL.md §5.2. Field order is normative. */
function readInfo(cursor) {
  return {
    protocol: cursor.u7(),
    firmware: [cursor.u7(), cursor.u7(), cursor.u7()].join('.'),
    backend: cursor.u7(),
    caps: cursor.u14(),
    maxRaw: cursor.u14(),
    rxBuffer: cursor.u14(),
    maxBaud: cursor.u32(),
  };
}

/**
 * Finds the bridge among the granted MIDI ports: same-name input/output pairs
 * first (a device's two directions carry the same product string), then pairs
 * where both names match a needle, ordered by needle priority — and every
 * candidate must answer the HELLO probe before it is believed.
 *
 * @param {MIDIAccess} access
 * @param {{match?: string, probeTimeoutMs?: number, maxProbes?: number}} [options]
 * @returns {Promise<{input: MIDIInput, output: MIDIOutput, info: object}|null>}
 */
export async function findBridgePort(access, options = {}) {
  const match = options.match ?? DEFAULT_MIDI_PORT_MATCH;
  const probeTimeoutMs = options.probeTimeoutMs ?? 500;
  const maxProbes = options.maxProbes ?? 8;

  const needles = match
    .split(',')
    .map((s) => s.trim().toLowerCase())
    .filter(Boolean);
  const rank = (name) => {
    const lower = (name || '').toLowerCase();
    const hit = needles.findIndex((needle) => lower.includes(needle));
    return hit < 0 ? needles.length : hit;
  };

  const inputs = [...access.inputs.values()];
  const outputs = [...access.outputs.values()];

  const pairs = [];
  const seen = new Set();
  const consider = (input, output) => {
    const key = `${input.id ?? input.name} ${output.id ?? output.name}`;
    if (seen.has(key)) return;
    seen.add(key);
    pairs.push({ input, output, rank: Math.min(rank(input.name), rank(output.name)) });
  };

  for (const output of outputs) {
    const input = inputs.find((p) => p.name === output.name);
    if (input) consider(input, output);
  }
  // Cross pairs cover an OS that names the two directions differently, but
  // only among name matches — probing every combination on a busy MIDI setup
  // would take pairs × timeout to fail.
  for (const output of outputs) {
    if (rank(output.name) >= needles.length) continue;
    for (const input of inputs) {
      if (rank(input.name) < needles.length) consider(input, output);
    }
  }

  pairs.sort((a, b) => a.rank - b.rank); // stable: same-name pairs stay first

  for (const { input, output } of pairs.slice(0, maxProbes)) {
    const info = await probeBridge(input, output, probeTimeoutMs);
    if (info) return { input, output, info };
  }
  return null;
}

// ------------------------------ src/webserial.js ------------------------------

// A Web Serial-shaped port over the Web MIDI API.
//
// For applications written against navigator.serial — webchirp's
// BrowserSerialBridge among them — that need to reach the bridge from a
// browser that only sees a MIDI device. `createMidiBridgeSerial()` returns a
// provider with the same `requestPort()` contract as webchirp's
// `createWebUsbSerial()`, and the port it yields implements the subset of
// `SerialPort` those drivers agree on: open, readable, writable, setSignals,
// getSignals, getInfo, close, forget.
//
// Semantics, where MIDI forces a choice:
//  - `writable` has real end-to-end backpressure: a write resolves only when
//    the device has taken the bytes, paced by the credit window (§6).
//  - `readable` does not push backpressure to the device — BridgeClient
//    returns credit on arrival, so a slow consumer buffers in JS. Bounded in
//    practice by what the far end can say at ≤460800 baud.
//  - Any data loss is fatal: a device-reported ERROR or a receive-side
//    sequence gap errors `readable` rather than letting a byte stream that
//    silently lost bytes masquerade as a serial cable. Reopen to recover.
//  - getInfo() has no USB VID/PID to give — Web MIDI does not expose them.
//    It reports the MIDI port name and the device's INFO instead.

const PARITY_BY_NAME = {
  none: Parity.NONE,
  odd: Parity.ODD,
  even: Parity.EVEN,
};

export function hasWebMidi() {
  return typeof navigator !== 'undefined' && 'requestMIDIAccess' in navigator;
}

export class MidiBridgeSerialPort {
  /**
   * @param {{createTransport: () => import('./client.js').Transport,
   *          name?: string, info?: object|null, timeoutMs?: number}} options
   *   `createTransport` is called on every open(), so a close/reopen cycle
   *   gets fresh state. `info` is the INFO the discovery probe already
   *   collected, so getInfo() can answer before the first open().
   */
  constructor({ createTransport, name = '', info = null, timeoutMs = 1000 }) {
    this.createTransport = createTransport;
    this.name = name;
    this.info = info;
    this.timeoutMs = timeoutMs;

    this.client = null;
    this.readable = null;
    this.writable = null;

    this.#closed = true;
  }

  #closed;
  #fatal = null;
  #wakers = new Set();

  /** No USB identity to report; see the header. */
  getInfo() {
    return { midiPortName: this.name, bridge: this.info };
  }

  /**
   * @param {{baudRate: number, dataBits?: number, stopBits?: number,
   *          parity?: 'none'|'odd'|'even',
   *          flowControl?: 'none'|'hardware'}} options
   */
  async open(options = {}) {
    if (this.client) {
      throw new DOMException('The port is already open.', 'InvalidStateError');
    }
    const baudRate = Number(options.baudRate);
    if (!Number.isInteger(baudRate) || baudRate <= 0) {
      throw new TypeError(`invalid baudRate: ${options.baudRate}`);
    }
    const parityName = options.parity ?? 'none';
    if (!(parityName in PARITY_BY_NAME)) {
      throw new TypeError(`invalid parity: ${options.parity}`);
    }

    const transport = this.createTransport();
    const client = new BridgeClient(transport, { timeoutMs: this.timeoutMs });
    try {
      await client.hello();
      await client.open({
        baud: baudRate,
        databits: options.dataBits ?? 8,
        parity: PARITY_BY_NAME[parityName],
        stopbits: options.stopBits ?? 1,
        flags: options.flowControl === 'hardware' ? OPEN_FLAG_RTSCTS : 0,
      });
    } catch (error) {
      client.destroy(); // destroy() also closes the transport
      throw error;
    }

    this.client = client;
    this.info = client.info;
    this.#closed = false;
    this.#fatal = null;

    client.on('data', () => this.#wake());
    // Losing bytes disqualifies a serial cable; see the header. SEQ resyncs
    // the counters, but the bytes it counted are still gone.
    client.on('error', (error) => this.#fail(error));
    client.on('gap', ({ expected, got }) =>
      this.#fail(new Error(`bytes lost in transit (expected seq ${expected}, got ${got})`)),
    );
    transport.onDisconnect?.((error) => this.#fail(error));

    this.readable = this.#makeReadable(client);
    this.writable = this.#makeWritable(client);
  }

  async close() {
    const client = this.client;
    if (!client) return;
    this.client = null;
    this.readable = null;
    this.writable = null;
    this.#closed = true;
    this.#wake();
    try {
      await client.close(); // tell the firmware; it may already be unplugged
    } catch {
      // The transport-level teardown below is what actually matters.
    }
    client.destroy(); // clears the credit timer, rejects stragglers, closes transport
  }

  /** Web Serial has this for revoking permission; MIDI permission is not
   *  per-port, so there is nothing to revoke. Kept for interface parity. */
  async forget() {}

  /** @param {{dataTerminalReady?: boolean, requestToSend?: boolean,
   *           break?: boolean}} signals */
  async setSignals(signals = {}) {
    if (!this.client) {
      throw new DOMException('The port is not open.', 'InvalidStateError');
    }
    let mask = 0;
    let values = 0;
    const line = (key, bit) => {
      if (!(key in signals)) return;
      mask |= bit;
      if (signals[key]) values |= bit;
    };
    line('dataTerminalReady', Line.DTR);
    line('requestToSend', Line.RTS);
    line('break', Line.BREAK);
    if (mask === 0) return;
    this.client.setLines(mask, values);
  }

  async getSignals() {
    if (!this.client) {
      throw new DOMException('The port is not open.', 'InvalidStateError');
    }
    const status = await this.client.getStatus();
    return {
      clearToSend: Boolean(status.inLines & InLine.CTS),
      dataSetReady: Boolean(status.inLines & InLine.DSR),
      dataCarrierDetect: Boolean(status.inLines & InLine.DCD),
      ringIndicator: Boolean(status.inLines & InLine.RI),
    };
  }

  // ---- internals -----------------------------------------------------------

  #fail(error) {
    if (this.#fatal || this.#closed) return;
    this.#fatal = error;
    this.#wake();
  }

  #wake() {
    const wakers = [...this.#wakers];
    this.#wakers.clear();
    for (const resolve of wakers) resolve();
  }

  #waitForWake() {
    return new Promise((resolve) => this.#wakers.add(resolve));
  }

  #makeReadable(client) {
    return new ReadableStream({
      // pull must not resolve without enqueuing (or ending the stream): a pull
      // that returns empty-handed is never invoked again.
      pull: async (controller) => {
        while (true) {
          if (this.#fatal) {
            controller.error(this.#fatal);
            return;
          }
          if (this.#closed) {
            try {
              controller.close();
            } catch {
              // cancel() already closed it.
            }
            return;
          }
          if (client.available > 0) {
            controller.enqueue(client.read());
            return;
          }
          await this.#waitForWake();
        }
      },
      cancel: () => {
        this.#closed = true;
        this.#wake();
      },
    });
  }

  #makeWritable(client) {
    return new WritableStream({
      write: async (chunk) => {
        if (this.#fatal) throw this.#fatal;
        const bytes =
          chunk instanceof Uint8Array
            ? chunk
            : chunk instanceof ArrayBuffer
              ? new Uint8Array(chunk)
              : new Uint8Array(chunk.buffer, chunk.byteOffset ?? 0, chunk.byteLength);
        // Resolves when the device has the bytes — the credit window is the
        // stream's backpressure.
        await client.write(bytes);
      },
    });
  }
}

/**
 * A Web Serial-shaped provider, interchangeable with `navigator.serial` and
 * webchirp's `createWebUsbSerial()` where only `requestPort()` is used.
 *
 * `requestPort()` asks for MIDI access (the browser prompts, `sysex: true`),
 * then finds the bridge by name and HELLO probe. Web MIDI has no per-device
 * chooser, so unlike the WebUSB path there is nothing for the user to pick —
 * the probe is what guarantees the port returned is really the bridge.
 *
 * @param {{match?: string, probeTimeoutMs?: number, timeoutMs?: number,
 *          requestMIDIAccess?: (options: object) => Promise<MIDIAccess>}} [options]
 *   `requestMIDIAccess` is injectable for tests; the default uses the browser's.
 */
export function createMidiBridgeSerial(options = {}) {
  const {
    match = DEFAULT_MIDI_PORT_MATCH,
    probeTimeoutMs = 500,
    timeoutMs,
    requestMIDIAccess,
  } = options;

  return {
    async requestPort() {
      const request =
        requestMIDIAccess ??
        ((midiOptions) => {
          if (!hasWebMidi()) {
            throw new Error(
              'Web MIDI is not supported in this browser (iOS Safari has no Web MIDI).',
            );
          }
          return navigator.requestMIDIAccess(midiOptions);
        });

      const access = await request({ sysex: true });
      const found = await findBridgePort(access, { match, probeTimeoutMs });
      if (!found) {
        const { inputs, outputs } = listMidiPorts(access);
        throw new Error(
          `no MIDI device answered the bridge handshake (looked for "${match}").\n` +
            `  inputs:  ${inputs.length ? inputs.join(', ') : '(none)'}\n` +
            `  outputs: ${outputs.length ? outputs.join(', ') : '(none)'}`,
        );
      }
      return new MidiBridgeSerialPort({
        createTransport: () => new WebMidiTransport(found.input, found.output),
        name: found.output.name || found.input.name || 'MIDI bridge',
        info: found.info,
        timeoutMs,
      });
    },
  };
}
