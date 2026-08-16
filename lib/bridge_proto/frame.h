// Frame construction and parsing — PROTOCOL.md §1, §3.
//
// Both halves are bounds-checked and allocation-free. The writer accumulates
// an overflow flag rather than truncating silently, so a caller that gets a
// non-zero length from end() knows the whole frame is present.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bridge_proto.h"

namespace bridge {

class FrameWriter {
 public:
  FrameWriter(uint8_t* buf, size_t capacity) : buf_(buf), cap_(capacity) {}

  // Emits F0 7D 55 01 <cmd>. Resets any previous contents.
  void begin(uint8_t cmd);
  void begin(Rsp cmd) { begin(static_cast<uint8_t>(cmd)); }
  void begin(Cmd cmd) { begin(static_cast<uint8_t>(cmd)); }

  void u7(uint8_t v);
  void u14(uint16_t v);
  void u21(uint32_t v);
  void u32(uint32_t v);

  // Copies bytes that are already 7-bit safe. Any byte with bit 7 set is a
  // programming error and trips the overflow flag rather than corrupting the
  // stream.
  void bytes(const uint8_t* p, size_t n);

  // Copies bytes through 7-in-8 packing.
  void packed(const uint8_t* p, size_t n);

  // Appends F7 and returns the total frame length, or 0 if anything overflowed.
  size_t end();

  bool ok() const { return ok_; }
  size_t length() const { return len_; }

 private:
  void put(uint8_t v);

  uint8_t* buf_;
  size_t cap_;
  size_t len_ = 0;
  bool ok_ = true;
};

class FrameReader {
 public:
  enum class Status : uint8_t {
    Ok = 0,
    NotSysEx,     // does not start F0 / end F7
    NotOurs,      // different manufacturer or magic — ignore silently
    BadVersion,   // ours, but a protocol version we do not speak
    TooShort,     // truncated before the command byte
  };

  // Validates the header of a complete SysEx message (F0 … F7 inclusive).
  static Status parse(const uint8_t* msg, size_t n, FrameReader& out);

  uint8_t cmd() const { return cmd_; }
  size_t remaining() const { return end_ - pos_; }

  // Each returns false — and consumes nothing — if the payload is too short.
  bool u7(uint8_t& v);
  bool u14(uint16_t& v);
  bool u21(uint32_t& v);
  bool u32(uint32_t& v);

  // Copies the rest of the payload verbatim (still 7-bit).
  bool rest(uint8_t* out, size_t capacity, size_t& len);

  // Unpacks the rest of the payload from 7-in-8.
  bool unpackRest(uint8_t* out, size_t capacity, size_t& len);

 private:
  const uint8_t* payload_ = nullptr;
  size_t pos_ = 0;
  size_t end_ = 0;
  uint8_t cmd_ = 0;
};

}  // namespace bridge
