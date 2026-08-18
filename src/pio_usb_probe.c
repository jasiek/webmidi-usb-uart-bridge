#ifdef BRIDGE_BACKEND_CDC_HOST

#include "pio_usb_probe.h"

#include <pio_usb_ll.h>

void bridge_pio_usb_probe(bridge_pio_usb_port_t* out) {
  const root_port_t* rp = PIO_USB_ROOT_PORT(0);
  out->initialized = rp->initialized;
  out->connected = rp->connected;
  out->is_fullspeed = rp->is_fullspeed;
  out->suspended = rp->suspended;
  out->pin_dp = rp->pin_dp;
  out->pin_dm = rp->pin_dm;
  out->ep_error = rp->ep_error;
  out->ep_stalled = rp->ep_stalled;
}

#endif  // BRIDGE_BACKEND_CDC_HOST
