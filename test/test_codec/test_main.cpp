// Host-side unit tests for the 7-in-8 codec and the frame reader/writer.
// Run with:  pio test -e native -f test_codec

#include <unity.h>

#include <string.h>

#include "frame.h"
#include "sysex7.h"

using namespace bridge;

// Deterministic pseudo-random filler — a fixed seed keeps a failure
// reproducible, which a real RNG would not.
static uint32_t lcg = 0x12345678u;
static uint8_t nextByte() {
  lcg = lcg * 1664525u + 1013904223u;
  return static_cast<uint8_t>(lcg >> 24);
}

// ---- sysex7 ----------------------------------------------------------------

static void test_packed_len_matches_pack(void) {
  uint8_t raw[300], packed[512];
  for (size_t n = 0; n <= 300; ++n) {
    for (size_t i = 0; i < n; ++i) raw[i] = nextByte();
    const size_t written = sysex7::pack(raw, n, packed);
    TEST_ASSERT_EQUAL_size_t(sysex7::packedLen(n), written);
  }
}

static void test_pack_unpack_roundtrip(void) {
  uint8_t raw[300], packed[512], out[300];
  for (size_t n = 0; n <= 300; ++n) {
    for (size_t i = 0; i < n; ++i) raw[i] = nextByte();
    const size_t p = sysex7::pack(raw, n, packed);

    for (size_t i = 0; i < p; ++i)
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, packed[i] & 0x80,
                                     "packed byte must be 7-bit");

    const auto r = sysex7::unpack(packed, p, out, sizeof(out));
    TEST_ASSERT_EQUAL(sysex7::UnpackStatus::Ok, r.status);
    TEST_ASSERT_EQUAL_size_t(n, r.len);
    if (n) TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, out, n);
  }
}

// The worked example in PROTOCOL.md §2. If this test and the spec ever
// disagree, one of them is wrong and it matters which.
static void test_spec_vector(void) {
  const uint8_t raw[] = {0xFF, 0x00, 0x80, 0x7F};
  const uint8_t expect[] = {0x05, 0x7F, 0x00, 0x00, 0x7F};
  uint8_t packed[16];
  const size_t n = sysex7::pack(raw, sizeof(raw), packed);
  TEST_ASSERT_EQUAL_size_t(sizeof(expect), n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, packed, n);
}

static void test_pack_empty(void) {
  uint8_t packed[4] = {0xAA, 0xAA, 0xAA, 0xAA};
  TEST_ASSERT_EQUAL_size_t(0, sysex7::pack(nullptr, 0, packed));
  TEST_ASSERT_EQUAL_HEX8(0xAA, packed[0]);

  uint8_t out[4];
  const auto r = sysex7::unpack(packed, 0, out, sizeof(out));
  TEST_ASSERT_EQUAL(sysex7::UnpackStatus::Ok, r.status);
  TEST_ASSERT_EQUAL_size_t(0, r.len);
}

static void test_max_raw_for_is_inverse(void) {
  for (size_t cap = 0; cap <= 512; ++cap) {
    const size_t n = sysex7::maxRawFor(cap);
    TEST_ASSERT_TRUE_MESSAGE(sysex7::packedLen(n) <= cap, "maxRawFor overshot");
    TEST_ASSERT_TRUE_MESSAGE(sysex7::packedLen(n + 1) > cap,
                             "maxRawFor was not maximal");
  }
}

static void test_unpack_rejects_high_bit(void) {
  const uint8_t bad[] = {0x00, 0x41, 0xC2};  // third byte has bit 7 set
  uint8_t out[8];
  const auto r = sysex7::unpack(bad, sizeof(bad), out, sizeof(out));
  TEST_ASSERT_EQUAL(sysex7::UnpackStatus::HighBitSet, r.status);
}

static void test_unpack_rejects_overclaiming_msb(void) {
  // Two data bytes follow, so only bits 0 and 1 of the MSB byte may be set.
  const uint8_t bad[] = {0x04, 0x41, 0x42};
  uint8_t out[8];
  const auto r = sysex7::unpack(bad, sizeof(bad), out, sizeof(out));
  TEST_ASSERT_EQUAL(sysex7::UnpackStatus::BadMsbByte, r.status);
}

