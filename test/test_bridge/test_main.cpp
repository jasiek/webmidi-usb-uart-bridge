// Host-side unit tests for the protocol engine: flow control, sequencing,
// error paths and back-pressure, all against a mock backend and a mock USB
// endpoint. No hardware involved.
//
// Run with:  pio test -e native -f test_bridge

#include <unity.h>

#include <deque>
#include <utility>
#include <vector>

#include "bridge.h"

using namespace bridge;

// ---- test doubles ----------------------------------------------------------

class MockBackend : public Backend {
 public:
  BackendId id() const override { return BackendId::HardwareUart; }
  uint16_t caps() const override { return kCapRts | kCapCts | kCapBreak; }
  uint32_t maxBaud() const override { return 921600; }

  bool open(const PortConfig& c) override {
    cfg = c;
    if (!openSucceeds) return false;
    isOpen_ = true;
    return true;
  }
  void close() override { isOpen_ = false; }
  bool isOpen() const override { return isOpen_; }
  bool present() const override { return presentFlag; }
  uint32_t presence() const override {
    return (presenceChanges << 1) | (presentFlag ? 1u : 0u);
  }

  size_t writable() const override {
    const size_t pending = txCapacity > sent.size() ? txCapacity - sent.size() : 0;
    return pending;
  }
  size_t write(const uint8_t* p, size_t n) override {
    const size_t room = writable();
    const size_t take = n < room ? n : room;
    for (size_t i = 0; i < take; ++i) sent.push_back(p[i]);
    return take;
  }

  size_t readable() const override { return incoming.size(); }
  size_t read(uint8_t* p, size_t n) override {
    const size_t take = n < incoming.size() ? n : incoming.size();
    for (size_t i = 0; i < take; ++i) {
      p[i] = incoming.front();
      incoming.pop_front();
    }
    return take;
  }

  void flush(uint8_t what) override { flushed |= what; }
  void setLines(uint8_t mask, uint8_t values) override {
    outLines = static_cast<uint8_t>((outLines & ~mask) | (values & mask));
  }
  uint8_t outputLines() const override { return outLines; }
  uint8_t inputLines() const override { return inLines; }
  uint8_t takeErrorFlags() override {
    const uint8_t v = errFlags;
    errFlags = 0;
    return v;
  }

  // Test knobs.
  bool openSucceeds = true;
  bool presentFlag = true;  // a far end that can be unplugged, as in phase 2
  // How many times presentFlag has flipped, as the real backend counts it.
  // Bumping it by two without touching presentFlag is an unplug and replug
  // that both happened between two polls — invisible to the level alone.
  uint32_t presenceChanges = 0;
  size_t txCapacity = 100000;  // effectively unlimited unless a test narrows it
  std::vector<uint8_t> sent;       // bytes the bridge pushed to the far end
  std::deque<uint8_t> incoming;    // bytes the far end will deliver
  uint8_t outLines = 0;
  uint8_t inLines = 0;
  uint8_t errFlags = 0;
  uint8_t flushed = 0;
  PortConfig cfg;

 private:
  bool isOpen_ = false;
};

struct CapturedFrame {
  uint8_t cmd = 0;
  std::vector<uint8_t> bytes;
};

class MockSink : public FrameSink {
 public:
  bool ready() const override { return isReady; }
  bool send(const uint8_t* frame, size_t len) override {
    if (!isReady) return false;
    CapturedFrame f;
    f.bytes.assign(frame, frame + len);
    f.cmd = len > kHeaderLen ? frame[4] : 0;
    frames.push_back(f);
    return true;
  }

  void discardQueued() override {
    ++discards;
    frames.clear();
  }

  bool isReady = true;
  int discards = 0;
  std::vector<CapturedFrame> frames;

  void clear() { frames.clear(); }

  int count(Rsp cmd) const {
    int n = 0;
    for (const auto& f : frames)
      if (f.cmd == static_cast<uint8_t>(cmd)) ++n;
    return n;
  }

  const CapturedFrame* first(Rsp cmd) const {
    for (const auto& f : frames)
      if (f.cmd == static_cast<uint8_t>(cmd)) return &f;
    return nullptr;
  }

  const CapturedFrame* last(Rsp cmd) const {
    const CapturedFrame* hit = nullptr;
    for (const auto& f : frames)
      if (f.cmd == static_cast<uint8_t>(cmd)) hit = &f;
    return hit;
  }
};

// ---- fixture ---------------------------------------------------------------

static MockBackend* backend;
static MockSink* sink;
static Bridge* br;
static uint32_t clockMs;

void setUp(void) {
  backend = new MockBackend();
  sink = new MockSink();
  br = new Bridge(*backend, *sink);
  clockMs = 0;
  br->begin(clockMs);
}

void tearDown(void) {
  delete br;
  delete sink;
  delete backend;
}

// Builds a host→device frame and feeds it in.
static std::vector<uint8_t> buildOpen(uint32_t baud, uint16_t hostWindow) {
  uint8_t buf[64];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Open);
  w.u32(baud);
  w.u7(8);
  w.u7(0);
  w.u7(1);
  w.u7(0);
  w.u14(hostWindow);
  const size_t n = w.end();
  return std::vector<uint8_t>(buf, buf + n);
}

static void feed(const std::vector<uint8_t>& v) {
  br->onSysEx(v.data(), v.size(), clockMs);
}

static void feedData(uint8_t seq, const uint8_t* p, size_t n) {
  uint8_t buf[kMaxFrameLen];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Data);
  w.u7(seq);
  w.packed(p, n);
  const size_t len = w.end();
  TEST_ASSERT_TRUE(len > 0);
  br->onSysEx(buf, len, clockMs);
}

