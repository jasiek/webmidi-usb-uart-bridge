// A byte ring whose producer and consumer run on different cores.
//
// lib/bridge_proto/ringbuf.h is the same idea for one core and says in its
// header comment that a core1 backend would need release/acquire ordering on
// the indices. This is that: the PIO-USB host stack owns core1, the protocol
// engine owns core0, and these two rings are the entire surface between them
// for bulk data. Both cores are Cortex-M0+ with no cache, so the ordering is
// purely a compiler and store-buffer question and plain 32-bit loads and
// stores are already atomic — but the indices still have to be published in
// the right order relative to the payload, which is what the release/acquire
// pairs below buy.
//
// The producer owns head_ and only ever reads tail_; the consumer owns tail_
// and only ever reads head_. Neither index is ever written by both cores,
// which is what makes this lock-free without a compare-and-swap.
#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace bridge {

template <size_t N>
class SpscRing {
  static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

 public:
  static constexpr size_t capacity() { return N - 1; }  // one slot marks full

  // Safe from either core; the answer is a lower bound from the reader's point
  // of view, because the other side can only ever make it better.
  size_t size() const {
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t t = tail_.load(std::memory_order_acquire);
    return (h - t) & (N - 1);
  }
  size_t space() const { return capacity() - size(); }
  bool empty() const { return size() == 0; }

  // ---- producer side ----

  size_t write(const uint8_t* p, size_t n) {
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t t = tail_.load(std::memory_order_acquire);
    const size_t room = capacity() - ((h - t) & (N - 1));
    const size_t take = n < room ? n : room;
    size_t i = h;
    for (size_t k = 0; k < take; ++k) {
      buf_[i] = p[k];
      i = (i + 1) & (N - 1);
    }
    // Release: the bytes above are visible to the consumer before the index
    // that admits they exist.
    head_.store(i, std::memory_order_release);
    return take;
  }

  // ---- consumer side ----

  size_t read(uint8_t* p, size_t n) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t avail = (h - t) & (N - 1);
    const size_t take = n < avail ? n : avail;
    size_t i = t;
    for (size_t k = 0; k < take; ++k) {
      p[k] = buf_[i];
      i = (i + 1) & (N - 1);
    }
    tail_.store(i, std::memory_order_release);
    return take;
  }

  // Reads without consuming — used when the destination may accept only part
  // of what is offered, which is the normal case for a USB FIFO.
  size_t peek(uint8_t* p, size_t n) const {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t avail = (h - t) & (N - 1);
    const size_t take = n < avail ? n : avail;
    size_t i = t;
    for (size_t k = 0; k < take; ++k) {
      p[k] = buf_[i];
      i = (i + 1) & (N - 1);
    }
    return take;
  }

  void discard(size_t n) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t avail = (h - t) & (N - 1);
    const size_t take = n < avail ? n : avail;
    tail_.store((t + take) & (N - 1), std::memory_order_release);
  }

  // ---- either side, but only when the other is known to be quiet ----

  // Discards everything. Safe only when the producer is not producing — on
  // this backend that means from core1, between attach and open, or with the
  // port closed. A concurrent clear() would race the producer's head_.
  void clear() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_release);
  }

 private:
  uint8_t buf_[N];
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};

}  // namespace bridge
