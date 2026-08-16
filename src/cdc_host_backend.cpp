// Only built into the phase 2 environments. The phase 1 firmware has no USB
// host stack linked in at all, and pulling one in for a file it never calls
// would cost flash and enumeration time for nothing.
#ifdef BRIDGE_BACKEND_CDC_HOST

#include "cdc_host_backend.h"

#include <Adafruit_TinyUSB.h>
#include <hardware/clocks.h>

namespace bridge {

CdcHostBackend gCdcHost;

namespace {

// One USB packet. Both TinyUSB host CDC FIFOs are 128 bytes, so a couple of
// these per direction empties them and the loops below always terminate
// quickly.
constexpr size_t kChunk = 64;

}  // namespace

// ---- core0: the Backend interface ------------------------------------------

uint16_t CdcHostBackend::caps() const {
  // What a CDC/ACM control pipe actually gives us, and nothing else:
  //
  //   DTR, RTS   SET_CONTROL_LINE_STATE, which every driver TinyUSB carries
  //              implements — ACM, FTDI, CP210x, CH34x, PL2303.
  //   hotplug    the entire point of a host port: EVT_ATTACH / EVT_DETACH.
  //
  // Deliberately absent:
  //
  //   BREAK      CDC defines SEND_BREAK, but TinyUSB's host CDC driver does
  //              not expose it — there is no tuh_cdc_send_break().
  //   CTS/DSR/   carried by the ACM notification endpoint as SERIAL_STATE.
  //   DCD/RI     TinyUSB's host driver consumes those notifications without
  //              surfacing them, so we have no way to read or report them.
  //   RTS/CTS    the adapter's own hardware flow control is a property of its
  //   flow       far side, not something the host protocol can switch on here.
  //
  // See FINDINGS.md; INFO.caps staying honest is what stops a host waiting
  // for an EVT_LINES that can never arrive.
  return kCapDtr | kCapRts | kCapHotplug;
}

bool CdcHostBackend::open(const PortConfig& cfg) {
  if (!present()) return false;  // nothing to open a port on
  // Claim before writing pending_: an OPEN arriving while core1 still owns a
  // timed-out op must be refused, not allowed to rewrite the arguments that op
  // is in the middle of reading.
  if (!claimOp()) return false;
  pending_ = cfg;
  if (!runOp(Op::Open)) return false;
  // Ours to drop because core0 is this ring's consumer — see the note on
  // executeOp(). Done after the op rather than before it, so anything core1
  // moved across before it cleared the adapter's FIFO goes too.
  fromDevice_.discard(fromDevice_.size());
  open_.store(true, std::memory_order_release);
  return true;
}

void CdcHostBackend::close() {
  // Order matters: stop core0 handing out buffer space first, then tell core1
  // to stop moving bytes. The reverse would let a write() land in a ring that
  // core1 has already stopped draining.
  open_.store(false, std::memory_order_release);
  if (!claimOp()) return;
  (void)runOp(Op::Close);
}

size_t CdcHostBackend::writable() const {
  if (!isOpen() || !present()) return 0;
  return toDevice_.space();
}

size_t CdcHostBackend::write(const uint8_t* p, size_t n) {
  if (!isOpen() || !present()) return 0;
  return toDevice_.write(p, n);
}

size_t CdcHostBackend::readable() const {
  // Deliberately not gated on isOpen(): bytes the device sent before it was
  // closed or unplugged were received, and dropping them at the last moment
  // would be a silent loss of exactly the kind this project exists to avoid.
  return fromDevice_.size();
}

size_t CdcHostBackend::read(uint8_t* p, size_t n) { return fromDevice_.read(p, n); }

void CdcHostBackend::flush(uint8_t what) {
  if (!present()) return;
  if (!claimOp()) return;
  pendingFlush_ = what;
  const bool ok = runOp(Op::Flush);
  // Core0 owns this ring's consumer end, so dropping its contents here needs
  // no cooperation from core1 at all — see the note on executeOp(). Only if
  // the op really ran, though: a FLUSH that timed out has not cleared the
  // adapter's own FIFO, and throwing away our copy would be a loss on top of
  // a failure rather than the discard the host asked for.
  if (ok && (what & kFlushDiscardRx)) fromDevice_.discard(fromDevice_.size());
}

void CdcHostBackend::setLines(uint8_t mask, uint8_t values) {
  const uint8_t before = outLines_.load(std::memory_order_relaxed);
  const uint8_t after = static_cast<uint8_t>((before & ~mask) | (values & mask));
  // BREAK is in the protocol's line mask but not in caps(); accepting it
  // silently would be a lie, so only the two lines we can drive are stored.
  const uint8_t wanted = after & (kLineDtr | kLineRts);
  if (wanted == (before & (kLineDtr | kLineRts))) return;
  outLines_.store(wanted, std::memory_order_release);
  if (!present()) return;
  if (!claimOp()) return;
  // The value to put on the wire is staged separately from outLines_, which is
  // core0's record of what the host asked for and can move again at any time.
  // What core1 applies has to be the state this call was made about.
  pendingLines_ = wanted;
  // Backend::setLines returns void, so there is nowhere to report a failed
  // control transfer to — STATUS will go on reporting what the host asked for
  // rather than what the adapter acknowledged. The two only diverge when the
  // adapter has stopped answering at all, which the next OPEN reports as
  // ERR_BACKEND, and which the debug build's op_timeouts counter shows.
  (void)runOp(Op::SetLines);
}

uint8_t CdcHostBackend::inputLines() const {
  // CTS/DSR/DCD/RI arrive on the ACM notification endpoint, which TinyUSB's
  // host driver does not surface. caps() says so; reporting a constant 0 here
  // means the engine never emits an EVT_LINES that would be fiction.
  return 0;
}

uint8_t CdcHostBackend::takeErrorFlags() {
  // Overrun, framing, parity and break all ride the same unsurfaced
  // notification endpoint as the input lines above. The one error this
  // backend can genuinely observe — a device disappearing — is reported as
  // EVT_DETACH rather than as a flag.
  return errFlags_.exchange(0, std::memory_order_acq_rel);
}

// ---- the mailbox -----------------------------------------------------------

bool CdcHostBackend::claimOp() {
  // A previous op that timed out is still owned by core1, and its arguments
  // are still being read. Taking the slot is therefore the first thing a
  // caller does, before it writes anything: checking afterwards would be
  // checking a lock we had already broken.
  //
  // Only core0 ever stores a non-None value, so a plain load and store would
  // do — the compare-exchange is here because it says what is meant.
  uint8_t expected = static_cast<uint8_t>(Op::None);
  if (op_.compare_exchange_strong(expected, static_cast<uint8_t>(Op::Claimed),
                                  std::memory_order_acq_rel,
                                  std::memory_order_acquire))
    return true;

  // Counted with the timeouts because that is what it means: the only way the
  // mailbox is still busy is that an earlier op has not come back.
  ++opTimeouts_;
  return false;
}

bool CdcHostBackend::runOp(Op op) {
  opOk_.store(false, std::memory_order_relaxed);
  // Release: pending_ / pendingFlush_ / pendingLines_, written by the caller
  // since it claimed the slot, are published before the op code that tells
  // core1 to read them.
  op_.store(static_cast<uint8_t>(op), std::memory_order_release);

  const uint32_t startedAt = millis();
  while (op_.load(std::memory_order_acquire) != static_cast<uint8_t>(Op::None)) {
    if (millis() - startedAt >= kOpTimeoutMs) {
      // TinyUSB's blocking control transfer has no timeout of its own — see
      // FINDINGS.md — so an unresponsive adapter would otherwise take core0
      // down with it, and with it the MIDI tunnel that is the only way to ask
      // what went wrong.
      ++opTimeouts_;
      return false;
    }
    tight_loop_contents();
  }
  return opOk_.load(std::memory_order_acquire);
}

// ---- core1: the USB host side ----------------------------------------------

void CdcHostBackend::beginHost() {
  clockOk_ = (clock_get_hz(clk_sys) % 12000000u) == 0;
}

// Each ring is emptied by whichever core consumes it, never by the other:
// core1 drops what is left in toDevice_, core0 drops what is left in
// fromDevice_. `discard()` only moves the consumer's own tail index, so it is
// safe against a producer running flat out on the other core and needs no
// agreement about who is blocked when.
//
// SpscRing::clear() would be the obvious call and is the wrong one. It resets
// both indices, so it is only safe while the *other* core is quiet — an
// invariant the synchronous mailbox usually provides and cannot promise,
// because core0 abandons an op after a second (FINDINGS.md: TinyUSB's blocking
// control transfer has no timeout, so core1 can be stuck inside one). Exactly
// then, with core0 running again and core1 arriving late at the op, clear()
// would be racing the very core it was supposed to be excluding.
void CdcHostBackend::executeOp() {
  const Op op = static_cast<Op>(op_.load(std::memory_order_acquire));
  // Claimed means core0 has taken the mailbox but has not finished filling it
  // in. Nothing to read yet.
  if (op == Op::None || op == Op::Claimed) return;

  bool ok = false;
  const uint8_t idx = cdcIdx_.load(std::memory_order_relaxed);
  const bool mounted = present();

  switch (op) {
    case Op::Open:
      toDevice_.discard(toDevice_.size());
      if (mounted) {
        tuh_cdc_read_clear(idx);
        tuh_cdc_write_clear(idx);
        ok = applyLineCoding();
      }
      break;

    case Op::Close:
      toDevice_.discard(toDevice_.size());
      ok = true;
      break;

    case Op::SetLines:
      ok = mounted ? applyControlLines() : false;
      break;

    case Op::Flush:
      if (pendingFlush_ & kFlushDiscardTx) {
        toDevice_.discard(toDevice_.size());
        if (mounted) tuh_cdc_write_clear(idx);
      }
      if (pendingFlush_ & kFlushDiscardRx) {
        // Only the adapter's FIFO here; the ring is core0's to empty, and it
        // does so as soon as this op comes back to it.
        if (mounted) tuh_cdc_read_clear(idx);
      }
      if (pendingFlush_ & kFlushDrainTx) {
        // Push what core0 has already handed us at the adapter, then ask
        // TinyUSB to put it on the wire. Bounded: the ring is finite and
        // write_available() shrinks to zero, so this cannot spin.
        pumpDevice();
        if (mounted) tuh_cdc_write_flush(idx);
      }
      ok = true;
      break;

    case Op::None:
    case Op::Claimed:
      break;
  }

  opOk_.store(ok, std::memory_order_relaxed);
  // Release: everything above lands before core0 is allowed to observe the
  // op as finished.
  op_.store(static_cast<uint8_t>(Op::None), std::memory_order_release);
}

bool CdcHostBackend::applyLineCoding() {
  cdc_line_coding_t lc;
  lc.bit_rate = pending_.baud;
  lc.data_bits = pending_.databits;
  // bridge::Parity and CDC_LINE_CODING_PARITY_* agree on 0/1/2 = none/odd/even.
  lc.parity = static_cast<uint8_t>(pending_.parity);
  lc.stop_bits = pending_.stopbits == 2 ? CDC_LINE_CODING_STOP_BITS_2
                                        : CDC_LINE_CODING_STOP_BITS_1;

  // Blocking form — a null callback makes tuh_cdc_set_line_coding drive the
  // transfer to completion, pumping tuh_task() itself while it waits. That is
  // safe here and nowhere else: this runs from loop1(), not from inside a
  // TinyUSB callback. It also transparently handles the adapters that need
  // baud and format set as two separate requests (FTDI, CP210x, CH34x).
  xfer_result_t result = XFER_RESULT_INVALID;
  const uint8_t idx = cdcIdx_.load(std::memory_order_relaxed);
  if (!tuh_cdc_set_line_coding(idx, &lc, nullptr, reinterpret_cast<uintptr_t>(&result)))
    return false;
  return result == XFER_RESULT_SUCCESS;
}

bool CdcHostBackend::applyControlLines() {
  // kLineDtr/kLineRts are bits 0 and 1, and so are CDC's DTR and RTS.
  const uint16_t state = pendingLines_ & (kLineDtr | kLineRts);

  xfer_result_t result = XFER_RESULT_INVALID;
  const uint8_t idx = cdcIdx_.load(std::memory_order_relaxed);
  if (!tuh_cdc_set_control_line_state(idx, state, nullptr,
                                      reinterpret_cast<uintptr_t>(&result)))
    return false;
  return result == XFER_RESULT_SUCCESS;
}

void CdcHostBackend::pumpDevice() {
  if (!present()) return;
  const uint8_t idx = cdcIdx_.load(std::memory_order_relaxed);
  uint8_t buf[kChunk];

  // core0 → adapter. peek/discard rather than read, so a short accept by the
  // USB FIFO leaves the remainder in the ring instead of dropping it.
  while (true) {
    const uint32_t room = tuh_cdc_write_available(idx);
    if (room == 0) break;
    size_t want = toDevice_.size();
    if (want > room) want = room;
    if (want > kChunk) want = kChunk;
    if (want == 0) break;

    const size_t got = toDevice_.peek(buf, want);
    const uint32_t wrote = tuh_cdc_write(idx, buf, got);
    if (wrote == 0) break;
    toDevice_.discard(wrote);
    bytesToDevice_ += wrote;
  }
  tuh_cdc_write_flush(idx);

  // adapter → core0. Stopping when the ring is full is not a loss: the bytes
  // stay in TinyUSB's FIFO and, once that fills too, the bulk IN endpoint is
  // simply not polled, which is how USB applies back-pressure.
  while (true) {
    const uint32_t avail = tuh_cdc_read_available(idx);
    if (avail == 0) break;
    size_t want = fromDevice_.space();
    if (want > avail) want = avail;
    if (want > kChunk) want = kChunk;
    if (want == 0) break;

    const uint32_t got = tuh_cdc_read(idx, buf, want);
    if (got == 0) break;
    fromDevice_.write(buf, got);
    bytesFromDevice_ += got;
  }
}

void CdcHostBackend::serviceHost() {
  ++hostTasks_;
  executeOp();
  if (open_.load(std::memory_order_acquire)) pumpDevice();
}

void CdcHostBackend::setPresent(bool present) {
  const uint32_t was = presence_.load(std::memory_order_relaxed);
  if (((was & 1u) != 0) == present) return;  // not a transition
  // One store, so core0 cannot read a level and a change count that disagree.
  // The count is what lets the engine notice an unplug and replug that both
  // land between two of its polls; the level alone reads identically before
  // and after, and the port would be left open against a different device.
  presence_.store((was & ~1u) + 2u + (present ? 1u : 0u),
                  std::memory_order_release);
}

void CdcHostBackend::onMount(uint8_t idx) {
  cdcIdx_.store(idx, std::memory_order_relaxed);
  // TinyUSB's host CDC driver asserts DTR and RTS during enumeration
  // (CFG_TUH_CDC_LINE_CONTROL_ON_ENUM, which the core's tusb_config defines
  // unconditionally so a build flag cannot override it — FINDINGS.md). Seed
  // our idea of the lines from what it actually did rather than from what we
  // would have chosen: STATUS then reports the truth, and no host-visible
  // OPEN has to toggle DTR to get there. Toggling DTR resets most of the
  // boards anyone would plug in.
  uint8_t seeded = 0;
  if (tuh_cdc_get_dtr(idx)) seeded |= kLineDtr;
  if (tuh_cdc_get_rts(idx)) seeded |= kLineRts;
  outLines_.store(seeded, std::memory_order_relaxed);

  setPresent(true);
}

void CdcHostBackend::onUnmount(uint8_t idx) {
  if (idx != cdcIdx_.load(std::memory_order_relaxed)) return;
  setPresent(false);
  // Deliberately not dropping toDevice_ here, even though this core could do
  // it safely. The engine calls close() on seeing the detach and Op::Close is
  // where that belongs, so there is one place that decides an unplugged device
  // means the queue is undeliverable rather than two that have to agree. What
  // came *from* the adapter is left alone either way, so its last bytes still
  // reach the host.
}

}  // namespace bridge

// ---- TinyUSB host callbacks, on core1 --------------------------------------

extern "C" void tuh_cdc_mount_cb(uint8_t idx) { bridge::gCdcHost.onMount(idx); }

extern "C" void tuh_cdc_umount_cb(uint8_t idx) { bridge::gCdcHost.onUnmount(idx); }

#endif  // BRIDGE_BACKEND_CDC_HOST