static void feedCredit(uint16_t delta) {
  uint8_t buf[32];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Credit);
  w.u14(delta);
  const size_t n = w.end();
  br->onSysEx(buf, n, clockMs);
}

static void feedSimple(Cmd cmd) {
  uint8_t buf[32];
  FrameWriter w(buf, sizeof(buf));
  w.begin(cmd);
  const size_t n = w.end();
  br->onSysEx(buf, n, clockMs);
}

// Gets to a known open port and forgets how we got there — including the
// discard OPEN itself performs, so a test that counts discards is counting
// only the ones it caused.
static void openPort(uint16_t hostWindow = kRxBufferSize) {
  feed(buildOpen(115200, hostWindow));
  sink->clear();
  sink->discards = 0;
}

static void tick(uint32_t advanceMs = 1) {
  clockMs += advanceMs;
  br->poll(clockMs);
}

// Reassembles every DATA frame the device emitted into one byte stream, and
// checks the sequence numbers are contiguous from 0 along the way.
static std::vector<uint8_t> collectData(uint8_t startSeq = 0) {
  std::vector<uint8_t> out;
  uint8_t expect = startSeq;
  for (const auto& f : sink->frames) {
    if (f.cmd != static_cast<uint8_t>(Rsp::Data)) continue;
    FrameReader r;
    TEST_ASSERT_EQUAL(FrameReader::Status::Ok,
                      FrameReader::parse(f.bytes.data(), f.bytes.size(), r));
    uint8_t seq = 0;
    TEST_ASSERT_TRUE(r.u7(seq));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expect, seq, "DATA sequence gap");
    expect = static_cast<uint8_t>((expect + 1) & kSeqMask);

    uint8_t raw[kMaxDataRaw];
    size_t len = 0;
    TEST_ASSERT_TRUE(r.unpackRest(raw, sizeof(raw), len));
    out.insert(out.end(), raw, raw + len);
  }
  return out;
}

static uint8_t errorCode(const CapturedFrame* f) {
  FrameReader r;
  FrameReader::parse(f->bytes.data(), f->bytes.size(), r);
  uint8_t code = 0;
  r.u7(code);
  return code;
}

static uint8_t errorDetail(const CapturedFrame* f) {
  FrameReader r;
  FrameReader::parse(f->bytes.data(), f->bytes.size(), r);
  uint8_t code = 0, detail = 0;
  r.u7(code);
  r.u7(detail);
  return detail;
}

// ---- handshake -------------------------------------------------------------

static void test_hello_returns_info(void) {
  uint8_t buf[32];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Hello);
  w.u14(4096);
  w.u14(64);
  const size_t n = w.end();
  br->onSysEx(buf, n, clockMs);

  const CapturedFrame* info = sink->first(Rsp::Info);
  TEST_ASSERT_NOT_NULL(info);

  FrameReader r;
  FrameReader::parse(info->bytes.data(), info->bytes.size(), r);
  uint8_t proto = 0, maj = 0, min = 0, pat = 0, backendId = 0;
  uint16_t caps = 0, maxRaw = 0, rxBuf = 0;
  uint32_t maxBaud = 0;
  TEST_ASSERT_TRUE(r.u7(proto));
  TEST_ASSERT_TRUE(r.u7(maj));
  TEST_ASSERT_TRUE(r.u7(min));
  TEST_ASSERT_TRUE(r.u7(pat));
  TEST_ASSERT_TRUE(r.u7(backendId));
  TEST_ASSERT_TRUE(r.u14(caps));
  TEST_ASSERT_TRUE(r.u14(maxRaw));
  TEST_ASSERT_TRUE(r.u14(rxBuf));
  TEST_ASSERT_TRUE(r.u32(maxBaud));

  TEST_ASSERT_EQUAL_UINT8(kProtocolVersion, proto);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BackendId::HardwareUart), backendId);
  TEST_ASSERT_EQUAL_UINT16(kMaxDataRaw, maxRaw);
  TEST_ASSERT_EQUAL_UINT16(kRxBufferSize, rxBuf);
  TEST_ASSERT_EQUAL_UINT32(921600, maxBaud);
  TEST_ASSERT_EQUAL_size_t(0, r.remaining());
}

static void test_hello_caps_device_frame_size_to_host_limit(void) {
  uint8_t buf[32];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Hello);
  w.u14(kRxBufferSize);
  w.u14(32);  // host will not accept more than 32 raw bytes per DATA
  br->onSysEx(buf, w.end(), clockMs);

  openPort();
  for (int i = 0; i < 100; ++i) backend->incoming.push_back(static_cast<uint8_t>(i));
  tick();

  TEST_ASSERT_TRUE(sink->count(Rsp::Data) >= 4);  // 100 bytes in ≤32-byte frames
  for (const auto& f : sink->frames) {
    if (f.cmd != static_cast<uint8_t>(Rsp::Data)) continue;
    FrameReader r;
    FrameReader::parse(f.bytes.data(), f.bytes.size(), r);
    uint8_t seq = 0;
    r.u7(seq);
    uint8_t raw[kMaxDataRaw];
    size_t len = 0;
    TEST_ASSERT_TRUE(r.unpackRest(raw, sizeof(raw), len));
    TEST_ASSERT_TRUE_MESSAGE(len <= 32, "frame exceeded the host's stated limit");
  }
}

static void test_data_before_open_is_rejected(void) {
  const uint8_t payload[] = {1, 2, 3};
  feedData(0, payload, sizeof(payload));
  const CapturedFrame* err = sink->first(Rsp::Error);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::NotOpen), errorCode(err));
}

