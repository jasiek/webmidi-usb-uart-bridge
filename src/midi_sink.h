// FrameSink over the USB MIDI device endpoint.
//
// TinyUSB's MIDI TX FIFO is fixed at 64 bytes (CFG_TUD_MIDI_TX_BUFSIZE is
// #define'd unconditionally in the Adafruit port, so no build flag can raise
// it), and our largest frame is 154. A frame therefore cannot be handed to
// TinyUSB in one call, and a FrameSink that reported success on a partial
// write would emit truncated SysEx.
//
// So this holds its own outbound buffer: ready() is true only when a whole
// frame fits, send() takes all of it, and service() feeds TinyUSB whatever it
// will accept each time round the loop.
//
// The stall handling exists because `mounted` is a statement about USB, not
// about anyone listening. When a MIDI client goes away the host stops draining
// the bulk IN endpoint; TinyUSB's FIFO fills, this buffer fills behind it, and
// ready() would otherwise stay false for ever — leaving the device unable to
// answer the *next* client that connects.
#pragma once

#include <Adafruit_TinyUSB.h>

#include "backend.h"
#include "bridge.h"
#include "ringbuf.h"
#include "usb_lock.h"

namespace bridge {

namespace {
inline bool midiMounted() {
  UsbLock lock;
  return tud_midi_mounted();
}
}  // namespace

// Room for several frames, so a burst does not stall on the USB endpoint.
constexpr size_t kMidiOutSlots = 4096;

// How long the buffer may sit unable to make progress before we conclude
// nobody is reading and drop what is queued. Comfortably longer than any
// legitimate USB scheduling gap.
constexpr uint32_t kSinkStallMs = 1500;

class UsbMidiSink : public FrameSink {
 public:
  bool ready() const override {
    return midiMounted() && out_.space() >= kMaxFrameLen;
  }

  bool send(const uint8_t* frame, size_t len) override {
    if (!midiMounted() || out_.space() < len) {
      ++dropped_;
      return false;
    }
    return out_.write(frame, len) == len;
  }

  void discardQueued() override {
    out_.clear();
    blockedSinceMs_ = 0;
  }

  // Pushes buffered bytes into TinyUSB. Call once per loop.
  void service(uint32_t nowMs) {
    if (!midiMounted()) {
      // Nothing can leave while unplugged, and replaying stale frames at a
      // host that has reset its sequence numbers would be worse than useless.
      out_.clear();
      blockedSinceMs_ = 0;
      return;
    }

    bool progressed = false;
    uint8_t chunk[64];
    while (!out_.empty()) {
      const size_t want = out_.peek(chunk, sizeof(chunk));
      uint32_t wrote = 0;
      {
        UsbLock lock;
        wrote = tud_midi_stream_write(0, chunk, want);
      }
      if (wrote == 0) break;
      out_.discard(wrote);
      bytesOut_ += wrote;
      progressed = true;
      if (wrote < want) break;
    }

    if (out_.empty() || progressed) {
      blockedSinceMs_ = 0;
      return;
    }

    // No progress and still holding data: start, or check, the stall clock.
    if (blockedSinceMs_ == 0) {
      blockedSinceMs_ = nowMs == 0 ? 1 : nowMs;  // 0 is the "not stalled" value
    } else if (nowMs - blockedSinceMs_ >= kSinkStallMs) {
      out_.clear();
      blockedSinceMs_ = 0;
      ++stalls_;
    }
  }

  size_t buffered() const { return out_.size(); }
  uint32_t dropped() const { return dropped_; }
  uint32_t stalls() const { return stalls_; }
  uint32_t bytesOut() const { return bytesOut_; }
  bool blocked() const { return blockedSinceMs_ != 0; }

 private:
  RingBuf<kMidiOutSlots> out_;
  uint32_t blockedSinceMs_ = 0;
  uint32_t dropped_ = 0;
  uint32_t stalls_ = 0;
  uint32_t bytesOut_ = 0;
};

}  // namespace bridge
