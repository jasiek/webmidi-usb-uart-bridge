# Open issues

What is known to be wrong and not yet fixed, as of the phase 2 hardware
bring-up on 2026-08-18. FINDINGS.md records what was *learned*; this records
what is still owed.

Each entry says what the symptom is, what has been ruled out, and what the next
step would be — so that picking one up does not start by repeating the
elimination.

The bench rig for all of this: a Pico 1 with a Pico-PIO-USB host port on
GPIO16/17 (22 Ω series, 15 kΩ pull-downs to ground, 5 cm of wire), an FTDI
FT232R (`0403:6001`) plugged into it with its serial TX shorted to its RX, and
`pio run -e pico_cdc_debug` for the counters.

---

## 1. Core1 stalls inside `tuh_task()` — intermittent, needs a power cycle

**Severity: blocker for phase 2.**

`host_tasks` stops advancing and never resumes. The debug build reports
`CORE1-STALLED` and `core1 stopped in: usb_task`, i.e. inside
`Adafruit_USBH_Host::task()` → `tuh_task_ext()`, not in any of our code.

Intermittent. The same adapter on the same wiring enumerates cleanly on one
boot and hangs the core on the next, and it can also hang mid-transfer after
minutes of correct operation.

**Ruled out.** Not our mailbox — the clearest instance had `enum=0` *and*
`op_timeouts=0`, so core1 died before an op was ever posted. Not the two
causes already fixed in `a82f34e`: the phase marker moved off `execute_op` when
the control transfers went asynchronous, and off `pump_device` when the pump
loops were bounded. Not the clock (dividers are exact at 120 MHz), not the
wiring (a low-speed mouse and the FTDI both enumerate over it), not VBUS
(5.14 V measured at the socket under load).

**Likely cause.** `tuh_control_xfer()` spins on
`while (result == XFER_RESULT_INVALID) tuh_task_ext(0, false);` with a
`// TODO probably some timeout to prevent hanged` above it, and the
`timeout_ms` field in `tuh_xfer_t` is present with `not supported yet` beside
it. Enumeration and the class drivers both reach it. A device that stops
answering mid-request therefore takes the core with it, and nothing in TinyUSB
will ever give it back.

**Next step.** This is not fixable from our side of the API, so the realistic
options are containment rather than repair:

- A supervisory reset of core1 when `hostAlive()` goes false — `rp2040`
  exposes `restartCore1()`, but re-running `USBHost.begin(1)` against PIO state
  machines and DMA channels that are already claimed needs checking before it
  can be relied on, and a half-initialised host port is worse than a dead one.
- Failing that, a full software reset via the existing watchdog path, which the
  protocol already models honestly: the far end goes away and comes back as
  `EVT_DETACH` / `EVT_ATTACH`, which the engine and the host client both
  already handle.

Core0 already detects the condition and reports `ERR_BACKEND` promptly rather
than presenting a dead host stack as an empty port, so the failure is at least
visible. It is not recoverable.

---

## 2. Nothing restarts a wedged core1 — recoverable now, on the host's word

**Severity: reduced from high on 2026-08-18. Recovery exists; it costs a reboot.**

Surviving the hang and recovering from it are different things, and only the
first is done. When core1 stops:

- core0 keeps running, the MIDI tunnel stays up, and the watchdog keeps being
  fed — this all works and is the point of the two-core split (DECISIONS.md D8).
- `claimOp()` sees `hostAlive()` false and fails immediately, so ops no longer
  cost a one-second timeout each. Also works.
- The port stays `Fault`, and nothing reclaims a mailbox from a core that is
  never coming back or restarts the core. **Still true**, and not fixable in
  place — see below.
- **What changed:** it no longer needs physical access. `REBOOT` (PROTOCOL.md
  §5.10, DECISIONS.md D15) lets the host reset the board over the tunnel that
  is still up, and `client.reboot()` drives it. The device never does this on
  its own initiative, which is deliberate: D15 has the argument.

Confirmed end to end on hardware — acknowledged in 1 ms, board reset and came
back reporting `boot=REBOOT-cmd`, with the loopback suite clean afterwards.

**And confirmed against the real fault**, which is better evidence than the
staged one. During the issue 3 work core1 wedged for real (`CORE1-STALLED`,
`core1 stopped in: pump_device`, `host_tasks` frozen at 24889759 across seven
consecutive samples). `REBOOT` recovered it over the still-live MIDI tunnel in
1 ms, with no physical access. Note the port had to be left closed: `OPEN`
fails with `ERR_BACKEND` on a dead backend, so a rescue path that opened one
first would never reach the reboot. That `REBOOT` is legal with no port open
(D15) is what made the rescue possible, and it was very nearly a nicety.

