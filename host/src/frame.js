// Frame construction and parsing — PROTOCOL.md §1, §3.
// Host-side counterpart of lib/bridge_proto/frame.cpp.

import {
  HEADER_LEN,
  MAGIC,
  MANUFACTURER_ID,
  PROTOCOL_VERSION,
  SYSEX_END,
  SYSEX_START,
} from './constants.js';
import { pack, unpack } from './sysex7.js';

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
