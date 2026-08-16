// WebMIDI ↔ USB/UART bridge — firmware entry point.
//
// The Pico enumerates as a class-compliant USB MIDI device (so an iPad will
// talk to it without a driver) and tunnels a serial port through SysEx.
// PROTOCOL.md is the wire format; everything interesting lives in
// lib/bridge_proto, which is host-testable. This file is only the wiring.

#include <Adafruit_TinyUSB.h>
#include <Arduino.h>
#include <hardware/watchdog.h>
#include <pico/bootrom.h>

#include "bridge.h"
#include "midi_sink.h"
#include "sysex_assembler.h"
#include "usb_lock.h"
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

// If loop() ever stops making progress the device becomes unreachable — USB
// enumeration survives, because TinyUSB runs from an interrupt, so the host
// sees a device that is present and mute. There is no reset interface to fall
// back on, so the watchdog is what stops that needing a physical replug.
// Nothing in loop() blocks for more than a few milliseconds.
constexpr uint32_t kWatchdogMs = 4000;

// The on-board LED doubles as the only status output this board has: slow
// heartbeat when idle, solid once a port is open.
constexpr uint32_t kHeartbeatMs = 1000;
uint32_t lastBlinkAt = 0;

// Counters, kept unconditionally — three increments per loop is nothing, and
// they are the only way to tell a wedged loop() from a silent transport.
uint32_t loops = 0;
uint32_t midiBytesIn = 0;
uint32_t sysexIn = 0;
uint32_t midiCapped = 0;  // times the MIDI drain hit its per-loop bound
int bootReason = 0;

// Worst-case time spent in each phase of loop(), in microseconds, since the
// last report. The watchdog fires after kWatchdogMs of no progress, and the
// once-a-second snapshot cannot show what happened in the moment before that —
// these can, because whichever phase is about to overrun shows up as an
// outlier in the report before it.
struct PhaseMax {
  uint32_t midi = 0;
  uint32_t poll = 0;
  uint32_t sink = 0;
  uint32_t loop = 0;
  void clear() { midi = poll = sink = loop = 0; }
};
PhaseMax phaseMax;

inline void recordMax(uint32_t& slot, uint32_t started) {
  const uint32_t took = micros() - started;
  if (took > slot) slot = took;
}

// Phase codes for the marker below. 1–9 are loop() phases, 10+ come from
// Bridge::poll via BRIDGE_PHASE.
constexpr unsigned kPhaseMidi = 1;
constexpr unsigned kPhasePoll = 2;
constexpr unsigned kPhaseSink = 3;
constexpr unsigned kPhaseLed = 4;
constexpr unsigned kPhaseDebug = 5;
unsigned lastPhaseBeforeReset = 0;

const char* phaseName(unsigned code) {
  switch (code) {
    case kPhaseMidi:  return "pumpUsbMidi";
    case kPhasePoll:  return "Bridge::poll (entry)";
    case kPhaseSink:  return "sink.service";
    case kPhaseLed:   return "serviceLed";
    case kPhaseDebug: return "serviceDebug";
    case 10: return "poll/pumpToBackend";
    case 11: return "poll/pumpFromBackend";
    case 12: return "poll/pumpLines";
    case 13: return "poll/credit";
    case 14: return "poll/sendDataChunk";
    case 15: return "poll (done)";
    default: return "none";
  }
}

#ifdef BRIDGE_DEBUG
// A once-a-second line on the CDC interface, which rides along beside MIDI and
// does not disturb it. `screen /dev/cu.usbmodem* 115200` to watch. If these
// lines stop, loop() has stopped; if they continue while MIDI is silent, the
// problem is the transport, not the firmware.
constexpr uint32_t kDebugMs = 1000;
uint32_t lastDebugAt = 0;

const char* resetReasonName(int reason) {
  switch (reason) {
    case rp2040.PWRON_RESET:    return "power-on";
    case rp2040.RUN_PIN_RESET:  return "run-pin";
    case rp2040.SOFT_RESET:     return "soft";
    case rp2040.WDT_RESET:      return "WATCHDOG";
    case rp2040.DEBUG_RESET:    return "debug";
    case rp2040.GLITCH_RESET:   return "glitch";
    case rp2040.BROWNOUT_RESET: return "brownout";
    default:                    return "unknown";
  }
}

