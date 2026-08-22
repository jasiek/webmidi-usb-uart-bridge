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


### D14. One TinyUSB version, pinned on the base environment

**Question.** `pico` built against the 3.4.4 bundled with the arduino-pico
core and `pico_cdc` against 3.7.7 from the registry, because naming
Pico-PIO-USB in `lib_deps` makes the dependency finder resolve TinyUSB from the
registry too. Two USB stacks in one project. Unify them, or record the split?

**Decision.** Unify on 3.7.7, pinned in `[env:pico]` so every environment
inherits it. 3.7.7 is also the newest published version (2026-05-12), so this
is not a bump on both sides — it is the device-only environments moving up to
what phase 2 was already running.

**Why the version is not the main point.** The split made `pico_cdc`'s stack a
side effect of a *different* dependency. Remove or reorder the Pico-PIO-USB
entry and phase 2 silently falls back to 3.4.4, where the FTDI async control
path is a `// TODO not implemented yet` stub — which was cause 1 of the core1
hang fixed in `a82f34e`. Pinning on the base environment removes that failure
mode. It also removes the one that already cost real time: reading the core's
3.4.4 source while the registry's 3.7.7 was being compiled produced two
confident and wrong conclusions (FINDINGS.md).

**What it cost, and why it was cheap.** Phase 1's numbers were measured on
3.4.4, so they had to be re-measured rather than assumed. On the loopback rig,
on 3.7.7: five bauds returning all 4096 bytes; five throughput sweeps matching
the recorded table within noise (50.2 → 50.0–50.2 kB/s at 460800); zero bytes
lost anywhere; and the `__usb_mutex` wedge did not recur — 82 seconds of
one-a-second status lines across two sweeps with monotonic uptime, `boot=soft`,
`last=none`, no watchdog reset and no drops or stalls. That last check is the
one that mattered: the wedge used to appear within one or two sweeps, and
`src/usb_lock.h` binds to whichever TinyUSB copy is compiled, because the
core's own `__usb_mutex` is compiled out under `USE_TINYUSB`
(`RP2040USB.cpp:22`) and the library's rp2040 port defines it instead.

**What this does not settle.** Phase 2 throughput is still unmeasured
(OPEN-ISSUES 6), and unifying the version did not fix the phase 2 byte loss —
it did usefully narrow it, since phase 1 now loses nothing at 9600 on the same
stack that loses ~600 bytes there through the CDC backend.

### D15. Recovery from a wedged core1 is `REBOOT`, and the host asks for it

**Question.** Core0 can see that core1's USB host stack has stopped
(OPEN-ISSUES 1) but cannot restart it — `tuh_deinit()` is inert for this port,
Pico-PIO-USB unclaims nothing, and a second `USBHost.begin(1)` panics in the
SDK. So the only recovery left is resetting the board. Should the device do
that by itself?

**Decision.** No. A new protocol command, `REBOOT` (0x0B), acknowledged with
`EVT_REBOOTING` and followed by a reset. The device never reboots on its own
initiative.

**Why not automatically.** `src/main.cpp` already says, deliberately, that "a
core1 wedged inside a control transfer to a misbehaving adapter should leave
the MIDI tunnel up to say so, not reboot the board out from under the host that
is asking." That is still right. Core0 surviving core1 is the entire point of
the two-core split (D8), and the value of surviving is that someone can be
*told*. A device that reacts to a fault by vanishing from the bus destroys the
evidence and interrupts whoever was mid-conversation, on its own authority, to
fix something the host might not even care about — the far end may be an
adapter the host has finished with.

The host has everything it needs to decide: `ERR_BACKEND`, a port stuck in
`Fault`, and a tunnel that still answers. So it decides.

**Why a new command rather than overloading `RESET`.** `RESET` is a session
boundary with defined semantics — it discards buffers, resets sequences and
keeps the link up. Making it sometimes mean "and also disappear from USB for
two seconds" would make a documented guarantee conditional on device state the
host cannot see. Same reasoning as D5: a field a host trusts must mean one
thing.

**Shape of it.** `REBOOT` is legal with no port open, because the case it
exists for is a backend that cannot be opened at all. It closes nothing and
discards nothing first: a `CLOSE` would post an op to the very core that is not
answering, and a discard would throw away the acknowledgement with everything
else. The engine only decides *when* — `rebootDue()` — because
`lib/bridge_proto` stays free of Arduino and RP2040 headers (D1); `main.cpp`
decides how, and waits for the MIDI sink to drain first so the acknowledgement
is genuinely on the wire before the board goes.

**One bit of intent survives the reset.** `watchdog_hw->scratch[5]` carries a
magic value across the reboot, because `rp2040.reboot()` leaves exactly the
same reset reason as an upload's soft reset. Without it a board that rebooted
because it was told to is indistinguishable from one that rebooted for reasons
unknown, and only the second is worth investigating. The debug build reports
`boot=REBOOT-cmd`; confirmed on hardware.

**Cost, stated plainly.** The board leaves the USB bus and re-enumerates, so
this is not a session boundary but the device going away: everything buffered
in both directions is lost and the host must reconnect with a fresh `HELLO`.
PROTOCOL.md §5.10 says so rather than leaving a host to find out. It also does
not *fix* OPEN-ISSUES 1 — a wedge still costs a reboot. It removes the need for
physical access, which for a device driven from a phone is the difference
between a fault and a brick.

