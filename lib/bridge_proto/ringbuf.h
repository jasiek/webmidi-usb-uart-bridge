// Fixed-capacity byte ring buffer.
//
// Single producer, single consumer, both on the same core. If the PIO-USB
// backend later moves to core1 this needs release/acquire ordering on the
// indices — see FINDINGS.md.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bridge {

template <size_t N>
class RingBuf {
  static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

 public:
  size_t capacity() const { return N - 1; }  // one slot distinguishes full/empty
  size_t size() const { return (head_ - tail_) & (N - 1); }
  size_t space() const { return capacity() - size(); }
  bool empty() const { return head_ == tail_; }

  void clear() { head_ = tail_ = 0; }

  // Returns how many bytes were actually taken.
  size_t write(const uint8_t* p, size_t n) {
    const size_t take = n < space() ? n : space();
    for (size_t i = 0; i < take; ++i) {
      buf_[head_] = p[i];
      head_ = (head_ + 1) & (N - 1);
    }
    return take;
  }

  size_t read(uint8_t* p, size_t n) {
    const size_t take = n < size() ? n : size();
    for (size_t i = 0; i < take; ++i) {
      p[i] = buf_[tail_];
      tail_ = (tail_ + 1) & (N - 1);
    }
    return take;
  }

  // Reads without consuming — used when the destination may accept only part.
  size_t peek(uint8_t* p, size_t n) const {
    const size_t take = n < size() ? n : size();
    size_t t = tail_;
    for (size_t i = 0; i < take; ++i) {
      p[i] = buf_[t];
      t = (t + 1) & (N - 1);
    }
    return take;
  }

  void discard(size_t n) {
    const size_t take = n < size() ? n : size();
    tail_ = (tail_ + take) & (N - 1);
  }

 private:
  uint8_t buf_[N];
  size_t head_ = 0;
  size_t tail_ = 0;
};

}  // namespace bridge
