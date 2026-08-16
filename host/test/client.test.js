// BridgeClient against the software device model. Covers the parts of the
// protocol the host side owns: chunking to the device's limits, respecting the
// credit window, returning credit, and surfacing errors as rejections.

import assert from 'node:assert/strict';
import test from 'node:test';

import { BridgeClient, ProtocolError } from '../src/client.js';
import {
  BackendId,
  Cap,
  Cmd,
  Err,
  MAX_DATA_RAW,
  Parity,
  PortState,
  Rsp,
} from '../src/constants.js';
import { FakeDevice } from '../src/fake-device.js';
import { ParseStatus, parseFrame } from '../src/frame.js';

function makeRandom(seed = 0xbadc0de) {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return (state >>> 24) & 0xff;
  };
}

/** Wraps a device so the frames crossing the link can be inspected. */
function tapped(device) {
  const sent = [];
  const received = [];
  const transport = {
    send(msg) {
      sent.push(msg);
      device.send(msg);
    },
    onSysEx(cb) {
      device.onSysEx((msg) => {
        received.push(msg);
        cb(msg);
      });
    },
    close: () => device.close(),
  };
  return { transport, sent, received };
}

function commandsOf(frames, cmd) {
  return frames
    .map((f) => parseFrame(f))
    .filter((p) => p.status === ParseStatus.OK && p.cmd === cmd);
}

test('hello reports what the device is', async () => {
  const device = new FakeDevice();
  const client = new BridgeClient(device);
  const info = await client.hello();

  assert.equal(info.protocol, 1);
  assert.equal(info.firmware, '0.1.0');
  assert.equal(info.maxRaw, MAX_DATA_RAW);
  assert.equal(info.rxBuffer, 2048);
  assert.equal(info.maxBaud, 921600);
  client.destroy();
});

test('open reports the port as open', async () => {
  const device = new FakeDevice();
  const client = new BridgeClient(device);
  const status = await client.open({ baud: 115200 });
  assert.equal(status.state, PortState.OPEN);
  assert.equal(client.state, PortState.OPEN);
  client.destroy();
});

test('open rejects a baud the device will not take', async () => {
  const device = new FakeDevice({ maxBaud: 115200 });
  const client = new BridgeClient(device);
  await client.hello();
  await assert.rejects(() => client.open({ baud: 2000000 }), (e) => {
    assert.ok(e instanceof ProtocolError);
    assert.equal(e.code, Err.BAD_PARAM);
    assert.equal(e.detail, 0); // field 0 is baud
    return true;
  });
  client.destroy();
});

test('a byte written comes back through the loopback', async () => {
  const device = new FakeDevice();
  const client = new BridgeClient(device);
  await client.open();

  await client.write(Uint8Array.from([0x41]));
  const back = await client.readExactly(1);
  assert.deepEqual(back, Uint8Array.from([0x41]));
  client.destroy();
});

test('every byte value survives the round trip', async () => {
  const device = new FakeDevice();
  const client = new BridgeClient(device);
  await client.open();

  // 0x00–0xFF is the whole point: SysEx cannot carry the top half directly.
  const payload = Uint8Array.from({ length: 256 }, (_, i) => i);
  await client.write(payload);
  const back = await client.readExactly(payload.length);
  assert.deepEqual(back, payload);
  client.destroy();
});

test('writes are chunked to the device limit and numbered in order', async () => {
  const device = new FakeDevice();
  const { transport, sent } = tapped(device);
  const client = new BridgeClient(transport);
  await client.open();
  sent.length = 0;

  const next = makeRandom();
  const payload = Uint8Array.from({ length: 1000 }, next);
  await client.write(payload);

  const dataFrames = commandsOf(sent, Cmd.DATA);
  assert.equal(dataFrames.length, Math.ceil(1000 / MAX_DATA_RAW));

  let expectSeq = 0;
  let reassembled = [];
  for (const frame of dataFrames) {
    assert.equal(frame.cursor.u7(), expectSeq);
    expectSeq = (expectSeq + 1) & 0x7f;
    const bytes = frame.cursor.unpackRest();
    assert.ok(bytes.length <= MAX_DATA_RAW, 'chunk exceeded the device limit');
    reassembled.push(...bytes);
  }
  assert.deepEqual(Uint8Array.from(reassembled), payload);
  client.destroy();
});

