# Hardware

Target board: **Raspberry Pi Pico 1 (RP2040)**,
<https://www.raspberrypi.com/products/raspberry-pi-pico/>.

The Pico's own micro-USB port is the *upstream* connection — it enumerates as a
USB MIDI device to the phone or computer. Everything below is about the
*downstream* side: the serial port being tunnelled.

## Phase 1 — hardware UART

This is what the current firmware drives (`src/uart_backend.cpp`), and it needs
no components at all beyond wires.

| Pico pin | GPIO   | Signal | Notes                                       |
| -------- | ------ | ------ | ------------------------------------------- |
| 1        | GPIO0  | TX     | uart0 TX → far end's RX                     |
| 2        | GPIO1  | RX     | uart0 RX ← far end's TX                     |
| 3        | GND    | GND    | must be common with the far end             |
| 4        | GPIO2  | RTS    | optional; only if `OPEN` sets the RTS/CTS flag |
| 5        | GPIO3  | CTS    | optional; same                              |

### Loopback jumper

The whole test suite in `host/bin/loopback.js` runs off a single jumper:

```
GPIO0 (pin 1) ──────── GPIO1 (pin 2)
```

Every byte written goes straight back in. Nothing else is required — no second
board, no USB-serial cable.

### Levels

**The RP2040 is 3.3 V and its GPIOs are not 5 V tolerant.** A 5 V TTL serial
device needs a level shifter or at minimum a series resistor divider on the
line driving GPIO1. Connecting a 5 V TX line directly will damage the pin.

## Phase 2 — Pico-PIO-USB host

Not yet implemented in firmware; recorded here so the board can be built once.
See DECISIONS.md D1 for the phasing.

Pico-PIO-USB bit-bangs a USB host port using PIO. Its constraints are specific
and unforgiving:

- **D+ and D− must be consecutive GPIOs**, D+ on the lower number. GPIO0/1 are
  taken by the UART above, so use **D+ = GPIO16, D− = GPIO17**.
- **The system clock must be a multiple of 12 MHz.** The Pico's default
  125 MHz is *not*, and USB will not enumerate on it. Call
  `set_sys_clock_khz(120000, true)` before starting the stack — and note that
  this changes UART divisors, so the UART must be reconfigured afterwards.
- 22–27 Ω series resistors on D+ and D−, close to the RP2040.
- 15 kΩ pull-downs to ground on both D+ and D−, which is what makes the port a
  host and lets device attach/detach be detected.
- The downstream device needs **5 V on VBUS**. The Pico's VBUS (pin 40) can
  supply it when the Pico is itself bus-powered, but see the power budget below
  before relying on that.

| Pico pin | GPIO   | Signal | Notes                                    |
| -------- | ------ | ------ | ---------------------------------------- |
| 21       | GPIO16 | USB D+ | 27 Ω series, 15 kΩ to GND                |
| 22       | GPIO17 | USB D− | 27 Ω series, 15 kΩ to GND                |
| 40       | VBUS   | +5 V   | to the downstream port's VBUS            |
| 38       | GND    | GND    | to the downstream port's GND             |

## Power, and the iOS case

This matters more than it looks, because the point of the project is that an
iPad will talk to it.

An iOS device supplies limited current through a Lightning or USB-C camera
adapter, and will refuse a device that asks for more than it is prepared to
give. The USB descriptor's `bMaxPower` must stay modest — TinyUSB's default of
100 mA is correct and should not be raised.

That budget is for the Pico alone. It does **not** stretch to also powering a
downstream USB device off VBUS in phase 2. A phase 2 board should either:

- take 5 V from a powered hub or external supply for the downstream port, or
- be used with a powered camera adapter (one with its own charging input),

rather than trying to draw a second device's current through the phone.

## Enumeration

The firmware enumerates as a composite **USB MIDI + CDC** device. MIDI is the
tunnel; CDC is a debug console that comes along for the ride because it cannot
be switched off — `-DCFG_TUD_CDC=0` does not compile against Adafruit's Arduino
layer. See FINDINGS.md.

iOS uses the MIDI interface and ignores the CDC one. On macOS the device shows
up in Audio MIDI Setup, and `npm run list` in `host/` prints the port name.
