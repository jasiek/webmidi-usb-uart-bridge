// Reassembles complete SysEx messages from a raw MIDI byte stream.
//
// Kept here rather than in src/ because the interesting cases — System
// Real-Time bytes interleaved *inside* a SysEx message, a status byte
// aborting one mid-flight — are pure stream logic and worth testing on the
// host. Getting the real-time case wrong corrupts payloads only when the
// upstream device happens to send clock, which is the worst kind of bug to
// find on hardware.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bridge {

template <size_t N>
class SysExAssembler {
 public:
  // Feeds `n` bytes of raw MIDI. `fn(const uint8_t*, size_t)` is invoked once
  // per complete message, F0 … F7 inclusive.
  template <typename F>
  void feed(const uint8_t* p, size_t n, F&& fn) {
    for (size_t i = 0; i < n; ++i) feedByte(p[i], fn);
  }

  // True if a message was dropped for exceeding N since the last check.
  bool takeOverflow() {
    const bool v = overflow_;
    overflow_ = false;
    return v;
  }

  void reset() {
    len_ = 0;
    active_ = false;
  }

 private:
  template <typename F>
  void feedByte(uint8_t b, F& fn) {
    // System Real-Time may be interleaved anywhere, including between the
    // bytes of a SysEx message. It never disturbs the message in progress.
    if (b >= 0xF8) return;

    if (b == 0xF0) {
      // A second F0 abandons whatever was in flight — the previous message
      // was truncated by definition.
      active_ = true;
      len_ = 0;
      buf_[len_++] = b;
      return;
    }

    if (!active_) return;

    if (b == 0xF7) {
      if (len_ < N) {
        buf_[len_++] = b;
        fn(buf_, len_);
      } else {
        overflow_ = true;
      }
      reset();
      return;
    }

    if (b & 0x80) {
      // Any other status byte cancels the message in progress (MIDI 1.0).
      reset();
      return;
    }

    if (len_ >= N) {
      // The message is already too big to be one of ours. Drop it and stay
      // idle until the next F0, rather than emitting a truncated frame.
      overflow_ = true;
      active_ = false;
      return;
    }
    buf_[len_++] = b;
  }

  uint8_t buf_[N];
  size_t len_ = 0;
  bool active_ = false;
  bool overflow_ = false;
};

}  // namespace bridge
