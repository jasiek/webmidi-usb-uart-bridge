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
  the op it abandoned still runs later — see DECISIONS.md D12.
- `Adafruit_USBH_Host::task()` defaults to `timeout_ms = UINT32_MAX`, which
  blocks in the event queue until the USB stack has something to say. In a
  `loop1()` that also has to move bytes, that default means the byte-moving
  half runs only when USB happens to generate an event. `USBHost.task(0)`.
- **Pico-PIO-USB enables the RP2040's own pull-downs on D+ and D-**
  (`gpio_pull_down()` in `pio_usb.c`, and again in the rx/tx PIO helpers), so
  the external 15 kO pull-downs the reference circuit asks for are a
  signal-integrity and spec-compliance measure rather than the thing that makes
  attach detection work. A port with none fitted still reads a clean 0,0 idle
  and still detects a device. Worth knowing during bring-up, because "no
  pull-downs" is the first thing suspected and it is usually not the fault.
- **The pull-downs are two resistors to ground, not one across the pair.** A
  single 15 kO bridging D+ to D- looks like the same part count and is not the
  same circuit: with a full-speed device's 1.5 kO pull-up on D+ it drags D-
  up with it (~3.1 V and ~2.4 V against the internal 50 kO pull-downs), which
  is SE1 — an illegal bus state that no host will enumerate through.
- The bus pins can be read straight back as GPIOs while the bus is idle, which
  is the fastest way to split "nothing is attached" from "something is attached
  and enumeration is failing" without an oscilloscope. The debug build prints
  the raw pad levels as `bus=<dp><dm>`.
- **`gpio_get()` lies about these two pins.** `pio_usb_host_add_port()` calls
  `gpio_set_inover(pin, GPIO_OVERRIDE_INVERT)` on D+ and D−, and that override
  sits between the pad and SIO — so `gpio_get()` returns the *complement* of
  the line. The library's own `pio_usb_bus_get_line_state()` un-inverts it
  again (`dp = gpio_get(pin_dp) ? 0 : 1`), which is why its `PORT_PIN_FS_IDLE`
  really is the textbook full-speed idle of D+ high and D− low. Nothing about
  the library's convention is unusual; the trap is reading those pins with
  `gpio_get()` and believing the answer.

  Read the pad instead: `INFROMPAD` in `io_bank0_hw->io[pin].status` is
  upstream of the override. `bus=` reports that, so it now means what it says:

  | `bus=` (D+,D− at the pad) | Meaning                                |
  | ------------------------- | -------------------------------------- |
  | `00`                      | idle, nothing attached                 |
  | `10`                      | full-speed device (pull-up on D+)      |
  | `01`                      | low-speed device (pull-up on D−)       |
  | `11`                      | SE1, illegal — pull-downs wrong        |

  This cost two wrong diagnoses before it was found: first a crossed D+/D−
  pair that was not crossed, then an "inverted library convention" that was
  really our own inverted read. Both were self-consistent enough to be
  convincing. The lesson is narrow and worth keeping — when a reading disagrees
  with a library's own verdict on the same pins, suspect the read, and go
  looking for an override before theorising about conventions.

- **Low speed is not a thing a serial adapter can be.** The USB spec allows
  low-speed devices control and interrupt transfers only — bulk endpoints are
  prohibited below full speed — and CDC/ACM moves its data over bulk IN/OUT.
  So `fullspeed=0` on this port never means "a slow serial adapter"; it means
  the pair is crossed, or what is plugged in is a keyboard.
- **`enum=` and `bus=` still cannot tell "never detected" from "detected and
  enumeration failed".** Both read `enum=0`, and that is the difference between
  a wiring fault and a signal-integrity one. The root port itself knows:
  `pio_usb_root_port[0].connected` is set from the line-state poll before any
  transfer is attempted, and `ep_error` counts transfers that came back broken.
  The debug build's `port:` row reports them.
- `pio_usb_ll.h` **does not compile as C++** — it assigns an `int` to a
  `port_pin_status_t` and redeclares an `inline` the Pico SDK already has, so
  including it from a `.cpp` fails on both. `src/pio_usb_probe.c` is a C file
  for that reason alone; re-declaring `root_port_t` in C++ instead would
  compile and would silently break the day the library reorders a field.
