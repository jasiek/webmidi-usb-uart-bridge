// Phase 1 backend: the hardware UART on GPIO0/GPIO1 (uart0).
//
// See DECISIONS.md D1 for why this exists before the PIO-USB CDC host.
#pragma once

#include <Arduino.h>

#include "backend.h"
#include "bridge_proto.h"

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

// uart0 on a 125 MHz clock divides down cleanly well past this; the limit is
// what the far end and the MIDI transport can actually keep up with.
constexpr uint32_t kMaxBaud = 921600;

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
