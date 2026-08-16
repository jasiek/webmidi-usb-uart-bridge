# Wire protocol v1

A bidirectional byte pipe (plus serial control lines) tunnelled through USB MIDI
1.0 System Exclusive messages.

Everything in this document is normative for both the firmware
(`src/`, `lib/bridge_proto/`) and the host library (`host/src/`). The two
implementations are independent; the round-trip tests in `host/test/` are what
keep them honest.

## 1. Framing

Every message is exactly one SysEx message:

```
F0  7D  55  01  <cmd>  <payload …>  F7
│   │   │   │   │      │            └─ SysEx end
│   │   │   │   │      └────────────── 0…n bytes, each 0x00–0x7F
│   │   │   │   └───────────────────── command byte, 0x00–0x7F
│   │   │   └───────────────────────── protocol version, 1
│   │   └───────────────────────────── magic 'U' — this product
│   └───────────────────────────────── 0x7D, non-commercial manufacturer ID
└───────────────────────────────────── SysEx start
```

`0x7D` is the ID the MIDI Association reserves for non-commercial, educational
and private use. It must never appear in a shipped commercial product, which is
exactly right for this one. The `55 01` that follows distinguishes us from any
other `7D` device sharing the bus, and lets a receiver reject a future protocol
version without having to parse the body.

A receiver **must** silently ignore any SysEx message that does not begin
`F0 7D 55 01`; other devices on the same virtual MIDI bus are none of our
business. A message that begins `F0 7D 55` with a version other than `01`
**must** be answered with `ERROR(ERR_VERSION)` once, and then ignored.

Non-SysEx MIDI (notes, clock, active sensing) is ignored in both directions.

### 1.1 Frame size

`MAX_DATA_RAW = 128` — the largest raw payload one `DATA` message may carry.
That packs to 147 bytes (§2), giving a worst-case SysEx message of 154 bytes,
or 52 USB-MIDI packets. Both sides advertise their real limit in `HELLO`/`INFO`
and must honour the other side's; 128 is the floor every implementation
supports.

## 2. 7-in-8 MSB packing

SysEx payload bytes may not have bit 7 set, so raw 8-bit data is packed.

Input is split into groups of 7 bytes; the final group may be shorter (1–7).
Each group of `k` bytes (`1 ≤ k ≤ 7`) becomes `k + 1` transmitted bytes:

```
raw:     b0        b1        b2       …  b(k-1)
packed:  M  (b0&0x7F)  (b1&0x7F)  (b2&0x7F)  …  (b(k-1)&0x7F)

M = Σ  ((bi >> 7) & 1) << i        for i in 0 … k-1
```

That is: **bit `i` of the MSB byte carries bit 7 of the `i`-th byte of the
group**, LSB-first. Bits of `M` above `k-1` are zero.

An empty input packs to zero bytes. `packed_len(n) = n + ceil(n / 7)`.

Worked example — `0xFF 0x00 0x80 0x7F`:

```
M = 1<<0 | 0<<1 | 1<<2 | 0<<3 = 0x05
packed = 05 7F 00 00 7F
```

The unpacker must reject a group whose MSB byte has bits set above the number
of data bytes actually present, and any input byte with bit 7 set — both mean
the stream is corrupt, not merely truncated.

## 3. Integer encoding

Multi-byte integers are little-endian septets: the low 7 bits first.

| Name  | Bytes | Range        |
| ----- | ----- | ------------ |
| `u7`  | 1     | 0 … 127      |
| `u14` | 2     | 0 … 16383    |
| `u21` | 3     | 0 … 2097151  |
| `u32` | 5     | 0 … 2³²−1    |

`u32` uses five septets (35 bits available, top 3 unused and must be zero).
Baud rates are `u32`; counters are `u21` and wrap.

## 4. Commands

Command bytes are split by direction to make a captured dump unambiguous:
host→device commands are `0x01–0x3F`, device→host are `0x41–0x7F`.

### 4.1 Host → device

| Code | Name        | Payload                                            |
| ---- | ----------- | -------------------------------------------------- |
| 0x01 | `HELLO`     | `u14` host RX buffer size, `u14` host max raw data |
| 0x02 | `OPEN`      | see §5.1                                            |
| 0x03 | `CLOSE`     | —                                                   |
| 0x04 | `DATA`      | `u7` seq, packed payload                            |
| 0x05 | `SET_LINES` | `u7` mask, `u7` values (§5.3)                       |
| 0x06 | `FLUSH`     | `u7` what: bit0 drain TX, bit1 discard TX, bit2 discard RX |
| 0x07 | `CREDIT`    | `u14` bytes freed since last `CREDIT`               |
| 0x08 | `PING`      | 0–8 opaque bytes, echoed verbatim                   |
| 0x09 | `GET_STATUS`| —                                                   |
| 0x0A | `RESET`     | —                                                   |

### 4.2 Device → host