static void test_open_reports_status(void) {
  feed(buildOpen(115200, kRxBufferSize));
  const CapturedFrame* st = sink->first(Rsp::Status);
  TEST_ASSERT_NOT_NULL(st);
  FrameReader r;
  FrameReader::parse(st->bytes.data(), st->bytes.size(), r);
  uint8_t state = 0;
  r.u7(state);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PortState::Open), state);
  TEST_ASSERT_EQUAL_UINT32(115200, backend->cfg.baud);
  TEST_ASSERT_TRUE(backend->isOpen());
}

static void test_open_rejects_excessive_baud(void) {
  feed(buildOpen(2000000, kRxBufferSize));  // above the backend's maxBaud
  const CapturedFrame* err = sink->first(Rsp::Error);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::BadParam), errorCode(err));
  TEST_ASSERT_EQUAL_UINT8(0, errorDetail(err));  // field 0 is baud
  TEST_ASSERT_FALSE(backend->isOpen());
}

static void test_open_backend_failure_faults(void) {
  backend->openSucceeds = false;
  feed(buildOpen(115200, kRxBufferSize));
  const CapturedFrame* err = sink->first(Rsp::Error);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::Backend), errorCode(err));
  TEST_ASSERT_EQUAL(PortState::Fault, br->state());
}

// ---- host → far end --------------------------------------------------------

static void test_host_data_reaches_backend(void) {
  openPort();
  uint8_t payload[kMaxDataRaw];
  for (size_t i = 0; i < sizeof(payload); ++i)
    payload[i] = static_cast<uint8_t>(i * 7 + 1);

  feedData(0, payload, sizeof(payload));
  tick();

  TEST_ASSERT_EQUAL_size_t(sizeof(payload), backend->sent.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, backend->sent.data(), sizeof(payload));
  TEST_ASSERT_EQUAL_UINT32(sizeof(payload), br->txCount());
}

static void test_high_bytes_survive_the_tunnel(void) {
  openPort();
  // Every possible byte value, including the ones SysEx cannot carry directly.
  uint8_t payload[256];
  for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<uint8_t>(i);

  feedData(0, payload, 128);
  feedData(1, payload + 128, 128);
  tick();

  TEST_ASSERT_EQUAL_size_t(256, backend->sent.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, backend->sent.data(), 256);
}

static void test_credit_returned_after_half_buffer_drains(void) {
  openPort();
  uint8_t chunk[kMaxDataRaw];
  for (size_t i = 0; i < sizeof(chunk); ++i) chunk[i] = 0xA5;

  // Below the half-buffer threshold and inside the idle window: silence.
  feedData(0, chunk, sizeof(chunk));
  br->poll(clockMs);
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Credit));

  // Past half the 2048-byte window: a grant is due.
  for (uint8_t seq = 1; seq < 10; ++seq) feedData(seq, chunk, sizeof(chunk));
  br->poll(clockMs);
  TEST_ASSERT_TRUE(sink->count(Rsp::Credit) >= 1);

  const CapturedFrame* c = sink->first(Rsp::Credit);
  FrameReader r;
  FrameReader::parse(c->bytes.data(), c->bytes.size(), r);
  uint16_t delta = 0;
  TEST_ASSERT_TRUE(r.u14(delta));
  TEST_ASSERT_TRUE(delta >= kRxBufferSize / 2);
}

static void test_credit_tail_is_returned_on_the_idle_timer(void) {
  openPort();
  const uint8_t chunk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  feedData(0, chunk, sizeof(chunk));

  br->poll(clockMs);
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Credit));

  // Eight bytes will never reach half the buffer; the timer is what covers it.
  tick(kCreditIdleMs + 1);
  TEST_ASSERT_EQUAL_INT(1, sink->count(Rsp::Credit));

  const CapturedFrame* c = sink->first(Rsp::Credit);
  FrameReader r;
  FrameReader::parse(c->bytes.data(), c->bytes.size(), r);
  uint16_t delta = 0;
  r.u14(delta);
  TEST_ASSERT_EQUAL_UINT16(8, delta);
}

static void test_backend_backpressure_holds_data(void) {
  openPort();
  backend->txCapacity = 10;  // the far end only accepts 10 bytes

  uint8_t chunk[64];
  for (size_t i = 0; i < sizeof(chunk); ++i) chunk[i] = static_cast<uint8_t>(i);
  feedData(0, chunk, sizeof(chunk));
  tick();

  TEST_ASSERT_EQUAL_size_t(10, backend->sent.size());

  // Credit is returned only for what actually left, not for what we buffered.
  tick(kCreditIdleMs + 1);
  const CapturedFrame* c = sink->first(Rsp::Credit);
  TEST_ASSERT_NOT_NULL(c);
  FrameReader r;
  FrameReader::parse(c->bytes.data(), c->bytes.size(), r);
  uint16_t delta = 0;
  r.u14(delta);
  TEST_ASSERT_EQUAL_UINT16(10, delta);

  // When the far end drains, the rest follows.
  backend->txCapacity = 64;
  tick();
  TEST_ASSERT_EQUAL_size_t(64, backend->sent.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(chunk, backend->sent.data(), 64);
}

static void test_sequence_gap_is_reported_but_data_kept(void) {
  openPort();
  const uint8_t a[] = {0x11, 0x22};
  const uint8_t b[] = {0x33, 0x44};
  feedData(0, a, sizeof(a));
  feedData(2, b, sizeof(b));  // seq 1 went missing
  tick();

  const CapturedFrame* err = sink->first(Rsp::Error);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::Seq), errorCode(err));
  TEST_ASSERT_EQUAL_UINT8(1, errorDetail(err));  // the seq we expected

  // The payload is still delivered — dropping it too would help nobody.
  TEST_ASSERT_EQUAL_size_t(4, backend->sent.size());

  // And the receiver resynchronised, so seq 3 is now clean.
  sink->clear();
  const uint8_t c[] = {0x55};
  feedData(3, c, sizeof(c));
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Error));
}

