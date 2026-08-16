// Phase 2 backend: a USB CDC/ACM serial adapter attached to a bit-banged USB
// host port (Pico-PIO-USB) on GPIO16/GPIO17.
//
// See DECISIONS.md D1 for the phasing and D7 for why the host stack lives on
// core1, hardware/README.md for the port's electrical requirements.
//
// ---- the two-core split ----------------------------------------------------
//
// Pico-PIO-USB reconstructs a full-speed bus in software. Its PIO state
// machines do the line coding, but the packet-level work happens in an
// interrupt that must be serviced inside a bit time, and the RP2040's own USB
// device interrupt — the MIDI side, which is the whole point of the product —
// is not going to yield to it. So the host stack gets core1 to itself and the
// protocol engine keeps core0.
//
// That makes every method below a cross-core call, and they come in two kinds:
//
//   * Bulk data goes through the two SpscRings. Lock-free, no handshake, and
//     the only thing on the hot path.
//   * Control operations (open, close, set lines, flush) are posted to core1
//     through a single-slot mailbox and *waited on*. They are rare — a handful
//     per session — and being synchronous is what makes them safe: while core0
//     is waiting it is provably not touching the rings, which is what lets
//     core1 clear them for a flush.
#pragma once

#include <Arduino.h>
#include <atomic>

#include "backend.h"
#include "bridge_proto.h"
#include "spsc_ring.h"

namespace bridge {

// Pico-PIO-USB needs D+ and D− on consecutive GPIOs with D+ the lower of the
// two. GPIO0/1 belong to the phase 1 UART, so the port starts at GPIO16.
#ifndef BRIDGE_PIO_USB_DP
#define BRIDGE_PIO_USB_DP 16
#endif
constexpr int kPinUsbDp = BRIDGE_PIO_USB_DP;
constexpr int kPinUsbDm = BRIDGE_PIO_USB_DP + 1;

// Pico-PIO-USB will not enumerate unless the system clock is a multiple of
// 12 MHz. 120 MHz is the usual choice and what platformio.ini sets; the
// firmware checks it at boot rather than failing mysteriously later.
constexpr uint32_t kRequiredClockKhz = 120000;

// Same ceiling as the UART backend, for the same reason: it is the tunnel that
// runs out of capacity at ~50 kB/s, not the far end. The USB link to the
// adapter is 12 Mbit/s and the adapter's own UART will usually clock far
// higher than this — advertising that would be promising throughput the SysEx
// tunnel cannot carry. See DECISIONS.md D5.
constexpr uint32_t kMaxBaud = 460800;

// Buffering between the cores. 4 KB each way is ~90 ms of slack at 460800 —
// enough that a core0 loop delayed by a long USB interrupt costs nothing, and
// cheap on a chip with 264 KB of SRAM.
constexpr size_t kHostRingSlots = 4096;

class CdcHostBackend : public Backend {
 public:
  // ---- core0: the Backend interface ----

  BackendId id() const override { return BackendId::PioUsbCdc; }
  uint16_t caps() const override;
  uint32_t maxBaud() const override { return kMaxBaud; }

  bool open(const PortConfig& cfg) override;
  void close() override;
  bool isOpen() const override { return open_.load(std::memory_order_acquire); }
  bool present() const override {
    return mounted_.load(std::memory_order_acquire);
  }

  size_t writable() const override;
  size_t write(const uint8_t* p, size_t n) override;

  size_t readable() const override;
  size_t read(uint8_t* p, size_t n) override;

  void flush(uint8_t what) override;

  void setLines(uint8_t mask, uint8_t values) override;
  uint8_t outputLines() const override {
    return outLines_.load(std::memory_order_relaxed);
  }
  uint8_t inputLines() const override;

  uint8_t takeErrorFlags() override;

  // ---- core1: the USB host side ----

  // Called once from setup1(), before the host stack starts.
  void beginHost();
  // Called from loop1(), after tuh_task().
  void serviceHost();

  // TinyUSB host CDC callbacks, which fire inside tuh_task() on core1.
  void onMount(uint8_t idx);
  void onUnmount(uint8_t idx);

  // Diagnostics for the debug build. Reads are racy by nature and that is
  // fine — they are counters, not control flow.
  uint32_t hostTasks() const { return hostTasks_; }
  uint32_t bytesToDevice() const { return bytesToDevice_; }
  uint32_t bytesFromDevice() const { return bytesFromDevice_; }
  uint32_t opTimeouts() const { return opTimeouts_; }
  bool clockOk() const { return clockOk_; }

 private:
  // The mailbox. Core0 fills the argument fields, stores `op_`, and spins
  // until core1 zeroes it again.
  enum class Op : uint8_t {
    None = 0,
    Open,
    Close,
    SetLines,
    Flush,
  };

  // An open() has to cross to core1, issue up to two USB control transfers and
  // come back. Each transfer is one or two 1 ms frames unless the device is
  // misbehaving; a second is generous and still an order of magnitude inside
  // the 4 s watchdog.
  static constexpr uint32_t kOpTimeoutMs = 1000;

  bool runOp(Op op);           // core0: post and wait. False on timeout.
  void executeOp();            // core1: perform whatever core0 posted.
  bool applyLineCoding();      // core1
  bool applyControlLines();    // core1
  void pumpDevice();           // core1: move bytes both ways

  // --- shared state, written by exactly one core each ---

  std::atomic<bool> mounted_{false};   // core1 writes
  std::atomic<uint8_t> cdcIdx_{0};     // core1 writes
  std::atomic<bool> open_{false};      // core0 writes
  std::atomic<uint8_t> outLines_{0};   // core0 writes, core1 applies
  std::atomic<uint8_t> errFlags_{0};   // core1 sets, core0 takes

  std::atomic<uint8_t> op_{static_cast<uint8_t>(Op::None)};
  std::atomic<bool> opOk_{false};
  // Only read by core1 while core0 is blocked in runOp(), so it needs no
  // atomicity of its own — the op_ release/acquire pair publishes it.
  PortConfig pending_;
  uint8_t pendingFlush_ = 0;

  SpscRing<kHostRingSlots> toDevice_;    // core0 produces, core1 consumes
  SpscRing<kHostRingSlots> fromDevice_;  // core1 produces, core0 consumes

  // Counters. Written by core1 only.
  uint32_t hostTasks_ = 0;
  uint32_t bytesToDevice_ = 0;
  uint32_t bytesFromDevice_ = 0;
  // Written by core0 only.
  uint32_t opTimeouts_ = 0;
  bool clockOk_ = false;
};

// The one instance, so the TinyUSB C callbacks have something to reach.
extern CdcHostBackend gCdcHost;

}  // namespace bridge
