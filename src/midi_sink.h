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
#pragma once

#include <Adafruit_TinyUSB.h>

#include "backend.h"
#include "bridge.h"
#include "ringbuf.h"

namespace bridge {

// Room for several frames, so a burst does not stall on the USB endpoint.
constexpr size_t kMidiOutSlots = 4096;

class UsbMidiSink : public FrameSink {
 public:
  bool ready() const override {
    return tud_midi_mounted() && out_.space() >= kMaxFrameLen;
  }

  bool send(const uint8_t* frame, size_t len) override {
    if (!tud_midi_mounted() || out_.space() < len) return false;
    return out_.write(frame, len) == len;
  }

  // Pushes buffered bytes into TinyUSB. Call once per loop.
  void service() {
    if (!tud_midi_mounted()) {
      // Nothing can leave while unplugged; dropping it keeps a reconnect from
      // replaying stale frames at a host that has reset its sequence numbers.
      out_.clear();
      return;
    }
    uint8_t chunk[64];
    while (!out_.empty()) {
      const size_t want = out_.peek(chunk, sizeof(chunk));
      const uint32_t wrote = tud_midi_stream_write(0, chunk, want);
      if (wrote == 0) break;
      out_.discard(wrote);
      if (wrote < want) break;
    }
  }

  size_t buffered() const { return out_.size(); }

 private:
  RingBuf<kMidiOutSlots> out_;
};

}  // namespace bridge