static void test_overrunning_the_window_is_reported(void) {
  openPort();
  uint8_t chunk[kMaxDataRaw];
  for (size_t i = 0; i < sizeof(chunk); ++i) chunk[i] = 0x5A;

  // Never poll, so nothing drains: the buffer fills and the excess is dropped
  // loudly rather than silently.
  for (int i = 0; i < 40; ++i)
    feedData(static_cast<uint8_t>(i & kSeqMask), chunk, sizeof(chunk));

  bool sawNoCredit = false;
  for (const auto& f : sink->frames)
    if (f.cmd == static_cast<uint8_t>(Rsp::Error) &&
        errorCode(&f) == static_cast<uint8_t>(Err::NoCredit))
      sawNoCredit = true;
  TEST_ASSERT_TRUE_MESSAGE(sawNoCredit, "expected ERR_NO_CREDIT on overflow");
}

static void test_bad_encoding_is_rejected(void) {
  openPort();
  // Hand-built DATA whose packed section over-claims its MSB byte.
  const uint8_t frame[] = {0xF0, 0x7D, 0x55, 0x01,
                           static_cast<uint8_t>(Cmd::Data),
                           0x00,        // seq
                           0x04, 0x41,  // MSB claims bit 2, only 1 byte follows
                           0xF7};
  br->onSysEx(frame, sizeof(frame), clockMs);

  const CapturedFrame* err = sink->first(Rsp::Error);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::BadEncoding), errorCode(err));
  TEST_ASSERT_EQUAL_size_t(0, backend->sent.size());
}

// ---- far end → host --------------------------------------------------------

static void test_backend_data_reaches_host(void) {
  openPort();
  std::vector<uint8_t> expect;
  for (int i = 0; i < 500; ++i) {
    const uint8_t b = static_cast<uint8_t>(i * 13 + 5);
    backend->incoming.push_back(b);
    expect.push_back(b);
  }
  tick();

  const std::vector<uint8_t> got = collectData();
  TEST_ASSERT_EQUAL_size_t(expect.size(), got.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expect.data(), got.data(), expect.size());
  TEST_ASSERT_EQUAL_UINT32(500, br->rxCount());
}

static void test_device_respects_its_send_window(void) {
  openPort(100);  // the host will only accept 100 bytes before granting more
  for (int i = 0; i < 400; ++i)
    backend->incoming.push_back(static_cast<uint8_t>(i));
  tick();

  TEST_ASSERT_EQUAL_size_t(100, collectData().size());

  sink->clear();
  feedCredit(150);
  tick();
  TEST_ASSERT_EQUAL_size_t(150, collectData(1).size());
}

static void test_zero_window_stops_the_device_entirely(void) {
  openPort(0);
  for (int i = 0; i < 64; ++i) backend->incoming.push_back(static_cast<uint8_t>(i));
  tick();
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Data));

  feedCredit(64);
  tick();
  TEST_ASSERT_EQUAL_size_t(64, collectData().size());
}

static void test_busy_usb_endpoint_costs_no_data(void) {
  openPort();
  for (int i = 0; i < 300; ++i)
    backend->incoming.push_back(static_cast<uint8_t>(i));

  sink->isReady = false;
  tick();
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Data));

  // Nothing was dropped while the endpoint was busy — it comes out intact.
  sink->isReady = true;
  tick();
  const std::vector<uint8_t> got = collectData();
  TEST_ASSERT_EQUAL_size_t(300, got.size());
  for (size_t i = 0; i < got.size(); ++i)
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(i), got[i]);
}

// ---- control ---------------------------------------------------------------

static void test_ping_echoes_its_cookie(void) {
  uint8_t buf[32];
  const uint8_t cookie[] = {0x01, 0x23, 0x45, 0x67};
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Ping);
  w.bytes(cookie, sizeof(cookie));
  br->onSysEx(buf, w.end(), clockMs);

  const CapturedFrame* pong = sink->first(Rsp::Pong);
  TEST_ASSERT_NOT_NULL(pong);
  FrameReader r;
  FrameReader::parse(pong->bytes.data(), pong->bytes.size(), r);
  uint8_t out[8];
  size_t len = 0;
  TEST_ASSERT_TRUE(r.rest(out, sizeof(out), len));
  TEST_ASSERT_EQUAL_size_t(sizeof(cookie), len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(cookie, out, len);
}

static void test_set_lines_applies_only_the_mask(void) {
  openPort();
  backend->outLines = kLineDtr;

  uint8_t buf[32];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::SetLines);
  w.u7(kLineRts);  // touch RTS only
  w.u7(kLineRts);
  br->onSysEx(buf, w.end(), clockMs);

  TEST_ASSERT_EQUAL_HEX8(kLineDtr | kLineRts, backend->outputLines());
}

static void test_input_line_change_raises_an_event(void) {
  openPort();
  backend->inLines = kLineCts;
  tick();

  const CapturedFrame* ev = sink->first(Rsp::Event);
  TEST_ASSERT_NOT_NULL(ev);
  FrameReader r;
  FrameReader::parse(ev->bytes.data(), ev->bytes.size(), r);
  uint8_t evt = 0, arg = 0;
  r.u7(evt);
  r.u7(arg);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Evt::Lines), evt);
  TEST_ASSERT_EQUAL_HEX8(kLineCts, arg);

  // A steady line does not keep re-announcing itself.
  sink->clear();
  tick();
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Event));
}

