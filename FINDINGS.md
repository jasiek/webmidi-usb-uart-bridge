# Findings

Things that were not obvious in advance, recorded as they were learned so the
next person does not rediscover them.

## Toolchain

- PlatformIO's official `raspberrypi` platform only ships the old Arduino-mbed
  core. The Earle Philhower `arduino-pico` core — the one with Adafruit
  TinyUSB, `Serial1`, and Pico-PIO-USB support — comes from
  `https://github.com/maxgerhardt/platform-raspberrypi.git`. Installing the
  platform by name gets you a core that cannot do any of what this project
  needs.
- `board_build.core = earlephilhower` does not rescue that: the registry
  platform ignores the key silently, installs `framework-arduino-mbed` anyway,
  and the build dies on `<Adafruit_TinyUSB.h>` and then on `<Arduino.h>`. A
  machine where someone had once installed the fork by hand built fine, which
  is what hid it — `platform` has to name the fork, pinned to a tag, and now
  does.
- `pio` is not on `PATH` by default. `./bootstrap.sh` puts it at `.venv/bin/pio`,
  installed from the version pinned in `requirements.txt`; before that script
  existed it was wherever the installer had left it, typically
  `~/.platformio/penv/bin/pio`.
- asdf shims are stubs that re-exec `asdf` itself, so putting only
  `~/.asdf/shims` on `PATH` yields `exec: asdf: not found` from every shimmed
  binary. Both `~/.asdf/bin` and `~/.asdf/shims` have to be there — including in
  `$GITHUB_PATH`, where the failure surfaces one step later than the mistake.
- A bash `EXIT` trap sets the script's exit status from the last command it
  runs, so a trap ending in a falsy test (`[ -n "$tmp" ] && rm -rf "$tmp"`)
  turns every clean exit into exit 1. `bootstrap.sh` ends its cleanup with an
  explicit `return 0`.
- `asdf install python` builds CPython from source, which needs the openssl,
  zlib and libffi headers present *before* it starts. `bootstrap.sh` probes for
  them with the compiler and names the missing packages, because python-build's
  own failure is 200 lines of make output that does not.

## MIDI on a headless machine

- There is no `/dev/snd` on a stock Linux CI runner, so RtMidi's ALSA backend
  cannot create a sequencer client and `new midi.Input()` throws "Failed to
  initialise RtMidi" — before any port is opened. Anything that enumerates or
  opens MIDI ports (`npm run list`, `probe`, `loopback` against hardware) is
  therefore not runnable in CI on Linux; `npm test` and `npm run loopback --
  --fake` are, because neither touches RtMidi. That is a second reason for
  `host/src/fake-device.js` beyond the one in the loopback notes.
- `@julusian/midi` is an N-API addon (`napi_versions: [7]`), so its prebuilt
  binaries are keyed to the ABI version rather than to a Node release. Moving
  the pinned Node version does not force a compile, and a machine with no ALSA
  headers can still install it.

## Testing

- Unity's `TEST_ASSERT_EQUAL_HEX8_ARRAY` **fails** when the length is zero —
  "You Asked Me To Compare Nothing, Which Was Pointless." A loop that sweeps
  lengths from 0 upward has to guard the zero case, which is exactly the case
  worth sweeping.
- The `native` test environment needs `lib_compat_mode = off`, otherwise the
  library dependency finder refuses `lib/bridge_proto` on the grounds that it
  declares no compatible framework. Keeping the protocol library free of
  Arduino headers is what makes host-side testing possible at all, so this is
  a setting worth having rather than a workaround.

## TinyUSB on the RP2040

- `CFG_TUD_MIDI_TX_BUFSIZE` is `#define`d **unconditionally** at 64 in
  `Adafruit_TinyUSB_Arduino/src/arduino/ports/rp2040/tusb_config_rp2040.h`, so
  no build flag can raise it. Our largest frame is 154 bytes, which means a
  frame can never be handed to `tud_midi_stream_write()` in one call. Anything
  that treats a short write as success will emit truncated SysEx. `UsbMidiSink`
  keeps its own 4 KB outbound buffer for exactly this reason.
