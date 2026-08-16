#include "sysex7.h"

namespace bridge {
namespace sysex7 {

size_t pack(const uint8_t* raw, size_t n, uint8_t* out) {
  size_t written = 0;
  for (size_t i = 0; i < n; i += 7) {
    const size_t k = (n - i < 7) ? (n - i) : 7;
    uint8_t* msb = &out[written++];
    *msb = 0;
    for (size_t j = 0; j < k; ++j) {
      const uint8_t b = raw[i + j];
      *msb |= static_cast<uint8_t>((b >> 7) & 1u) << j;
      out[written++] = b & 0x7F;
    }
  }
  return written;
}

UnpackResult unpack(const uint8_t* packed, size_t n, uint8_t* out,
                    size_t outCapacity) {
  size_t written = 0;
  size_t i = 0;
  while (i < n) {
    const uint8_t msb = packed[i++];
    if (msb & 0x80) return {UnpackStatus::HighBitSet, 0};

    const size_t remaining = n - i;
    if (remaining == 0) return {UnpackStatus::Truncated, 0};
    const size_t k = remaining < 7 ? remaining : 7;

    // The MSB byte may only claim high bits for bytes that actually follow.
    // A group of k bytes leaves bits k..6 of the MSB byte unused.
    if (k < 7 && (msb >> k) != 0) return {UnpackStatus::BadMsbByte, 0};

    if (written + k > outCapacity) return {UnpackStatus::OutputTooSmall, 0};
    for (size_t j = 0; j < k; ++j) {
      const uint8_t b = packed[i + j];
      if (b & 0x80) return {UnpackStatus::HighBitSet, 0};
      out[written++] = static_cast<uint8_t>(b | (((msb >> j) & 1u) << 7));
    }
    i += k;
  }
  return {UnpackStatus::Ok, written};
}

}  // namespace sysex7
}  // namespace bridge