static void test_reopen_resets_sequences_and_buffers(void) {
  openPort();
  const uint8_t chunk[] = {1, 2, 3};
  feedData(0, chunk, sizeof(chunk));
  for (int i = 0; i < 50; ++i) backend->incoming.push_back(0x77);
  tick();
  TEST_ASSERT_TRUE(br->rxCount() > 0);

  openPort();
  TEST_ASSERT_EQUAL_UINT32(0, br->rxCount());
  TEST_ASSERT_EQUAL_UINT32(0, br->txCount());

  // Sequence numbers restart, in both directions.
  backend->incoming.push_back(0x99);
  tick();
  TEST_ASSERT_EQUAL_size_t(1, collectData(0).size());

  sink->clear();
  feedData(0, chunk, sizeof(chunk));
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Error));
}

static void test_reset_closes_the_port(void) {
  openPort();
  feedSimple(Cmd::Reset);
  TEST_ASSERT_EQUAL(PortState::Closed, br->state());
  TEST_ASSERT_FALSE(backend->isOpen());
  TEST_ASSERT_EQUAL_UINT32(0, br->rxCount());
}

static void test_flush_discards_the_requested_direction(void) {
  openPort();
  backend->txCapacity = 0;  // nothing can leave, so the buffer holds
  const uint8_t chunk[] = {1, 2, 3, 4};
  feedData(0, chunk, sizeof(chunk));
  tick();
  TEST_ASSERT_EQUAL_size_t(0, backend->sent.size());

  uint8_t buf[32];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Flush);
  w.u7(kFlushDiscardTx);
  br->onSysEx(buf, w.end(), clockMs);

  backend->txCapacity = 100;
  tick();
  TEST_ASSERT_EQUAL_size_t(0, backend->sent.size());  // discarded, not deferred
  TEST_ASSERT_EQUAL_HEX8(kFlushDiscardTx, backend->flushed);
}

static void test_unknown_command_is_reported(void) {
  openPort();
  const uint8_t frame[] = {0xF0, 0x7D, 0x55, 0x01, 0x3E, 0xF7};
  br->onSysEx(frame, sizeof(frame), clockMs);
  const CapturedFrame* err = sink->first(Rsp::Error);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::BadCmd), errorCode(err));
  TEST_ASSERT_EQUAL_UINT8(0x3E, errorDetail(err));
}

static void test_foreign_sysex_is_ignored_silently(void) {
  openPort();
  const uint8_t yamaha[] = {0xF0, 0x43, 0x10, 0x4C, 0xF7};
  br->onSysEx(yamaha, sizeof(yamaha), clockMs);
  TEST_ASSERT_EQUAL_size_t(0, sink->frames.size());
}

static void test_future_version_gets_one_error(void) {
  const uint8_t future[] = {0xF0, 0x7D, 0x55, 0x02, 0x01, 0xF7};
  br->onSysEx(future, sizeof(future), clockMs);
  const CapturedFrame* err = sink->first(Rsp::Error);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::Version), errorCode(err));
  TEST_ASSERT_EQUAL_UINT8(kProtocolVersion, errorDetail(err));
}

// A host that reconnects after an unclean exit finds the device holding frames
// addressed to the conversation that just died. On real hardware that buffer
// filled, ready() went false permanently, and the device could no longer
// answer anyone — it looked wedged. HELLO means "new conversation", so
// whatever is queued is stale by definition and must go before the reply.
static void test_hello_discards_stale_output(void) {
  openPort();
  for (int i = 0; i < 200; ++i) backend->incoming.push_back(0x5A);
  tick();
  TEST_ASSERT_TRUE(sink->count(Rsp::Data) > 0);

  uint8_t buf[32];
  FrameWriter w(buf, sizeof(buf));
  w.begin(Cmd::Hello);
  w.u14(kRxBufferSize);
  w.u14(kMaxDataRaw);
  br->onSysEx(buf, w.end(), clockMs);

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, sink->discards,
                                "HELLO must clear the outbound queue");
  // And the INFO reply is the first thing the new host sees, not a tail of
  // DATA frames belonging to somebody else's session.
  TEST_ASSERT_TRUE(sink->frames.size() > 0);
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(Rsp::Info), sink->frames[0].cmd);
}

static void test_reset_discards_stale_output(void) {
  openPort();
  for (int i = 0; i < 200; ++i) backend->incoming.push_back(0x5A);
  tick();

  feedSimple(Cmd::Reset);
  TEST_ASSERT_EQUAL_INT(1, sink->discards);
  TEST_ASSERT_TRUE(sink->frames.size() > 0);
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(Rsp::Status), sink->frames[0].cmd);
}

// ---- the session boundary (PROTOCOL.md §5.9) -------------------------------

// The device keeps delivering what it received before the port closed, and
// that delivery is credit-paced like any other. With CREDIT answered
// ERR_NOT_OPEN the drain stopped at whatever window happened to be left —
// here 64 of 300 bytes, with the other 236 stranded in a buffer nothing would
// ever empty, and an error frame the host had not asked for on top.
static void test_the_tail_of_a_closed_session_is_credited_out(void) {
  openPort(64);  // a window small enough that the tail needs several grants
  for (int i = 0; i < 300; ++i)
    backend->incoming.push_back(static_cast<uint8_t>(i));
  tick();
  TEST_ASSERT_EQUAL_size_t(64, collectData().size());

  sink->clear();
  feedSimple(Cmd::Close);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PortState::Closed),
                        static_cast<int>(br->state()));

  // The host reads what it was sent and hands the window back, port or no port.
  for (int round = 0; round < 10; ++round) {
    feedCredit(64);
    tick();
  }

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, sink->count(Rsp::Error),
                                "CREDIT on a closed port must not be an error");
  const std::vector<uint8_t> got = collectData(1);
  TEST_ASSERT_EQUAL_size_t(236, got.size());
  for (size_t i = 0; i < got.size(); ++i)
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(64 + i), got[i]);
}