- The device cannot be MIDI-only: `-DCFG_TUD_CDC=0` fails to compile, because
  `Adafruit_TinyUSB_API.cpp` calls `tud_cdc_n_write_flush()` without guarding
  on `CFG_TUD_CDC`. The bridge therefore enumerates as composite MIDI + CDC.
  No real loss — the CDC interface is a free debug console — but it is not a
  choice, it is a constraint.
- `tud_task()` does not need pumping from `loop()`: the rp2040 port hangs a
  shared handler off `USBCTRL_IRQ` that raises a soft IRQ to run it.

## The Arduino UART layer (arduino-pico)

- `SerialUART::write()` calls `uart_putc_raw()`, which **spins** until the
  hardware FIFO has room, and `availableForWrite()` returns only 0 or 1 — not
  a byte count. Both are unusable for a non-blocking bridge: blocking in the
  TX path stalls the USB service loop and costs MIDI packets. `UartBackend`
  writes to `uart_get_hw(uart0)->dr` directly instead, guarded by
  `uart_is_writable()`. The RX side's software FIFO is fine and is used as-is.
- The core's UART ISR **silently discards** characters with framing or parity
  errors (`SerialUART.cpp`, `if (raw & 0x300) continue;`) and records nothing,
  so those two conditions cannot be reported to the host through this core.
  Break and software-FIFO overflow *are* available, via `getBreakReceived()`
  and `overflow()`. `INFO.caps` reflects what is genuinely there.
- `setRTS()`/`setCTS()` do not give you settable modem lines: `begin()` passes
  them to `uart_set_hw_flow()`, so the UART drives RTS itself. RTS/CTS are a
  *mode* on this backend, not lines the host can poke — hence `kCapFlowRtsCts`
  without `kCapRts`/`kCapCts`.

## The wedge: TinyUSB calls race with tud_task()

The one that cost the most to find, so the reasoning is worth keeping.

**Symptom.** Under sustained traffic the device would stop answering — MIDI
completely mute — while remaining fully enumerated on USB, with its CDC port
still present. Intermittent: sometimes minutes of load, sometimes seconds.

**Why it looked impossible.** USB stayed up because on this port `tud_task()`
does **not** run from `loop()` — Adafruit's rp2040 backend hangs a handler off
`USBCTRL_IRQ` that raises a soft IRQ to run it. So enumeration, and the CDC
interface, survive a completely dead `loop()`. "The device is still there" says
nothing about whether the firmware is running.

**Finding it.** A once-a-second status line on CDC showed `loop()` stopping
dead — no gradual slowdown, max loop time 759 µs right up to the last line.
That rules out anything cumulative. Since the hang takes the CPU with it, the
only way to see where it was is a marker that survives the reset the watchdog
causes: the RP2040's **watchdog scratch registers** (`scratch[4..7]` are free
for application use). Writing a phase code there each step of `loop()`, and
printing it on the next boot, named the culprit in one run: `pumpUsbMidi`, and
sometimes `sink.service`.

**Cause.** Both call `tud_midi_*` directly from `loop()`, and `tud_task()` can
preempt them from IRQ at any point — including part-way through updating the
very endpoint FIFOs those calls are reading and writing. Nothing in the
Adafruit Arduino wrappers guards against it.

The interlock exists, but it is the *caller's* job to use it. Adafruit's task
runner does `mutex_try_enter(&__usb_mutex)` and skips its turn when it cannot
get the lock — the comment says "if the mutex is already owned, then we are in
user code which will do a tud_task itself". That only works if user code
actually holds `__usb_mutex`. `src/usb_lock.h` does.

**Fix and evidence.** Before: wedged within one or two throughput sweeps, every
time. After: four consecutive sweeps clean, then the full loopback suite, a
64 KB bulk transfer and another sweep with no watchdog reset at all. It also
made things *faster* — 64 KB round trip went from 7110 ms to 5898 ms, because
the races were corrupting work that then had to be redone.

**Two lessons worth keeping.** Hold `__usb_mutex` around every `tud_*` call on
this port. And bound work per `loop()` iteration rather than looping until a
producer-fed queue is empty — `pumpUsbMidi` now stops after 16 reads, which was
not the cause here (the counter showed it never reached the bound) but is the
difference between a slow loop and one that never returns.

