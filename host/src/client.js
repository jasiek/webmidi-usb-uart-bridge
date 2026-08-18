// Host-side protocol client — PROTOCOL.md, from the other end.
//
// Takes a transport that can send and receive complete SysEx messages, and
// presents a serial-port-shaped API: open, write, read, control lines. The
// credit windowing in §6 is handled here, so callers can write more than the
// device's buffer holds and simply wait.
//
// No Node built-ins: transports live behind an interface so the same client
// works over the Web MIDI API. See DECISIONS.md D3.

import {
  Cap,
  Cmd,
  CREDIT_IDLE_MS,
  ERR_NAMES,
  Err,
  Evt,
  Flush,
  MAX_DATA_RAW,
  Parity,
  PortState,
  RX_BUFFER_SIZE,
  Rsp,
  SEQ_MASK,
} from './constants.js';
import { Emitter } from './emitter.js';
import { FrameBuilder, ParseStatus, parseFrame } from './frame.js';

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