| Code | Name      | Payload                                     |
| ---- | --------- | ------------------------------------------- |
| 0x41 | `INFO`    | see §5.2 — reply to `HELLO`                 |
| 0x42 | `STATUS`  | see §5.4 — reply to `GET_STATUS`, and sent unsolicited on state change |
| 0x44 | `DATA`    | `u7` seq, packed payload                    |
| 0x47 | `CREDIT`  | `u14` bytes freed since last `CREDIT`       |
| 0x48 | `PONG`    | the `PING` payload, verbatim                |
| 0x4E | `ERROR`   | `u7` code, `u7` detail (§5.5)               |
| 0x4F | `EVENT`   | `u7` event, `u7` arg (§5.6)                 |

`RESET` and `HELLO` are always legal. Any other host→device command received
while the port is closed is answered with `ERROR(ERR_NOT_OPEN)`.

## 5. Payload formats

### 5.1 `OPEN`

| Field       | Type  | Meaning                                              |
| ----------- | ----- | ---------------------------------------------------- |
| `baud`      | `u32` | bits per second, e.g. 115200                         |
| `databits`  | `u7`  | 5, 6, 7 or 8                                         |
| `parity`    | `u7`  | 0 none, 1 odd, 2 even                                |
| `stopbits`  | `u7`  | 1 or 2                                               |
| `flags`     | `u7`  | bit0 RTS/CTS hardware flow control on the far side   |
| `rx_buffer` | `u14` | host's receive window, in raw bytes                  |

The device replies `STATUS` on success, `ERROR` otherwise. Opening an already
open port re-configures it and resets both credit windows and both sequence
counters — it is the idempotent way to get back to a known state.

### 5.2 `INFO`

| Field         | Type  | Meaning                                          |
| ------------- | ----- | ------------------------------------------------ |
| `proto`       | `u7`  | protocol version, 1                              |
| `fw_major`    | `u7`  |                                                  |
| `fw_minor`    | `u7`  |                                                  |
| `fw_patch`    | `u7`  |                                                  |
| `backend`     | `u7`  | 0 hardware UART, 1 PIO-USB CDC                   |
| `caps`        | `u14` | capability bits, §5.7                            |
| `max_raw`     | `u14` | largest raw `DATA` payload the device accepts    |
| `rx_buffer`   | `u14` | device receive window, in raw bytes              |
| `max_baud`    | `u32` | highest baud the backend will accept             |

### 5.3 Control lines

One bitmask layout is used for both directions.

Output lines (`SET_LINES`, and `out_lines` in `STATUS`):

| Bit | Line    |
| --- | ------- |
| 0   | DTR     |
| 1   | RTS     |
| 2   | BREAK   |

Input lines (`in_lines` in `STATUS`, and `EVT_LINES`):

| Bit | Line |
| --- | ---- |
| 0   | CTS  |
| 1   | DSR  |
| 2   | DCD  |
| 3   | RI   |

`SET_LINES` changes only the lines set in `mask`. Setting a line the backend
does not implement is not an error — it is a no-op, and `caps` says which are
real. On the hardware-UART backend, DTR/DSR/DCD/RI have no pin and are always
no-ops; RTS/CTS exist only if the board is built with the flow-control pins
populated.

### 5.4 `STATUS`

| Field       | Type  | Meaning                                       |
| ----------- | ----- | --------------------------------------------- |
| `state`     | `u7`  | 0 closed, 1 open, 2 fault                     |
| `out_lines` | `u7`  | §5.3                                          |
| `in_lines`  | `u7`  | §5.3                                          |
| `errflags`  | `u7`  | sticky, cleared by reading: bit0 UART overrun, bit1 framing, bit2 parity, bit3 break seen, bit4 host-side overflow |
| `rx_count`  | `u21` | bytes received from the far end since `OPEN`  |
| `tx_count`  | `u21` | bytes sent to the far end since `OPEN`        |
| `credit`    | `u14` | credit the device currently has for host→device data |
| `present`   | `u7`  | 1 if a far end is attached, 0 if not (§5.8)   |

`present` was added after the first release. A host that finds the payload ends
before it must treat the far end as attached: that is the correct answer for
every backend that cannot be unplugged, which is the only kind that existed
when the field did not.

### 5.5 Error codes

| Code | Name                | Meaning                                        |
| ---- | ------------------- | ---------------------------------------------- |
| 0x01 | `ERR_VERSION`       | unsupported protocol version                   |
| 0x02 | `ERR_BAD_CMD`       | unknown or wrong-direction command             |
| 0x03 | `ERR_BAD_LENGTH`    | payload too short or too long for the command  |
| 0x04 | `ERR_BAD_ENCODING`  | 7-in-8 unpacking failed                        |
| 0x05 | `ERR_NOT_OPEN`      | command requires an open port                  |
| 0x06 | `ERR_BAD_PARAM`     | e.g. unsupported baud; `detail` = field index  |
| 0x07 | `ERR_NO_CREDIT`     | sender exceeded its window — data was dropped  |
| 0x08 | `ERR_SEQ`           | sequence gap; `detail` = expected seq          |
| 0x09 | `ERR_OVERFLOW`      | receive buffer overran despite credit          |
| 0x0A | `ERR_BACKEND`       | backend-specific failure; `detail` = sub-code  |

