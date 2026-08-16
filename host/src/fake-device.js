// A software stand-in for the Pico: implements the device side of
// PROTOCOL.md with its UART TX looped straight back to RX.
//
// Two jobs. It lets the client's flow control, chunking and sequencing be
// tested without hardware, and it lets bin/loopback.js be run — and therefore
// debugged — before anything is plugged in. It is a model of the firmware, not
// a second implementation of it: when the two disagree, the firmware and its
// own Unity tests are right.

import {
  Cmd,
  CREDIT_IDLE_MS,
  Err,
  Evt,
  MAX_DATA_RAW,
  PortState,
  RX_BUFFER_SIZE,
  Rsp,
  SEQ_MASK,
  BackendId,
  Cap,
  Flush,
} from './constants.js';
import { FrameBuilder, ParseStatus, parseFrame } from './frame.js';

const RING_CAPACITY = 4095; // matches Bridge::kBufferSlots - 1

export class FakeDevice {
  /**
   * @param {{rxBuffer?: number, maxRaw?: number, maxBaud?: number,
   *          baudLimited?: boolean, latencyMs?: number,
   *          hotplug?: boolean}} [options]
   */
  constructor(options = {}) {
    this.rxBuffer = options.rxBuffer ?? RX_BUFFER_SIZE;
    this.maxRaw = options.maxRaw ?? MAX_DATA_RAW;
    this.maxBaud = options.maxBaud ?? 921600;
    // Off by default: tests want speed, bin/loopback.js --fake wants realism.
    this.baudLimited = options.baudLimited ?? false;
    this.latencyMs = options.latencyMs ?? 0;
    // Models the phase 2 backend instead of the hardware UART: a far end that
    // can be unplugged, driven from the test with attach()/detach(). §5.8.
    this.hotplug = options.hotplug ?? false;
    this.present = true;

    this.handler = null;
    this.state = PortState.CLOSED;
    this.cfg = null;

    this.toBackend = []; // host → UART, awaiting the wire
    this.toHost = [];    // UART → host, awaiting a DATA frame
    this.hostMaxRaw = MAX_DATA_RAW;

    this.sendCredit = 0; // what we may send to the host
    this.freed = 0;      // drained, not yet credited back
    this.txSeq = 0;
    this.rxSeq = 0;

    this.rxCount = 0;
    this.txCount = 0;
    this.errFlags = 0;

    this.creditTimer = null;
    this.drainTimer = null;
    this.closed = false;
  }

  // ---- transport interface, as seen by BridgeClient ------------------------

  /** @param {Uint8Array} msg */
  send(msg) {
    if (this.closed) return;
    this.#deliver(() => this.#onSysEx(msg));
  }

  /** @param {(msg: Uint8Array) => void} cb */
  onSysEx(cb) {
    this.handler = cb;
  }

  close() {
    this.closed = true;
    if (this.creditTimer) clearTimeout(this.creditTimer);
    if (this.drainTimer) clearTimeout(this.drainTimer);
    this.creditTimer = null;
    this.drainTimer = null;
    this.handler = null;
  }

  // ---- device logic --------------------------------------------------------