## Measured limits

From `host/bin/throughput.js` on a Pico 1 over CoreMIDI, with the loopback
jumper fitted:

| Baud   | Line rate  | host → UART | UART → host | Lost |
| ------ | ---------- | ----------- | ----------- | ---- |
| 57600  | 5.6 kB/s   | 6.4 kB/s    | 5.6 kB/s    | 0    |
| 115200 | 11.3 kB/s  | 12.8 kB/s   | 11.2 kB/s   | 0    |
| 230400 | 22.5 kB/s  | 25.5 kB/s   | 22.5 kB/s   | 0    |
| 460800 | 45.0 kB/s  | 50.2 kB/s   | 44.6 kB/s   | 0    |
| 921600 | 90.0 kB/s  | 53.4 kB/s   | —           | 0    |

The tunnel flattens at **~53 kB/s one-way**, and ~48 kB/s in each direction
concurrently. That is the real limit, not the UART: at 921600 the line rate is
90 kB/s and the tunnel simply cannot carry it.

`INFO.maxBaud` now reports 460800 rather than the 921600 uart0 can clock. The
old value was a promise the bridge could not keep — a host trusts that field to
choose a safe rate.

A caveat worth stating: **a loopback cannot demonstrate overrun**, because the
device cannot receive faster than it transmits and its transmission is
credit-paced. Nothing was lost even at 921600 for that reason alone. A far end
that transmits independently has no such limit, so 460800 (45 kB/s each way,
against a ~48 kB/s ceiling) has very little margin. 230400 and below have
plenty.

## On real hardware

- **CoreMIDI caches MIDI port names by VID/PID.** Setting
  `TinyUSBDevice.setProductDescriptor("UART Bridge")` changes what the USB
  descriptor reports — `ioreg` confirms it immediately — but a Mac that has
  already seen the board goes on calling the MIDI port "Pico" indefinitely.
  The USB descriptor and the MIDI port name are not the same string as far as
  macOS is concerned. Rather than delete the user's MIDI configuration, the
  host tools try several name candidates in order (`DEFAULT_PORT_MATCH`).
- The interface string descriptor (`usb_midi.setStringDescriptor`) is *not*
  what macOS surfaces as the port name; the device product descriptor is.
  Setting only the former, as the Adafruit examples do, leaves the port named
  after the board.
- Measured round-trip latency for a `PING`/`PONG` over USB MIDI is **≈1 ms**
  (0.77 ms best of 10). SysEx through CoreMIDI is not the bottleneck anyone
  worries it will be.
- A floating RX pin is not silent. With no loopback jumper, an all-zeros
  payload on the adjacent TX pin produced 124 "received" bytes out of 256 via
  crosstalk, with no error flags — because the core's ISR discards framing
  errors silently (see above). An incrementing payload produced none. If a
  loopback test half-works, suspect the wire before the code.

## The host client

- **Never `unref()` a timer that carries protocol traffic.** Both the credit
  idle timer and the fake device's wire timer were originally unref'd, on the
  reasoning that a CLI should not be held open by housekeeping. But a credit
  window means both ends spend most of a throttled transfer waiting on the
  other, and at that moment the only pending work in the process *is* those
  timers — so Node saw an empty event loop and exited mid-transfer, with exit
  code 0 and no error. `npm run loopback -- --fake` printed its header, no
  result rows, and "passed" nothing. `destroy()`/`close()` is the right place
  to stop the timers; the event loop is not.
- That bug was found by the software device model, not by hardware, and only
  at payload sizes large enough to be throttled. It is the clearest argument
  for `host/src/fake-device.js` existing at all.

## Protocol

- 7-in-8 packing has no single canonical bit order. Both "MSB byte first, bit
  `i` = byte `i`" and "MSB byte last" conventions exist in shipped products,
  and they are not interoperable. `PROTOCOL.md` §2 pins ours down with a worked
  example, and `test_spec_vector` asserts it, so a future reimplementation
  cannot silently drift.
- Credits have to be cumulative deltas rather than absolute levels. With
  absolute levels a lost message leaves the sender believing it has *more*
  window than the receiver has buffer, which overflows silently; with deltas
  the same loss shrinks the window and the link stalls visibly instead.
