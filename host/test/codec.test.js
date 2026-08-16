// Host-side codec tests, and the fixed vectors that keep the JS and the C++
// implementations from drifting apart. test/test_codec/test_main.cpp asserts
// the same bytes; if one implementation changes its bit order, one of the two
// suites goes red instead of the link quietly corrupting data.

import assert from 'node:assert/strict';
import test from 'node:test';

import { Cmd, MAX_DATA_RAW, Rsp } from '../src/constants.js';
import { FrameBuilder, ParseStatus, parseFrame } from '../src/frame.js';
import { UnpackError, maxRawFor, pack, packedLen, unpack } from '../src/sysex7.js';

// A deterministic generator: a failing case stays reproducible.
function makeRandom(seed = 0x12345678) {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return (state >>> 24) & 0xff;
  };
}

test('packedLen agrees with pack for every length', () => {
  const next = makeRandom();
  for (let n = 0; n <= 300; n++) {
    const raw = Uint8Array.from({ length: n }, next);
    assert.equal(pack(raw).length, packedLen(n));
  }
});

test('pack/unpack round-trips and stays 7-bit', () => {
  const next = makeRandom();
  for (let n = 0; n <= 300; n++) {
    const raw = Uint8Array.from({ length: n }, next);
    const packed = pack(raw);
    for (const b of packed) assert.equal(b & 0x80, 0, 'packed byte must be 7-bit');
    assert.deepEqual(unpack(packed), raw);
  }
});

test('the PROTOCOL.md §2 worked example', () => {
  const raw = Uint8Array.from([0xff, 0x00, 0x80, 0x7f]);
  assert.deepEqual(pack(raw), Uint8Array.from([0x05, 0x7f, 0x00, 0x00, 0x7f]));
  assert.deepEqual(unpack(Uint8Array.from([0x05, 0x7f, 0x00, 0x00, 0x7f])), raw);
});

test('a full 7-byte group packs to 8 bytes', () => {
  const raw = Uint8Array.from([0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86]);
  // Every byte has bit 7 set, so the MSB byte is 0b1111111.
  assert.deepEqual(
    pack(raw),
    Uint8Array.from([0x7f, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06]),
  );
});

test('empty input packs to nothing', () => {
  assert.equal(pack(new Uint8Array(0)).length, 0);
  assert.equal(unpack(new Uint8Array(0)).length, 0);
});

test('maxRawFor is the exact inverse of packedLen', () => {
  for (let cap = 0; cap <= 512; cap++) {
    const n = maxRawFor(cap);
    assert.ok(packedLen(n) <= cap, `maxRawFor(${cap}) overshot`);
    assert.ok(packedLen(n + 1) > cap, `maxRawFor(${cap}) was not maximal`);
  }
});

test('unpack rejects a data byte with bit 7 set', () => {
  assert.throws(() => unpack(Uint8Array.from([0x00, 0x41, 0xc2])), UnpackError);
});

test('unpack rejects an MSB byte claiming absent bytes', () => {
  // Two data bytes follow, so only bits 0 and 1 may be set.
  assert.throws(() => unpack(Uint8Array.from([0x04, 0x41, 0x42])), UnpackError);
});

test('unpack rejects a trailing MSB byte', () => {
  const bad = Uint8Array.from([0x00, 1, 2, 3, 4, 5, 6, 7, 0x00]);
  assert.throws(() => unpack(bad), UnpackError);
});

// ---- frames ----------------------------------------------------------------

test('integers round-trip through a frame', () => {
  const frame = new FrameBuilder(Cmd.OPEN)
    .u32(115200)
    .u7(8)
    .u14(2048)
    .u21(1234567)
    .build();

  assert.equal(frame[0], 0xf0);
  assert.equal(frame.at(-1), 0xf7);
  for (const b of frame.subarray(1, frame.length - 1)) assert.equal(b & 0x80, 0);

  const { status, cmd, cursor } = parseFrame(frame);
  assert.equal(status, ParseStatus.OK);
  assert.equal(cmd, Cmd.OPEN);
  assert.equal(cursor.u32(), 115200);
  assert.equal(cursor.u7(), 8);
  assert.equal(cursor.u14(), 2048);
  assert.equal(cursor.u21(), 1234567);
  assert.equal(cursor.remaining, 0);
});

test('u32 survives bit 31 being set', () => {
  const frame = new FrameBuilder(Cmd.OPEN).u32(0xffffffff).build();
  const { cursor } = parseFrame(frame);
  assert.equal(cursor.u32(), 0xffffffff);
});

test('a maximum-size DATA frame fits the advertised budget', () => {
  const next = makeRandom(0xc0ffee);
  const raw = Uint8Array.from({ length: MAX_DATA_RAW }, next);
  const frame = new FrameBuilder(Rsp.DATA).u7(42).packed(raw).build();

  // 5 header + 1 seq + packedLen(128) + 1 terminator
  assert.equal(frame.length, 5 + 1 + packedLen(MAX_DATA_RAW) + 1);
  assert.equal(frame.length, 154);

  const { cursor } = parseFrame(frame);
  assert.equal(cursor.u7(), 42);
  assert.deepEqual(cursor.unpackRest(), raw);
});

test('other manufacturers are not ours', () => {
  const yamaha = Uint8Array.from([0xf0, 0x43, 0x00, 0x01, 0x02, 0xf7]);
  assert.equal(parseFrame(yamaha).status, ParseStatus.NOT_OURS);
});

test('a wrong magic byte is not ours', () => {
  const other = Uint8Array.from([0xf0, 0x7d, 0x11, 0x01, 0x02, 0xf7]);
  assert.equal(parseFrame(other).status, ParseStatus.NOT_OURS);
});

test('a future protocol version is flagged, not ignored', () => {
  const future = Uint8Array.from([0xf0, 0x7d, 0x55, 0x09, 0x02, 0xf7]);
  assert.equal(parseFrame(future).status, ParseStatus.BAD_VERSION);
});

test('non-SysEx is rejected', () => {
  assert.equal(parseFrame(Uint8Array.from([0x90, 0x40, 0x7f])).status, ParseStatus.NOT_SYSEX);
});

test('reading past the end throws rather than returning junk', () => {
  const frame = Uint8Array.from([0xf0, 0x7d, 0x55, 0x01, Cmd.CREDIT, 0x01, 0xf7]);
  const { cursor } = parseFrame(frame);
  assert.throws(() => cursor.u14(), RangeError);
});

test('the builder refuses payload bytes that are not 7-bit safe', () => {
  assert.throws(() => new FrameBuilder(Cmd.PING).raw([0x01, 0x80]), RangeError);
});
