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

#ifdef __cplusplus
}
#endif
