// 7-in-8 MSB packing — PROTOCOL.md §2.
//
// Each group of up to 7 raw bytes is transmitted as one MSB byte followed by
// the group's bytes with bit 7 cleared. Bit i of the MSB byte holds bit 7 of
// the i-th byte of the group, LSB first.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace bridge {
namespace sysex7 {

// Bytes on the wire needed to carry `rawLen` raw bytes.
constexpr size_t packedLen(size_t rawLen) { return rawLen + (rawLen + 6) / 7; }

// Largest raw length whose packed form still fits in `capacity` bytes — the
// inverse of packedLen, used to size buffers without trial and error.
constexpr size_t maxRawFor(size_t capacity) {
  return (capacity / 8) * 7 + (capacity % 8 == 0 ? 0 : capacity % 8 - 1);
}

// Writes packedLen(n) bytes to `out`, which must have room. Returns the count.
size_t pack(const uint8_t* raw, size_t n, uint8_t* out);

enum class UnpackStatus : uint8_t {
  Ok = 0,
  HighBitSet,   // an input byte had bit 7 set — not valid SysEx data
  BadMsbByte,   // MSB byte claimed high bits for bytes that are not present
  Truncated,    // trailing MSB byte with no data bytes after it
  OutputTooSmall,
};

struct UnpackResult {
  UnpackStatus status;
  size_t len;  // raw bytes written on success, 0 otherwise
};

// Decodes `n` packed bytes into `out`. Rejects malformed input rather than
// guessing: a corrupt stream must be visible, not silently reinterpreted.
UnpackResult unpack(const uint8_t* packed, size_t n, uint8_t* out,
                    size_t outCapacity);

}  // namespace sysex7
}  // namespace bridge
