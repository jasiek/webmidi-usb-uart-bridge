#!/usr/bin/env node
// Loopback test across a range of baud rates.
//
// Wire GPIO0 (TX) to GPIO1 (RX) on the Pico, then:
//
//   npm run loopback
//   npm run loopback -- --baud 115200 --bytes 65536
//   npm run loopback -- --fake          # no hardware; exercises the harness
//   npm run loopback -- --fake-cdc      # same, modelling the phase 2 host port
//
// Every byte sent must come back, in order, at every speed. Anything else is
// a failure with a non-zero exit code.

import { BridgeClient } from '../src/client.js';
import { Cap, ErrFlag, BackendId } from '../src/constants.js';
import { FakeDevice } from '../src/fake-device.js';
import { DEFAULT_PORT_MATCH, NodeMidiTransport } from '../src/transport-node.js';

const DEFAULT_BAUDS = [9600, 19200, 38400, 57600, 115200];

function parseArgs(argv) {
  const args = {
    port: DEFAULT_PORT_MATCH,
    bauds: DEFAULT_BAUDS,
    bytes: 4096,
    fake: false,
    fakeHotplug: false,
    timeoutMs: 30000,
    // Only consulted on a hot-plug backend: how long to hold the run open
    // waiting for an adapter to be plugged into the host port.
    attachTimeoutMs: 30000,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--fake') args.fake = true;
    else if (arg === '--fake-cdc') {
      // The software model wearing the phase 2 backend's identity: hot-plug
      // capability, PIO-USB CDC backend id. Exercises the attach path in the
      // harness itself, which is where its bugs would otherwise hide.
      args.fake = true;
      args.fakeHotplug = true;
    }
    else if (arg === '--port') args.port = argv[++i];
    else if (arg === '--bytes') args.bytes = Number(argv[++i]);
    else if (arg === '--timeout') args.timeoutMs = Number(argv[++i]);
    else if (arg === '--attach-timeout') args.attachTimeoutMs = Number(argv[++i]);
    else if (arg === '--baud') args.bauds = argv[++i].split(',').map(Number);
    else if (arg === '--help' || arg === '-h') {
      console.log(
        'usage: loopback [--port <name>] [--baud 9600,115200] [--bytes N]\n' +
          '                [--fake | --fake-cdc]\n' +
          '                [--timeout ms] [--attach-timeout ms]',
      );
      process.exit(0);
    } else {
      console.error(`unknown argument: ${arg}`);
      process.exit(2);
    }
  }
  return args;
}

/** Deterministic payload: a failure is reproducible from the seed alone. */
function payloadOf(length, seed) {
  let state = seed >>> 0;
  const out = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    out[i] = (state >>> 24) & 0xff;
  }
  return out;
}

function firstDifference(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) return i;
  return a.length === b.length ? -1 : n;
}

function describeCaps(caps) {
  const names = Object.entries(Cap)
    .filter(([, bit]) => caps & bit)
    .map(([name]) => name.toLowerCase());
  return names.length ? names.join(', ') : 'none';
}

function describeErrFlags(flags) {
  const names = Object.entries(ErrFlag)
    .filter(([, bit]) => flags & bit)
    .map(([name]) => name.toLowerCase());
  return names.join(', ');
}

async function runOne(client, baud, bytes, timeoutMs) {
  await client.open({ baud });

  const latency = await client.ping();
  const payload = payloadOf(bytes, baud);

  const warnings = [];
  const onWarning = (w) => warnings.push(w);
  client.on('warning', onWarning);

  const started = performance.now();
  await client.write(payload);
  const echoed = await client.readExactly(payload.length, timeoutMs);
  const elapsed = performance.now() - started;

  client.off('warning', onWarning);

  const diff = firstDifference(payload, echoed);
  const status = await client.getStatus();

  return {
    baud,
    bytes,
    latency,
    elapsed,
    // Bytes went out and came back, so the wire carried twice the payload.
    throughput: (bytes * 2) / (elapsed / 1000),
    ok: diff === -1 && warnings.length === 0,
    diff,
    warnings,
    errFlags: status.errFlags,
  };
}

async function main() {
  const args = parseArgs(process.argv.slice(2));

  let transport;
  if (args.fake) {
    console.log(
      `running against the software device model (${args.fakeHotplug ? '--fake-cdc' : '--fake'})\n`,
    );
    transport = new FakeDevice({ baudLimited: true, hotplug: args.fakeHotplug });
  } else {
    transport = new NodeMidiTransport(args.port);
    console.log(`in:  ${transport.inputName}`);
    console.log(`out: ${transport.outputName}\n`);
  }

  const client = new BridgeClient(transport, { timeoutMs: 5000 });
  let failures = 0;

  try {
    const info = await client.hello();
    const backend =
      info.backend === BackendId.HARDWARE_UART ? 'hardware UART' : 'PIO-USB CDC';
    console.log(`firmware ${info.firmware}, protocol v${info.protocol}, ${backend}`);
    console.log(`window ${info.rxBuffer} B, frame ${info.maxRaw} B, max ${info.maxBaud} baud`);
    console.log(`capabilities: ${describeCaps(info.caps)}\n`);

    if (info.caps & Cap.HOTPLUG) {
      // On a host-port build there may be nothing downstream yet. Waiting is
      // friendlier than a run of ERR_BACKEND, and it makes "plug it in now" a
      // valid way to start the test.
      if (!(await client.getStatus()).present) {
        console.log('waiting for a serial adapter on the host port…');
      }
      await client.waitForAttach(args.attachTimeoutMs);
      console.log('far end attached\n');
    }

    console.log('  baud     bytes      time    throughput   ping   result');
    console.log('  ' + '-'.repeat(60));

    for (const baud of args.bauds) {
      if (baud > info.maxBaud) {
        console.log(`  ${String(baud).padStart(6)}  — skipped, above the device maximum`);
        continue;
      }

      let result;
      try {
        result = await runOne(client, baud, args.bytes, args.timeoutMs);
      } catch (error) {
        failures++;
        console.log(`  ${String(baud).padStart(6)}  FAILED: ${error.message}`);
        continue;
      }

      const line = [
        String(result.baud).padStart(6),
        String(result.bytes).padStart(9),
        `${result.elapsed.toFixed(0).padStart(6)} ms`,
        `${(result.throughput / 1024).toFixed(1).padStart(8)} kB/s`,
        `${result.latency.toFixed(1).padStart(5)} ms`,
        result.ok ? 'ok' : 'FAILED',
      ].join('  ');
      console.log(`  ${line}`);

      if (!result.ok) {
        failures++;
        if (result.diff >= 0) {
          console.log(`         first difference at byte ${result.diff}`);
        }
        for (const w of result.warnings) console.log(`         ${w}`);
      }
      if (result.errFlags) {
        console.log(`         device flags: ${describeErrFlags(result.errFlags)}`);
      }
    }

    await client.close();
  } finally {
    client.destroy();
  }

  console.log();
  if (failures > 0) {
    console.log(`${failures} of ${args.bauds.length} speeds failed`);
    process.exitCode = 1;
  } else {
    console.log(`all ${args.bauds.length} speeds passed`);
  }
}

main().catch((error) => {
  console.error(`\n${error.message}`);
  process.exitCode = 1;
});
