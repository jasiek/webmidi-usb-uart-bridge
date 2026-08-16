// Wire protocol constants — the single source of truth for the firmware side.
// Mirrored by host/src/constants.js; PROTOCOL.md is the specification both
// implementations are written against.
//
// This header is deliberately free of Arduino and RP2040 dependencies so that
// lib/bridge_proto can be compiled and unit-tested on the host (env:native).
#pragma once

#include <stddef.h>
#include <stdint.h>

// Progress marker for instrumented firmware builds. The firmware stores the
// code somewhere that survives a watchdog reset, so a hang can be located even
// though it takes the CPU with it. Compiled out of every other build, so the
// portable library stays free of side effects.
#ifdef BRIDGE_TRACE_PHASE
extern "C" void bridgePhase(unsigned code);
#define BRIDGE_PHASE(c) bridgePhase(c)
#else
#define BRIDGE_PHASE(c) \
  do {                  \
  } while (0)
#endif

namespace bridge {

// ---- framing ---------------------------------------------------------------

constexpr uint8_t kSysExStart = 0xF0;
constexpr uint8_t kSysExEnd = 0xF7;
constexpr uint8_t kManufacturerId = 0x7D;  // non-commercial / educational
constexpr uint8_t kMagic = 0x55;           // 'U' — this product
constexpr uint8_t kProtocolVersion = 0x01;

// F0 7D 55 01 <cmd> … F7
constexpr size_t kHeaderLen = 5;
constexpr size_t kFrameOverhead = kHeaderLen + 1;

// Largest raw (pre-packing) payload one DATA message may carry. PROTOCOL.md §1.1.
constexpr size_t kMaxDataRaw = 128;

// Receive window each side advertises, in raw bytes. Generous because the
// RP2040 has 264 KB of SRAM and a stalled window costs more than the memory.
constexpr uint16_t kRxBufferSize = 2048;

// ---- commands --------------------------------------------------------------

// Host → device, 0x01–0x3F.
enum class Cmd : uint8_t {
  Hello = 0x01,
  Open = 0x02,
  Close = 0x03,
  Data = 0x04,
  SetLines = 0x05,
  Flush = 0x06,
  Credit = 0x07,
  Ping = 0x08,
  GetStatus = 0x09,
  Reset = 0x0A,
};

// Device → host, 0x41–0x7F.
enum class Rsp : uint8_t {
  Info = 0x41,
  Status = 0x42,
  Data = 0x44,
  Credit = 0x47,
  Pong = 0x48,
  Error = 0x4E,
  Event = 0x4F,
};

// ---- errors, events, capabilities ------------------------------------------

enum class Err : uint8_t {
  Version = 0x01,
  BadCmd = 0x02,
  BadLength = 0x03,
  BadEncoding = 0x04,
  NotOpen = 0x05,
  BadParam = 0x06,
  NoCredit = 0x07,
  Seq = 0x08,
  Overflow = 0x09,
  Backend = 0x0A,
};

enum class Evt : uint8_t {
  Lines = 0x01,
  Break = 0x02,
  Overrun = 0x03,
  Attach = 0x04,
  Detach = 0x05,
};

// Output control lines (SET_LINES, STATUS.out_lines).
constexpr uint8_t kLineDtr = 1u << 0;
constexpr uint8_t kLineRts = 1u << 1;
constexpr uint8_t kLineBreak = 1u << 2;

// Input control lines (STATUS.in_lines, EVT_LINES).
constexpr uint8_t kLineCts = 1u << 0;
constexpr uint8_t kLineDsr = 1u << 1;
constexpr uint8_t kLineDcd = 1u << 2;
constexpr uint8_t kLineRi = 1u << 3;

// Capability bits (INFO.caps).
constexpr uint16_t kCapDtr = 1u << 0;
constexpr uint16_t kCapRts = 1u << 1;
constexpr uint16_t kCapBreak = 1u << 2;
constexpr uint16_t kCapCts = 1u << 3;
constexpr uint16_t kCapDsr = 1u << 4;
constexpr uint16_t kCapDcd = 1u << 5;
constexpr uint16_t kCapRi = 1u << 6;
constexpr uint16_t kCapFlowRtsCts = 1u << 7;
constexpr uint16_t kCapHotplug = 1u << 8;

// Sticky error flags (STATUS.errflags), cleared when read.
constexpr uint8_t kErrFlagOverrun = 1u << 0;
constexpr uint8_t kErrFlagFraming = 1u << 1;
constexpr uint8_t kErrFlagParity = 1u << 2;
constexpr uint8_t kErrFlagBreak = 1u << 3;
constexpr uint8_t kErrFlagHostOverflow = 1u << 4;

// FLUSH bits.
constexpr uint8_t kFlushDrainTx = 1u << 0;
constexpr uint8_t kFlushDiscardTx = 1u << 1;
constexpr uint8_t kFlushDiscardRx = 1u << 2;

enum class PortState : uint8_t { Closed = 0, Open = 1, Fault = 2 };

enum class BackendId : uint8_t { HardwareUart = 0, PioUsbCdc = 1 };

// ---- port configuration ----------------------------------------------------

enum class Parity : uint8_t { None = 0, Odd = 1, Even = 2 };

constexpr uint8_t kOpenFlagRtsCts = 1u << 0;

struct PortConfig {
  uint32_t baud = 115200;
  uint8_t databits = 8;
  Parity parity = Parity::None;
  uint8_t stopbits = 1;
  uint8_t flags = 0;
};

// Sequence numbers are 7-bit and wrap.
constexpr uint8_t kSeqMask = 0x7F;

}  // namespace bridge