- **First working enumeration on the bench port was a low-speed device, and
  the full-speed one on the same wiring still fails.** A Logitech mouse came up
  as `enum=1(046d:c077) fullspeed=0`, which proves the whole lower half at
  once: VBUS, ground, the pull-downs, the 22 O series pair, the PIO state
  machines, the frame interrupt on core1, TinyUSB's enumeration and our
  callbacks. `attached=0` alongside it is correct and is the counter earning
  its keep — a mouse enumerates but is not a serial port, so the engine never
  sees a far end.

  A USB-serial adapter on the same 5 cm of wire reaches `conn=1 fullspeed=1
  susp=0` — detected, and bus-reset by TinyUSB — and then never completes
  enumeration, with `ep_err=0`. Low speed is 1.5 Mbit/s and full speed is
  12 Mbit/s, so a port that does one and not the other is not a wiring
  topology fault; it is either signal integrity at the higher rate or the
  device browning out. Worth remembering that a successful enumeration proves
  much less than it looks like it proves if it was a low-speed one.
- **Two different full-speed devices fail identically where a low-speed one
  succeeds**, on the same 5 cm of wire and the same 22 O series pair: both
  reach `conn=1 fullspeed=1 susp=0` and neither ever fires `tuh_mount_cb`,
  while a low-speed mouse enumerates every time. Recording the VID/PID on
  every mount is what makes that readable — with a full-speed device attached
  and `enum=` still naming the low-speed one, the currently attached device
  demonstrably did not mount, which a bare counter could not have shown.
- The clock dividers are exact at 120 MHz and are not the suspect: the host
  path computes `cpu_freq / 48000000` for TX and `cpu_freq / 96000000` for RX
  (`pio_usb_host.c`), giving 2.5 and 1.25, both exactly representable in the
  PIO's 8.8 fixed-point divider. A wrong divider would also be likelier to
  break the slower mode, not spare it.
- Which leaves the supply as the first thing to rule out, and the asymmetry
  is suggestive rather than mysterious: the device that works is a mouse
  drawing ~25 mA, and the ones that fail are serial adapters that draw more
  and have an inrush at power-up. hardware/README.md already warns that the
  Pico's VBUS pin is not a power budget for a downstream device; a far end
  that browns out part-way through enumeration presents exactly as this does,
  with the pull-up asserted, the reset accepted, and then silence.
## Two TinyUSB versions in one project

- **`pico` builds against a different TinyUSB from `pico_cdc`, and nothing says
  so.** Naming Pico-PIO-USB in `lib_deps` makes the library dependency finder
  resolve Adafruit TinyUSB from the *registry* for that environment, instead of
  using the copy bundled with the core. So `pico` compiled 3.4.4 out of
  `framework-arduinopico/libraries/` and `pico_cdc` compiled 3.7.7 out of
  `.pio/libdeps/`, unpinned, for however long that had been true.
- The two differ in exactly the places phase 2 depends on. In 3.4.4 the FTDI
  driver's `set_line_coding`, `set_data_format` and async paths are `// TODO
  not implemented yet` stubs that return false, and there is no PL2303 driver
  at all. In 3.7.7 all of them exist. Reading the core's copy while the
  registry's copy was being compiled produced two confident and wrong
  conclusions — that FTDI could not be configured at all, and that a PL2303
  adapter could never work.
- The way to check which one is being built is the object file, not the source
  tree: `find .pio/build/<env> -name 'cdc_host*.o'` names the library directory
  it came from.
- **Resolved by unifying on 3.7.7**, pinned in `[env:pico]` so every
  environment inherits it rather than each resolving its own (DECISIONS.md
  D14). The subtle part is why the pin belongs on the *base* environment: with
  it only on `pico_cdc`, that environment's 3.7.7 was contingent on the
  Pico-PIO-USB entry beside it, and removing that entry would have dropped
  phase 2 back to the 3.4.4 stub that caused the core1 hang.
- The phase 1 measurements below were taken on 3.4.4 and were re-taken on
  3.7.7 rather than assumed to carry over: same throughput within noise, no
  bytes lost, and no recurrence of the `__usb_mutex` wedge. That last one is
  not free — `src/usb_lock.h` binds to whichever TinyUSB copy is compiled,
  because the core's own `__usb_mutex` is compiled out under `USE_TINYUSB`
  (`RP2040USB.cpp:22`) and the library's rp2040 port defines it instead.
  OPEN-ISSUES.md 5 has the numbers.

## Phase 2 on hardware, first run

- **The port works, and the adapter matters more than anything else.** An FTDI
  FT232R (`0403:6001`) enumerates, mounts as CDC and carries data both ways
  through the whole path — MIDI, SysEx, core0's ring, core1, the USB host, the
  adapter's UART, and back through a TX/RX loop. Two other full-speed adapters
  on the same wiring never enumerated at all. "Full speed does not work on this
  rig" was the wrong conclusion from those two; the rig was fine.
- **The blocking control transfer really does hang core1, and the timed mailbox
  really does survive it.** After a sweep, `host_tasks` stops advancing
  entirely — core1 is stuck inside `tuh_cdc_set_line_coding()`, which has no
  timeout — while core0 goes on running, `op_timeouts` reaches 2, the engine
  reports `ERR_BACKEND` and the MIDI tunnel stays up to say so. That is
  DECISIONS.md D8 working exactly as designed, and it had never been observed
  before.
- **But it cannot recover.** The op core1 abandoned still owns the mailbox, so
  every subsequent `claimOp()` fails and the port is permanently `Fault` until
  a reboot. Surviving the hang was the design goal and is not the same as
  recovering from it; nothing reclaims a mailbox from a core that is never
  coming back.
- **The wedge and the byte loss are two different bugs**, which one run made
  look like one. Instrumenting core1 with a heartbeat separated them: the
  stall shows `host_tasks` frozen, and the byte loss shows `host_tasks`
  climbing happily with `from_dev` stuck. They have to be chased separately.
- **The stall is in TinyUSB's enumeration, not in our mailbox.** The clearest
  instance had `enum=0` and `op_timeouts=0` — core1 died before an op was ever
  posted, so the timed mailbox was never involved. `tuh_control_xfer()` spins
  `while (result == XFER_RESULT_INVALID) tuh_task_ext(0, false);` with a
  `// TODO probably some timeout` above it, and enumeration uses it. It is
  intermittent: the same FTDI on the same wiring enumerates cleanly on one
  boot and hangs the core on the next.