**Limitation found doing it: the far end does not come back.** After the
reboot core1 was healthy — `host_tasks` climbing, `op_timeouts=0` — but the
FT232R stayed `attached=0 enum=0` indefinitely, with the bus levels unchanged
at `bus=10`. A warm RP2040 reset does not cycle VBUS, so the adapter keeps the
USB address it was given before the reset and never presents the fresh
connect that TinyUSB enumerates on. Recovering the *bridge* therefore does not
recover the *link*: a downstream replug is still needed. Worth fixing by
forcing a port reset when a device is already present at startup, rather than
waiting for an attach edge that has already happened.

What is still owed here is the *in-place* recovery: a wedge costs a full reboot
and everything buffered with it. That is a real cost, and if issue 1 is ever
fixed upstream this issue mostly goes away with it.

Worth separating from issue 1 because it is *ours* and would be worth doing
even if TinyUSB grew a timeout tomorrow: any backend on a separate core needs a
recovery story, not just a detection story.

**Restarting core1 was investigated on 2026-08-18 and does not work.** The
option this issue used to prefer — `restartCore1()` and re-run
`USBHost.begin(1)` — is not merely risky, it is blocked at four separate
points, none of which is ours to move:

1. `tuh_deinit()` exists in TinyUSB 3.7.7 but is a no-op for this port. It
   does `TU_ASSERT(hcd_deinit(rhport))`, and the PIO-USB host controller
   defines no `hcd_deinit` — so the weak stub in `usbh.c:53` returns `false`
   and `tuh_deinit()` bails out having torn nothing down.
2. `pio_usb_host_stop()` only cancels the library's alarm-pool timer. It
   unclaims nothing.
3. Worse, it spins: `while (cancel_timer_flag) continue;`, and the flag is
   cleared by a callback serviced on core1. Calling it from core0 to recover a
   wedged core1 risks taking core0 down as well, which is the one thing the
   two-core split exists to prevent.
4. `pio_usb_bus_init()` claims three PIO state machines (`pio_usb.c:377-379`)
   and a DMA channel (`dma_claim_mask`, `pio_usb.c:394`), and nothing in the
   library ever unclaims them. A second `USBHost.begin(1)` therefore reaches
   `pio_sm_claim` on an already-claimed SM, and the SDK's `hw_claim_or_assert`
   **panics**.

The claims are reachable — `pio_usb_ll.h:110` exports `pio_port[1]`, so the SM
and DMA numbers could be unclaimed by hand — but that is reaching past two
libraries' interfaces into their internal state to undo initialisation neither
of them supports undoing, and it still leaves TinyUSB's device tree, endpoint
state and alarm pool dangling. Not a foundation for a recovery path.

**Which leaves the full software reset**, and that is now a decision rather
than a fallback, because `src/main.cpp:393` currently rules it out on purpose:
"a core1 wedged inside a control transfer to a misbehaving adapter should leave
the MIDI tunnel up to say so, not reboot the board out from under the host that
is asking." Rebooting automatically reverses that. The alternative is to make
the reboot host-commanded — the host can already see `ERR_BACKEND` and a
`Fault` port, so it has what it needs to decide — which means a new protocol
command, since overloading `RESET` would make a defined session-reset sometimes
mean something else.

---

## 3. The loopback loses bytes, and it is not slowness

**Severity: high. Undiagnosed.**

With both stalls fixed, all five bauds run to completion but none returns the
full payload:

| Baud   | Returned of 4096 | Short by |
| ------ | ---------------- | -------- |
| 9600   | 3491             | 605      |
| 19200  | 3566             | 530      |
| 38400  | 4070             | 26       |
| 57600  | 4073             | 23       |
| 115200 | 4091             | 5        |

The shortfall shrinks as the baud rises, which is the wrong way round for
anything driven by throughput pressure and looks like running out of time.

**It is not running out of time.** A 180 000 ms timeout at 9600 still stops at
~3494. It is a genuine stall, and the ~600-byte figure at 9600 is reproducible
across runs (3420, 3454, 3491, 3494, 3499, 3506).

