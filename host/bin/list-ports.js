#!/usr/bin/env node
// Lists the MIDI ports this machine can see, so you can tell whether the Pico
// enumerated and under what name.

import { listPorts } from '../src/transport-node.js';

const { inputs, outputs } = listPorts();

const show = (label, names) => {
  console.log(`${label}:`);
  if (names.length === 0) {
    console.log('  (none)');
    return;
  }
  names.forEach((name, i) => console.log(`  ${i}  ${name}`));
};

show('MIDI inputs', inputs);
console.log();
show('MIDI outputs', outputs);

if (inputs.length === 0 && outputs.length === 0) {
  console.log('\nNothing found. If the Pico is plugged in, check that it is running');
  console.log('the bridge firmware and not sitting in BOOTSEL mode.');
}
