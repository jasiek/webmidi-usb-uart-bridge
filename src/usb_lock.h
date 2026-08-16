// Serialises our TinyUSB calls against tud_task().
//
// On this port tud_task() does not run from loop() — Adafruit's rp2040 backend
// hangs a handler off USBCTRL_IRQ that raises a soft IRQ to run it. So every
// tud_midi_* call we make from loop() can be preempted, mid-update, by the
// stack servicing the very endpoints we are reading and writing.
//
// Adafruit's own task runner uses mutex_try_enter on __usb_mutex and skips its
// turn when it cannot get it — "if the mutex is already owned, then we are in
// user code which will do a tud_task itself". That is the interlock, and it
// only works if user code actually takes the mutex. Nothing in the Adafruit
// Arduino wrappers does, so we do it here.
//
// Hold it across the tud_* call and nothing else: work done under this lock is
// USB service time the stack does not get.
#pragma once

#include <pico/mutex.h>

// Defined in Adafruit_TinyUSB_Arduino/src/arduino/ports/rp2040/Adafruit_TinyUSB_rp2040.cpp
extern mutex_t __usb_mutex;

namespace bridge {

class UsbLock {
 public:
  UsbLock() { mutex_enter_blocking(&__usb_mutex); }
  ~UsbLock() { mutex_exit(&__usb_mutex); }

  UsbLock(const UsbLock&) = delete;
  UsbLock& operator=(const UsbLock&) = delete;
};

}  // namespace bridge
