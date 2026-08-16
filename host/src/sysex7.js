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
