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
