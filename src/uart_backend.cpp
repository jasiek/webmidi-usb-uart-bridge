#include "uart_backend.h"

#include <hardware/uart.h>

namespace bridge {

// Serial1 is HW UART 0 (SerialUART.h). We drive TX through the SDK directly,
// so we need the same instance the Arduino layer configured.
static uart_inst_t* const kUart = uart0;

// The PL011 in the RP2040 has 32-byte TX and RX FIFOs. `writable()` only has
// to return a non-zero upper bound — write() reports what actually fit, and
// Bridge::pumpToBackend handles a short write.
constexpr size_t kHwFifoDepth = 32;

static uint16_t configWord(const PortConfig& cfg) {
  uint16_t v = 0;
  switch (cfg.databits) {
    case 5: v |= SERIAL_DATA_5; break;
    case 6: v |= SERIAL_DATA_6; break;
    case 7: v |= SERIAL_DATA_7; break;
    default: v |= SERIAL_DATA_8; break;
  }
  switch (cfg.parity) {
    case Parity::Odd: v |= SERIAL_PARITY_ODD; break;
    case Parity::Even: v |= SERIAL_PARITY_EVEN; break;
    default: v |= SERIAL_PARITY_NONE; break;
  }
  v |= (cfg.stopbits == 2) ? SERIAL_STOP_BIT_2 : SERIAL_STOP_BIT_1;
  return v;
}

uint16_t UartBackend::caps() const {
  // Deliberately narrow, and honest about it. On this backend RTS and CTS are
  // wired to the UART's own hardware flow control (SerialUART calls
  // uart_set_hw_flow), so they are not lines the host can drive or read —
  // only a mode it can switch on. DTR/DSR/DCD/RI have no pin at all.
  return kCapBreak | kCapFlowRtsCts;
}

bool UartBackend::open(const PortConfig& cfg) {
  if (open_) close();

  Serial1.setTX(kPinTx);
  Serial1.setRX(kPinRx);
  // The core's default 32-byte software FIFO is under 3 ms of slack at 115200
  // baud — enough to lose bytes any time a USB interrupt runs long.
  Serial1.setFIFOSize(kUartFifoSize);

  if (cfg.flags & kOpenFlagRtsCts) {
    Serial1.setRTS(kPinRts);
    Serial1.setCTS(kPinCts);
  } else {
    // Must be cleared explicitly: the setting survives a previous open().
    Serial1.setRTS(UART_PIN_NOT_DEFINED);
    Serial1.setCTS(UART_PIN_NOT_DEFINED);
  }

  Serial1.begin(cfg.baud, configWord(cfg));
  if (!Serial1) return false;

  cfg_ = cfg;
  open_ = true;
  outLines_ = 0;
  return true;
}

void UartBackend::close() {
  if (!open_) return;
  uart_set_break(kUart, false);
  Serial1.end();
  open_ = false;
  outLines_ = 0;
}

size_t UartBackend::writable() const {
  if (!open_) return 0;
  return uart_is_writable(kUart) ? kHwFifoDepth : 0;
}

size_t UartBackend::write(const uint8_t* p, size_t n) {
  if (!open_) return 0;
  // Not Serial1.write(): that calls uart_putc_raw, which spins until the FIFO
  // drains. Blocking here would stall the USB service loop and cost us MIDI
  // packets — the whole point of the credit window is to never need to block.
  size_t i = 0;
  while (i < n && uart_is_writable(kUart)) {
    uart_get_hw(kUart)->dr = p[i++];
  }
  return i;
}

size_t UartBackend::readable() const {
  if (!open_) return 0;
  return static_cast<size_t>(Serial1.available());
}

size_t UartBackend::read(uint8_t* p, size_t n) {
  if (!open_) return 0;
  size_t i = 0;
  while (i < n && Serial1.available() > 0) {
    const int c = Serial1.read();
    if (c < 0) break;
    p[i++] = static_cast<uint8_t>(c);
  }
  return i;
}

void UartBackend::flush(uint8_t what) {
  if (!open_) return;
  if (what & kFlushDiscardRx) {
    while (Serial1.available() > 0) (void)Serial1.read();
  }
  if (what & kFlushDrainTx) {
    // Blocks until the hardware FIFO empties — at most 32 bytes, so under
    // 3 ms at 115200. Acceptable for an explicit, host-requested drain.
    Serial1.flush();
  }
}

void UartBackend::setLines(uint8_t mask, uint8_t values) {
  if (!open_) return;
  outLines_ = static_cast<uint8_t>((outLines_ & ~mask) | (values & mask));
  if (mask & kLineBreak) uart_set_break(kUart, (values & kLineBreak) != 0);
  // DTR and RTS are accepted and recorded but have no pin on this backend;
  // caps() already tells the host not to expect them to do anything.
}

uint8_t UartBackend::inputLines() const {
  return 0;  // no readable modem lines on a bare UART
}

uint8_t UartBackend::takeErrorFlags() {
  if (!open_) return 0;
  uint8_t flags = 0;
  if (Serial1.overflow()) flags |= kErrFlagOverrun;
  if (Serial1.getBreakReceived()) flags |= kErrFlagBreak;
  // Framing and parity errors are not reportable here: the core's UART ISR
  // discards those characters without recording anything. See FINDINGS.md.
  return flags;
}

}  // namespace bridge
