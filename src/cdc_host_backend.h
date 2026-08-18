// Phase 2 backend: a USB CDC/ACM serial adapter attached to a bit-banged USB
// host port (Pico-PIO-USB) on GPIO16/GPIO17.
//
// See DECISIONS.md D1 for the phasing and D8 for why the host stack lives on
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
//     per session — and being synchronous is what lets a caller report the
//     result: open() has to say whether the adapter accepted the line coding.
//
// Emptying a ring is not one of the things the handshake is trusted for. Core0
// gives up on an op after a second, so "core0 is blocked, therefore quiet" is
// an invariant with a hole in it; each ring is instead emptied by the core
// that consumes it, which is safe on its own. See executeOp().
#pragma once

#include <Arduino.h>
#include <atomic>

#include "backend.h"
#include "bridge_proto.h"
#include "spsc_ring.h"

namespace bridge {

// Pico-PIO-USB needs D+ and D− on consecutive GPIOs. GPIO0/1 belong to the
// phase 1 UART, so the port starts at GPIO16. This names the GPIO the socket's
// D+ actually reaches, which is the thing a meter can check.
#ifndef BRIDGE_PIO_USB_DP
#define BRIDGE_PIO_USB_DP 16
#endif
constexpr int kPinUsbDp = BRIDGE_PIO_USB_DP;

// The pair has to be adjacent; it does not have to be in that order. The
// library takes either way round: PIO_USB_PINOUT_DPDM puts D− above D+ and
// DMDP puts it below, so a socket that came out crossed is a build flag
// rather than a rework. hardware/README.md documents D+ low and that stays the
// default, because a firmware that quietly accommodates a miswired board is a
// firmware that hides one.
#ifdef BRIDGE_PIO_USB_SWAP
constexpr int kPinUsbDm = BRIDGE_PIO_USB_DP - 1;
constexpr bool kPinUsbSwapped = true;
#else
constexpr int kPinUsbDm = BRIDGE_PIO_USB_DP + 1;
constexpr bool kPinUsbSwapped = false;
#endif

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
  bool present() const override { return (presence() & 1u) != 0; }
  uint32_t presence() const override {
    return presence_.load(std::memory_order_acquire);
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

  // Device-level mount, one level below the CDC one above. A port with nothing
  // on it and a port with something on it that is not a serial adapter both
  // report attached=0, and during bring-up those two have completely different
  // causes — the first is the wiring, the second is the device. Recording every
  // enumeration, whatever class it turns out to be, is what separates them.
  void onDeviceMount(uint8_t daddr, uint16_t vid, uint16_t pid);
  void onDeviceUnmount(uint8_t daddr);

  // Diagnostics for the debug build. Reads are racy by nature and that is
  // fine — they are counters, not control flow.
  uint32_t hostTasks() const { return hostTasks_; }
  uint32_t bytesToDevice() const { return bytesToDevice_; }
  uint32_t bytesFromDevice() const { return bytesFromDevice_; }
  uint32_t opTimeouts() const { return opTimeouts_; }

  // Where the bytes are, when the count that left core0 and the count that
  // came back disagree. Sampled by core1 at the end of each pump rather than
  // called from core0: tuh_* is core1's, and asking the host stack a question
  // from the wrong core is how this backend got its first wedge.
  uint32_t hostTxSpace() const { return hostTxSpace_; }
  uint32_t hostRxAvail() const { return hostRxAvail_; }
  uint32_t paceStalls() const { return paceDropped_; }

  // The adapter's transmit FIFO is the thing being protected, so the burst
  // allowed after an idle period is sized to fit inside one. 64 is the FT232R's
  // smaller (transmit) FIFO halved — small enough to be safe on anything with
  // a 128-byte buffer, large enough that a full USB packet still goes out in
  // one turn once tokens have accrued.
  static constexpr uint32_t kPaceBurstBytes = 64;
  // Safe from either core by construction — see spsc_ring.h.
  size_t toDeviceDepth() const { return toDevice_.size(); }
  size_t fromDeviceDepth() const { return fromDevice_.size(); }
  bool clockOk() const { return clockOk_; }

  // Whether core1 is still going round its loop. Core1 can stop for good —
  // TinyUSB's blocking control transfers have no timeout and its enumeration
  // uses them, so a far end that stops answering mid-enumeration takes the
  // host stack with it. Observed on hardware: `host_tasks` stops advancing
  // with op_timeouts still 0, i.e. before our mailbox is ever involved.
  //
  // Core0 has to be able to tell that apart from an empty port, because they
  // look identical from the engine's side — nothing attached, nothing
  // happening — and they need opposite responses from whoever is watching.
  // Not const: it samples, so it keeps the last reading it took. Core0 only.
  bool hostAlive(uint32_t nowMs);

  // Where core1 was when it last checked in. When it stops, the beat above
  // says so and this says where — the same trick the phase 1 wedge was found
  // with, minus the watchdog scratch registers, because core0 is still alive
  // to read it directly. See kPhase* below.
  uint8_t hostPhase() const { return core1Phase_.load(std::memory_order_relaxed); }
  static const char* hostPhaseName(uint8_t phase);

  // Core1 loop stages, in the order loop1() runs them.
  static constexpr uint8_t kPhaseTask = 1;     // inside USBHost.task()
  static constexpr uint8_t kPhaseOp = 2;       // executeOp()
  static constexpr uint8_t kPhasePump = 3;     // pumpDevice()
  static constexpr uint8_t kPhaseIdle = 4;     // between turns

  // A control transfer core1 started has finished. Called from TinyUSB's
  // completion callback, which runs on core1 inside tuh_task().
  void onXferComplete(uint32_t gen, bool ok);

  void setHostPhase(uint8_t phase) {
    core1Phase_.store(phase, std::memory_order_relaxed);
  }
  uint32_t deviceMounts() const { return deviceMounts_; }
  // The raw bus levels, for when nothing enumerates and the question is
  // whether anything is electrically there. An idle port with nothing plugged
  // in reads 0,0. Beyond that, do not decode these with the USB convention:
  // Pico-PIO-USB inverts both pins before deciding what the line state is, so
  // the library calls 0,1 full speed and 1,0 low speed — the opposite way
  // round from the J state a reference describes. Trust the debug build's
  // `port: fullspeed=` over these. FINDINGS.md has the table.
  bool dpLevel() const;
  bool dmLevel() const;
  uint16_t lastVid() const { return lastVid_; }
  uint16_t lastPid() const { return lastPid_; }

 private:
  // The mailbox. Core0 claims the slot, fills the argument fields, stores the
  // op code, and spins until core1 zeroes it again.
  //
  // `Claimed` is what makes that order safe. The arguments are written *after*
  // the slot is taken and *before* the op code that publishes them, so a
  // caller arriving while core1 still owns a previous op is turned away
  // before it can touch anything core1 might be reading. Core1 treats it
  // exactly like None: nothing to do yet.
  enum class Op : uint8_t {
    None = 0,
    Claimed,
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

  bool claimOp();              // core0: take the mailbox, or fail if it is busy
  bool runOp(Op op);           // core0: publish a claimed op and wait for it.
  void executeOp();            // core1: perform whatever core0 posted.
  bool startLineCoding();      // core1: post the request, do not wait for it
  bool startControlLines();    // core1: ditto
  void finishOp(bool ok);      // core1: release the mailbox back to core0
  void pumpDevice();           // core1: move bytes both ways

  // How many bytes the far end's line can have swallowed since the last pump,
  // and the bucket that meters them out. See pumpDevice() for why this is
  // needed at all.
  void refillPaceBudget();
  uint32_t frameBits() const;

  // core1: publish an attach or a detach as one store. Bit 0 is the level and
  // the bits above it count the flips, so core0 can never read a level from
  // one transition and a count from another.
  void setPresent(bool present);

  // --- shared state, written by exactly one core each ---

  // Bit 0 attached, bits 1.. transitions. Boots at 0: nothing attached, and
  // nothing has happened yet. core1 writes.
  std::atomic<uint32_t> presence_{0};
  std::atomic<uint8_t> cdcIdx_{0};     // core1 writes
  std::atomic<bool> open_{false};      // core0 writes
  std::atomic<uint8_t> outLines_{0};   // core0 writes, core1 applies
  std::atomic<uint8_t> errFlags_{0};   // core1 sets, core0 takes

  std::atomic<uint8_t> op_{static_cast<uint8_t>(Op::None)};
  std::atomic<bool> opOk_{false};
  // Arguments to whatever op_ names. Written by core0 only while it holds the
  // mailbox and only between claimOp() and runOp(), so they need no atomicity
  // of their own — the op_ release/acquire pair publishes them, and the claim
  // is what stops a second caller overwriting them mid-flight.
  PortConfig pending_;
  uint8_t pendingFlush_ = 0;
  uint8_t pendingLines_ = 0;

  // An outstanding control transfer. Core1 touches these only, from loop1()
  // and from the completion callback, which is also core1 — so no atomics.
  //
  // The generation is what makes abandoning one safe. A transfer that answers
  // after core1 has given up on it would otherwise complete whatever op the
  // mailbox holds by then, which is not the op it belongs to; carrying the
  // generation in user_data and checking it on the way back makes a late
  // answer a no-op instead.
  bool xferPending_ = false;
  uint32_t xferGen_ = 0;
  uint32_t xferStartedAt_ = 0;

  // Core1's own patience, deliberately longer than core0's kOpTimeoutMs so
  // that in the ordinary case core0 reports the failure first and core1 is
  // only tidying up after it. Its job is not to make the caller wait less —
  // it is to give the mailbox back, so the *next* op is not refused for ever
  // by an op that is never coming home.
  static constexpr uint32_t kXferTimeoutMs = 1500;

  SpscRing<kHostRingSlots> toDevice_;    // core0 produces, core1 consumes
  SpscRing<kHostRingSlots> fromDevice_;  // core1 produces, core0 consumes

  // Counters. Written by core1 only.
  uint32_t hostTasks_ = 0;
  uint32_t bytesToDevice_ = 0;
  uint32_t bytesFromDevice_ = 0;
  uint32_t deviceMounts_ = 0;
  uint32_t hostTxSpace_ = 0;
  uint32_t hostRxAvail_ = 0;
  uint32_t paceDropped_ = 0;   // pumps that had bytes but no budget

  // Token bucket for the outbound pump, in bytes. core1 only.
  uint32_t paceTokens_ = 0;
  uint32_t paceLastUs_ = 0;
  uint32_t paceRem_ = 0;       // sub-byte remainder, kept so slow bauds accrue
  PortConfig openCfg_;         // what the port was actually opened with
  uint16_t lastVid_ = 0;
  uint16_t lastPid_ = 0;
  // Bumped by core1 every time round loop1(). Core0 watches it for movement
  // rather than for any particular value.
  std::atomic<uint32_t> beat_{0};
  std::atomic<uint8_t> core1Phase_{0};

  // Written by core0 only.
  uint32_t opTimeouts_ = 0;
  bool clockOk_ = false;
  uint32_t lastBeat_ = 0;
  uint32_t lastBeatAt_ = 0;
  bool beatSeen_ = false;

  // Long enough that a slow enumeration is not mistaken for a dead core —
  // core1's loop runs a quarter of a million times a second when it is well,
  // so a whole second of silence is already far outside normal.
  static constexpr uint32_t kHostStallMs = 2000;
};

// The one instance, so the TinyUSB C callbacks have something to reach.
extern CdcHostBackend gCdcHost;

}  // namespace bridge