### D16. The bridge sets the FTDI's latency timer, and sets it high

**Question.** The FT232R loses bytes at every USB IN-packet boundary on this
stack — 715 of 4096 at 9600 baud, in 185 gaps whose spacing is a clean multiple
of the adapter's 16 ms latency timer. The timer is what creates those
boundaries on a slow line. What should the bridge do about it?

**Decision.** Send `FTDI_SIO_SET_LATENCY_TIMER` ourselves on mount, with a
value of **100 ms**, from `CdcHostBackend::startLatencyTimer()`.

**Why we send it rather than TinyUSB.** TinyUSB has `CFG_TUH_CDC_FTDI_LATENCY`
for exactly this and it cannot be used: the code behind the `#ifdef` calls an
undeclared `ftdi_process_config` and declares a variable inside a `switch` case
without braces, so defining the macro breaks the build (`cdc_host.c:1241`). It
is dead code nobody has enabled. Sending the vendor request from our own
backend needs no patched library and no build flag, and it is the same
asynchronous shape as the line-coding transfer the backend already does.

**Why high, when every instinct says low.** This is a latency knob turned the
wrong way on purpose. Bytes are lost *per packet boundary*, so fewer boundaries
means less loss, and on a slow line the timer is what manufactures them. At
9600 baud the default 16 ms is one boundary every 15.5 bytes; at 100 ms the
same transfer loses 1 byte instead of 715. The adapter also sends as soon as it
has 62 bytes of payload — 64 ms at 9600 — so above roughly that the timer stops
being the trigger at all and raising it further buys nothing.

**Cost, stated plainly.** This is the longest a lone byte can sit inside the
adapter before the bridge sees it. On a quiet line that is a real delay, and
100 ms would be noticeable to anyone typing at a terminal through the tunnel.
It is used because it is the value that was measured; the loss-against-latency
sweep that would justify something smaller has not been run, and until it has,
picking a smaller number would be a guess dressed up as a decision. Recorded as
provisional for that reason.

**And it is a workaround.** The per-boundary loss is a defect in the stack
below us, not something a timer value fixes — it makes it rare rather than
absent. The right repair is upstream, in whatever drops bytes at packet
boundaries; this buys a working phase 2 in the meantime and says so.

**Evidence.** Five bauds, 4096 bytes each, all returning complete, twice in a
row — against 3491 / 3566 / 4070 / 4073 / 4091 before. Confirmed independently
first with a patched library and then with this implementation.
## 2026-08-22 — Browser consumption (webchirp)

### D17. Browser API: the Web Serial `SerialPort` shape, shipped as one generated file

**Question.** The sibling webchirp project should be able to drive the bridge
from a browser. What interface does it get, and in how many files?

**Decision.** A Web Serial-shaped port over the Web MIDI API:
`createMidiBridgeSerial()` returns a provider with the same `requestPort()`
contract as `navigator.serial`, and the port implements the `SerialPort`
subset webchirp's four WebUSB chip drivers already agree on (`open`,
`readable`, `writable`, `setSignals`, `getSignals`, `getInfo`, `close`).
Delivered as **one self-contained ES module**, `dist/midi-bridge-serial.js`,
*generated* from `host/src/` by `bin/build-webserial.js` — concatenation in
dependency order with internal imports stripped, verified by import and by the
loopback tests on every `npm test`.

**Why the Web Serial shape.** webchirp's `BrowserSerialBridge` treats any
provider whose `requestPort()` yields that subset as interchangeable — its
FTDI, PL2303, CH340 and CP2102 WebUSB drivers all present it. Matching the
shape means the MIDI transport slots in with no change to webchirp's
consumption model; inventing a bridge-specific API would push MIDI knowledge
into every caller.

**Why one generated file rather than hand-written or multi-file.** webchirp
vendors self-contained single-file drivers into `web/js/`; a directory of six
modules does not fit that convention. But hand-writing a self-contained file
would create a *third* implementation of the protocol (firmware and host
client are deliberately two, kept honest by tests — a third would be kept
honest by nothing). Generating the file from the tested sources gives the
single-file ergonomics without the drift.

**Consequences worth recording.**

- Web MIDI has no per-device chooser — permission covers the whole MIDI
  system — so `requestPort()` *discovers* the bridge: name-ranked candidates,
  then a HELLO probe each must answer. Names alone are never trusted, because
  CoreMIDI caches them by VID/PID (FINDINGS.md).
- Data loss is fatal to the stream: a device `ERROR` or a receive-side
  sequence gap errors `readable` instead of resyncing quietly. For a port
  that claims to be a serial cable — under a radio-cloning app, no less — a
  stream that drops bytes and keeps going is worse than one that fails.
- `writable` carries real backpressure (a write resolves when the device has
  taken the bytes, paced by credit); `readable` does not push backpressure to
  the device, because the client returns credit on arrival. Bounded in
  practice by the ≤460800-baud far end.
- iOS Safari has no Web MIDI, so this file serves desktop/Android browsers;
  the iOS story remains native CoreMIDI apps.