void serviceDebug(uint32_t nowMs) {
  if (nowMs - lastDebugAt < kDebugMs) return;
  lastDebugAt = nowMs;
  // Never block on the debug channel: with no reader its FIFO fills, and a
  // diagnostic that can hang the loop it is diagnosing is worse than none.
  if (!SerialTinyUSB || SerialTinyUSB.availableForWrite() < 96) return;

  SerialTinyUSB.printf(
      "t=%lus boot=%s(last=%s) loops=%lu midi_in=%lu sysex=%lu | state=%d rx=%lu tx=%lu"
      " | sink buf=%u out=%lu dropped=%lu stalls=%lu%s"
      " | capped=%lu max_us midi=%lu poll=%lu sink=%lu loop=%lu\r\n",
      static_cast<unsigned long>(nowMs / 1000), resetReasonName(bootReason),
      phaseName(lastPhaseBeforeReset),
      static_cast<unsigned long>(loops), static_cast<unsigned long>(midiBytesIn),
      static_cast<unsigned long>(sysexIn), static_cast<int>(gBridge.state()),
      static_cast<unsigned long>(gBridge.rxCount()),
      static_cast<unsigned long>(gBridge.txCount()),
      static_cast<unsigned>(sink.buffered()),
      static_cast<unsigned long>(sink.bytesOut()),
      static_cast<unsigned long>(sink.dropped()),
      static_cast<unsigned long>(sink.stalls()),
      sink.blocked() ? " BLOCKED" : "",
      static_cast<unsigned long>(midiCapped),
      static_cast<unsigned long>(phaseMax.midi),
      static_cast<unsigned long>(phaseMax.poll),
      static_cast<unsigned long>(phaseMax.sink),
      static_cast<unsigned long>(phaseMax.loop));
  phaseMax.clear();
}
#else
inline void serviceDebug(uint32_t) {}
#endif

void serviceLed(uint32_t nowMs) {
  if (gBridge.state() == bridge::PortState::Open) {
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }
  if (nowMs - lastBlinkAt < kHeartbeatMs) return;
  lastBlinkAt = nowMs;
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}

// Draining USB MIDI must be bounded, not "until empty". A host streaming at
// full rate refills the endpoint FIFO as fast as we empty it, so an
// until-empty loop never returns — and while it does not return, Bridge::poll
// never runs, so nothing drains to the UART, no credit goes back, and
// sink.service never pushes a byte. loop() stops making progress entirely and
// the watchdog fires. The symptom is a device that is still enumerated and
// completely mute. See FINDINGS.md.
//
// 16 reads is 1 KB per iteration, comfortably above the ~53 kB/s the tunnel
// sustains at the loop rates we see, so this is not a throughput limit — it
// just guarantees the rest of loop() gets a turn.
constexpr int kMaxMidiReadsPerLoop = 16;

void pumpUsbMidi(uint32_t nowMs) {
  uint8_t buf[64];
  for (int i = 0; i < kMaxMidiReadsPerLoop; ++i) {
    uint32_t n = 0;
    {
      // The lock covers the TinyUSB calls only. Parsing and everything
      // downstream of it happens outside, so the stack is not held off while
      // we open a UART or build a reply.
      bridge::UsbLock lock;
      if (!tud_midi_available()) break;
      n = tud_midi_stream_read(buf, sizeof(buf));
    }
    if (n == 0) break;
    midiBytesIn += n;
    if (i + 1 == kMaxMidiReadsPerLoop) ++midiCapped;
    assembler.feed(buf, n, [nowMs](const uint8_t* msg, size_t len) {
      ++sysexIn;
      gBridge.onSysEx(msg, len, nowMs);
    });
  }
}

}  // namespace

// Watchdog scratch registers survive the reset the watchdog causes, which is
// the only way to find out where a hang was when the hang takes the CPU with
// it. scratch[4..7] are free for application use.
extern "C" void bridgePhase(unsigned code) { watchdog_hw->scratch[4] = code; }

// The 1200-baud touch — the convention every Arduino-family board uses to mean
// "reboot into the bootloader". arduino-pico only implements it in its own
// SerialUSB, which is compiled out under USE_TINYUSB, so without this the
// board can only be reflashed by physically holding BOOTSEL. See FINDINGS.md.
extern "C" void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* coding) {
  (void)itf;
  if (coding->bit_rate == 1200) {
    reset_usb_boot(0, 0);
  }
}

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

  bootReason = rp2040.getResetReason();
  // Read the marker before anything overwrites it: after a watchdog reset it
  // still holds the phase that was executing when the CPU stopped.
  lastPhaseBeforeReset = watchdog_hw->scratch[4];
  watchdog_hw->scratch[4] = 0;

  gBridge.begin(millis());
  rp2040.wdt_begin(kWatchdogMs);
}

void loop() {
#ifdef TINYUSB_NEED_POLLING_TASK
  TinyUSBDevice.task();
#endif

  const uint32_t nowMs = millis();
  const uint32_t loopStart = micros();

  uint32_t phase = micros();
  bridgePhase(kPhaseMidi);
  pumpUsbMidi(nowMs);
  recordMax(phaseMax.midi, phase);

  phase = micros();
  bridgePhase(kPhasePoll);
  gBridge.poll(nowMs);
  recordMax(phaseMax.poll, phase);

  phase = micros();
  bridgePhase(kPhaseSink);
  sink.service(nowMs);
  recordMax(phaseMax.sink, phase);

  bridgePhase(kPhaseLed);
  serviceLed(nowMs);
  recordMax(phaseMax.loop, loopStart);
  bridgePhase(kPhaseDebug);
  serviceDebug(nowMs);

  ++loops;
  rp2040.wdt_reset();

  // Deliberately no delay(): at 115200 baud the UART produces a byte every
  // 87 µs, and the RP2040 has nothing else to do with the time.
}