**One variable removed.** Phase 1 and phase 2 now build the same TinyUSB
(issue 5), and on that same stack the hardware-UART loopback returns all 4096
bytes at 9600 where the CDC backend loses ~600. The loss is in the CDC path or
the adapter, not in the engine, the SysEx framing, the credit windowing or the
USB device stack — all of which the phase 1 rig exercises identically and
without loss.

**Ruled out at the desk, 2026-08-18 — do not re-test these at the bench.**

- *Floating CTS on the bench rig.* The theory was that `lines=0x03` (TinyUSB
  asserts DTR and RTS during enumeration via `CFG_TUH_CDC_LINE_CONTROL_ON_ENUM`)
  plus an unconnected CTS would stop the FT232R transmitting. It cannot:
  `cdc_host.c:1257` sends `FTDI_SIO_DISABLE_FLOW_CTRL` as part of the FTDI
  set-config sequence, so hardware flow control is off on the adapter and CTS
  gates nothing. No jumper on the FTDI's CTS is worth fitting.
- *A second, shorter timeout hiding behind `--timeout`.* `readExactly`
  (`host/src/client.js:250`) arms exactly one `setTimeout` for the whole read
  and has no idle or quiet timer, so the 180 000 ms run really did wait 180 s.
  The stall is confirmed genuine rather than an artefact of the measurement.
- *The host withholding credit.* `#maybeReturnCredit` returns credit as soon as
  `freed` reaches half the window, and otherwise arms a `CREDIT_IDLE_MS`
  fallback that fires on the tail. A host that has stopped receiving still
  grants. If credit is the mechanism, the accounting error is device-side, in
  `toHost_`/`fromDevice_`, not in the client.

**Where to look now.** The FT232R's 256-byte FIFOs and its latency timer on a
TX/RX loop are what remain of the original candidate list. Device-side credit
accounting — `toHost_` and `fromDevice_` in `cdc_host_backend` — is the new
one, and is where the desk work above points.

**The measurement was taken on 2026-08-18, and it clears our firmware.**

Sampling once a second through a 9600-baud run (4096 bytes, 3472 returned):

```
to_dev=960  from_dev=564      to_dev=3840 from_dev=3047
to_dev=1920 from_dev=1372     to_dev=4096 from_dev=3472   <- from_dev freezes here
to_dev=2880 from_dev=2207     to_dev=4096 from_dev=3472   ... and never moves again
```

`from_dev` freezes at the instant `to_dev` completes, and roughly 25 seconds
*before* the client's timeout expires — so this is not the client giving up and
the back-pressure story is not what stops it.

With the four buffers along the path instrumented, the state after the freeze
is unambiguous:

```
to_dev_ring=0  from_dev_ring=0  tu_tx_space=128  tu_rx_avail=0
```

Both our rings empty, TinyUSB's TX FIFO empty (128 is its whole capacity) and
its RX FIFO empty. Every byte was accepted by the USB host controller and
handed to the adapter; fewer came back. **The loss is downstream of our USB
host controller** — in the FT232R or the loopback wire — and not in the engine,
the rings, the pumps, the credit windowing or the device stack.

**It is not a large-payload overrun.** A payload-size sweep at 9600 loses bytes
at every size: 128 → 122, 256 → 230. So "we firehose the adapter faster than
its UART drains" does not survive contact either, at least not on its own.

**It is not truncation — individual bytes are dropped from the middle.** Sending
`0x00,0x01,0x02,…` and aligning what came back against what went out:

```
sent 4096, returned 3443, short by 653
diverges at offset 12
sent : 08 09 0a 0b 0c 0d 0e 0f 10 11 12 13
got  : 08 09 0a 0b 0d 0e 0f 10 11 12 13 14
                    ^^ 0x0c never came back; everything after shifts by one
```

So the far end is not stopping early and it is not stalling — it is losing
bytes one at a time, all the way through, at roughly 16% of the stream. That
kills "it ran out of time" for good, and it also kills the framing of this
issue as a *stall*: the earlier `from_dev` freeze is the tail of the loss, not
its mechanism.

**Leading hypothesis: nothing paces our writes to the far end's line rate.**
`tuh_cdc_write()` accepts bytes at USB speed and `tuh_cdc_write_available()`
reports space in *TinyUSB's* FIFO, not the adapter's. At 9600 baud the FT232R
can clock out 960 bytes a second and we hand it bytes several thousand times
faster, with no end-to-end back-pressure anywhere in between. Phase 1 never had
this problem because `UartBackend::write` gates on `uart_is_writable()`, which
is real hardware back-pressure; the USB hop hides it. It also explains the baud
dependence exactly — the faster the line, the smaller the mismatch.