// The other half of the same bargain. Delivery outlives the port, so OPEN has
// to be a hard boundary — and clearing toHost_ is not enough, because frames
// that have already been framed and queued are past that point. They would
// arrive after the host reset its own session, numbered for the old one:
// stale bytes injected into a new session, plus a sequence-gap warning.
static void test_open_discards_frames_left_over_from_the_previous_session(void) {
  openPort();
  for (int i = 0; i < 200; ++i) backend->incoming.push_back(0x5A);
  tick();
  TEST_ASSERT_TRUE(sink->count(Rsp::Data) > 0);
  feedSimple(Cmd::Close);

  sink->clear();
  sink->discards = 0;
  feed(buildOpen(115200, kRxBufferSize));
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, sink->discards,
                                "OPEN must clear the outbound queue");
  // And STATUS is the first thing the new session sees, not a tail of DATA
  // frames numbered for the old one.
  TEST_ASSERT_TRUE(sink->frames.size() > 0);
  TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(Rsp::Status), sink->frames[0].cmd);

  // A rejected OPEN is not a boundary: nothing changed, so nothing is thrown
  // away.
  sink->discards = 0;
  feed(buildOpen(2000000, kRxBufferSize));  // above the backend's maxBaud
  TEST_ASSERT_EQUAL_INT(0, sink->discards);
}

// ---- end to end ------------------------------------------------------------

static void test_full_duplex_bulk_transfer(void) {
  openPort();

  std::vector<uint8_t> toFar, fromFar;
  uint32_t lcg = 0xC0FFEEu;
  auto nextByte = [&]() {
    lcg = lcg * 1664525u + 1013904223u;
    return static_cast<uint8_t>(lcg >> 24);
  };

  const size_t kTotal = 4000;
  for (size_t i = 0; i < kTotal; ++i) {
    toFar.push_back(nextByte());
    fromFar.push_back(nextByte());
  }

  size_t sentIdx = 0, injectIdx = 0;
  uint8_t seq = 0;
  std::vector<uint8_t> received;
  uint16_t deviceCredit = kRxBufferSize;  // what the host is allowed to send

  for (int round = 0; round < 400; ++round) {
    sink->clear();

    // Host sends what its window allows.
    while (sentIdx < kTotal) {
      size_t chunk = kTotal - sentIdx;
      if (chunk > kMaxDataRaw) chunk = kMaxDataRaw;
      if (chunk > deviceCredit) break;
      feedData(seq, &toFar[sentIdx], chunk);
      seq = static_cast<uint8_t>((seq + 1) & kSeqMask);
      sentIdx += chunk;
      deviceCredit = static_cast<uint16_t>(deviceCredit - chunk);
    }

    // Far end trickles bytes in.
    for (int i = 0; i < 64 && injectIdx < kTotal; ++i)
      backend->incoming.push_back(fromFar[injectIdx++]);

    tick(2);

    // Host processes what came back: data out, credit in.
    for (const auto& f : sink->frames) {
      FrameReader r;
      FrameReader::parse(f.bytes.data(), f.bytes.size(), r);
      if (f.cmd == static_cast<uint8_t>(Rsp::Data)) {
        uint8_t s = 0;
        r.u7(s);
        uint8_t raw[kMaxDataRaw];
        size_t len = 0;
        TEST_ASSERT_TRUE(r.unpackRest(raw, sizeof(raw), len));
        received.insert(received.end(), raw, raw + len);
      } else if (f.cmd == static_cast<uint8_t>(Rsp::Credit)) {
        uint16_t d = 0;
        r.u14(d);
        deviceCredit = static_cast<uint16_t>(deviceCredit + d);
      } else if (f.cmd == static_cast<uint8_t>(Rsp::Error)) {
        TEST_FAIL_MESSAGE("bulk transfer produced a protocol error");
      }
    }

    // Keep the device's send window open.
    feedCredit(1024);

    if (sentIdx == kTotal && injectIdx == kTotal &&
        received.size() == kTotal && backend->sent.size() == kTotal)
      break;
  }

  TEST_ASSERT_EQUAL_size_t(kTotal, backend->sent.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(toFar.data(), backend->sent.data(), kTotal);
  TEST_ASSERT_EQUAL_size_t(kTotal, received.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(fromFar.data(), received.data(), kTotal);
}

// ---- hot-plug (phase 2) ----------------------------------------------------

// True if the device emitted the given event; the event's argument, if any,
// comes back through argOut.
static bool sawEvent(Evt evt, uint8_t* argOut = nullptr) {
  for (const auto& f : sink->frames) {
    if (f.cmd != static_cast<uint8_t>(Rsp::Event)) continue;
    FrameReader r;
    if (FrameReader::parse(f.bytes.data(), f.bytes.size(), r) !=
        FrameReader::Status::Ok)
      continue;
    uint8_t code = 0, arg = 0;
    if (!r.u7(code) || !r.u7(arg)) continue;
    if (code != static_cast<uint8_t>(evt)) continue;
    if (argOut) *argOut = arg;
    return true;
  }
  return false;
}

static void test_far_end_present_all_along_raises_no_event(void) {
  openPort();
  tick();
  tick();
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Event));
}

