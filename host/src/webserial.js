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

import { BridgeClient } from './client.js';
import { InLine, Line, OPEN_FLAG_RTSCTS, Parity } from './constants.js';
import {
  DEFAULT_MIDI_PORT_MATCH,
  WebMidiTransport,
  findBridgePort,
  listMidiPorts,
} from './transport-webmidi.js';

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
