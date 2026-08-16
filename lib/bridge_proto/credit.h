// Credit-based flow control — PROTOCOL.md §6.
//
// Two independent halves. SendWindow tracks how much we are still allowed to
// transmit; RecvWindow accumulates how much we have drained and decides when
// that is worth telling the other side about.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bridge {

// Time to wait before returning a below-threshold credit balance. Covers the
// tail of a transfer, where `freed` never reaches half the buffer again.
constexpr uint32_t kCreditIdleMs = 10;

class SendWindow {
 public:
  void reset(uint16_t initialCredit) { credit_ = initialCredit; }

  bool canSend(size_t n) const { return n <= credit_; }
  uint16_t credit() const { return credit_; }

  void consume(size_t n) {
    credit_ = static_cast<uint16_t>(credit_ - (n > credit_ ? credit_ : n));
  }

  // Credits arrive as cumulative deltas; saturate rather than wrap, since a
  // wrapped window would silently permit an overflow.
  void grant(uint16_t delta) {
    const uint32_t sum = static_cast<uint32_t>(credit_) + delta;
    credit_ = sum > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(sum);
  }

 private:
  uint16_t credit_ = 0;
};

class RecvWindow {
 public:
  void reset(uint16_t bufferSize, uint32_t nowMs) {
    bufferSize_ = bufferSize;
    freed_ = 0;
    lastGrantMs_ = nowMs;
  }

  // Called as bytes leave our receive buffer for their real destination.
  void freed(uint16_t n) {
    const uint32_t sum = static_cast<uint32_t>(freed_) + n;
    freed_ = sum > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(sum);
  }

  uint16_t pending() const { return freed_; }

  bool shouldGrant(uint32_t nowMs) const {
    if (freed_ == 0) return false;
    if (freed_ >= bufferSize_ / 2) return true;
    return (nowMs - lastGrantMs_) >= kCreditIdleMs;
  }

  // Returns the delta to put in a CREDIT message and clears the accumulator.
  uint16_t takeGrant(uint32_t nowMs) {
    const uint16_t v = freed_;
    freed_ = 0;
    lastGrantMs_ = nowMs;
    return v;
  }

 private:
  uint16_t bufferSize_ = 0;
  uint16_t freed_ = 0;
  uint32_t lastGrantMs_ = 0;
};

}  // namespace bridge
