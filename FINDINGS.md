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