- Core0 can see it, and could not before. `hostAlive()` samples a beat core1
  bumps every turn; a changed beat restarts the clock, an unchanged one is
  evidence only after 2 s. `claimOp()` now fails immediately when core1 is
  gone rather than stalling the engine for a second per op — which turned a
  five-baud sweep from five seconds of dead air into an immediate honest
  failure. `hostPhase()` records which stage core1 was in when it stopped.
- **The stall had three causes, not one, and the phase marker walked through
  them.** Each fix moved `core1 stopped in:` to the next one:
  1. `execute_op` — the blocking control transfer. TinyUSB 3.7.7's
     `tuh_cdc_set_line_coding()` finds no `set_line_coding` for FTDI and falls
     through to setting baud and format as two separate transfers; with a null
     callback each is a `tuh_control_xfer()` that spins with no timeout. Fixed
     by passing a real completion callback, which takes the chained
     non-blocking path instead. This is only possible on 3.7.7 — on the 3.4.4
     the core bundles, the async path is an unimplemented stub.
  2. `pump_device` — both transfer loops ran until their ring was empty, and
     both rings are fed by the other core, so the producer can keep that false
     indefinitely. Core1 then never returns to `loop1()` and never calls
     `tuh_task()`. This is precisely the lesson phase 1 recorded about
     `pumpUsbMidi` and bounded at 16 reads; the other pump never got it. Now
     bounded at 8 chunks each way per turn.
  3. `usb_task` — inside `tuh_task()` itself, which is where it still is. Not
     ours to fix from here, and the reason a supervisory reset of core1 is
     probably unavoidable.
- With the first two fixed, all five bauds run to completion instead of taking
  the backend down: 3491, 3566, 4070, 4073, 4091 of 4096 at 9600 through
  115200. The shortfall shrinks as the baud rises, which looks like running out
  of time rather than losing data — except that a 180 s timeout at 9600 still
  stops at ~3494, so it is a genuine stall and not slowness. Still undiagnosed.

- **Neither library supports being torn down, so the host port is a one-shot.**
  `tuh_deinit()` is present in TinyUSB 3.7.7 and does nothing here: it asserts
  on `hcd_deinit()`, the PIO-USB controller defines none, and the weak stub
  (`usbh.c:53`) returns false. Pico-PIO-USB's `pio_usb_host_stop()` cancels its
  alarm-pool timer and unclaims nothing — and it spins on a flag cleared by a
  callback serviced on core1, so calling it from core0 to recover a wedged
  core1 can take core0 with it. `pio_usb_bus_init()` claims three PIO state
  machines and a DMA channel and never unclaims them, so a second
  `USBHost.begin(1)` panics inside the SDK's `hw_claim_or_assert`. Anything
  built on "restart the host stack" has to solve all four first; OPEN-ISSUES 2
  has the detail.

- **A warm reset recovers the bridge but not the far end.** `REBOOT` brings
  core1 back, but the downstream adapter stays unenumerated afterwards: an
  RP2040 warm reset does not cycle VBUS, so the device keeps the USB address it
  was assigned before the reset and never presents the connect edge TinyUSB
  enumerates on. The bus levels look identical to a healthy attach, which makes
  this easy to misread as a wiring fault. A replug fixes it.
