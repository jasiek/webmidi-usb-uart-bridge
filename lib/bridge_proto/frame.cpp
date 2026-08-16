#include "frame.h"

#include "sysex7.h"

namespace bridge {

// ---- FrameWriter -----------------------------------------------------------

void FrameWriter::put(uint8_t v) {
  if (len_ >= cap_) {
    ok_ = false;
    return;
  }
  buf_[len_++] = v;
}

void FrameWriter::begin(uint8_t cmd) {
  len_ = 0;
  ok_ = true;
  put(kSysExStart);
  put(kManufacturerId);
  put(kMagic);
  put(kProtocolVersion);
  put(cmd);
}

void FrameWriter::u7(uint8_t v) { put(v & 0x7F); }

void FrameWriter::u14(uint16_t v) {
  put(v & 0x7F);
  put(static_cast<uint8_t>((v >> 7) & 0x7F));
}

void FrameWriter::u21(uint32_t v) {
  put(v & 0x7F);
  put(static_cast<uint8_t>((v >> 7) & 0x7F));
  put(static_cast<uint8_t>((v >> 14) & 0x7F));
}

void FrameWriter::u32(uint32_t v) {
  for (int i = 0; i < 5; ++i) put(static_cast<uint8_t>((v >> (7 * i)) & 0x7F));
}

void FrameWriter::bytes(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i] & 0x80) {
      ok_ = false;
      return;
    }
    put(p[i]);
  }
}

void FrameWriter::packed(const uint8_t* p, size_t n) {
  if (len_ + sysex7::packedLen(n) > cap_) {
    ok_ = false;
    return;
  }
  len_ += sysex7::pack(p, n, &buf_[len_]);
}

size_t FrameWriter::end() {
  put(kSysExEnd);
  return ok_ ? len_ : 0;
}

// ---- FrameReader -----------------------------------------------------------

FrameReader::Status FrameReader::parse(const uint8_t* msg, size_t n,
                                       FrameReader& out) {
  if (n < 2 || msg[0] != kSysExStart || msg[n - 1] != kSysExEnd)
    return Status::NotSysEx;
  // Header is 4 bytes plus the command byte, then the terminating F7.
  if (n < kHeaderLen + 1) {
    // Too short to even carry our header — but if the first bytes that are
    // present already disagree with us, it is somebody else's message.
    if (n >= 2 && msg[1] != kManufacturerId) return Status::NotOurs;
    if (n >= 3 && msg[2] != kMagic) return Status::NotOurs;
    return Status::TooShort;
  }
  if (msg[1] != kManufacturerId || msg[2] != kMagic) return Status::NotOurs;
  if (msg[3] != kProtocolVersion) return Status::BadVersion;

  out.cmd_ = msg[4];
  out.payload_ = msg;
  out.pos_ = kHeaderLen;
  out.end_ = n - 1;  // exclusive of the trailing F7
  return Status::Ok;
}

bool FrameReader::u7(uint8_t& v) {
  if (remaining() < 1) return false;
  v = payload_[pos_++] & 0x7F;
  return true;
}

bool FrameReader::u14(uint16_t& v) {
  if (remaining() < 2) return false;
  v = static_cast<uint16_t>((payload_[pos_] & 0x7F) |
                            ((payload_[pos_ + 1] & 0x7F) << 7));
  pos_ += 2;
  return true;
}

bool FrameReader::u21(uint32_t& v) {
  if (remaining() < 3) return false;
  v = static_cast<uint32_t>(payload_[pos_] & 0x7F) |
      (static_cast<uint32_t>(payload_[pos_ + 1] & 0x7F) << 7) |
      (static_cast<uint32_t>(payload_[pos_ + 2] & 0x7F) << 14);
  pos_ += 3;
  return true;
}

bool FrameReader::u32(uint32_t& v) {
  if (remaining() < 5) return false;
  v = 0;
  for (int i = 0; i < 5; ++i)
    v |= static_cast<uint32_t>(payload_[pos_ + i] & 0x7F) << (7 * i);
  pos_ += 5;
  return true;
}

bool FrameReader::rest(uint8_t* out, size_t capacity, size_t& len) {
  const size_t n = remaining();
  if (n > capacity) return false;
  for (size_t i = 0; i < n; ++i) out[i] = payload_[pos_ + i];
  pos_ += n;
  len = n;
  return true;
}

bool FrameReader::unpackRest(uint8_t* out, size_t capacity, size_t& len) {
  const auto r = sysex7::unpack(&payload_[pos_], remaining(), out, capacity);
  if (r.status != sysex7::UnpackStatus::Ok) return false;
  pos_ = end_;
  len = r.len;
  return true;
}

}  // namespace bridge
