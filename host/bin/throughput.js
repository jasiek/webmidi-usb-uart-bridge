#!/usr/bin/env node
// Measures what the MIDI tunnel can actually carry, per direction.
//
// The point is to find the ceiling that is *not* the UART. Run it with the
// loopback jumper fitted and sweep upward: while the UART is the bottleneck,
// delivered throughput tracks the baud rate; once the tunnel is the
// bottleneck, it flattens and bytes start going missing. The rate at which it
// flattens is the number INFO.maxBaud should be honest about.
//
//   npm run throughput
//   npm run throughput -- --baud 115200,230400,460800,921600
//
// Unlike the loopback rig this never waits for a complete payload — it
// collects for a bounded window and reports the loss, so an overrun costs a
// second rather than a timeout.

import { BridgeClient } from '../src/client.js';
import { ErrFlag } from '../src/constants.js';
import { DEFAULT_PORT_MATCH, NodeMidiTransport } from '../src/transport-node.js';

const args = {
  port: DEFAULT_PORT_MATCH,
  bauds: [57600, 115200, 230400, 460800, 921600],
  bytes: 16384,
  quietMs: 400,
};
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  if (argv[i] === '--port') args.port = argv[++i];
  else if (argv[i] === '--bytes') args.bytes = Number(argv[++i]);
  else if (argv[i] === '--baud') args.bauds = argv[++i].split(',').map(Number);
  else if (argv[i] === '--help' || argv[i] === '-h') {
    console.log('usage: throughput [--port <name>] [--baud a,b,c] [--bytes N]');
    process.exit(0);
  }
}

function payloadOf(length, seed) {
  let state = seed >>> 0;
  const out = new Uint8Array(length);
  for (let i = 0; i < length; i++) {
    state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
    out[i] = (state >>> 24) & 0xff;
  }
  return out;
}

/**
 * Collects until the payload is complete or the link goes quiet.
 *
 * `started` has to be passed in from before the write, not captured here:
 * with the loopback fitted the echo arrives *while* the write is still going,
 * so measuring from the end of the write times only the tail and reports a
 * throughput several times the line rate.
 */
function collectUntilQuiet(client, want, started, quietMs, hardCapMs) {
  return new Promise((resolve) => {
    let lastArrival = performance.now();
    const onData = () => {
      lastArrival = performance.now();
    };
    const timer = setInterval(() => {
      const now = performance.now();
      const done = client.available >= want;
      if (done || now - lastArrival > quietMs || now - started > hardCapMs) {
        clearInterval(timer);
        client.off('data', onData);
        resolve({ elapsed: lastArrival - started, complete: done });
      }
    }, 20);
    client.on('data', onData);
  });
}

const transport = new NodeMidiTransport(args.port);
console.log(`in:  ${transport.inputName}`);
console.log(`out: ${transport.outputName}\n`);

const client = new BridgeClient(transport, { timeoutMs: 5000 });

try {
  const info = await client.hello();
  console.log(`firmware ${info.firmware}, device says max ${info.maxBaud} baud\n`);

  console.log('   baud    line rate    host->uart    uart->host      lost   flags');
  console.log('  ' + '-'.repeat(68));

  for (const baud of args.bauds) {
    if (baud > info.maxBaud) {
      console.log(`${String(baud).padStart(7)}  — skipped, above the device maximum`);
      continue;
    }
    await client.open({ baud });
    client.read(); // discard anything left over from the last round

    const payload = payloadOf(args.bytes, baud);
    const lineRate = baud / 10; // bytes/s on the wire, 8N1

    const t0 = performance.now();
    await client.write(payload);
    const handoff = (performance.now() - t0) / 1000;

    const { elapsed } = await collectUntilQuiet(
      client,
      payload.length,
      t0,
      args.quietMs,
      20000,
    );
    const got = client.available;
    client.read();

    const status = await client.getStatus();
    const flags = Object.entries(ErrFlag)
      .filter(([, bit]) => status.errFlags & bit)
      .map(([name]) => name.toLowerCase())
      .join(',');

    const outRate = payload.length / handoff;
    const inRate = elapsed > 0 ? got / (elapsed / 1000) : 0;
    const lost = payload.length - got;

    console.log(
      [
        String(baud).padStart(7),
        `${(lineRate / 1024).toFixed(1).padStart(8)} kB/s`,
        `${(outRate / 1024).toFixed(1).padStart(9)} kB/s`,
        `${(inRate / 1024).toFixed(1).padStart(9)} kB/s`,
        String(lost).padStart(8),
        flags || 'clean',
      ].join('  '),
    );
  }

  await client.close();
  console.log('\nWhere host->uart and uart->host stop tracking the line rate,');
  console.log('the tunnel has become the bottleneck. Bytes lost above that point');
  console.log('are the UART outrunning it, with no backpressure to stop it.');
} catch (error) {
  console.error(`\nfailed: ${error.message}`);
  process.exitCode = 1;
} finally {
  client.destroy();
}