- **Instrument all four buffers, not the endpoints.** "Bytes went in and fewer
  came out" has five possible homes on this backend — our two rings, TinyUSB's
  two FIFOs, and the adapter. Printing all four at once
  (`to_dev_ring / from_dev_ring / tu_tx_space / tu_rx_avail`) turned a
  three-week-old open question into one measurement: all four empty means the
  bytes really did leave, and the argument moves downstream of our code. Sample
  them from core1, where the `tuh_*` calls belong; asking the host stack a
  question from core0 is how this backend got its first wedge.

- **`connected && suspended` is a trap the root port cannot leave.** The only
  disconnect detection, `connection_check()`, is the last term of a `&&` chain
  guarded by `!root->suspended` (`pio_usb_host.c:266`), and the new-connection
  scan needs `!root->connected` (line 332). A port that ends up in both states
  can therefore see neither edge, and replugging the device changes nothing —
  `conn` stays 1 across an unplug, which reads like a wiring fault and is not
  one. Entered legitimately on every connect (`suspended = true; // need a bus
  reset before operating`), and left only by `pio_usb_host_port_reset_end()`,
  which TinyUSB calls only while enumerating. So any enumeration that hangs
  strands the port permanently.
- **The recovery is a replug *and* a reboot, in that order.** Neither works
  alone: a suspended port cannot see the replug, and a warm reboot leaves the
  adapter holding the USB address it was given before the reset. Together they
  enumerate first time.
- **The phase 2 byte loss drops single bytes from the middle of the stream**,
  not the tail. Sending `00 01 02 …` and aligning the return shows `0x0c`
  missing at offset 12 with everything after shifted by one, ~16% of the stream
  gone that way. Worth knowing before theorising: "short by 600 bytes" sounds
  like a truncation and is not one, and the counter that made it look like a
  stall was measuring the end of the loss rather than its cause.

- **The FT232R's latency timer is the phase 2 byte loss.** Its default is
  16 ms, and at 9600 baud that is 15.5 bytes — exactly the period the drops
  cluster on (`15x74 16x43 31x41 30x16` in `gap-analysis.mjs`). Bytes are lost
  at IN-packet boundaries, so the loss rate follows how many boundaries there
  are, which is why it looked baud-dependent and why it looked like a stall.
  Raising the timer to 100 ms took 9600 from 715 bytes lost to 1, and the
  loopback suite from five failures to four clean passes.
- **A period is worth more than a total.** "Short by 600 bytes" supported four
  different wrong theories for weeks. The gap *spacing* named the mechanism in
  one run, and the only reason it was available is that the analysis aligns the
  returned stream against the sent one instead of counting it.
- **TinyUSB's `CFG_TUH_CDC_FTDI_LATENCY` does not compile.** `cdc_host.c:1241`
  calls an undeclared `ftdi_process_config` and declares a variable inside a
  `switch` case without braces. It is dead code behind an `#ifdef` that nobody
  has enabled, so defining the macro breaks the build rather than configuring
  anything. Send the vendor request from our own backend instead — request
  `0x09`, type `0x40`, value = ms — which is the same shape as the line-coding
  transfer the backend already does asynchronously.

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
jumper fitted. Originally measured on TinyUSB 3.4.4 and confirmed unchanged on
3.7.7 (DECISIONS.md D14); the 921600 row is from before `INFO.maxBaud` dropped
to 460800, and the sweep now skips it:

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
- **A session boundary has to be enforced on both sides of the link and in
  both of the places a frame can be sitting.** Clearing the outbound *buffer*
  at `OPEN` is not enough: anything already framed and handed to the USB sink
  is past that point, and arrives in the next session carrying the previous
  session's sequence number. `sink_.discardQueued()` is the other half. On the
  host the mirror image applies — resetting the client's session before
  sending `OPEN` counts the old session's tail against the new sequence, so it
  has to happen after the `STATUS` reply, which ordered SysEx guarantees comes
  last.
- **A feature that is announced in the spec can be entirely absent from the
  wire while the code that implements it looks complete.** The post-close
  drain (§5.9) was defeated by an allow-list two functions away — `CREDIT` was
  answered `ERR_NOT_OPEN`, so it stopped after one window, 64 bytes of 300.
  Nothing in the drain itself was wrong. Measuring how many bytes actually
  arrive is the only check that catches that class of bug.
- **Presence has to be watched as an edge, not a level.** A far end unplugged
  and replaced between two polls reads identically on both sides of the gap.
  The port stays open, against a device TinyUSB has meanwhile re-enumerated
  and reset to its own defaults — so the host is talking to a stranger on a
  port it believes it configured. `Backend::presence()` returns the level and
  a flip count in one word, so a reader cannot get a level from one transition
  and a count from another.
- Credits have to be cumulative deltas rather than absolute levels. With
  absolute levels a lost message leaves the sender believing it has *more*
  window than the receiver has buffer, which overflows silently; with deltas
  the same loss shrinks the window and the link stalls visibly instead.
