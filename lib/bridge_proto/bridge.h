// The protocol engine: SysEx in, SysEx out, bytes to and from a Backend.
//
// Deliberately free of Arduino, TinyUSB and RP2040 dependencies — time arrives
// as a parameter to poll(), the far end is a Backend, and output goes to a
// FrameSink. That is what lets test/test_bridge exercise the flow control and
// error paths on the host with no hardware attached.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "backend.h"
#include "bridge_proto.h"
#include "credit.h"
#include "frame.h"
#include "ringbuf.h"
#include "sysex7.h"

namespace bridge {

constexpr size_t kMaxFrameLen =
    kFrameOverhead + 1 + sysex7::packedLen(kMaxDataRaw);

// One slot is reserved to tell full from empty, so 4096 comfortably holds the
// 2048-byte window we advertise. Overflowing is then a protocol violation by
// the peer rather than something we can cause ourselves.
constexpr size_t kBufferSlots = 4096;

struct Firmware {
  uint8_t major = 0;
  uint8_t minor = 1;
  uint8_t patch = 0;
};

class Bridge {
 public:
  Bridge(Backend& backend, FrameSink& sink) : backend_(backend), sink_(sink) {}

  void begin(uint32_t nowMs);

  // Feed one complete SysEx message, F0 … F7 inclusive.
  void onSysEx(const uint8_t* msg, size_t n, uint32_t nowMs);

  // Pump both directions. Call as often as convenient.
  void poll(uint32_t nowMs);

  PortState state() const { return state_; }
  uint32_t rxCount() const { return rxCount_; }
  uint32_t txCount() const { return txCount_; }

 private:
  // --- command handlers ---
  void handleHello(FrameReader& r);
  void handleOpen(FrameReader& r, uint32_t nowMs);
  void handleClose();
  void handleData(FrameReader& r);
  void handleSetLines(FrameReader& r);
  void handleFlush(FrameReader& r);
  void handleCredit(FrameReader& r);
  void handlePing(FrameReader& r);
  void handleReset(uint32_t nowMs);

  // --- outbound frames ---
  void sendInfo();
  void sendStatus();
  void sendCredit(uint16_t delta);
  void sendError(Err code, uint8_t detail = 0);
  void sendEvent(Evt evt, uint8_t arg);
  bool sendDataChunk();

  // --- pumps ---
  void pumpToBackend();
  void pumpFromBackend();
  void pumpLines();

  void resetSession(uint32_t nowMs);

  Backend& backend_;
  FrameSink& sink_;

  PortState state_ = PortState::Closed;
  PortConfig cfg_;
  Firmware fw_;

  // Host → backend. We are the receiver, so we return credit as it drains.
  RingBuf<kBufferSlots> toBackend_;
  RecvWindow recvWin_;
  uint8_t rxSeq_ = 0;  // next Cmd::Data sequence we expect

  // Backend → host. We are the sender, so the host grants us credit.
  RingBuf<kBufferSlots> toHost_;
  SendWindow sendWin_;
  uint8_t txSeq_ = 0;  // next Rsp::Data sequence we will emit

  // Host capabilities, learned from HELLO; defaults are the protocol floor.
  uint16_t hostRxBuffer_ = kRxBufferSize;
  uint16_t hostMaxRaw_ = kMaxDataRaw;

  uint8_t lastInputLines_ = 0;
  uint8_t errFlags_ = 0;
  uint32_t rxCount_ = 0;
  uint32_t txCount_ = 0;
  uint32_t nowMs_ = 0;

  uint8_t frameBuf_[kMaxFrameLen];
  uint8_t scratch_[kMaxDataRaw];
};

}  // namespace bridge