static void test_detach_faults_the_port_and_is_announced(void) {
  openPort();
  tick();
  sink->clear();

  backend->presentFlag = false;
  tick();

  uint8_t arg = 0xFF;
  TEST_ASSERT_TRUE(sawEvent(Evt::Detach, &arg));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BackendId::HardwareUart), arg);
  TEST_ASSERT_FALSE(backend->isOpen());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PortState::Fault),
                        static_cast<int>(br->state()));

  // And the port really is shut: data for a device that is gone is refused
  // rather than buffered for whatever gets plugged in next.
  sink->clear();
  const uint8_t payload[] = {0xAA};
  feedData(0, payload, sizeof(payload));
  const CapturedFrame* err = sink->first(Rsp::Error);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::NotOpen), err->bytes[5]);
}

static void test_detach_is_announced_once_not_every_poll(void) {
  openPort();
  backend->presentFlag = false;
  tick();
  tick();
  tick();
  TEST_ASSERT_EQUAL_INT(1, sink->count(Rsp::Event));
}

static void test_reattach_clears_the_fault_and_reopens(void) {
  openPort();
  backend->presentFlag = false;
  tick();
  sink->clear();

  backend->presentFlag = true;
  tick();

  uint8_t arg = 0xFF;
  TEST_ASSERT_TRUE(sawEvent(Evt::Attach, &arg));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(BackendId::HardwareUart), arg);
  // Attaching does not open a port — it only means there is one to open.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PortState::Closed),
                        static_cast<int>(br->state()));
  TEST_ASSERT_FALSE(backend->isOpen());

  openPort();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PortState::Open),
                        static_cast<int>(br->state()));
  TEST_ASSERT_TRUE(backend->isOpen());
}

static void test_presence_event_waits_for_a_free_endpoint(void) {
  openPort();
  tick();
  sink->clear();

  // USB busy at the moment the cable is pulled.
  sink->isReady = false;
  backend->presentFlag = false;
  tick();
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Event));
  // The port still faults immediately — only the announcement is deferred.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PortState::Fault),
                        static_cast<int>(br->state()));

  sink->isReady = true;
  tick();
  TEST_ASSERT_TRUE(sawEvent(Evt::Detach));
}

// Reads STATUS's trailing `present` field, skipping the seven that precede it.
static uint8_t statusPresent(const CapturedFrame* f) {
  FrameReader r;
  TEST_ASSERT_EQUAL(FrameReader::Status::Ok,
                    FrameReader::parse(f->bytes.data(), f->bytes.size(), r));
  uint8_t u = 0;
  uint16_t w = 0;
  uint32_t d = 0;
  TEST_ASSERT_TRUE(r.u7(u));   // state
  TEST_ASSERT_TRUE(r.u7(u));   // out_lines
  TEST_ASSERT_TRUE(r.u7(u));   // in_lines
  TEST_ASSERT_TRUE(r.u7(u));   // errflags
  TEST_ASSERT_TRUE(r.u21(d));  // rx_count
  TEST_ASSERT_TRUE(r.u21(d));  // tx_count
  TEST_ASSERT_TRUE(r.u14(w));  // credit
  uint8_t present = 0xFF;
  TEST_ASSERT_TRUE(r.u7(present));
  return present;
}

static void test_status_reports_whether_a_far_end_is_attached(void) {
  openPort();
  feedSimple(Cmd::GetStatus);
  TEST_ASSERT_EQUAL_UINT8(1, statusPresent(sink->last(Rsp::Status)));

  // Answered from the backend, not from what poll() has got around to
  // noticing — so this holds with no tick() in between.
  backend->presentFlag = false;
  sink->clear();
  feedSimple(Cmd::GetStatus);
  TEST_ASSERT_EQUAL_UINT8(0, statusPresent(sink->last(Rsp::Status)));
}

// Every EVENT the device emitted, in order, as (event, arg) pairs. sawEvent()
// above answers "did it happen"; these tests are about what happened first.
static std::vector<std::pair<uint8_t, uint8_t>> eventsInOrder() {
  std::vector<std::pair<uint8_t, uint8_t>> out;
  for (const auto& f : sink->frames) {
    if (f.cmd != static_cast<uint8_t>(Rsp::Event)) continue;
    FrameReader r;
    if (FrameReader::parse(f.bytes.data(), f.bytes.size(), r) !=
        FrameReader::Status::Ok)
      continue;
    uint8_t code = 0, arg = 0;
    if (!r.u7(code) || !r.u7(arg)) continue;
    out.emplace_back(code, arg);
  }
  return out;
}

// Moves the far end the way the real backend publishes it: level and change
// count together, so the engine sees an edge rather than inferring one.
static void setPresent(bool present) {
  if (backend->presentFlag == present) return;
  backend->presentFlag = present;
  ++backend->presenceChanges;
}

// The announcement is deferred when the endpoint is busy; a second transition
// arriving in that window must not overwrite the first. Detach-then-attach
// collapsing to a bare ATTACH would tell the host its port is fine when in
// fact it faulted and needs reopening.
static void test_a_deferred_detach_survives_a_later_attach(void) {
  openPort();
  tick();
  sink->clear();

  sink->isReady = false;
  setPresent(false);
  tick();
  setPresent(true);
  tick();
  TEST_ASSERT_EQUAL_INT(0, sink->count(Rsp::Event));

  sink->isReady = true;
  tick();

  const auto evts = eventsInOrder();
  TEST_ASSERT_EQUAL_size_t(2, evts.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Evt::Detach), evts[0].first);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Evt::Attach), evts[1].first);
  // The fault the detach caused is real whether or not it could be announced.
  TEST_ASSERT_FALSE(backend->isOpen());
}

