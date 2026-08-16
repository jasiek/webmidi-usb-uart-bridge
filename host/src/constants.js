// Wire protocol constants — the host-side mirror of lib/bridge_proto/bridge_proto.h.
// PROTOCOL.md is the specification both are written against.
//
// Plain ES module with no Node built-ins, so a browser page using the Web MIDI
// API can import it unchanged. See DECISIONS.md D3.

export const SYSEX_START = 0xf0;
export const SYSEX_END = 0xf7;
export const MANUFACTURER_ID = 0x7d; // non-commercial / educational
export const MAGIC = 0x55; // 'U' — this product
export const PROTOCOL_VERSION = 0x01;

export const HEADER_LEN = 5; // F0 7D 55 01 <cmd>
export const FRAME_OVERHEAD = HEADER_LEN + 1;

export const MAX_DATA_RAW = 128;
export const RX_BUFFER_SIZE = 2048;
export const SEQ_MASK = 0x7f;

/** Host → device. */
export const Cmd = Object.freeze({
  HELLO: 0x01,
  OPEN: 0x02,
  CLOSE: 0x03,
  DATA: 0x04,
  SET_LINES: 0x05,
  FLUSH: 0x06,
  CREDIT: 0x07,
  PING: 0x08,
  GET_STATUS: 0x09,
  RESET: 0x0a,
});

/** Device → host. */
export const Rsp = Object.freeze({
  INFO: 0x41,
  STATUS: 0x42,
  DATA: 0x44,
  CREDIT: 0x47,
  PONG: 0x48,
  ERROR: 0x4e,
  EVENT: 0x4f,
});

export const Err = Object.freeze({
  VERSION: 0x01,
  BAD_CMD: 0x02,
  BAD_LENGTH: 0x03,
  BAD_ENCODING: 0x04,
  NOT_OPEN: 0x05,
  BAD_PARAM: 0x06,
  NO_CREDIT: 0x07,
  SEQ: 0x08,
  OVERFLOW: 0x09,
  BACKEND: 0x0a,
});

export const ERR_NAMES = Object.freeze({
  [Err.VERSION]: 'unsupported protocol version',
  [Err.BAD_CMD]: 'unknown or wrong-direction command',
  [Err.BAD_LENGTH]: 'payload length wrong for the command',
  [Err.BAD_ENCODING]: '7-in-8 unpacking failed',
  [Err.NOT_OPEN]: 'port is not open',
  [Err.BAD_PARAM]: 'unsupported parameter',
  [Err.NO_CREDIT]: 'sender exceeded its window — data was dropped',
  [Err.SEQ]: 'sequence gap',
  [Err.OVERFLOW]: 'receive buffer overran',
  [Err.BACKEND]: 'backend failure',
});

export const Evt = Object.freeze({
  LINES: 0x01,
  BREAK: 0x02,
  OVERRUN: 0x03,
  ATTACH: 0x04,
  DETACH: 0x05,
});

/** Output control lines (SET_LINES, STATUS.outLines). */
export const Line = Object.freeze({
  DTR: 1 << 0,
  RTS: 1 << 1,
  BREAK: 1 << 2,
});

/** Input control lines (STATUS.inLines, EVT_LINES). */
export const InLine = Object.freeze({
  CTS: 1 << 0,
  DSR: 1 << 1,
  DCD: 1 << 2,
  RI: 1 << 3,
});

export const Cap = Object.freeze({
  DTR: 1 << 0,
  RTS: 1 << 1,
  BREAK: 1 << 2,
  CTS: 1 << 3,
  DSR: 1 << 4,
  DCD: 1 << 5,
  RI: 1 << 6,
  FLOW_RTSCTS: 1 << 7,
  HOTPLUG: 1 << 8,
});

export const ErrFlag = Object.freeze({
  OVERRUN: 1 << 0,
  FRAMING: 1 << 1,
  PARITY: 1 << 2,
  BREAK: 1 << 3,
  HOST_OVERFLOW: 1 << 4,
});

export const Flush = Object.freeze({
  DRAIN_TX: 1 << 0,
  DISCARD_TX: 1 << 1,
  DISCARD_RX: 1 << 2,
});

export const PortState = Object.freeze({
  CLOSED: 0,
  OPEN: 1,
  FAULT: 2,
});

export const Parity = Object.freeze({
  NONE: 0,
  ODD: 1,
  EVEN: 2,
});

export const BackendId = Object.freeze({
  HARDWARE_UART: 0,
  PIO_USB_CDC: 1,
});

export const OPEN_FLAG_RTSCTS = 1 << 0;

/** Milliseconds of quiet before a below-threshold credit balance is returned. */
export const CREDIT_IDLE_MS = 10;