  // Not unref'd: this carries real protocol traffic, and a promise waiting on
  // it must keep the process alive until it lands.
  #deliver(fn) {
    if (this.latencyMs > 0) setTimeout(fn, this.latencyMs);
    else queueMicrotask(fn);
  }

  #emit(frame) {
    if (this.closed || !this.handler) return;
    this.#deliver(() => this.handler?.(frame));
  }

  #error(code, detail = 0) {
    this.#emit(new FrameBuilder(Rsp.ERROR).u7(code).u7(detail).build());
  }

  #onSysEx(msg) {
    const parsed = parseFrame(msg);
    if (parsed.status === ParseStatus.BAD_VERSION) return this.#error(Err.VERSION, 1);
    if (parsed.status !== ParseStatus.OK) return; // not ours

    const { cmd, cursor } = parsed;
    // CREDIT included: the device goes on delivering what it received before
    // the port closed, and that delivery is credit-paced. PROTOCOL.md §4.2.
    const alwaysLegal = [
      Cmd.HELLO,
      Cmd.RESET,
      Cmd.OPEN,
      Cmd.PING,
      Cmd.GET_STATUS,
      Cmd.CREDIT,
    ];
    if (this.state !== PortState.OPEN && !alwaysLegal.includes(cmd)) {
      return this.#error(Err.NOT_OPEN);
    }

    switch (cmd) {
      case Cmd.HELLO: {
        cursor.u14(); // host rx buffer — informational until OPEN
        const hostMaxRaw = cursor.u14();
        if (hostMaxRaw > 0) this.hostMaxRaw = Math.min(hostMaxRaw, MAX_DATA_RAW);
        this.#emit(
          new FrameBuilder(Rsp.INFO)
            .u7(1)
            .u7(0)
            .u7(1)
            .u7(0)
            .u7(this.hotplug ? BackendId.PIO_USB_CDC : BackendId.HARDWARE_UART)
            .u14(
              this.hotplug
                ? Cap.DTR | Cap.RTS | Cap.HOTPLUG
                : Cap.BREAK | Cap.FLOW_RTSCTS,
            )
            .u14(this.maxRaw)
            .u14(this.rxBuffer)
            .u32(this.maxBaud)
            .build(),
        );
        break;
      }

      case Cmd.OPEN: {
        const baud = cursor.u32();
        const databits = cursor.u7();
        const parity = cursor.u7();
        const stopbits = cursor.u7();
        const flags = cursor.u7();
        const hostRx = cursor.u14();

        if (baud === 0 || baud > this.maxBaud) return this.#error(Err.BAD_PARAM, 0);
        if (databits < 5 || databits > 8) return this.#error(Err.BAD_PARAM, 1);
        if (parity > 2) return this.#error(Err.BAD_PARAM, 2);
        if (stopbits < 1 || stopbits > 2) return this.#error(Err.BAD_PARAM, 3);

        this.cfg = { baud, databits, parity, stopbits, flags };
        this.state = PortState.OPEN;
        this.#resetSession();
        this.sendCredit = hostRx;
        this.#sendStatus();
        this.#pump();
        break;
      }

      case Cmd.CLOSE:
        this.state = PortState.CLOSED;
        this.#sendStatus();
        break;

      case Cmd.RESET:
        this.state = PortState.CLOSED;
        this.#resetSession();
        this.#sendStatus();
        break;

      case Cmd.GET_STATUS:
        this.#sendStatus();
        break;

      case Cmd.PING:
        this.#emit(new FrameBuilder(Rsp.PONG).raw(cursor.rest()).build());
        break;

      case Cmd.DATA: {
        const seq = cursor.u7();
        let bytes;
        try {
          bytes = cursor.unpackRest();
        } catch {
          return this.#error(Err.BAD_ENCODING);
        }
        if (seq !== this.rxSeq) this.#error(Err.SEQ, this.rxSeq);
        this.rxSeq = (seq + 1) & SEQ_MASK;

        const room = RING_CAPACITY - this.toBackend.length;
        const taken = Math.min(room, bytes.length);
        for (let i = 0; i < taken; i++) this.toBackend.push(bytes[i]);
        if (taken < bytes.length) {
          this.#error(Err.NO_CREDIT, Math.min(127, bytes.length - taken));
        }
        this.#pump();
        break;
      }

      case Cmd.CREDIT:
        this.sendCredit += cursor.u14();
        this.#pump();
        break;

      case Cmd.SET_LINES:
        cursor.u7();
        cursor.u7();
        break;

      case Cmd.FLUSH: {
        const what = cursor.u7();
        if (what & Flush.DISCARD_TX) this.toBackend.length = 0;
        if (what & Flush.DISCARD_RX) this.toHost.length = 0;
        this.#sendStatus();
        break;
      }

      default:
        this.#error(Err.BAD_CMD, cmd);
    }
  }

  #resetSession() {
    this.toBackend = [];
    this.toHost = [];
    this.sendCredit = 0;
    this.freed = 0;
    this.txSeq = 0;
    this.rxSeq = 0;
    this.rxCount = 0;
    this.txCount = 0;
  }

  #sendStatus() {
    const errFlags = this.errFlags;
    this.errFlags = 0;
    this.#emit(
      new FrameBuilder(Rsp.STATUS)
        .u7(this.state)
        .u7(0)
        .u7(0)
        .u7(errFlags)
        .u21(this.rxCount)
        .u21(this.txCount)
        .u14(this.sendCredit)
        .u7(this.present ? 1 : 0)
        .build(),
    );
  }

  // ---- hot-plug, driven by the test ---------------------------------------

  /** Pulls the far end out from under an open port. §5.8. */
  detach() {
    if (!this.hotplug || !this.present) return;
    this.present = false;
    if (this.state === PortState.OPEN) {
      // Deliberately not clearing toHost: bytes already received stay
      // deliverable, exactly as in Bridge::pumpPresence.
      this.toBackend = [];
      this.state = PortState.FAULT;
    }
    this.#emit(new FrameBuilder(Rsp.EVENT).u7(Evt.DETACH).u7(BackendId.PIO_USB_CDC).build());
    this.#pump();
  }

  /** Plugs one back in. Does not open a port — that is the host's move. */
  attach() {
    if (!this.hotplug || this.present) return;
    this.present = true;
    if (this.state === PortState.FAULT) this.state = PortState.CLOSED;
    this.#emit(new FrameBuilder(Rsp.EVENT).u7(Evt.ATTACH).u7(BackendId.PIO_USB_CDC).build());
  }

  /** Moves bytes across the looped-back wire, then out to the host. */
  #pump() {
    if (this.state === PortState.OPEN) {
      // TX is jumpered to RX: everything written comes straight back.
      const budget = this.baudLimited ? this.#byteBudget() : this.toBackend.length;
      const moved = this.toBackend.splice(0, budget);
      if (moved.length > 0) {
        this.txCount += moved.length;
        this.rxCount += moved.length;
        this.toHost.push(...moved);
        this.freed += moved.length;
      }

      this.#maybeReturnCredit();
    }

    // Outside the state check, mirroring Bridge::poll: bytes that reached
    // toHost while the port was open are still deliverable after a CLOSE or a
    // detach, and the host keeps answering with CREDIT to pace them.
    // PROTOCOL.md §5.9. OPEN and RESET are what clear them.
    this.#drainToHost();

    // If the wire throttled us, come back for the rest.
    if (this.baudLimited && (this.toBackend.length > 0 || this.toHost.length > 0)) {
      if (!this.drainTimer) {
        // Not unref'd: this is the simulated wire still moving bytes, and a
        // caller awaiting them must keep the process alive. close() clears it.
        this.drainTimer = setTimeout(() => {
          this.drainTimer = null;
          this.#pump();
        }, 1);
      }
    }
  }

  #byteBudget() {
    // 10 bits per byte on the wire — 1 start, 8 data, 1 stop — over 1 ms.
    return Math.max(1, Math.round(this.cfg.baud / 10 / 1000));
  }

  #drainToHost() {
    while (this.toHost.length > 0) {
      const chunk = Math.min(this.toHost.length, this.hostMaxRaw, this.sendCredit);
      if (chunk <= 0) return;
      const slice = Uint8Array.from(this.toHost.splice(0, chunk));
      this.#emit(new FrameBuilder(Rsp.DATA).u7(this.txSeq).packed(slice).build());
      this.txSeq = (this.txSeq + 1) & SEQ_MASK;
      this.sendCredit -= chunk;
    }
  }

  #maybeReturnCredit() {
    if (this.freed === 0) return;
    if (this.freed >= this.rxBuffer / 2) {
      this.#returnCredit();
      return;
    }
    if (this.creditTimer) return;
    // Not unref'd, for the same reason as the client's: this CREDIT is what
    // unblocks the host's next write.
    this.creditTimer = setTimeout(() => {
      this.creditTimer = null;
      if (this.freed > 0) this.#returnCredit();
    }, CREDIT_IDLE_MS);
  }

  #returnCredit() {
    const delta = this.freed;
    this.freed = 0;
    if (this.creditTimer) {
      clearTimeout(this.creditTimer);
      this.creditTimer = null;
    }
    this.#emit(new FrameBuilder(Rsp.CREDIT).u14(delta).build());
  }
}
