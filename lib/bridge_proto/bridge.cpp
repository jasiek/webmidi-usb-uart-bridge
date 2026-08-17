#include "bridge.h"

namespace bridge {

void Bridge::begin(uint32_t nowMs) {
  nowMs_ = nowMs;
  resetSession(nowMs);
  state_ = PortState::Closed;
  // Sampled rather than assumed, so a device already attached at boot is not
  // announced as an attach to a host that was not there to miss it. The change
  // count is sampled with it for the same reason: whatever happened before we
  // were running is not news.
  const uint32_t presence = backend_.presence();
  lastPresent_ = (presence & 1u) != 0;
  lastPresenceChanges_ = presence >> 1;
  presenceQueued_ = 0;
}

void Bridge::resetSession(uint32_t nowMs) {
  toBackend_.clear();
  toHost_.clear();
  recvWin_.reset(kRxBufferSize, nowMs);
  sendWin_.reset(0);  // nothing may be sent until the host advertises a window
  rxSeq_ = 0;
  txSeq_ = 0;
  rxCount_ = 0;
  txCount_ = 0;
  errFlags_ = 0;
}

// ---- inbound ---------------------------------------------------------------

void Bridge::onSysEx(const uint8_t* msg, size_t n, uint32_t nowMs) {
  nowMs_ = nowMs;

  FrameReader r;
  switch (FrameReader::parse(msg, n, r)) {
    case FrameReader::Status::Ok:
      break;
    case FrameReader::Status::BadVersion:
      sendError(Err::Version, kProtocolVersion);
      return;
    case FrameReader::Status::TooShort:
      sendError(Err::BadLength);
      return;
    default:
      // Not SysEx, or somebody else's device on the same bus. Not our business.
      return;
  }

  const Cmd cmd = static_cast<Cmd>(r.cmd());

  // Legal with no port: the handshake, the two that ask about or change the
  // port's existence, and CREDIT. CREDIT belongs on the list because the
  // device goes on delivering what it received before a CLOSE or a detach
  // (poll(), below), and that delivery is credit-paced like any other — a
  // closed port that answered ERR_NOT_OPEN would strand everything past the
  // host's opening window, and the spurious error would reject whatever
  // unrelated request the host had in flight. It grants a window; it cannot
  // do anything to a port that is not there. PROTOCOL.md §4.2.
  if (state_ != PortState::Open && cmd != Cmd::Hello && cmd != Cmd::Reset &&
      cmd != Cmd::Open && cmd != Cmd::Ping && cmd != Cmd::GetStatus &&
      cmd != Cmd::Credit) {
    sendError(Err::NotOpen);
    return;
  }

  switch (cmd) {
    case Cmd::Hello:      handleHello(r); break;
    case Cmd::Open:       handleOpen(r, nowMs); break;
    case Cmd::Close:      handleClose(); break;
    case Cmd::Data:       handleData(r); break;
    case Cmd::SetLines:   handleSetLines(r); break;
    case Cmd::Flush:      handleFlush(r); break;
    case Cmd::Credit:     handleCredit(r); break;
    case Cmd::Ping:       handlePing(r); break;
    case Cmd::GetStatus:  sendStatus(); break;
    case Cmd::Reset:      handleReset(nowMs); break;
    default:
      sendError(Err::BadCmd, r.cmd());
      break;
  }
}

void Bridge::handleHello(FrameReader& r) {
  // A HELLO is a host introducing itself, which makes anything still queued
  // for the previous host stale. Dropping it here also guarantees there is
  // room for the INFO reply, so a client reconnecting after an unclean exit
  // gets an answer instead of silence.
  sink_.discardQueued();

  uint16_t rxBuf = 0, maxRaw = 0;
  if (r.u14(rxBuf) && r.u14(maxRaw)) {
    hostRxBuffer_ = rxBuf;
    // Never emit a frame larger than the protocol floor the peer must support,
    // and never larger than our own build allows.
    hostMaxRaw_ = maxRaw == 0 || maxRaw > kMaxDataRaw
                      ? static_cast<uint16_t>(kMaxDataRaw)
                      : maxRaw;
  }
  sendInfo();
}

void Bridge::handleOpen(FrameReader& r, uint32_t nowMs) {
  uint32_t baud = 0;
  uint8_t databits = 0, parity = 0, stopbits = 0, flags = 0;
  uint16_t hostRx = 0;
  if (!r.u32(baud) || !r.u7(databits) || !r.u7(parity) || !r.u7(stopbits) ||
      !r.u7(flags) || !r.u14(hostRx)) {
    sendError(Err::BadLength);
    return;
  }

  if (baud == 0 || baud > backend_.maxBaud()) {
    sendError(Err::BadParam, 0);
    return;
  }
  if (databits < 5 || databits > 8) {
    sendError(Err::BadParam, 1);
    return;
  }
  if (parity > 2) {
    sendError(Err::BadParam, 2);
    return;
  }
  if (stopbits < 1 || stopbits > 2) {
    sendError(Err::BadParam, 3);
    return;
  }

  PortConfig cfg;
  cfg.baud = baud;
  cfg.databits = databits;
  cfg.parity = static_cast<Parity>(parity);
  cfg.stopbits = stopbits;
  cfg.flags = flags;

  if (!backend_.open(cfg)) {
    state_ = PortState::Fault;
    sendError(Err::Backend);
    return;
  }

  cfg_ = cfg;
  state_ = PortState::Open;
  // The tail of the previous session goes here, and it has to go from both
  // places it can be sitting. resetSession() empties toHost_, but anything
  // already framed and handed to the sink is past that point — and the
  // sequence counters restart below, so those frames would arrive in the new
  // session numbered for the old one and be read as a gap. PROTOCOL.md §5.1
  // makes OPEN the way back to a known state; this is what that costs.
  sink_.discardQueued();
  // Re-opening is the idempotent way back to a known state: buffers, windows
  // and both sequence counters all restart here.
  resetSession(nowMs);
  hostRxBuffer_ = hostRx;
  sendWin_.reset(hostRx);
  lastInputLines_ = backend_.inputLines();
  sendStatus();
}

void Bridge::handleClose() {
  backend_.close();
  state_ = PortState::Closed;
  sendStatus();
}

void Bridge::handleData(FrameReader& r) {
  uint8_t seq = 0;
  if (!r.u7(seq)) {
    sendError(Err::BadLength);
    return;
  }

  size_t len = 0;
  if (!r.unpackRest(scratch_, sizeof(scratch_), len)) {
    sendError(Err::BadEncoding);
    return;
  }

  if (seq != rxSeq_) {
    // Report the gap, then accept the frame anyway and resynchronise — losing
    // the payload as well as the sequence helps nobody. PROTOCOL.md §7.
    sendError(Err::Seq, rxSeq_);
  }
  rxSeq_ = static_cast<uint8_t>((seq + 1) & kSeqMask);

  const size_t taken = toBackend_.write(scratch_, len);
  if (taken < len) {
    // The peer sent past its window. Say so, with the loss quantified.
    const size_t dropped = len - taken;
    errFlags_ |= kErrFlagHostOverflow;
    sendError(Err::NoCredit, static_cast<uint8_t>(dropped > 127 ? 127 : dropped));
  }
}

void Bridge::handleSetLines(FrameReader& r) {
  uint8_t mask = 0, values = 0;
  if (!r.u7(mask) || !r.u7(values)) {
    sendError(Err::BadLength);
    return;
  }
  backend_.setLines(mask, values);
}

void Bridge::handleFlush(FrameReader& r) {
  uint8_t what = 0;
  if (!r.u7(what)) {
    sendError(Err::BadLength);
    return;
  }
  if (what & kFlushDiscardTx) toBackend_.clear();
  if (what & kFlushDiscardRx) toHost_.clear();
  backend_.flush(what);
  sendStatus();
}

void Bridge::handleCredit(FrameReader& r) {
  uint16_t delta = 0;
  if (!r.u14(delta)) {
    sendError(Err::BadLength);
    return;
  }
  sendWin_.grant(delta);
}

void Bridge::handlePing(FrameReader& r) {
  uint8_t cookie[8];
  size_t len = 0;
  if (!r.rest(cookie, sizeof(cookie), len)) {
    sendError(Err::BadLength);
    return;
  }
  FrameWriter w(frameBuf_, sizeof(frameBuf_));
  w.begin(Rsp::Pong);
  w.bytes(cookie, len);
  const size_t n = w.end();
  if (n) sink_.send(frameBuf_, n);
}

void Bridge::handleReset(uint32_t nowMs) {
  sink_.discardQueued();
  backend_.close();
  state_ = PortState::Closed;
  resetSession(nowMs);
  sendStatus();
}

// ---- outbound --------------------------------------------------------------

void Bridge::sendInfo() {
  FrameWriter w(frameBuf_, sizeof(frameBuf_));
  w.begin(Rsp::Info);
  w.u7(kProtocolVersion);
  w.u7(fw_.major);
  w.u7(fw_.minor);
  w.u7(fw_.patch);
  w.u7(static_cast<uint8_t>(backend_.id()));
  w.u14(backend_.caps());
  w.u14(kMaxDataRaw);
  w.u14(kRxBufferSize);
  w.u32(backend_.maxBaud());
  const size_t n = w.end();
  if (n) sink_.send(frameBuf_, n);
}

void Bridge::sendStatus() {
  FrameWriter w(frameBuf_, sizeof(frameBuf_));
  w.begin(Rsp::Status);
  w.u7(static_cast<uint8_t>(state_));
  w.u7(backend_.outputLines());
  w.u7(backend_.inputLines());
  w.u7(errFlags_ | backend_.takeErrorFlags());
  errFlags_ = 0;  // sticky flags are cleared by being read. PROTOCOL.md §5.4
  w.u21(rxCount_);
  w.u21(txCount_);
  w.u14(sendWin_.credit());
  // Trailing, so a host built against the original layout stops before it and
  // is none the wiser. PROTOCOL.md §5.4.
  // Asked of the backend rather than read from lastPresent_: a STATUS that
  // arrives between an attach and the poll() that notices it should say what
  // is true now, not what we have got around to announcing.
  w.u7(backend_.present() ? 1 : 0);
  const size_t n = w.end();
  if (n) sink_.send(frameBuf_, n);
}

void Bridge::sendCredit(uint16_t delta) {
  FrameWriter w(frameBuf_, sizeof(frameBuf_));
  w.begin(Rsp::Credit);
  w.u14(delta);
  const size_t n = w.end();
  if (n) sink_.send(frameBuf_, n);
}

void Bridge::sendError(Err code, uint8_t detail) {
  if (!sink_.ready()) return;
  FrameWriter w(frameBuf_, sizeof(frameBuf_));
  w.begin(Rsp::Error);
  w.u7(static_cast<uint8_t>(code));
  w.u7(detail);
  const size_t n = w.end();
  if (n) sink_.send(frameBuf_, n);
}

void Bridge::sendEvent(Evt evt, uint8_t arg) {
  FrameWriter w(frameBuf_, sizeof(frameBuf_));
  w.begin(Rsp::Event);
  w.u7(static_cast<uint8_t>(evt));
  w.u7(arg);
  const size_t n = w.end();
  if (n) sink_.send(frameBuf_, n);
}

bool Bridge::sendDataChunk() {
  size_t want = toHost_.size();
  if (want > hostMaxRaw_) want = hostMaxRaw_;
  if (want > kMaxDataRaw) want = kMaxDataRaw;
  if (want == 0) return false;
  if (!sendWin_.canSend(want)) {
    want = sendWin_.credit();
    if (want == 0) return false;
  }

  const size_t got = toHost_.peek(scratch_, want);
  FrameWriter w(frameBuf_, sizeof(frameBuf_));
  w.begin(Rsp::Data);
  w.u7(txSeq_);
  w.packed(scratch_, got);
  const size_t n = w.end();
  if (!n) return false;
  if (!sink_.send(frameBuf_, n)) return false;

  // Only now is the data really gone: commit the buffer, the window and the
  // sequence number together, so a refused send costs us nothing.
  toHost_.discard(got);
  sendWin_.consume(got);
  txSeq_ = static_cast<uint8_t>((txSeq_ + 1) & kSeqMask);
  return true;
}

// ---- pumps -----------------------------------------------------------------

void Bridge::pumpToBackend() {
  while (!toBackend_.empty()) {
    const size_t room = backend_.writable();
    if (room == 0) break;
    size_t want = toBackend_.size();
    if (want > room) want = room;
    if (want > sizeof(scratch_)) want = sizeof(scratch_);

    const size_t got = toBackend_.peek(scratch_, want);
    const size_t wrote = backend_.write(scratch_, got);
    if (wrote == 0) break;
    toBackend_.discard(wrote);
    txCount_ += static_cast<uint32_t>(wrote);
    // Space in our receive buffer is what the host's credit actually buys, so
    // credit is returned here and nowhere else.
    recvWin_.freed(static_cast<uint16_t>(wrote));
    if (wrote < got) break;
  }
}

void Bridge::pumpFromBackend() {
  while (backend_.readable() > 0 && toHost_.space() > 0) {
    size_t want = backend_.readable();
    if (want > toHost_.space()) want = toHost_.space();
    if (want > sizeof(scratch_)) want = sizeof(scratch_);
    const size_t got = backend_.read(scratch_, want);
    if (got == 0) break;
    toHost_.write(scratch_, got);
    rxCount_ += static_cast<uint32_t>(got);
  }
}

void Bridge::pumpLines() {
  const uint8_t lines = backend_.inputLines();
  if (lines != lastInputLines_) {
    lastInputLines_ = lines;
    if (sink_.ready()) sendEvent(Evt::Lines, lines);
  }

  const uint8_t flags = backend_.takeErrorFlags();
  if (flags) {
    errFlags_ |= flags;
    if (sink_.ready()) {
      if (flags & kErrFlagBreak) sendEvent(Evt::Break, 0);
      if (flags & kErrFlagOverrun) sendEvent(Evt::Overrun, 0);
    }
  }
}

// Announcing a presence change is deferred when the USB endpoint is busy, so
// the events queue rather than overwrite one another. The state change they
// describe is never deferred: the port faults the moment the far end goes,
// whether or not anyone can be told yet.
void Bridge::queuePresence(Evt evt) {
  if (presenceQueued_ < 2) {
    presenceQueue_[presenceQueued_++] = evt;
    return;
  }
  // Full. Drop the oldest, not the newest: the newest two still alternate and
  // still end where the far end actually is, so the host is left with an
  // accurate picture rather than a stale one.
  presenceQueue_[0] = presenceQueue_[1];
  presenceQueue_[1] = evt;
}

void Bridge::applyPresence(bool present) {
  if (present) {
    // A previous detach left us in Fault. The far end is new, so the fault
    // is over — but the port is not open until the host says so.
    if (state_ == PortState::Fault) state_ = PortState::Closed;
    queuePresence(Evt::Attach);
    return;
  }

  if (state_ == PortState::Open) {
    // Last chance to collect what the far end already sent. Once close()
    // has run, a backend is entitled to forget it.
    pumpFromBackend();
    backend_.close();
    state_ = PortState::Fault;
    // Bytes still queued for a port that no longer exists are not going to
    // arrive; holding them would deliver them to whatever is plugged in
    // next. What came *from* the device is still ours to deliver, so
    // toHost_ is left alone.
    toBackend_.clear();
  }
  queuePresence(Evt::Detach);
}

// Attach and detach are the one thing a host cannot discover by asking: until
// something is attached there is nothing to open, and GET_STATUS on a closed
// port looks the same either way. So they are watched as edges, counted by the
// backend — a level would miss a detach and re-attach that both happen between
// two polls, and leave the port Open against an adapter TinyUSB has since
// reset to its own defaults. See Backend::presence().
void Bridge::pumpPresence() {
  const uint32_t presence = backend_.presence();
  const bool present = (presence & 1u) != 0;
  const uint32_t changes = presence >> 1;

  uint32_t delta = changes - lastPresenceChanges_;
  // A backend that cannot be unplugged leaves the count at zero for ever, and
  // so does any Backend that has not overridden presence(). For those the
  // level is the only evidence there is, and a change in it is one edge.
  if (delta == 0 && present != lastPresent_) delta = 1;

  if (delta != 0) {
    lastPresenceChanges_ = changes;
    // The run began by flipping away from what we last saw and ended at
    // `present`; only those two ends are announced. A host acts on "my port
    // faulted" and "there is one to open now", not on a count of how many
    // times a connector bounced — and the first of the two is what carries the
    // fault, so nothing that matters is collapsed away.
    applyPresence(!lastPresent_);
    if (delta >= 2 && present == lastPresent_) applyPresence(present);
    lastPresent_ = present;
  }

  while (presenceQueued_ > 0 && sink_.ready()) {
    const Evt evt = presenceQueue_[0];
    presenceQueue_[0] = presenceQueue_[1];
    --presenceQueued_;
    sendEvent(evt, static_cast<uint8_t>(backend_.id()));
  }
}

void Bridge::poll(uint32_t nowMs) {
  nowMs_ = nowMs;

  // Before the state check, deliberately: a detach is most of what a host in
  // Fault or Closed is waiting to hear about.
  BRIDGE_PHASE(16);
  pumpPresence();

  if (state_ == PortState::Open) {
    BRIDGE_PHASE(10);
    pumpToBackend();
    BRIDGE_PHASE(11);
    pumpFromBackend();
    BRIDGE_PHASE(12);
    pumpLines();

    BRIDGE_PHASE(13);
    if (sink_.ready() && recvWin_.shouldGrant(nowMs))
      sendCredit(recvWin_.takeGrant(nowMs));
  }

  // Outside the state check on purpose. Bytes already in toHost_ were received
  // while the port was open; a detach or a CLOSE arriving a millisecond later
  // does not un-receive them, and stranding them here would be exactly the
  // silent loss the rest of this design goes out of its way to avoid.
  //
  // Two things have to be true for that to work rather than merely look like
  // it works. The host's CREDIT has to be accepted with the port closed, or
  // the drain stops at whatever window was left over (see onSysEx). And OPEN
  // and RESET have to call sink_.discardQueued() as well as clearing toHost_,
  // or a frame that left before the boundary arrives after it — carrying the
  // old session's sequence number into the new one, which the host reads as a
  // gap and as bytes it never asked for. DECISIONS.md D13.
  BRIDGE_PHASE(14);
  while (sink_.ready() && sendDataChunk()) {
  }
  BRIDGE_PHASE(15);
}

}  // namespace bridge
