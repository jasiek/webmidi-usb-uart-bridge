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

function findPort(port, needle) {
  const wanted = needle.toLowerCase();
  for (let i = 0; i < port.getPortCount(); i++) {
    if (port.getPortName(i).toLowerCase().includes(wanted)) return i;
  }
  return -1;
}

export class NodeMidiTransport {
  /**
   * @param {string} [match] substring of the port name, case-insensitive
   */
  constructor(match = 'bridge') {
    this.input = new midi.Input();
    this.output = new midi.Output();
    this.handler = null;

    const inIndex = findPort(this.input, match);
    const outIndex = findPort(this.output, match);
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