static void test_unpack_rejects_trailing_msb(void) {
  // A full 7-byte group, then an MSB byte with nothing after it.
  const uint8_t bad[] = {0x00, 1, 2, 3, 4, 5, 6, 7, 0x00};
  uint8_t out[16];
  const auto r = sysex7::unpack(bad, sizeof(bad), out, sizeof(out));
  TEST_ASSERT_EQUAL(sysex7::UnpackStatus::Truncated, r.status);
}

static void test_unpack_respects_output_capacity(void) {
  uint8_t raw[64], packed[128], out[16];
  for (size_t i = 0; i < sizeof(raw); ++i) raw[i] = nextByte();
  const size_t p = sysex7::pack(raw, sizeof(raw), packed);
  const auto r = sysex7::unpack(packed, p, out, sizeof(out));
  TEST_ASSERT_EQUAL(sysex7::UnpackStatus::OutputTooSmall, r.status);
}

// ---- frames ----------------------------------------------------------------

static void test_frame_integer_roundtrip(void) {
  uint8_t buf[64];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Open);
  w.u32(115200);
  w.u7(8);
  w.u14(2048);
  w.u21(1234567);
  const size_t n = w.end();
  TEST_ASSERT_TRUE(n > 0);

  TEST_ASSERT_EQUAL_HEX8(kSysExStart, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(kSysExEnd, buf[n - 1]);
  for (size_t i = 1; i + 1 < n; ++i)
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, buf[i] & 0x80, "body must be 7-bit");

  FrameReader r;
  TEST_ASSERT_EQUAL(FrameReader::Status::Ok, FrameReader::parse(buf, n, r));
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(Cmd::Open), r.cmd());

  uint32_t baud = 0, big = 0;
  uint8_t bits = 0;
  uint16_t win = 0;
  TEST_ASSERT_TRUE(r.u32(baud));
  TEST_ASSERT_TRUE(r.u7(bits));
  TEST_ASSERT_TRUE(r.u14(win));
  TEST_ASSERT_TRUE(r.u21(big));
  TEST_ASSERT_EQUAL_UINT32(115200, baud);
  TEST_ASSERT_EQUAL_UINT8(8, bits);
  TEST_ASSERT_EQUAL_UINT16(2048, win);
  TEST_ASSERT_EQUAL_UINT32(1234567, big);
  TEST_ASSERT_EQUAL_size_t(0, r.remaining());
}

static void test_frame_u32_extremes(void) {
  uint8_t buf[32];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Open);
  w.u32(0xFFFFFFFFu);
  const size_t n = w.end();
  FrameReader r;
  TEST_ASSERT_EQUAL(FrameReader::Status::Ok, FrameReader::parse(buf, n, r));
  uint32_t v = 0;
  TEST_ASSERT_TRUE(r.u32(v));
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, v);
}

static void test_frame_data_roundtrip(void) {
  uint8_t raw[kMaxDataRaw], buf[256], out[kMaxDataRaw];
  for (size_t i = 0; i < sizeof(raw); ++i) raw[i] = nextByte();

  FrameWriter w(buf, sizeof(buf));
  w.begin(Rsp::Data);
  w.u7(42);
  w.packed(raw, sizeof(raw));
  const size_t n = w.end();
  TEST_ASSERT_TRUE(n > 0);
  // The frame must fit the budget the protocol advertises.
  TEST_ASSERT_TRUE(n <= kFrameOverhead + 1 + sysex7::packedLen(kMaxDataRaw));

  FrameReader r;
  TEST_ASSERT_EQUAL(FrameReader::Status::Ok, FrameReader::parse(buf, n, r));
  uint8_t seq = 0;
  size_t len = 0;
  TEST_ASSERT_TRUE(r.u7(seq));
  TEST_ASSERT_EQUAL_UINT8(42, seq);
  TEST_ASSERT_TRUE(r.unpackRest(out, sizeof(out), len));
  TEST_ASSERT_EQUAL_size_t(sizeof(raw), len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, out, len);
}