### 5.6 Events

| Code | Name           | Arg                                  |
| ---- | -------------- | ------------------------------------ |
| 0x01 | `EVT_LINES`    | new input-line bitmask (§5.3)        |
| 0x02 | `EVT_BREAK`    | 0                                    |
| 0x03 | `EVT_OVERRUN`  | 0                                    |
| 0x04 | `EVT_ATTACH`   | backend id — far end appeared        |
| 0x05 | `EVT_DETACH`   | backend id — far end went away       |

### 5.7 Capability bits

| Bit | Meaning                          |
| --- | -------------------------------- |
| 0   | DTR settable                     |
| 1   | RTS settable                     |
| 2   | BREAK settable                   |
| 3   | CTS readable                     |
| 4   | DSR readable                     |
| 5   | DCD readable                     |
| 6   | RI readable                      |
| 7   | hardware RTS/CTS flow control    |
| 8   | hot-plug (`EVT_ATTACH`/`DETACH`) |

### 5.8 Hot-plug

A backend whose far end can be physically removed — the PIO-USB CDC host, and
nothing else so far — sets capability bit 8. Such a device:

- emits `EVT_ATTACH` when a far end appears and `EVT_DETACH` when one goes
  away, each carrying the backend id as its argument;
- reports the current answer in `STATUS.present`, so a host that connects
  while a device is already attached does not have to infer it from silence;
- on detach, closes the port, discards anything still queued toward the far
  end, and moves to `state` = fault. Bytes already received from the far end
  are still delivered — a detach does not un-receive them.

Attaching does **not** open a port. It means there is one to open: the host
must send `OPEN` as it would have on connecting, which is also what re-arms
the port after the fault a detach leaves behind.

A device that does not set bit 8 never emits either event and always reports
`present` = 1.

## 6. Flow control

Each direction has an independent credit window, denominated in **raw**
(pre-packing) payload bytes.

1. At `OPEN`, each side's advertised `rx_buffer` becomes the *other* side's
   initial credit. Both `DATA` sequence counters reset to 0.
2. A sender may emit `DATA` only while `raw_length ≤ credit`, and subtracts
   `raw_length` from its credit.
3. A receiver adds arriving bytes to its buffer, and as that buffer drains
   (into the UART, or into the application) it accumulates a count of freed
   bytes.
4. The receiver emits `CREDIT(freed)` when `freed ≥ rx_buffer / 2`, or when
   `freed > 0` and 10 ms have passed with nothing sent. The sender adds the
   value to its credit.

Credits are **cumulative deltas**, not absolute levels: a lost `CREDIT` message
permanently shrinks the window rather than corrupting it. That is the safe
failure direction — the link stalls visibly instead of overflowing silently —
and the sequence numbers in §7 make the underlying loss loud.

A sender that ignores its window will have the excess dropped, and gets
`ERROR(ERR_NO_CREDIT)` with `detail` = bytes dropped, saturating at 127.

The half-buffer threshold is a deliberate trade: returning credit per-byte
would flood the MIDI bus with `CREDIT` messages and starve the data direction,
while waiting for the buffer to empty completely would stall the sender on
every round trip. The 10 ms timer covers the tail of a transfer, where `freed`
never reaches the threshold again.

## 7. Sequence numbers

Every `DATA` carries a `u7` sequence number, incrementing mod 128 per
direction, reset by `OPEN` and `RESET`. A receiver seeing anything other than
the expected value emits `ERROR(ERR_SEQ)` with `detail` = the value it
expected, then accepts the frame and resynchronises to `seq + 1`.

USB bulk transfers are reliable, so in-order delivery is the norm; this exists
because CoreMIDI, iOS and any USB-MIDI hub in between will drop whole messages
when their own buffers overflow, and a serial tunnel that loses bytes without
saying so is worse than one that fails.

## 8. Throughput

At 115200 8N1 the far end moves 11 520 bytes/s. Per direction that costs:

- 7-in-8 packing: ×8/7
- framing: 6 bytes per message, amortised over 128 raw bytes
- USB-MIDI: every 3 SysEx bytes ride in a 4-byte packet, ×4/3

so ≈ 11 520 × (8/7) × (154/153) × (4/3) ≈ **17.7 kB/s** of USB traffic per
direction, against a 12 Mbit/s full-speed bus. The bus is not the constraint;
per-message overhead in CoreMIDI and iOS is the thing to watch, which is why
`MAX_DATA_RAW` is 128 rather than 16.
