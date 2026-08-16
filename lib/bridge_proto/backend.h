// The far end of the tunnel, behind one interface.
//
// Phase 1 implements this over a hardware UART (src/backend_uart.*); phase 2
// adds a Pico-PIO-USB CDC/ACM host. Nothing above this line knows which.
// See DECISIONS.md D1.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bridge_proto.h"

namespace bridge {

class Backend {
 public:
  virtual ~Backend() = default;

  virtual BackendId id() const = 0;
  virtual uint16_t caps() const = 0;
  virtual uint32_t maxBaud() const = 0;

  virtual bool open(const PortConfig& cfg) = 0;
  virtual void close() = 0;
  virtual bool isOpen() const = 0;

  // Whether a far end physically exists right now, independent of whether a
  // port is open on it. A soldered-down UART has no way to tell and no reason
  // to care, so the default is "always there"; the PIO-USB host backend
  // reports device attach and detach through it, and the engine turns the
  // transitions into EVT_ATTACH / EVT_DETACH. Only meaningful when caps()
  // advertises kCapHotplug.
  virtual bool present() const { return true; }

  // Bytes that write() would accept right now.
  virtual size_t writable() const = 0;
  // Returns how many bytes were accepted; may be less than n.
  virtual size_t write(const uint8_t* p, size_t n) = 0;

  virtual size_t readable() const = 0;
  virtual size_t read(uint8_t* p, size_t n) = 0;

  // Bit combination of kFlush*.
  virtual void flush(uint8_t what) = 0;

  virtual void setLines(uint8_t mask, uint8_t values) = 0;
  virtual uint8_t outputLines() const = 0;
  virtual uint8_t inputLines() const = 0;

  // Sticky kErrFlag* bits, cleared by the read.
  virtual uint8_t takeErrorFlags() = 0;
};

// Where completed SysEx frames go — the USB MIDI device endpoint in the
// firmware, a capture list in the tests.
class FrameSink {
 public:
  virtual ~FrameSink() = default;

  // True when send() would succeed. The engine checks this before building a
  // frame so that back-pressure from USB never costs us buffered data.
  virtual bool ready() const = 0;

  // `frame` is one complete SysEx message, F0 … F7 inclusive.
  virtual bool send(const uint8_t* frame, size_t len) = 0;

  // Discards anything queued but not yet transmitted. Called when a host
  // starts a new conversation (HELLO) or demands a known state (RESET):
  // whatever is still queued was addressed to the previous conversation and
  // would only arrive as a confusing prefix to the reply.
  virtual void discardQueued() {}
};

}  // namespace bridge