**What would confirm it**, and is the obvious next step: pace the outbound pump
to the configured baud (a token bucket in `pumpDevice`, bytes ≈ elapsed × baud
÷ 10) and see whether the loss disappears. If it does, that is both the
diagnosis and the fix.

**Not yet characterised**, and worth doing first because it is cheap: the
*distribution* of the drops. A buffer overrun should lose contiguous runs when
the buffer fills, not scattered single bytes, and the one run measured so far
starts with a single-byte drop at offset 12 — early, and small. `gaps.mjs` in
the session scratchpad does this analysis; the run was cut short by issue 1
wedging core1 mid-transfer.

The older candidate — TinyUSB stripping the FTDI's 2-byte status header from
the start of the transfer rather than from each 64-byte packet
(`cdc_host.c:704-712`) — is now *less* likely: that would inject extra bytes,
and what is observed is bytes going missing.

---

## 4. Enumeration stops for good once the root port is stuck suspended

**Severity: medium. Possibly not our problem, but unexplained.**

The original USB-serial "loopback dongle" and a PL2303 both reach `conn=1
fullspeed=1 susp=0` — detected and bus-reset by TinyUSB — and never fire a
mount callback. On the same wiring an FTDI enumerates and a low-speed mouse
enumerates.

Note this is *not* the PL2303 driver gap it first looked like: TinyUSB 3.7.7,
which is what `pico_cdc` actually builds, has a `SERIAL_DRIVER_PL2303`. That
claim came from reading 3.4.4 by mistake (issue 5) and has been retracted.
Enumeration happens below the class drivers in any case, and enumeration is
what is failing.

**The FTDI does it too.** Observed 2026-08-18: after core1 wedged, the FT232R
that had been working stopped enumerating and would not start again.

**There are two distinct stuck states, and only one of them is the deadlock
below.** Both show `attached=0 enum=0` with core1 perfectly healthy, so the
`susp` field is the only thing that tells them apart:

- `conn=1 fullspeed=1 **susp=1**` — the library deadlock described below. The
  port never got its bus reset, so it is not operating at all.
- `conn=1 fullspeed=1 **susp=0**` — the port *is* operating, SOFs are going
  out, and `susp=0` proves TinyUSB did drive a port reset to completion. So
  enumeration was attempted and then quietly failed, with no mount callback and
  no error. This is the signature this issue was originally filed with, it is
  the state the bench was left in after the pacing build was flashed, and the
  deadlock does **not** explain it. Undiagnosed.

The deadlock, which was observed live and is certainly real:

Watching a live unplug/replug with the port stuck in the `susp=1` state:

| | `bus` | `conn` | `susp` |
| --- | --- | --- | --- |
| before unplug | 10 | 1 | 1 |
| unplugged     | 00 | 1 | 1 |
| replugged     | 10 | 1 | 1 |

`conn` never drops. It cannot, and the reason is a short-circuit in
`pio_usb_host.c:266`:

```c
if (!(root->initialized && root->connected && !root->suspended &&
      connection_check(root))) { continue; }
```

`connection_check()` is the *only* code that detects a disconnect and clears
`root->connected` (line 229). It is the last term of a `&&` chain, so it is
never evaluated while `root->suspended` is true. And the matching
new-connection scan (line 332) runs only `if (root->initialized &&
!root->connected)`.

So `connected && suspended` is a trap with no exit: disconnects cannot be seen
because the port is suspended, and connects cannot be seen because the port
still believes something is connected. Only `pio_usb_host_port_reset_end()`
clears `suspended`, and TinyUSB calls that solely as part of enumerating a
device it has just been told about — which is the thing that is not happening.

The state is entered legitimately: on connect the library sets
`suspended = true` with the comment `// need a bus reset before operating`
(line 340), expecting TinyUSB to reset the port promptly. If enumeration hangs
or never starts, the port is stuck there for good.

**The recovery that works is replug *and* `REBOOT`, in that order.** Neither
alone does: the replug cannot be seen by a suspended port, and a reboot alone
leaves the adapter holding its old USB address. Together they enumerate first
time — confirmed, `attached=1 enum=1(0403:6001) susp=0`.

