// Phase 1 backend: the hardware UART on GPIO0/GPIO1 (uart0).
//
// See DECISIONS.md D1 for why this exists before the PIO-USB CDC host.
#pragma once

#include <Arduino.h>

#include "backend.h"
#include "bridge_proto.h"

// Trace points inside open()/close(). These calls reach into the Arduino UART
// layer, which takes blocking mutexes and can reset the peripheral — if one of
// them never returns, the last trace printed is the one that hung. A
// once-a-second status line cannot show that, because the hang happens between
// two of them.
#ifdef BRIDGE_DEBUG
#include <Adafruit_TinyUSB.h>
#define BRIDGE_TRACE(msg)                                            \
  do {                                                               \
    if (SerialTinyUSB && SerialTinyUSB.availableForWrite() > 32) {   \
      SerialTinyUSB.printf("[trace] %s\r\n", msg);                   \
      SerialTinyUSB.flush();                                         \
    }                                                                \
  } while (0)
#else
#define BRIDGE_TRACE(msg) do { } while (0)
#endif

namespace bridge {

// Pin assignment. GPIO0/1 are uart0's default pins on the Pico.
constexpr int kPinTx = 0;
constexpr int kPinRx = 1;

// Optional hardware flow-control pins. Set to -1 when the board is built
// without them, which is the default; RTS/CTS then report as unsupported in
// INFO.caps rather than pretending to work.
constexpr int kPinRts = 2;
constexpr int kPinCts = 3;

// The Arduino layer's default FIFO is 32 bytes, which at 115200 baud is under
// 3 ms of slack — enough to lose bytes whenever a USB interrupt runs long.
constexpr size_t kUartFifoSize = 1024;

// Not what uart0 can clock — what the *tunnel* can carry, which is the number
// a host actually needs when choosing a safe rate.
//
// Measured on a Pico 1 over CoreMIDI (host/bin/throughput.js): the SysEx tunnel
// sustains ~53 kB/s one-way and ~48 kB/s in each direction concurrently. Line
// rate at 460800 8N1 is 45 kB/s, which fits; 921600 needs 90 kB/s and does not,
// so the device would silently drop whatever the far end sent beyond its
// capacity. Advertising 921600 because the UART divisor supports it was a
// promise the bridge could not keep.
//
// 460800 is the highest standard rate that fits, and it passed full-duplex
// loopback with zero loss. Its margin is thin though — a far end that
// transmits independently, rather than echoing what we send, has nothing
// throttling it. 230400 and below have comfortable headroom. See FINDINGS.md.
constexpr uint32_t kMaxBaud = 460800;

class UartBackend : public Backend {
 public:
  BackendId id() const override { return BackendId::HardwareUart; }
  uint16_t caps() const override;
  uint32_t maxBaud() const override { return kMaxBaud; }

  bool open(const PortConfig& cfg) override;
  void close() override;
  bool isOpen() const override { return open_; }

  size_t writable() const override;
  size_t write(const uint8_t* p, size_t n) override;

  size_t readable() const override;
  size_t read(uint8_t* p, size_t n) override;

  void flush(uint8_t what) override;

  void setLines(uint8_t mask, uint8_t values) override;
  uint8_t outputLines() const override { return outLines_; }
  uint8_t inputLines() const override;

  uint8_t takeErrorFlags() override;

 private:
  bool open_ = false;
  uint8_t outLines_ = 0;
  PortConfig cfg_;
};

}  // namespace bridge