test('a smaller device frame limit is honoured', async () => {
  const device = new FakeDevice({ maxRaw: 32 });
  const { transport, sent } = tapped(device);
  const client = new BridgeClient(transport);
  await client.open();
  sent.length = 0;

  await client.write(new Uint8Array(200));
  for (const frame of commandsOf(sent, Cmd.DATA)) {
    frame.cursor.u7();
    assert.ok(frame.cursor.unpackRest().length <= 32);
  }
  client.destroy();
});

test('the client stops at the window and resumes on credit', async () => {
  // A device with a tiny buffer and no credit returned until told to.
  const device = new FakeDevice({ rxBuffer: 64 });
  const { transport, sent } = tapped(device);
  const client = new BridgeClient(transport);
  await client.open();
  sent.length = 0;

  const payload = Uint8Array.from({ length: 512 }, (_, i) => i & 0xff);
  const done = client.write(payload);

  // The very first burst cannot exceed the advertised window.
  const firstBurst = commandsOf(sent, Cmd.DATA).reduce((total, f) => {
    f.cursor.u7();
    return total + f.cursor.unpackRest().length;
  }, 0);
  assert.ok(firstBurst <= 64, `sent ${firstBurst} bytes into a 64-byte window`);

  // It still completes: credit comes back as the device drains.
  await done;
  const back = await client.readExactly(payload.length);
  assert.deepEqual(back, payload);
  client.destroy();
});

test('the client returns credit as it consumes data', async () => {
  const device = new FakeDevice();
  const { transport, sent } = tapped(device);
  const client = new BridgeClient(transport, { rxBuffer: 256 });
  await client.open();
  sent.length = 0;

  await client.write(new Uint8Array(1024));
  await client.readExactly(1024);

  const credits = commandsOf(sent, Cmd.CREDIT);
  assert.ok(credits.length > 0, 'client never returned any credit');
  const total = credits.reduce((sum, f) => sum + f.cursor.u14(), 0);
  assert.equal(total, 1024, 'credit returned must match bytes consumed');
  client.destroy();
});

test('a write before open fails instead of hanging on credit', async () => {
  const device = new FakeDevice();
  const client = new BridgeClient(device);
  await client.hello();

  // Queueing would block on credit that only OPEN grants, so this must fail
  // straight away rather than wait for ever.
  await assert.rejects(() => client.write(Uint8Array.from([1, 2, 3])), /not open/);
  client.destroy();
});

test('a queued write is failed when the device says the port went away', async () => {
  const device = new FakeDevice({ rxBuffer: 8 });
  const client = new BridgeClient(device);
  await client.open();
  client.on('error', () => {}); // the rejection below is the assertion

  // More than the window, so part of it is still queued when the port closes.
  const pending = client.write(new Uint8Array(4096));
  device.state = PortState.CLOSED;

  await assert.rejects(() => pending, (e) => {
    assert.ok(e instanceof ProtocolError);
    assert.equal(e.code, Err.NOT_OPEN);
    return true;
  });
  client.destroy();
});

test('ping round-trips and reports a latency', async () => {
  const device = new FakeDevice({ latencyMs: 2 });
  const client = new BridgeClient(device);
  await client.open();
  const ms = await client.ping();
  assert.ok(ms >= 0 && ms < 1000, `implausible latency ${ms}`);
  client.destroy();
});

test('a request with no answer times out rather than hanging', async () => {
  const silent = { send() {}, onSysEx() {}, close() {} };
  const client = new BridgeClient(silent, { timeoutMs: 50 });
  await assert.rejects(() => client.hello(), /timed out/);
  client.destroy();
});

test('reopening restarts the sequence counters', async () => {
  const device = new FakeDevice();
  const { transport, sent } = tapped(device);
  const client = new BridgeClient(transport);
  await client.open();
  await client.write(new Uint8Array(300));
  await client.readExactly(300);

  sent.length = 0;
  await client.open();
  await client.write(Uint8Array.from([0xaa]));

  const first = commandsOf(sent, Cmd.DATA)[0];
  assert.equal(first.cursor.u7(), 0, 'sequence did not restart at OPEN');
  client.destroy();
});

test('read takes only what was asked for', async () => {
  const device = new FakeDevice();
  const client = new BridgeClient(device);
  await client.open();

  await client.write(Uint8Array.from([1, 2, 3, 4, 5]));
  await client.readExactly(5);
  assert.equal(client.available, 0);

  await client.write(Uint8Array.from([6, 7, 8]));
  const two = await client.readExactly(2);
  assert.deepEqual(two, Uint8Array.from([6, 7]));
  assert.equal(client.available, 1);
  assert.deepEqual(client.read(), Uint8Array.from([8]));
  client.destroy();
});

