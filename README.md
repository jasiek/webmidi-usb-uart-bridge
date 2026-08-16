# webmidi-usb-uart-bridge

A Raspberry Pi Pico 1 (RP2040) that presents itself as a class-compliant **USB
MIDI device** and tunnels a **serial port** through SysEx — so an iPad or
iPhone, which will happily talk to a MIDI device and will not talk to a USB
serial adapter, can drive a UART.

Speeds up to 115200 baud, arbitrary 8-bit data (7-bit packed, §2 of the
protocol), control lines, and credit-based flow control so a phone bursting
SysEx cannot silently overrun a slow wire.

## Layout

| Path                | What it is                                                     |
| ------------------- | -------------------------------------------------------------- |
| `PROTOCOL.md`       | The wire format. Normative for both implementations.            |
| `DECISIONS.md`      | Why the project is shaped the way it is.                        |
| `FINDINGS.md`       | Platform behaviour that was not obvious, learned the hard way.  |
| `lib/bridge_proto/` | The protocol engine. No Arduino or RP2040 dependencies.         |
| `src/`              | Firmware wiring: TinyUSB MIDI device, the two backends.         |
| `test/`             | Unity tests for the engine, run on the host.                    |
| `host/`             | Node client, software device model, loopback test rig.          |
| `hardware/`         | Pinout, the loopback jumper, and the phase 2 PIO-USB notes.     |

The protocol engine takes time as a `poll()` parameter, reaches the far end
through a `Backend` interface and emits through a `FrameSink`. That is what
lets the flow control, sequencing and error paths be tested on a laptop with
nothing plugged in — which is most of where the bugs would otherwise be.

## Building and testing

PlatformIO is the build system. On this machine it lives at
`~/.platformio/penv/bin/pio` rather than on `PATH`.

```sh
# Firmware — phase 1, hardware UART on GPIO0/1
pio run -e pico                  # build
pio run -e pico -t upload        # flash (hold BOOTSEL while plugging in)

# Firmware — phase 2, USB CDC/ACM host port on GPIO16/17
pio run -e pico_cdc
pio run -e pico_cdc -t upload

# Engine tests — no hardware needed
pio test -e native

# Host-side tests — no hardware needed
cd host && npm install && npm test
```

## The loopback test

Jumper **GPIO0 (pin 1) to GPIO1 (pin 2)** on the Pico — that is the entire test
rig. Then:

```sh
cd host
npm run list                     # confirm the Pico enumerated, and under what name
npm run probe                    # handshake only — no jumper needed
npm run loopback                 # 9600 … 115200, 4 KB each way
npm run loopback -- --baud 115200 --bytes 65536
npm run loopback -- --fake       # no hardware; exercises the harness itself
npm run loopback -- --fake-cdc   # same, modelling the phase 2 host port
npm run throughput               # where the tunnel, not the UART, becomes the limit
```

On a `pico_cdc` build the same commands apply, with a USB serial adapter in the
host port instead of a jumper — loop **its** TX to **its** RX. `npm run probe`
prints whether anything downstream enumerated, and `npm run loopback` waits for
an adapter rather than failing if the port is empty, so plugging one in is a
valid way to start the run.

If the board ever stops answering, it recovers itself — a 4 s hardware watchdog
reboots a hung `loop()`. To reflash without touching the board, open its CDC
port at 1200 baud and it drops into BOOTSEL:

```sh
stty -f /dev/cu.usbmodem* 1200   # then copy .pio/build/pico/firmware.uf2 to /Volumes/RPI-RP2
```

`pio run -e pico_debug -t upload` builds the same firmware with a once-a-second
status line on the CDC interface — loop rate, buffer levels, per-phase worst-case
timings, and the phase that was executing before a watchdog reset. That
instrumentation is what found the wedge described in FINDINGS.md.

Every byte must come back, in order, at every speed. The rig reports
throughput, round-trip latency and any error flags the device raised, and exits
non-zero if anything did not match.

`--fake` runs the same client against `host/src/fake-device.js`, a software
model of the firmware with its TX looped to its RX. It is how the harness gets
debugged before hardware is involved, and how the client's flow control is
tested in CI.

## Measured on hardware

Flashed to a Pico 1, jumper on GPIO0↔GPIO1, driven from macOS over CoreMIDI.
Loopback round trip — every byte out and back — at each speed:

| Baud   | 4 KB round trip | Throughput | Ping    | Lost |
| ------ | --------------- | ---------- | ------- | ---- |
| 9600   | 4270 ms         | 1.9 kB/s   | 1.0 ms  | 0    |
| 19200  | 2138 ms         | 3.7 kB/s   | 0.7 ms  | 0    |
| 38400  | 1070 ms         | 7.5 kB/s   | 0.9 ms  | 0    |
| 57600  | 713 ms          | 11.2 kB/s  | 0.7 ms  | 0    |
| 115200 | 358 ms          | 22.4 kB/s  | 0.8 ms  | 0    |

64 KB round trip at 115200: 6.5 s, exact, no sequence gaps or overruns. All
256 byte values verified through the tunnel. `PING` round trip is under 1 ms —
SysEx through CoreMIDI is not the bottleneck anyone expects it to be.

Per-direction ceiling, from `npm run throughput`: the tunnel sustains
**~50 kB/s** and flattens there. That is why `INFO.maxBaud` reports 460800
rather than the 921600 uart0 can clock — see DECISIONS.md D5, and note the
caveat there about what a loopback can and cannot prove.

## Status

- **Phase 1 — hardware UART: working, verified on hardware in both
  directions.** USB MIDI device, SysEx tunnel, credit windowing, control lines,
  test rig. 9600 → 460800 baud, zero loss.
- **Phase 2 — Pico-PIO-USB CDC/ACM host: written, builds, not yet run on
  hardware.** `pio run -e pico_cdc`. The USB host stack runs on core1 and
  reaches the protocol engine on core0 through lock-free rings and a timed
  mailbox (DECISIONS.md D7). ACM, FTDI, CP210x, CH34x and PL2303 adapters are
  all covered by TinyUSB's host CDC driver. Hot-plug is reported as
  `EVT_ATTACH`/`EVT_DETACH` and in `STATUS.present`; a detach faults the port
  rather than closing it quietly (D8). The engine's half of that is under test
  in `test/test_bridge` and `host/test/client.test.js`; **the USB host port
  itself has never been powered up** — building it needs the resistors and
  pull-downs in `hardware/README.md`.

Known gaps in phase 1, all of them honest in `INFO.caps` rather than faked:
DTR/DSR/DCD/RI have no pin on a bare UART; RTS/CTS are hardware flow control
rather than lines the host can drive; and framing and parity errors cannot be
reported, because the core's UART ISR discards those characters without
recording anything (FINDINGS.md).
