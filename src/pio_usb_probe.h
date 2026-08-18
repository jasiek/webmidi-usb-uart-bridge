// A read-only window onto the Pico-PIO-USB root port, for the debug build.
//
// TinyUSB's callbacks only fire once a device has enumerated, which leaves the
// most common bring-up failure — a device that is electrically present and
// never enumerates — reported identically to an empty socket. The root port
// knows the difference: it sets `connected` from the line-state poll, before
// any transfer is attempted.
//
// This lives in a C file of its own because pio_usb_ll.h does not compile as
// C++ (it assigns ints to an enum, and redeclares an inline the SDK already
// has). Rather than re-declare the struct here and let the two drift, the
// accessors are compiled as C against the real header.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool initialized;
  bool connected;    // a pull-up has been seen on the bus
  bool is_fullspeed; // which pull-up: D+ (true) or D- (false)
  bool suspended;
  uint8_t pin_dp;    // what the library believes the pins are
  uint8_t pin_dm;
  uint32_t ep_error;   // transfers that came back broken
  uint32_t ep_stalled;
} bridge_pio_usb_port_t;

void bridge_pio_usb_probe(bridge_pio_usb_port_t* out);

// Drive a bus reset on the root port, in two halves so the caller can hold the
// line low for the required time without blocking. Both are the library's own
// public entry points — the same pair TinyUSB's HCD calls to enumerate — so
// this changes no state the library does not change itself.
//
// The reason to want them: `suspended` is cleared *only* by the end half, and
// while it is set the library never evaluates `connection_check()`, which is
// the only thing that can notice a device being unplugged. A port left
// suspended therefore cannot see a disconnect or, afterwards, a fresh connect.
// See OPEN-ISSUES.md 4.
void bridge_pio_usb_port_reset_start(void);
void bridge_pio_usb_port_reset_end(void);

#ifdef __cplusplus
}
#endif