// Regression: the credit and wire timers used to be unref'd, so once a
// transfer was throttled to a baud rate and both ends were waiting on a timer,
// Node saw an empty event loop and exited mid-transfer. Silently — exit code 0,
// no rows printed. Anything that carries protocol traffic has to hold the loop.
test('a throttled transfer completes instead of the loop draining', async () => {
  const device = new FakeDevice({ baudLimited: true });
  const client = new BridgeClient(device);
  await client.open({ baud: 9600 });

  const payload = Uint8Array.from({ length: 3000 }, (_, i) => i & 0xff);
  await client.write(payload);
  const back = await client.readExactly(payload.length, 20000);

  assert.deepEqual(back, payload);
  client.destroy();
});

test('a bulk transfer arrives intact and in order', async () => {
  const device = new FakeDevice();
  const client = new BridgeClient(device);
  await client.open({ baud: 115200 });

  const next = makeRandom(0x5eed);
  const payload = Uint8Array.from({ length: 8192 }, next);
  const warnings = [];
  client.on('warning', (w) => warnings.push(w));

  await client.write(payload);
  const back = await client.readExactly(payload.length, 10000);

  assert.deepEqual(back, payload);
  assert.deepEqual(warnings, [], `unexpected warnings: ${warnings.join('; ')}`);
  client.destroy();
});

// ---- hot-plug (PROTOCOL.md §5.8) -------------------------------------------

test('a non-hotplug device always reads as attached', async () => {
  const device = new FakeDevice();
  const client = new BridgeClient(device);

  const info = await client.hello();
  assert.equal(info.caps & Cap.HOTPLUG, 0);
  const status = await client.getStatus();
  assert.equal(status.present, true);
  assert.equal(client.attached, true);

  // And waiting for an attach on such a device is a no-op, not a 30 s stall.
  await client.waitForAttach(50);
  client.destroy();
});

test('detach faults the port and fails the writes waiting on it', async () => {
  const device = new FakeDevice({ hotplug: true });
  const client = new BridgeClient(device);
  await client.open({ baud: 115200 });

  const detached = new Promise((resolve) => client.once('detach', resolve));
  // No credit was granted, so this write is parked waiting for some — exactly
  // the case that would otherwise hang for ever once the far end is gone.
  client.credit = 0;
  const parked = client.write(Uint8Array.from([1, 2, 3]));

  device.detach();
  const arg = await detached;

  assert.equal(arg, BackendId.PIO_USB_CDC);
  assert.equal(client.attached, false);
  assert.equal(client.state, PortState.FAULT);
  await assert.rejects(parked, (e) => e instanceof ProtocolError && e.code === Err.BACKEND);
  client.destroy();
});

test('bytes received before a detach still reach the host', async () => {
  const device = new FakeDevice({ hotplug: true });
  const client = new BridgeClient(device);
  await client.open({ baud: 115200 });

  const payload = Uint8Array.from([0x00, 0x7f, 0x80, 0xff]);
  await client.write(payload);
  // Pulled out in the same breath, before the device has framed them up.
  device.detach();

  const back = await client.readExactly(payload.length, 2000);
  assert.deepEqual(back, payload);
  client.destroy();
});

test('reattaching clears the fault and the port can be reopened', async () => {
  const device = new FakeDevice({ hotplug: true });
  const client = new BridgeClient(device);
  await client.open({ baud: 115200 });

  device.detach();
  await new Promise((resolve) => client.once('detach', resolve));

  const attached = new Promise((resolve) => client.once('attach', resolve));
  device.attach();
  await attached;
  assert.equal(client.attached, true);

  const status = await client.open({ baud: 115200 });
  assert.equal(status.state, PortState.OPEN);
  assert.equal(status.present, true);

  const payload = Uint8Array.from([0xde, 0xad, 0xbe, 0xef]);
  await client.write(payload);
  assert.deepEqual(await client.readExactly(payload.length, 2000), payload);
  client.destroy();
});

test('waitForAttach returns once something is plugged in', async () => {
  const device = new FakeDevice({ hotplug: true });
  const client = new BridgeClient(device);
  await client.hello();

  // Nothing attached when the host arrives: no EVT_ATTACH was ever sent to it,
  // so the answer has to come from STATUS.
  device.present = false;
  const waiting = client.waitForAttach(2000);
  setTimeout(() => device.attach(), 20);

  await waiting;
  assert.equal(client.attached, true);
  client.destroy();
});
