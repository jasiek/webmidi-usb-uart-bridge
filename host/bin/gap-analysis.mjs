// Where are the missing bytes, and are they missing from the middle?
//
// loopback.js answers "how many came back". When the answer is "fewer", the
// next question is whether the stream was truncated or perforated, and those
// have completely different causes. This sends a known ramp, aligns the return
// against it, and reports every gap and the spacing between them.
//
//   node bin/gap-analysis.mjs [baud]
//
// See OPEN-ISSUES.md 3.
import { BridgeClient } from '../src/client.js';
import { NodeMidiTransport } from '../src/transport-node.js';
const N = 4096, BAUD = Number(process.argv[2] || 9600);
const sent = Buffer.alloc(N);
for (let i = 0; i < N; i++) sent[i] = i & 0xff;
const t = new NodeMidiTransport(); const c = new BridgeClient(t);
await c.hello(); await c.open({ baud: BAUD });
const got = []; c.on('data', (b) => got.push(Buffer.from(b)));
await c.write(sent);
let last = 0, quiet = Date.now();
while (Date.now() - quiet < 3000) {
  await new Promise((r) => setTimeout(r, 200));
  const n = got.reduce((a, b) => a + b.length, 0);
  if (n !== last) { last = n; quiet = Date.now(); }
}
const back = Buffer.concat(got);
// Align: walk the received stream against the sent one, skipping sent bytes
// that never arrived. Records where each run of drops starts and how long.
let si = 0, gaps = [];
for (let bi = 0; bi < back.length; bi++) {
  if (back[bi] === sent[si]) { si++; continue; }
  const start = si, from = si;
  while (si < N && back[bi] !== sent[si]) si++;
  if (si >= N) { si = from; break; }
  gaps.push([start, si - start]);
  si++;
}
console.log(`baud ${BAUD}: sent ${N}, back ${back.length}, short ${N - back.length}`);
console.log(`${gaps.length} gaps; run lengths:`,
  JSON.stringify(gaps.reduce((m, [, l]) => (m[l] = (m[l] || 0) + 1, m), {})));
const starts = gaps.map(([s]) => s);
const deltas = starts.slice(1).map((s, i) => s - starts[i]);
console.log('first 12 gap offsets:', starts.slice(0, 12).join(', '));
console.log('gap-to-gap spacing, first 12:', deltas.slice(0, 12).join(', '));
const hist = deltas.reduce((m, d) => (m[d] = (m[d] || 0) + 1, m), {});
const top = Object.entries(hist).sort((a, b) => b[1] - a[1]).slice(0, 6);
console.log('most common spacings:', top.map(([d, n]) => `${d}x${n}`).join('  '));
c.destroy(); process.exit(0);
