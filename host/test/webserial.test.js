// The Web Serial-shaped wrapper against the software device model, driven the
// way webchirp's BrowserSerialBridge drives a port: open with Web Serial
// options, a reader on `readable`, a writer on `writable`, setSignals, close.
// Also proves the single-file bundle builds and behaves like the sources.

import assert from 'node:assert/strict';
import test from 'node:test';
import { pathToFileURL } from 'node:url';

import { buildBundle } from '../bin/build-webserial.js';
import { Cmd, Line, Parity, PortState } from '../src/constants.js';
import { FakeDevice } from '../src/fake-device.js';
import { ParseStatus, parseFrame } from '../src/frame.js';
import { MidiBridgeSerialPort, createMidiBridgeSerial } from '../src/webserial.js';

function makeRandom(seed = 0xbadc0de) {
  let state = seed >>> 0;
  return () => {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    return (state >>> 24) & 0xff;
  };
}

/** A port whose transport is a fresh FakeDevice per open(). */
function makePort(deviceOptions = {}, portOptions = {}) {
  const devices = [];
  const port = new MidiBridgeSerialPort({
    createTransport: () => {
      const device = new FakeDevice(deviceOptions);
      devices.push(device);
      return device;
    },
    name: 'Fake Bridge',
    ...portOptions,
  });
  return { port, devices };
}

async function readExactly(reader, count) {
  const out = new Uint8Array(count);
  let got = 0;
  while (got < count) {
    const { value, done } = await reader.read();
    assert.ok(!done, `stream ended after ${got} of ${count} bytes`);
    out.set(value, got);
    got += value.length;
  }
  return out;
}

test('bytes round-trip through readable/writable', async () => {
  const { port } = makePort();
  await port.open({ baudRate: 115200 });

  const rand = makeRandom();
  const payload = Uint8Array.from({ length: 4096 }, rand); // > rxBuffer, so credit cycles
  const reader = port.readable.getReader();
  const writer = port.writable.getWriter();

  await writer.write(payload);
  const echoed = await readExactly(reader, payload.length);
  assert.deepEqual(echoed, payload);

  reader.releaseLock();
  writer.releaseLock();
  await port.close();
});

test('open maps Web Serial options onto OPEN', async () => {
  const { port, devices } = makePort();
  await port.open({
    baudRate: 19200,
    dataBits: 7,
    stopBits: 2,
    parity: 'even',
    flowControl: 'hardware',
  });

  const cfg = devices[0].cfg;
  assert.equal(cfg.baud, 19200);
  assert.equal(cfg.databits, 7);
  assert.equal(cfg.stopbits, 2);
  assert.equal(cfg.parity, Parity.EVEN);
  assert.equal(cfg.flags & 1, 1);
  assert.equal(devices[0].state, PortState.OPEN);
  await port.close();
});

test('open rejects nonsense before touching the device', async () => {
  const { port, devices } = makePort();
  await assert.rejects(() => port.open({}), TypeError);
  await assert.rejects(() => port.open({ baudRate: 9600, parity: 'space' }), TypeError);
  assert.equal(devices.length, 0);
});

test('open rejects a baud the device will not take, and stays reopenable', async () => {
  const { port } = makePort({ maxBaud: 115200 });
  await assert.rejects(() => port.open({ baudRate: 921600 }));
  assert.equal(port.readable, null);

  await port.open({ baudRate: 115200 }); // the failure must not wedge the port
  await port.close();
});

test('a second open is an InvalidStateError', async () => {
  const { port } = makePort();
  await port.open({ baudRate: 9600 });
  await assert.rejects(() => port.open({ baudRate: 9600 }), (e) => {
    assert.equal(e.name, 'InvalidStateError');
    return true;
  });
  await port.close();
});

test('setSignals sends SET_LINES for exactly the lines named', async () => {
  const device = new FakeDevice();
  const sent = [];
  const port = new MidiBridgeSerialPort({
    createTransport: () => ({
      send: (msg) => {
        sent.push(msg);
        device.send(msg);
      },
      onSysEx: (cb) => device.onSysEx(cb),
      close: () => device.close(),
    }),
  });
  await port.open({ baudRate: 9600 });
  await port.setSignals({ dataTerminalReady: true, break: false });

  const frames = sent
    .map((f) => parseFrame(f))
    .filter((p) => p.status === ParseStatus.OK && p.cmd === Cmd.SET_LINES);
  assert.equal(frames.length, 1);
  assert.equal(frames[0].cursor.u7(), Line.DTR | Line.BREAK); // mask
  assert.equal(frames[0].cursor.u7(), Line.DTR); // values: DTR on, BREAK off
  await port.close();
});

