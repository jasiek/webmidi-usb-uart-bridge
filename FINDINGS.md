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
- On this machine `pio` is not on `PATH`; it lives at
  `~/.platformio/penv/bin/pio`.

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

## TinyUSB as a USB *host* (phase 2, Pico-PIO-USB)

- **The host stack is switched on by an include path, not by a flag.** The
  core's `tusb_config_rp2040.h` does
  `#if __has_include("pio_usb.h")` and enables `CFG_TUH_ENABLED` and
  `CFG_TUH_RPI_PIO_USB` if it succeeds — otherwise it enables a MAX3421E host
  instead. The `lib_deps` entry on Pico-PIO-USB is what makes it succeed, and
  it is sufficient on its own: PlatformIO puts the checked-out library's
  `src/` on the include path of every library that depends on it, Adafruit
  TinyUSB included, so the conditional resolves during TinyUSB's own
  compilation. No `-I` of ours is needed.
- **The `-I` we had for this was a no-op, and the reason we thought we needed
  it was wrong.** It read
  `-I$PROJECT_LIBDEPS_DIR/$PIOENV/Pico-PIO-USB/src`, which names a directory
  that has never existed: PlatformIO checks the dependency out under the name
  in its `library.json`, which is `Pico PIO USB` — with spaces. The comment
  above it claimed that without the flag the build falls back to a MAX3421E
  host. It does not. On a clean `pio run -e pico_cdc` with the flag deleted,
  `hcd_pio_usb.c.o` is 5228 bytes with 18 text symbols and `hcd_max3421.c.o`
  is 672 bytes with none, and `pio_usb_host_init` is in the ELF while nothing
  matching `max3421` is — i.e. `CFG_TUH_RPI_PIO_USB` is 1 and the MAX3421
  driver is compiled out to nothing. That is byte-for-byte what the build
  produced *with* the flag, which is the point.
- The way to check this for real, and the only way worth trusting, is at the
  object level rather than by reading the ini:

  ```
  cd '.pio/build/pico_cdc/libebb/Adafruit TinyUSB Library/portable'
  arm-none-eabi-nm raspberrypi/pio_usb/hcd_pio_usb.c.o | grep -c ' [Tt] '   # 18
  arm-none-eabi-nm analog/max3421/hcd_max3421.c.o      | grep -c ' [Tt] '   # 0
  arm-none-eabi-nm .pio/build/pico_cdc/firmware.elf | grep pio_usb_host_init
  ```

  The toolchain's `nm` is at
  `~/.platformio/packages/toolchain-rp2040-earlephilhower/bin/`, not on `PATH`.
- PlatformIO quotes the include paths it generates, so the space in
  `Pico PIO USB` is not a problem for it: `pio run -v` shows
  `"-I.pio/libdeps/pico_cdc/Pico PIO USB/src"` as one argument. A hand-written
  `-I` for the same directory would have to be quoted the same way, which is
  the other half of why the one we had could never have worked.
- Almost none of that config file is `#ifndef`-guarded, so build flags cannot
  override it. `CFG_TUH_CDC_RX_BUFSIZE`/`TX_BUFSIZE` are fixed at 128 bytes and
  `CFG_TUH_CDC_LINE_CONTROL_ON_ENUM` at `0x03` — meaning **TinyUSB asserts DTR
  and RTS on the downstream device during enumeration**, before any of our code
  runs. That resets most boards worth plugging in, and there is no supported
  way to stop it. What we can do is not make it worse: the backend seeds its
  idea of the lines from `tuh_cdc_get_dtr()`/`get_rts()` at mount, so `OPEN`
  does not toggle DTR a second time on its way to a state it is already in.
- `tuh_cdc_set_line_coding()` with a null callback is the one to use even
  though `set_baudrate` and `set_data_format` exist: for FTDI, CP210x and CH34x
  — which are not CDC at all, just re-using the driver API — it internally
  falls back to issuing the two as separate requests.
- **Blocking control transfers have no timeout.** `tuh_control_xfer()` spins on
  `while (result == XFER_RESULT_INVALID) tuh_task_ext(0, false);` with a
  `// TODO probably some timeout to prevent hanged` above it. An adapter that
  stops answering therefore hangs the core running the host stack, for ever.
  That is the reason the backend's mailbox is a timed handshake rather than a
  direct call: core1 can hang, but core0 gives up after a second, keeps feeding
  the watchdog, and the MIDI tunnel stays up to report `ERR_BACKEND`.
- A single-slot mailbox has to be **claimed before its arguments are written**,
  not checked afterwards. The first version of `runOp()` had callers fill in
  `pending_` / `pendingFlush_` and *then* call a function whose first act was
  to refuse if core1 still owned the previous op — by which point the
  arguments core1 was reading had already been overwritten. The guard was
  guarding a door it had already walked through. The check-and-claim is one
  compare-exchange and reads no worse.
- **An SPSC ring can be emptied safely by its consumer and only by its
  consumer.** `discard(n)` moves the tail, which the consumer already owns, so
  it races nothing; `clear()` resets both indices and is only safe when the
  other core is quiet. Relying on "core0 is blocked in the handshake, so it is
  quiet" does not work here, because the handshake gives up after a second and
  the op it abandoned still runs later — see DECISIONS.md D11.
- `Adafruit_USBH_Host::task()` defaults to `timeout_ms = UINT32_MAX`, which
  blocks in the event queue until the USB stack has something to say. In a
  `loop1()` that also has to move bytes, that default means the byte-moving
  half runs only when USB happens to generate an event. `USBHost.task(0)`.
- Pico-PIO-USB needs a system clock that is a multiple of 12 MHz and the Pico's
  default 125 MHz is not one. Setting it from `setup()` is too late — the core
  has already configured peripherals against the old divisors — so it belongs
  in `board_build.f_cpu`, which the core applies with `set_sys_clock_khz()`
  before anything else starts.
- arduino-pico launches core1 **before** `setup()` runs, not after
  (`cores/rp2040/main.cpp`), so `setup1()` and `setup()` race by default. The
  device and host stacks initialising concurrently is not a race worth finding
  out about: one atomic flag, set at the end of `setup()` and spun on at the
  top of `setup1()`, orders them.

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