// The level reads the same on both sides of the gap, so only the change count
// can say that the adapter now plugged in is not the one the port was opened
// on — and TinyUSB has already reset it to 115200 8N1 behind our back.
static void test_a_detach_and_reattach_between_polls_still_faults_the_port(void) {
  openPort();
  tick();
  sink->clear();
  TEST_ASSERT_TRUE(backend->isOpen());

  backend->presenceChanges += 2;  // out and back in, with nobody looking
  tick();

  TEST_ASSERT_FALSE_MESSAGE(backend->isOpen(),
                            "the port stayed open across a replug");
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(PortState::Open),
                        static_cast<int>(br->state()));

  const auto evts = eventsInOrder();
  TEST_ASSERT_EQUAL_size_t(2, evts.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Evt::Detach), evts[0].first);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Evt::Attach), evts[1].first);

  // And it is a fresh port: the host has to ask for it again, as after any
  // detach. PROTOCOL.md §5.8.
  const uint8_t payload[] = {0xAA};
  sink->clear();
  feedData(0, payload, sizeof(payload));
  const CapturedFrame* err = sink->first(Rsp::Error);
  TEST_ASSERT_NOT_NULL(err);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::NotOpen), errorCode(err));
}

// Three transitions with the endpoint busy throughout. The queue holds two, so
// the oldest goes — which still leaves the host with a detach it must react to
// and a final state that matches reality.
static void test_a_flapping_connector_leaves_the_newest_two_events(void) {
  openPort();
  tick();
  sink->clear();

  sink->isReady = false;
  setPresent(false);
  tick();
  setPresent(true);
  tick();
  setPresent(false);
  tick();

  sink->isReady = true;
  tick();

  const auto evts = eventsInOrder();
  TEST_ASSERT_EQUAL_size_t(2, evts.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Evt::Attach), evts[0].first);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Evt::Detach), evts[1].first);

  // Closed rather than Fault, and that is the right answer: the port faulted
  // on the first detach, the attach cleared it, and the second detach found
  // nothing open to take away. Fault is what happened to a port the host
  // asked for — DECISIONS.md D9.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PortState::Closed),
                        static_cast<int>(br->state()));
  feedSimple(Cmd::GetStatus);
  TEST_ASSERT_EQUAL_UINT8(0, statusPresent(sink->last(Rsp::Status)));
}

static void test_bytes_received_before_a_detach_still_reach_the_host(void) {
  openPort();
  const uint8_t fromFar[] = {0x01, 0x7F, 0x80, 0xFF};
  for (uint8_t b : fromFar) backend->incoming.push_back(b);

  // Unplugged in the same instant, before any poll has drained the backend.
  backend->presentFlag = false;
  tick();

  const std::vector<uint8_t> got = collectData();
  TEST_ASSERT_EQUAL_size_t(sizeof(fromFar), got.size());
  TEST_ASSERT_EQUAL_HEX8_ARRAY(fromFar, got.data(), sizeof(fromFar));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hello_returns_info);
  RUN_TEST(test_hello_caps_device_frame_size_to_host_limit);
  RUN_TEST(test_data_before_open_is_rejected);
  RUN_TEST(test_open_reports_status);
  RUN_TEST(test_open_rejects_excessive_baud);
  RUN_TEST(test_open_backend_failure_faults);
  RUN_TEST(test_host_data_reaches_backend);
  RUN_TEST(test_high_bytes_survive_the_tunnel);
  RUN_TEST(test_credit_returned_after_half_buffer_drains);
  RUN_TEST(test_credit_tail_is_returned_on_the_idle_timer);
  RUN_TEST(test_backend_backpressure_holds_data);
  RUN_TEST(test_sequence_gap_is_reported_but_data_kept);
  RUN_TEST(test_overrunning_the_window_is_reported);
  RUN_TEST(test_bad_encoding_is_rejected);
  RUN_TEST(test_backend_data_reaches_host);
  RUN_TEST(test_device_respects_its_send_window);
  RUN_TEST(test_zero_window_stops_the_device_entirely);
  RUN_TEST(test_busy_usb_endpoint_costs_no_data);
  RUN_TEST(test_ping_echoes_its_cookie);
  RUN_TEST(test_set_lines_applies_only_the_mask);
  RUN_TEST(test_input_line_change_raises_an_event);
  RUN_TEST(test_reopen_resets_sequences_and_buffers);
  RUN_TEST(test_reset_closes_the_port);
  RUN_TEST(test_flush_discards_the_requested_direction);
  RUN_TEST(test_unknown_command_is_reported);
  RUN_TEST(test_foreign_sysex_is_ignored_silently);
  RUN_TEST(test_future_version_gets_one_error);
  RUN_TEST(test_hello_discards_stale_output);
  RUN_TEST(test_reset_discards_stale_output);
  RUN_TEST(test_the_tail_of_a_closed_session_is_credited_out);
  RUN_TEST(test_open_discards_frames_left_over_from_the_previous_session);
  RUN_TEST(test_full_duplex_bulk_transfer);
  RUN_TEST(test_far_end_present_all_along_raises_no_event);
  RUN_TEST(test_detach_faults_the_port_and_is_announced);
  RUN_TEST(test_detach_is_announced_once_not_every_poll);
  RUN_TEST(test_reattach_clears_the_fault_and_reopens);
  RUN_TEST(test_presence_event_waits_for_a_free_endpoint);
  RUN_TEST(test_status_reports_whether_a_far_end_is_attached);
  RUN_TEST(test_a_deferred_detach_survives_a_later_attach);
  RUN_TEST(test_a_detach_and_reattach_between_polls_still_faults_the_port);
  RUN_TEST(test_a_flapping_connector_leaves_the_newest_two_events);
  RUN_TEST(test_bytes_received_before_a_detach_still_reach_the_host);
  return UNITY_END();
}
