// Node transport: CoreMIDI (or ALSA/WinMM) via RtMidi.
//
// The only Node-specific file in host/src. Everything else is plain ES modules
// so the same client can run against a Web MIDI transport in a browser.
// See DECISIONS.md D3.

import midi from '@julusian/midi';

/** @returns {{inputs: string[], outputs: string[]}} */
export function listPorts() {
  const input = new midi.Input();
  const output = new midi.Output();
  try {
    const inputs = [];
    for (let i = 0; i < input.getPortCount(); i++) inputs.push(input.getPortName(i));
    const outputs = [];
    for (let i = 0; i < output.getPortCount(); i++) outputs.push(output.getPortName(i));
    return { inputs, outputs };
  } finally {
    input.closePort();
    output.closePort();
  }
}

// Several candidates, tried in order, because CoreMIDI caches port names by
// VID/PID: a Mac that saw the board before its product descriptor was set will
// go on calling it "Pico" no matter what the device now reports. See
// FINDINGS.md — clearing that cache is the user's MIDI configuration, not
// ours to delete.
export const DEFAULT_PORT_MATCH = 'uart bridge,bridge,pico,rp2040';

function findPort(port, needles) {
  const names = [];
  for (let i = 0; i < port.getPortCount(); i++) names.push(port.getPortName(i));
  for (const needle of needles) {
    const wanted = needle.trim().toLowerCase();
    if (!wanted) continue;
    const hit = names.findIndex((name) => name.toLowerCase().includes(wanted));
    if (hit >= 0) return hit;
  }
  return -1;
}

export class NodeMidiTransport {
  /**
   * @param {string} [match] comma-separated name substrings, tried in order,
   *   case-insensitive
   */
  constructor(match = DEFAULT_PORT_MATCH) {
    const needles = match.split(',');
    this.input = new midi.Input();
    this.output = new midi.Output();
    this.handler = null;

    const inIndex = findPort(this.input, needles);
    const outIndex = findPort(this.output, needles);
    if (inIndex < 0 || outIndex < 0) {
      const { inputs, outputs } = listPorts();
      this.input.closePort();
      this.output.closePort();
      throw new Error(
        `no MIDI port matching "${match}".\n` +
          `  inputs:  ${inputs.length ? inputs.join(', ') : '(none)'}\n` +
          `  outputs: ${outputs.length ? outputs.join(', ') : '(none)'}`,
      );
    }

    this.inputName = this.input.getPortName(inIndex);
    this.outputName = this.output.getPortName(outIndex);

    // Without this, RtMidi filters SysEx out — which is the only thing we send.
    this.input.ignoreTypes(false, true, true);
    this.input.on('message', (_deltaTime, message) => {
      if (this.handler) this.handler(Uint8Array.from(message));
    });

    this.input.openPort(inIndex);
    this.output.openPort(outIndex);
  }

  /** @param {Uint8Array} msg */
  send(msg) {
    this.output.sendMessage(Array.from(msg));
  }

  /** @param {(msg: Uint8Array) => void} cb */
  onSysEx(cb) {
    this.handler = cb;
  }

  close() {
    this.handler = null;
    this.input.closePort();
    this.output.closePort();
  }
}
