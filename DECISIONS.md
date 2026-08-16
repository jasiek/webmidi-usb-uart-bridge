# Decisions

Design decisions taken on this project, newest section last. Each entry records
the question, the answer, and the reasoning, so that a later reader can tell
which constraints are real and which are just the path we happened to take.

## 2026-08-16 — Initial scoping

### D1. Far end of the tunnel: hardware UART first, PIO-USB CDC host second

**Question.** `CLAUDE.md` describes both "tunnels UART" and "attach a low-speed
USB device driven with the CDC/ACM class". Which is the thing the Pico actually
talks to?

**Decision.** Both, in two phases behind one abstraction:

- **Phase 1 (this iteration):** the far end is a hardware UART on GPIO0/GPIO1
  (`uart0`). Testable immediately with a jumper between TX and RX, or against a
  USB-serial cable.
- **Phase 2:** add a Pico-PIO-USB host backend that enumerates a CDC/ACM device
  and maps the same command set onto CDC line-coding / control-line requests.

Both sit behind a single `SerialBackend` interface, so the wire protocol, the
SysEx codec, the flow control and the host-side tests are written once and do
not change when the second backend lands.

**Why.** The protocol work (7-bit packing, framing, credit windowing) is the
part most likely to have subtle bugs, and it is entirely independent of which
backend moves the bytes. Bringing it up against a hardware UART means a
loopback jumper is a complete test rig. Debugging a PIO-USB enumeration failure
*and* a framing bug at the same time, with no serial console free, is the
scenario we are explicitly avoiding.

### D2. Binary→SysEx encoding: 7-in-8 MSB packing

**Question.** SysEx payload bytes must have bit 7 clear. How do we carry
arbitrary 8-bit UART data?

**Decision.** 7-in-8 MSB packing. Each group of up to 7 payload bytes is
transmitted as one *MSB byte* followed by the 7 payload bytes with bit 7
cleared. Bit `i` (0 = LSB) of the MSB byte holds the original bit 7 of the
`i`-th byte of the group. See `PROTOCOL.md` for the exact bit order and the
short-group rule.

**Why.** 87.5% efficiency (14.3% overhead), versus 100% overhead for
nibble-per-byte and 33% for base64. It is also the de-facto convention in the
MIDI world (Novation, Elektron, Sequential all use a variant), so anyone
reading a MIDI monitor dump will recognise the shape. The cost is that a dump
is not directly human-readable — accepted, because the Node harness can decode.

**Rejected.** Nibble-per-byte: readable on the wire, but halves an already
modest budget. At 115200 baud in both directions that is 23 kB/s of MIDI
payload before framing, which is enough to matter.

### D3. Host-side tests: Node.js, headless

**Question.** The repo is named `webmidi-usb-uart-bridge`; should the test
harness be a browser page using the Web MIDI API?

**Decision.** The automated loopback test is a headless Node.js program talking
to CoreMIDI. The codec and protocol layer are written as plain ES modules with
no Node-specific imports, so a browser page can `import` the same files later
without a build step.

**Why.** Tests have to run from the terminal and in CI. A browser page cannot
without dragging in Playwright and a headless Chrome that still needs a MIDI
permission prompt. Keeping the codec free of Node built-ins costs nothing now
and keeps the browser demo cheap to add.

### D4. Flow control: credit-based windowing

**Question.** How much flow control does this iteration need?

**Decision.** Credit-based windowing in both directions, denominated in bytes
of *raw* (pre-packing) payload. Each side advertises its receive buffer size at
`OPEN`; a sender may only transmit `DATA` whose raw length fits in its
remaining credit; a receiver returns credit as it drains. Plus a 7-bit
per-`DATA` sequence number so any loss is detected rather than silent.

**Why.** MIDI has no backpressure whatsoever. A phone can push SysEx into the
Pico far faster than a 115200-baud UART drains it — roughly an order of
magnitude — so *something* has to stop the sender, and the only alternatives
are dropping data or blocking the USB stack. Dropping silently is the worst
outcome for a transport that is meant to look like a serial cable. The
sequence number is cheap insurance: USB bulk is reliable, but CoreMIDI and iOS
will drop messages on their own buffer overflow, and we want that to surface as
an error rather than as corrupted firmware uploads on whatever is downstream.

**Note.** Credits are cumulative deltas, not absolute values, so a lost credit
message would desynchronise the window permanently. The sequence check exists
partly to make that failure loud.
