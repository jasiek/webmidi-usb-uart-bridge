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

## 2026-08-16 — After hardware bring-up

### D5. `INFO.maxBaud` reports the tunnel's limit, not the UART's

**Question.** `UartBackend::maxBaud()` originally returned 921600, which is
what uart0 can clock. Should it?

**Decision.** No — it returns 460800, chosen from measurement.
`host/bin/throughput.js` shows the SysEx tunnel flattening at ~53 kB/s one-way
and ~48 kB/s in each direction at once. 460800 8N1 is 45 kB/s and fits;
921600 needs 90 kB/s and does not.

**Why.** `INFO.maxBaud` is the field a host trusts when choosing a rate. A
device that accepts 921600 and then silently drops whatever the far end sends
beyond its capacity is worse than one that refuses. The UART divisor supporting
a rate is not the same claim as the bridge being able to carry it.

**Caveat, recorded because it limits the evidence.** The loopback rig cannot
demonstrate overrun: with TX jumpered to RX the device cannot receive faster
than it transmits, and its transmission is credit-paced, so the whole path is
self-limiting. That is why nothing was lost even at 921600. A far end that
transmits on its own has no such constraint, so 460800 has thin margin and
230400 is the highest rate with real headroom. Proving the top end needs an
independent traffic source, which is a phase 2 job.

### D6. Watchdog plus a software reset path

**Question.** The first wedge left the device enumerated but mute, with no way
to recover it except physically unplugging the board — the 1200-baud touch did
nothing and picotool reported no reset interface.

**Decision.** Both: an RP2040 hardware watchdog (4 s) so a hung `loop()`
recovers itself, and our own `tud_cdc_line_coding_cb` implementing the
1200-baud touch so the board can be put into BOOTSEL over USB.

**Why.** arduino-pico implements the 1200-baud reset only in its own
`SerialUSB`, which is compiled out under `USE_TINYUSB` (`SerialUSB.cpp` line 23),
so the convention every other Arduino board follows was simply absent. Between
the two, a wedge never again requires physical access — which matters for a
device whose entire purpose is being driven from a phone.

The watchdog also turned out to be the diagnostic that mattered: it is what
made the failure *visible* as `boot=WATCHDOG` rather than as an unexplained
silence, and its scratch registers are what survived to name the hung phase.

## 2026-08-16 — Phase 2: the CDC/ACM host backend

### D7. The USB host stack gets core1, and talks to core0 through rings

**Question.** Pico-PIO-USB and the protocol engine both need servicing
promptly. Where does `tuh_task()` run?

**Decision.** Core1, exclusively. Core0 keeps the MIDI device stack, the
protocol engine and the watchdog. Between them:

- two lock-free single-producer/single-consumer rings (`src/spsc_ring.h`) for
  bulk data, one per direction;
- a single-slot mailbox for control operations — open, close, set lines,
  flush — which core0 posts and then *waits on*, with a one-second timeout.

**Why.** Pico-PIO-USB reconstructs a full-speed bus in software; its interrupt
has to be serviced inside a bit time, and the RP2040's own USB device
interrupt — the MIDI side, which is the entire product — will not yield to it.
Sharing a core means one of the two is always the loser.

Making the *control* path synchronous is the part that looks wrong and is not.
These operations happen a handful of times per session, never on the hot path,
and blocking core0 for their duration is what makes them safe: while core0 is
waiting it is provably not touching the rings, which is the only window in
which core1 can clear them for a `FLUSH` or an `OPEN` without a race. The
alternative — asynchronous ops plus locks around the rings — costs more on the
path that actually matters, to avoid a cost on the path that does not.

The timeout is not defensive programming. TinyUSB's blocking control transfer
has no timeout of its own (FINDINGS.md), so an adapter that stops answering
hangs core1 permanently. Core0 giving up after a second means the board keeps
feeding its watchdog and keeps answering MIDI — so the failure surfaces as
`ERR_BACKEND` on the host's screen rather than as a device that went quiet.

**Note.** `lib/bridge_proto/ringbuf.h` says in its header comment that a core1
backend would need release/acquire ordering on its indices. It was right; that
is `SpscRing`, and the single-core `RingBuf` is left alone.

### D8. A detach faults the port; it does not quietly close it

**Question.** The adapter is unplugged mid-session. What should the host see?

**Decision.** `EVT_DETACH`, the port moves to `fault`, anything still queued
toward the far end is discarded — and anything already received from it is
still delivered. Re-attaching emits `EVT_ATTACH` and clears the fault back to
`closed`, but does **not** re-open the port: the host must `OPEN` again.

**Why.** `closed` is what a host asked for; `fault` is what happened to it.
Collapsing the two would mean a host that had opened a port and never closed it
could find it closed with no explanation, which is exactly the ambiguity the
state exists to resolve. Discarding the outbound queue matters more than it
looks: those bytes were addressed to a device that is gone, and holding them
would deliver a firmware upload's tail to whatever gets plugged in next.

Not auto-reopening is the same argument. The new device is a different device.
It may be a different *kind* of device. Re-applying the old port settings to it
without being asked is a guess, and `OPEN` is cheap.

### D9. `STATUS` gains a `present` field rather than relying on events alone

**Question.** A host connects to a device that already has an adapter
attached. `EVT_ATTACH` was emitted before it was listening. How does it find
out?

**Decision.** A trailing `present` byte on `STATUS` (PROTOCOL.md §5.4), plus
the events for changes. `waitForAttach()` on the host client reads the field
first and only then waits for an event.

**Why.** Inferring "nothing is attached" from the *absence* of an event is a
negative inference over an asynchronous channel — it can only be implemented
as a timeout, and a timeout cannot distinguish "nothing there" from "slow".
One byte on a frame that already exists to answer "what is going on" removes
the guesswork entirely.

Appending it is backward compatible: a host that stops reading after `credit`
never sees it, and its assumption that the far end is present is the right
answer for every backend that cannot be unplugged.

### D10. `INFO.caps` on this backend claims DTR, RTS and hot-plug — and nothing else

**Question.** CDC/ACM is a richer interface than a bare UART. How much of it
can we actually offer?

**Decision.** `DTR`, `RTS`, `HOTPLUG`. Not `BREAK`, not `CTS`/`DSR`/`DCD`/`RI`,
not RTS/CTS flow control.

**Why.** Each absence is a specific missing API rather than a choice.
`SEND_BREAK` exists in the CDC spec but TinyUSB's host driver does not expose
it. The input lines arrive on the ACM notification endpoint as `SERIAL_STATE`,
which TinyUSB consumes without surfacing — so we could neither report them in
`STATUS` nor raise `EVT_LINES` for them honestly. Hardware flow control is a
property of the adapter's far side, not something this protocol reaches.

This is the same discipline as D5 and the phase 1 caps: a capability bit is a
promise, and the failure mode of an over-claimed bit is a host waiting for an
event that can never arrive.