static void test_frame_writer_reports_overflow(void) {
  uint8_t raw[128];
  memset(raw, 0xAB, sizeof(raw));
  uint8_t small[32];
  FrameWriter w(small, sizeof(small));
  w.begin(Rsp::Data);
  w.packed(raw, sizeof(raw));
  TEST_ASSERT_FALSE(w.ok());
  TEST_ASSERT_EQUAL_size_t(0, w.end());
}

static void test_frame_writer_rejects_non_7bit_bytes(void) {
  const uint8_t bad[] = {0x01, 0x80};
  uint8_t buf[32];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Rsp::Pong);
  w.bytes(bad, sizeof(bad));
  TEST_ASSERT_FALSE(w.ok());
}

static void test_reader_ignores_other_manufacturers(void) {
  const uint8_t other[] = {0xF0, 0x43, 0x00, 0x01, 0x02, 0xF7};  // Yamaha
  FrameReader r;
  TEST_ASSERT_EQUAL(FrameReader::Status::NotOurs,
                    FrameReader::parse(other, sizeof(other), r));
}

static void test_reader_rejects_wrong_magic(void) {
  const uint8_t other[] = {0xF0, 0x7D, 0x11, 0x01, 0x02, 0xF7};
  FrameReader r;
  TEST_ASSERT_EQUAL(FrameReader::Status::NotOurs,
                    FrameReader::parse(other, sizeof(other), r));
}

static void test_reader_flags_bad_version(void) {
  const uint8_t future[] = {0xF0, 0x7D, 0x55, 0x09, 0x02, 0xF7};
  FrameReader r;
  TEST_ASSERT_EQUAL(FrameReader::Status::BadVersion,
                    FrameReader::parse(future, sizeof(future), r));
}

static void test_reader_rejects_non_sysex(void) {
  const uint8_t note[] = {0x90, 0x40, 0x7F};
  FrameReader r;
  TEST_ASSERT_EQUAL(FrameReader::Status::NotSysEx,
                    FrameReader::parse(note, sizeof(note), r));
}

static void test_reader_underrun_is_not_a_read(void) {
  const uint8_t frame[] = {0xF0, 0x7D, 0x55, 0x01, 0x07, 0x01, 0xF7};
  FrameReader r;
  TEST_ASSERT_EQUAL(FrameReader::Status::Ok,
                    FrameReader::parse(frame, sizeof(frame), r));
  uint16_t v = 0xBEEF;
  TEST_ASSERT_FALSE(r.u14(v));  // only one payload byte present
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, v);
  TEST_ASSERT_EQUAL_size_t(1, r.remaining());  // and nothing was consumed
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_packed_len_matches_pack);
  RUN_TEST(test_pack_unpack_roundtrip);
  RUN_TEST(test_spec_vector);
  RUN_TEST(test_pack_empty);
  RUN_TEST(test_max_raw_for_is_inverse);
  RUN_TEST(test_unpack_rejects_high_bit);
  RUN_TEST(test_unpack_rejects_overclaiming_msb);
  RUN_TEST(test_unpack_rejects_trailing_msb);
  RUN_TEST(test_unpack_respects_output_capacity);
  RUN_TEST(test_frame_integer_roundtrip);
  RUN_TEST(test_frame_u32_extremes);
  RUN_TEST(test_frame_data_roundtrip);
  RUN_TEST(test_frame_writer_reports_overflow);
  RUN_TEST(test_frame_writer_rejects_non_7bit_bytes);
  RUN_TEST(test_reader_ignores_other_manufacturers);
  RUN_TEST(test_reader_rejects_wrong_magic);
  RUN_TEST(test_reader_flags_bad_version);
  RUN_TEST(test_reader_rejects_non_sysex);
  RUN_TEST(test_reader_underrun_is_not_a_read);
  return UNITY_END();
}
