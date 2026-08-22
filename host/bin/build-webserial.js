#!/usr/bin/env node
// Builds dist/midi-bridge-serial.js — the browser-safe modules concatenated
// into one self-contained ES module, for consumers (webchirp) that vendor
// single-file drivers into a static web directory.
//
// This is concatenation, not compilation, and it stays honest only while the
// modules keep the properties they have today: every internal import is
// `from './<name>.js'` with single quotes, there are no cyclic imports, no
// default exports, and no two modules declare the same top-level name. The
// import-after-build check below is what catches a violation — a duplicate
// declaration or leftover import is a SyntaxError, not a silent misbundle.

import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

// Dependency order: each module only imports from the ones above it.
const MODULES = [
  'constants.js',
  'sysex7.js',
  'emitter.js',
  'frame.js',
  'client.js',
  'transport-webmidi.js',
  'webserial.js',
];

// The exports a consumer actually uses; the check fails loud if one goes missing.
const REQUIRED_EXPORTS = [
  'createMidiBridgeSerial',
  'MidiBridgeSerialPort',
  'hasWebMidi',
  'WebMidiTransport',
  'findBridgePort',
  'BridgeClient',
  'DEFAULT_MIDI_PORT_MATCH',
];

export const DEFAULT_OUT = fileURLToPath(
  new URL('../dist/midi-bridge-serial.js', import.meta.url),
);

const HEADER = `\
// midi-bridge-serial.js — generated file, do not edit.
//
// A Web Serial-shaped provider over the Web MIDI API, for talking to the
// webmidi-usb-uart-bridge (a Raspberry Pi Pico that tunnels a UART through
// USB MIDI SysEx). Built from host/src/ of that repository by
// host/bin/build-webserial.js; edit the sources there and rebuild with
// \`npm run build:webserial\`.
//
// Usage:
//   import { createMidiBridgeSerial } from './midi-bridge-serial.js';
//   const port = await createMidiBridgeSerial().requestPort();
//   await port.open({ baudRate: 115200 });
//   // then port.readable / port.writable / port.setSignals, as Web Serial.
`;

export async function buildBundle(outPath = DEFAULT_OUT) {
  let out = HEADER;
  for (const name of MODULES) {
    const src = await readFile(new URL(`../src/${name}`, import.meta.url), 'utf8');
    const stripped = src.replace(/^import\s[\s\S]*?from\s+'\.\/[^']+';\s*\n/gm, '');
    if (/^import\s/m.test(stripped)) {
      throw new Error(`${name}: an import survived stripping — bundle would not be self-contained`);
    }
    out += `\n// ------------------------------ src/${name} ------------------------------\n\n`;
    out += stripped;
  }

  await mkdir(dirname(outPath), { recursive: true });
  await writeFile(outPath, out);

  // Import what was written: proves it parses, that no two modules collided on
  // a name, and that the public surface survived. Cache-busted so a rebuild in
  // the same process (the tests) checks the new file, not the old one.
  const mod = await import(`${pathToFileURL(outPath).href}?built=${out.length.toString(36)}-${hash(out)}`);
  for (const name of REQUIRED_EXPORTS) {
    if (!(name in mod)) throw new Error(`bundle is missing export: ${name}`);
  }
  return outPath;
}

function hash(text) {
  let h = 0;
  for (let i = 0; i < text.length; i++) h = (Math.imul(h, 31) + text.charCodeAt(i)) >>> 0;
  return h.toString(36);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const outPath = await buildBundle();
  console.log(`wrote ${outPath}`);
}