**The fix this suggests** is ours to make and does not need the library
changed: `pio_usb_host_port_reset_start/end` are declared in `pio_usb_ll.h`,
which `src/pio_usb_probe.c` already includes. Core1 could watch for
`connected && suspended` persisting with nothing enumerated, and drive a reset
cycle to clear `suspended` so `connection_check()` can run again. Clearing
`connected` as well — to force the connect scan to re-fire — means writing to
another library's state, which is the same category of move that was rejected
for the core1 restart, so it wants a decision rather than a commit.

Still worth doing: try the two original adapters on a known-good host. But
"two bad adapters" is no longer the leading explanation, because the adapter
that works reaches the same state.

---

## 5. ~~`pico` and `pico_cdc` build against different TinyUSB versions~~ — fixed

**Resolved 2026-08-18. Kept for the re-validation evidence.**

Every environment now builds Adafruit TinyUSB 3.7.7, pinned in `[env:pico]` and
inherited by the rest. 3.7.7 was already the newest published version, so this
moved `pico` and `pico_debug` up to what phase 2 was running rather than
bumping both. DECISIONS.md D14 has the reasoning; the part worth repeating here
is that the split made phase 2's stack a side effect of the Pico-PIO-USB
dependency sitting next to it, so dropping that entry would have silently
reverted phase 2 to the 3.4.4 stub that caused the core1 hang.

Confirm which copy an environment compiled by the object path, not the source
tree:

```
find .pio/build/pico -name 'cdc_host.c.o'
# .pio/build/pico/lib4b0/Adafruit TinyUSB Library/...   registry, 3.7.7
# .pio/build/pico/libXXX/Adafruit_TinyUSB_Arduino/...   core's bundled 3.4.4
```

**Re-validated on the loopback rig** (GPIO0–GPIO1 jumper), because the phase 1
numbers in FINDINGS.md had been measured on 3.4.4:

- `loopback.js`, twice: all five bauds, 4096 of 4096 bytes each time.
- Five throughput sweeps, matching the recorded table within noise — 6.4/5.6,
  12.8/11.2, 25.4/22.5, 50.0–50.2/44.5 kB/s. Zero bytes lost at any baud.
- A 64 KB bulk transfer at 460800: 45.8 kB/s out, 44.7 kB/s back, 0 lost.
- No `__usb_mutex` wedge. It used to appear within one or two sweeps; two
  sweeps on `pico_debug` gave 82 consecutive status lines with monotonic
  uptime, `boot=soft(last=none)`, `dropped=0 stalls=0` and never `BLOCKED`.

921600 no longer appears in the sweep because `INFO.maxBaud` reports 460800
(D5). That is the old row disappearing, not a regression.

## 6. Phase 2 throughput has never been measured

**Severity: medium.**

`INFO.maxBaud` reports 460800 on the CDC backend, and that number is inherited
wholesale from phase 1's measured tunnel ceiling (DECISIONS.md D5). Nothing has
measured what this backend can actually carry — and it has a different shape,
with a USB hop and two cross-core rings where phase 1 had a UART register.

`host/bin/throughput.js` exists and is what produced the phase 1 table. It
cannot be run meaningfully until issue 3 is fixed, since a sweep that loses
bytes measures nothing.

Until then `maxBaud` is a promise the backend has not been shown to keep, which
is precisely the thing D5 exists to stop.

---

## 7. The bench rig cannot prove the top end

**Severity: low. Known limitation, already in `hardware/README.md`.**

A single adapter with TX shorted to RX cannot transmit faster than it is being
sent to, so it cannot demonstrate overrun or find the real ceiling. Two
adapters wired TX↔RX to each other is the rig that can. Same caveat as phase 1,
recorded here so it is not forgotten when issue 6 is picked up.

---

## 8. Flashing `pico_cdc` sometimes drops the board off USB entirely

**Severity: low. Possibly environmental.**

Three times during bring-up, `pio run -t upload` did the 1200-baud touch, the
board left MIDI mode, and it never came back as a BOOTSEL device — not
enumerating at all, with `picotool info -a` finding nothing. Recovery is a
manual BOOTSEL-held replug. `picotool reboot -f -u` does not help; the firmware
exposes no reset interface.

Twice the board was behind a VIA Labs hub, which is the obvious suspect, but it
happened at least once on a direct port. One of the three was self-inflicted
and is understood — a build that routed TinyUSB's host log to the shared CDC
console wedged the device stack, so the touch had nothing to listen for it (see
FINDINGS.md; do not retry that).

Worth watching rather than chasing. If it recurs on a direct port with a clean
build, it is real.
