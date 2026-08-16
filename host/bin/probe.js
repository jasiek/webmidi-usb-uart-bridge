#!/usr/bin/env node
// Handshake check: does the device answer, and what does it say it is?
//
// Needs no loopback jumper and does not open the port, so it is the first
// thing to run after flashing.
//
//   npm run probe
//   npm run probe -- --port pico

import { BridgeClient } from '../src/client.js';
import { BackendId, Cap } from '../src/constants.js';
import { DEFAULT_PORT_MATCH, NodeMidiTransport } from '../src/transport-node.js';

const args = process.argv.slice(2);
let match = DEFAULT_PORT_MATCH;
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--port') match = args[++i];
  else if (args[i] === '--help' || args[i] === '-h') {
    console.log('usage: probe [--port <name>]');
    process.exit(0);
  }
}

const transport = new NodeMidiTransport(match);
console.log(`in:  ${transport.inputName}`);
console.log(`out: ${transport.outputName}\n`);

const client = new BridgeClient(transport, { timeoutMs: 3000 });

try {
  const info = await client.hello();
  const backend =
    info.backend === BackendId.HARDWARE_UART ? 'hardware UART' : 'PIO-USB CDC';
  const caps = Object.entries(Cap)
    .filter(([, bit]) => info.caps & bit)
    .map(([name]) => name.toLowerCase());

  console.log(`firmware      ${info.firmware}`);
  console.log(`protocol      v${info.protocol}`);
  console.log(`backend       ${backend}`);
  console.log(`frame limit   ${info.maxRaw} B`);
  console.log(`rx window     ${info.rxBuffer} B`);
  console.log(`max baud      ${info.maxBaud}`);
  console.log(`capabilities  ${caps.length ? caps.join(', ') : 'none'}`);

  const samples = [];
  for (let i = 0; i < 10; i++) samples.push(await client.ping());
  samples.sort((a, b) => a - b);
  const mean = samples.reduce((a, b) => a + b, 0) / samples.length;
  console.log(
    `\nping          ${mean.toFixed(2)} ms mean, ` +
      `${samples[0].toFixed(2)} min, ${samples.at(-1).toFixed(2)} max (10 samples)`,
  );

  const status = await client.getStatus();
  console.log(`port state    ${['closed', 'open', 'fault'][status.state] ?? status.state}`);
  console.log('\ndevice is alive and speaking the protocol');
} catch (error) {
  console.error(`\nfailed: ${error.message}`);
  process.exitCode = 1;
} finally {
  client.destroy();
}
