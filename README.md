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
| `src/`              | Firmware wiring: TinyUSB MIDI device, UART backend.             |
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
# Firmware
pio run -e pico                  # build
pio run -e pico -t upload        # flash (hold BOOTSEL while plugging in)

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
```

Every byte must come back, in order, at every speed. The rig reports
throughput, round-trip latency and any error flags the device raised, and exits
non-zero if anything did not match.

`--fake` runs the same client against `host/src/fake-device.js`, a software
model of the firmware with its TX looped to its RX. It is how the harness gets
debugged before hardware is involved, and how the client's flow control is
tested in CI.

## Measured on hardware

Flashed to a Pico 1 and driven from macOS over CoreMIDI:

| Measurement                            | Result                          |
| -------------------------------------- | ------------------------------- |
| `PING`/`PONG` round trip                | 0.97 ms mean, 0.77 ms best      |
| 64 KB host → UART at 115200 8N1         | 11.2 kB/s, **99% of line rate** |
| Bytes delivered vs. sent                | 65536 / 65536, exact            |
| Sequence gaps, credit errors, overruns  | none                            |

The credit window paced a 64 KB write through a 2048-byte device buffer down to
the UART's own drain rate without a single dropped byte — which is the whole
reason §6 exists.

The UART → host direction still needs the loopback jumper to be verified.

## Status

- **Phase 1 — hardware UART: working, host → UART verified on hardware.** USB
  MIDI device, SysEx tunnel, credit windowing, control lines, test rig.
- **Phase 2 — Pico-PIO-USB CDC/ACM host: not started.** The `Backend`
  interface and the `INFO.backend` field exist for it; `hardware/README.md`
  records the pin and clock constraints it will impose.

Known gaps in phase 1, all of them honest in `INFO.caps` rather than faked:
DTR/DSR/DCD/RI have no pin on a bare UART; RTS/CTS are hardware flow control
rather than lines the host can drive; and framing and parity errors cannot be
reported, because the core's UART ISR discards those characters without
recording anything (FINDINGS.md).