test('getSignals reads STATUS input lines', async () => {
  const { port } = makePort();
  await port.open({ baudRate: 9600 });
  const signals = await port.getSignals();
  assert.deepEqual(signals, {
    clearToSend: false,
    dataSetReady: false,
    dataCarrierDetect: false,
    ringIndicator: false,
  });
  await port.close();
});

test('close resolves a pending read as done and the port reopens', async () => {
  const { port, devices } = makePort();
  await port.open({ baudRate: 9600 });

  const reader = port.readable.getReader();
  const pending = reader.read(); // nothing is coming — close() must end it
  await port.close(); // deliberately with the reader lock still held
  const { done } = await pending;
  assert.ok(done);
  assert.equal(port.readable, null);
  assert.equal(port.writable, null);

  await port.open({ baudRate: 9600 });
  assert.equal(devices.length, 2, 'each open gets a fresh transport');
  await port.close();
});

test('signal calls on a closed port are InvalidStateError', async () => {
  const { port } = makePort();
  await assert.rejects(() => port.setSignals({ dataTerminalReady: true }), (e) => {
    assert.equal(e.name, 'InvalidStateError');
    return true;
  });
  await assert.rejects(() => port.getSignals(), (e) => {
    assert.equal(e.name, 'InvalidStateError');
    return true;
  });
});

// ---- provider discovery ----------------------------------------------------

/** A MIDIInput/MIDIOutput pair. `device` handles SysEx; null never answers. */
function fakeMidiPair(name, device) {
  const input = {
    id: `${name}-in`,
    name,
    state: 'connected',
    onmidimessage: null,
    onstatechange: null,
    close() {},
  };
  const output = {
    id: `${name}-out`,
    name,
    state: 'connected',
    onstatechange: null,
    send(bytes) {
      device?.send(bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes));
    },
    close() {},
  };
  device?.onSysEx((msg) => input.onmidimessage?.({ data: msg }));
  return { input, output };
}

function fakeAccess(...pairs) {
  return {
    inputs: new Map(pairs.map((p) => [p.input.id, p.input])),
    outputs: new Map(pairs.map((p) => [p.output.id, p.output])),
  };
}

test('requestPort probes past a well-named impostor to the real device', async () => {
  // The impostor outranks the bridge on name — exactly the CoreMIDI
  // name-cache situation — so only the HELLO probe can tell them apart.
  const impostor = fakeMidiPair('UART Bridge Classic', null);
  const bridge = fakeMidiPair('Pico Thing', new FakeDevice());

  const provider = createMidiBridgeSerial({
    requestMIDIAccess: async (options) => {
      assert.equal(options.sysex, true);
      return fakeAccess(impostor, bridge);
    },
    probeTimeoutMs: 50,
  });

  const port = await provider.requestPort();
  assert.equal(port.getInfo().midiPortName, 'Pico Thing');
  assert.equal(port.getInfo().bridge.protocol, 1);

  await port.open({ baudRate: 9600 });
  const writer = port.writable.getWriter();
  const reader = port.readable.getReader();
  await writer.write(Uint8Array.from([0x00, 0x7f, 0x80, 0xff]));
  assert.deepEqual(await readExactly(reader, 4), Uint8Array.from([0x00, 0x7f, 0x80, 0xff]));
  reader.releaseLock();
  writer.releaseLock();
  await port.close();
});

test('requestPort names the ports it saw when nothing answers', async () => {
  const synth = fakeMidiPair('Volca FM', null);
  const provider = createMidiBridgeSerial({
    requestMIDIAccess: async () => fakeAccess(synth),
    probeTimeoutMs: 20,
  });
  await assert.rejects(() => provider.requestPort(), /Volca FM/);
});

// ---- the single-file bundle ------------------------------------------------

test('the bundle builds and its port loops back like the sources', async () => {
  const outPath = await buildBundle();
  const bundle = await import(`${pathToFileURL(outPath).href}?t=${Date.now()}`);

  const port = new bundle.MidiBridgeSerialPort({
    createTransport: () => new FakeDevice(),
  });
  await port.open({ baudRate: 115200 });
  const rand = makeRandom(0xfeed);
  const payload = Uint8Array.from({ length: 1024 }, rand);
  const writer = port.writable.getWriter();
  const reader = port.readable.getReader();
  await writer.write(payload);
  assert.deepEqual(await readExactly(reader, payload.length), payload);
  reader.releaseLock();
  writer.releaseLock();
  await port.close();
});
