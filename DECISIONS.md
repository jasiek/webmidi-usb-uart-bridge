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

## 2026-08-16 — Toolchain

### D7. One pinned toolchain, installed by `bootstrap.sh` via asdf

**Question.** The project needs four things before anything can be built:
PlatformIO, an RP2040 core, Node for the host client, and a Python for
PlatformIO to run on. None of them was pinned, and `pio` lived wherever each
machine's installer had left it. What installs them, and what fixes the
versions?

**Decision.** `./bootstrap.sh`, driven by three pinned files:

| File               | Pins                                     |
| ------------------ | ---------------------------------------- |
| `.tool-versions`   | nodejs and python, installed by asdf     |
| `requirements.txt` | PlatformIO, installed into `.venv/`      |
| `platformio.ini`   | the RP2040 platform fork, by git tag     |

The script installs asdf itself if it is absent — a pinned release, checked
against a sha256 embedded in the script — then the runtimes, then PlatformIO,
then the host's npm dependencies from the lockfile. It is idempotent, prints
what it skipped, and exits non-zero on the first thing it cannot do.

**Why asdf rather than the system package manager.** The versions have to be
identical on macOS, on Linux and on a CI runner, and no package manager spans
those three. `.tool-versions` is a file both asdf and mise read, and it is
already the convention for exactly this.

**Why PlatformIO lives in `.venv/` rather than in `.tool-versions`.** asdf has
no PlatformIO plugin, and PlatformIO's own installer puts it in a global
`~/.platformio/penv` shared by every project on the machine. A project-local
virtualenv pins the version per checkout and is deleted by deleting a
directory.

**Why the RP2040 platform is now a git URL with a tag.** `platform = raspberrypi`
resolves to the registry platform, which carries only the Arduino-mbed core and
ignores `board_build.core = earlephilhower` without a word. FINDINGS.md recorded
that months ago; `platformio.ini` did not act on it, so the firmware built only
on machines where someone had once installed the fork by hand. On a clean
checkout it failed on `<Arduino.h>`. Pinning the fork by tag is what makes
"clone, bootstrap, build" true rather than nearly true.

**Escape hatches, because a bootstrap that can only do one thing gets replaced.**
`--no-asdf` uses the `node` and `python3` already on `PATH` and refuses them if
they are not the pinned major version, which is what to use with
`actions/setup-node`; `--check` verifies an install and changes nothing, which
is what a CI job runs to fail fast with a legible message.

## 2026-08-16 — Phase 2: the CDC/ACM host backend

### D8. The USB host stack gets core1, and talks to core0 through rings

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

### D9. A detach faults the port; it does not quietly close it

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

### D10. `STATUS` gains a `present` field rather than relying on events alone

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

### D11. `INFO.caps` on this backend claims DTR, RTS and hot-plug — and nothing else

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

## 2026-08-16 — Phase 2 review

### D12. A ring is emptied by the core that consumes it, never by the other one

**Question.** D8 argued that the synchronous mailbox is what makes `OPEN` and
`FLUSH` able to empty the cross-core rings: core0 is blocked in `runOp()` while
core1 runs the op, so core1 can call `SpscRing::clear()` without racing
anyone. Does that hold?

**Decision.** No, and the rings are no longer cleared that way. `toDevice_` is
emptied by core1 and `fromDevice_` by core0 — in both cases by the core that
owns the ring's *consumer* end, calling `discard(size())`, which touches only
that core's own tail index. `clear()` stays in `spsc_ring.h` for the
initialisation case and is not called across cores at all.

**Why.** The invariant D8 relied on has a hole in it, and the hole is the
timeout D8 itself introduced. Core0 gives up after a second precisely because
core1 can be stuck for ever inside a TinyUSB control transfer (FINDINGS.md).
When it does give up, core1 still owns the op and will still run it whenever it
comes back — and by then core0 is running again. `clear()` resets both indices,
so at exactly that moment it would be racing the core it was meant to exclude.
Nothing in the old code could tell the two cases apart.

A consumer-side `discard()` needs no such agreement. The producer only ever
writes `head_` and the consumer only ever writes `tail_`, so dropping the
unread remainder is the same operation as reading it and throwing it away, and
is safe against a producer running flat out. The rule is now a property of the
data structure rather than of the handshake wrapped around it, which is the
kind of safety that survives someone changing the handshake.

The mailbox stays synchronous, for the reason that was always the strongest
one: `open()` has to be able to tell the engine whether the adapter accepted
the line coding.

### D13. Delivery outlives the port, and `OPEN` is a hard boundary

**Question.** `Bridge::poll()` drains `toHost_` outside the `state_ == Open`
check, so bytes received before a `CLOSE` or a detach still reach the host.
Review found the drain half-built: the host's `CREDIT` was answered
`ERR_NOT_OPEN`, so a 64-byte window let exactly 64 of 300 buffered bytes out
and stranded 236; and frames already handed to the sink crossed into the next
session, arriving after the host had reset its own sequence counter. Keep the
drain and finish it, or delete it?

**Decision.** Keep it, and finish it in three places.

- `CREDIT` is legal with no port open (PROTOCOL.md §4.2). It grants a window;
  there is nothing about a port for it to break.
- `OPEN` calls `sink_.discardQueued()` as well as clearing `toHost_`, so the
  boundary catches frames that have already been framed. `RESET` and `HELLO`
  already did this.
- The host client resets its session *after* the `STATUS` reply to `OPEN`
  rather than before sending the request. SysEx is ordered, so everything
  ahead of that reply belongs to the old session and is discarded with it.

**Why keep it.** The case it exists for is an adapter unplugged mid-transfer,
and the bytes at risk are the last ones the far end sent — the tail of a
firmware upload, the end of a log. PROTOCOL.md §5.8 has promised since phase 2
that "bytes already received from the far end are still delivered", and
`test_bytes_received_before_a_detach_still_reach_the_host` has asserted it. The
alternative was to delete the drain and amend that promise to "delivered if the
timing works out", which is the kind of qualified guarantee this project exists
not to make.

**Why it was worth checking rather than assuming.** Every one of the three
gaps was invisible from the code that had the drain in it. The drain looked
complete; what defeated it was an allow-list two functions away, a discard that
happened on one command and not its sibling, and an ordering on the far side of
the link. That is the shape of a feature that is announced but not delivered —
and the measurements in the review (64 of 300 bytes; 200 stale bytes and a
sequence-gap warning after `#resetSession`) are what turned "looks fine" into
"is not".

**Cost, stated plainly.** A host must accept `DATA` while its port is closed,
and must keep returning `CREDIT` until it stops arriving. A host that reopens
without reading the tail loses it — deliberately, because that is what a
session boundary is for, and losing it at a boundary the host chose is not the
same as losing it silently mid-stream.

