// WebMIDI ↔ USB/UART bridge — firmware entry point.
//
// The Pico enumerates as a class-compliant USB MIDI device (so an iPad will
// talk to it without a driver) and tunnels a serial port through SysEx.
// PROTOCOL.md is the wire format; everything interesting lives in
// lib/bridge_proto, which is host-testable. This file is only the wiring.

#include <Adafruit_TinyUSB.h>
#include <Arduino.h>

#include "bridge.h"
#include "midi_sink.h"
#include "sysex_assembler.h"
#include "uart_backend.h"

namespace {

Adafruit_USBD_MIDI usbMidi;

bridge::UartBackend backend;
bridge::UsbMidiSink sink;
bridge::Bridge gBridge(backend, sink);

// Sized to the largest frame the protocol allows; anything longer is not ours
// and is dropped by the assembler rather than truncated into a valid-looking
// one.
bridge::SysExAssembler<bridge::kMaxFrameLen> assembler;

// The on-board LED doubles as the only status output this board has: slow
// heartbeat when idle, solid once a port is open.
constexpr uint32_t kHeartbeatMs = 1000;
uint32_t lastBlinkAt = 0;

void serviceLed(uint32_t nowMs) {
  if (gBridge.state() == bridge::PortState::Open) {
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }
  if (nowMs - lastBlinkAt < kHeartbeatMs) return;
  lastBlinkAt = nowMs;
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

void pumpUsbMidi(uint32_t nowMs) {
  uint8_t buf[64];
  while (tud_midi_available()) {
    const uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
    if (n == 0) break;
    assembler.feed(buf, n, [nowMs](const uint8_t* msg, size_t len) {
      gBridge.onSysEx(msg, len, nowMs);
    });
  }
}

}  // namespace

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);

  // arduino-pico calls TinyUSB_Device_Init(0) before setup(), so by the time
  // we add an interface the device may already be enumerated; detach/attach
  // is what makes the host re-read the descriptors.
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);

  // The product descriptor is what CoreMIDI names the port, and what the host
  // tools match on by default — not the interface string below, which macOS
  // does not surface. Leaving it as the core's "Pico" makes every board on the
  // bench look identical.
  TinyUSBDevice.setManufacturerDescriptor("webmidi-usb-uart-bridge");
  TinyUSBDevice.setProductDescriptor("UART Bridge");

  usbMidi.setStringDescriptor("UART Bridge");
  usbMidi.begin();

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  gBridge.begin(millis());
}

void loop() {
#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif

  const uint32_t nowMs = millis();

  pumpUsbMidi(nowMs);
  gBridge.poll(nowMs);
  sink.service();
  serviceLed(nowMs);

  // Deliberately no delay(): at 115200 baud the UART produces a byte every
  // 87 µs, and the RP2040 has nothing else to do with the time.
}
