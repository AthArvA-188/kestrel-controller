"""Kestrel WiFi receiver.

Listens for Kestrel controllers on UDP and presents each one to Windows as a
virtual Xbox 360 pad through ViGEmBus. Rumble from games goes back to the
controller on the same socket, with any trigger effects set on the command
line. docs/wifi-protocol.md describes the packets.

    py -3.12 kestrel_receiver.py
    py -3.12 kestrel_receiver.py --sink log --right-trigger hair:70
"""
from __future__ import annotations

import argparse
import logging
import secrets
import socket
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional, Protocol

log = logging.getLogger("kestrel")

PORT = 47810
VERSION = 1
MAGIC = b"KS"
DISCOVER, OFFER, INPUT, FEEDBACK, BYE = 1, 2, 3, 4, 5

FLAG_CHARGING = 0x01
FLAG_MENU_OPEN = 0x02
FX_LEFT, FX_RIGHT = 1, 2
EFFECTS = {"off": 0, "resistance": 1, "hair": 2, "click": 3, "pulse": 4}

HEADER = struct.Struct("<2sBBHH")           # magic, version, type, seq, session
DISCOVER_BODY = struct.Struct("<6sHBB16s")  # device id, firmware, battery, flags, name
OFFER_BODY = struct.Struct("<16sHBx")       # name, rate, slot
INPUT_BODY = struct.Struct("<IhhhhBBBBI")   # buttons, lx, ly, rx, ry, lt, rt, battery, flags, time
FEEDBACK_BODY = struct.Struct("<BBBx4s4sH") # rumble left, right, fx valid, fx left, fx right, ack

# Bit positions in the INPUT packet's button field.
(BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, BTN_VIEW, BTN_MENU, BTN_HOME, BTN_L3, BTN_R3,
 BTN_DUP, BTN_DDOWN, BTN_DLEFT, BTN_DRIGHT, BTN_LT, BTN_RT, BTN_EX1, BTN_EX2,
 BTN_EX3) = (1 << n for n in range(20))

# The same buttons in an XInput report. LT, RT, and the extras have no bit there.
_XUSB = (
    (BTN_DUP, 0x0001), (BTN_DDOWN, 0x0002), (BTN_DLEFT, 0x0004), (BTN_DRIGHT, 0x0008),
    (BTN_MENU, 0x0010), (BTN_VIEW, 0x0020), (BTN_L3, 0x0040), (BTN_R3, 0x0080),
    (BTN_LB, 0x0100), (BTN_RB, 0x0200), (BTN_HOME, 0x0400),
    (BTN_A, 0x1000), (BTN_B, 0x2000), (BTN_X, 0x4000), (BTN_Y, 0x8000),
)


@dataclass(frozen=True)
class PadState:
    """Sticks run from -32768 to 32767 with +y up. Triggers run from 0 to 255."""
    buttons: int = 0
    lx: int = 0
    ly: int = 0
    rx: int = 0
    ry: int = 0
    lt: int = 0
    rt: int = 0


@dataclass(frozen=True)
class TriggerEffect:
    """All four values are bytes, as the controller stores them."""
    effect: int = 0
    start: int = 0
    strength: int = 0
    param: int = 0

    def pack(self) -> bytes:
        return bytes((self.effect, self.start, self.strength, self.param))


@dataclass(frozen=True)
class Discover:
    device_id: bytes
    fw_version: int
    battery_pct: int
    flags: int
    name: str


@dataclass(frozen=True)
class Offer:
    name: str
    rate_hz: int
    slot: int


@dataclass(frozen=True)
class InputReport:
    state: PadState
    battery_pct: int
    flags: int
    time_ms: int


@dataclass(frozen=True)
class Feedback:
    rumble_left: int
    rumble_right: int
    fx_valid: int
    fx_left: TriggerEffect
    fx_right: TriggerEffect
    ack_seq: int


@dataclass(frozen=True)
class Packet:
    type: int
    seq: int
    session: int
    body: object = None


def _name_bytes(name: str) -> bytes:
    return name.encode("utf-8", "replace")[:16]


def _name_text(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def _header(ptype: int, seq: int, session: int) -> bytes:
    return HEADER.pack(MAGIC, VERSION, ptype, seq & 0xFFFF, session & 0xFFFF)


def encode_discover(seq: int, d: Discover) -> bytes:
    return _header(DISCOVER, seq, 0) + DISCOVER_BODY.pack(
        d.device_id, d.fw_version, d.battery_pct, d.flags, _name_bytes(d.name))


def encode_offer(seq: int, session: int, o: Offer) -> bytes:
    return _header(OFFER, seq, session) + OFFER_BODY.pack(_name_bytes(o.name), o.rate_hz, o.slot)


def encode_input(seq: int, session: int, r: InputReport) -> bytes:
    s = r.state
    return _header(INPUT, seq, session) + INPUT_BODY.pack(
        s.buttons, s.lx, s.ly, s.rx, s.ry, s.lt, s.rt, r.battery_pct, r.flags, r.time_ms)


def encode_feedback(seq: int, session: int, f: Feedback) -> bytes:
    return _header(FEEDBACK, seq, session) + FEEDBACK_BODY.pack(
        f.rumble_left, f.rumble_right, f.fx_valid, f.fx_left.pack(), f.fx_right.pack(),
        f.ack_seq & 0xFFFF)


def encode_bye(seq: int, session: int) -> bytes:
    return _header(BYE, seq, session)


def decode(data: bytes) -> Optional[Packet]:
    """Returns None for anything that is not a valid version 1 packet.

    Bytes after the known fields are ignored so that later versions can add to
    a packet.
    """
    if len(data) < HEADER.size:
        return None
    magic, version, ptype, seq, session = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION:
        return None
    body = data[HEADER.size:]
    try:
        if ptype == DISCOVER:
            dev, fw, battery, flags, name = DISCOVER_BODY.unpack_from(body)
            return Packet(ptype, seq, session, Discover(dev, fw, battery, flags, _name_text(name)))
        if ptype == OFFER:
            name, rate, slot = OFFER_BODY.unpack_from(body)
            return Packet(ptype, seq, session, Offer(_name_text(name), rate, slot))
        if ptype == INPUT:
            buttons, lx, ly, rx, ry, lt, rt, battery, flags, time_ms = INPUT_BODY.unpack_from(body)
            state = PadState(buttons, lx, ly, rx, ry, lt, rt)
            return Packet(ptype, seq, session, InputReport(state, battery, flags, time_ms))
        if ptype == FEEDBACK:
            left, right, valid, fx_l, fx_r, ack = FEEDBACK_BODY.unpack_from(body)
            if fx_l[0] >= len(EFFECTS) or fx_r[0] >= len(EFFECTS):
                return None
            return Packet(ptype, seq, session,
                          Feedback(left, right, valid, TriggerEffect(*fx_l), TriggerEffect(*fx_r), ack))
        if ptype == BYE:
            return Packet(ptype, seq, session)
    except struct.error:
        return None  # shorter than its type requires
    return None


def xusb_buttons(buttons: int) -> int:
    """Converts the packet's button bits to an XInput wButtons value."""
    mask = 0
    for ours, theirs in _XUSB:
        if buttons & ours:
            mask |= theirs
    return mask


def parse_effect(text: str) -> TriggerEffect:
    """Parses name[:start[:strength[:param]]], for example resistance:60:180."""
    parts = text.split(":")
    if parts[0] not in EFFECTS:
        raise ValueError(f"unknown effect {parts[0]!r}; use one of {', '.join(EFFECTS)}")
    if len(parts) > 4:
        raise ValueError("an effect takes at most start, strength, and param")
    values = [int(p) for p in parts[1:]]
    if any(v < 0 or v > 255 for v in values):
        raise ValueError("effect values run from 0 to 255")
    defaults = [60, 160, 0]
    return TriggerEffect(EFFECTS[parts[0]], *(values + defaults[len(values):]))


class GamepadSink(Protocol):
    """Where a controller's state goes. One sink per connected controller."""

    def update(self, state: PadState) -> None: ...

    def set_feedback_handler(self, handler: Callable[[int, int], None]) -> None:
        """handler(large_motor, small_motor) with values from 0 to 255."""

    def close(self) -> None: ...


class ViGEmSink:
    """A virtual Xbox 360 pad. Needs the vgamepad package and the ViGEmBus driver."""

    def __init__(self) -> None:
        try:
            import vgamepad  # imported here so the rest of the module works without it
        except Exception as exc:
            raise RuntimeError(
                "The vigem sink needs the vgamepad package and the ViGEmBus driver. "
                "See host/README.md, or run with --sink log.") from exc
        self._handler: Optional[Callable[[int, int], None]] = None
        self._pad = vgamepad.VX360Gamepad()
        self._pad.register_notification(callback_function=self._notify)

    def _notify(self, client, target, large_motor, small_motor, led_number, user_data) -> None:
        if self._handler:
            self._handler(large_motor, small_motor)

    def set_feedback_handler(self, handler: Callable[[int, int], None]) -> None:
        self._handler = handler

    def update(self, state: PadState) -> None:
        report = self._pad.report
        report.wButtons = xusb_buttons(state.buttons)
        report.bLeftTrigger = state.lt
        report.bRightTrigger = state.rt
        report.sThumbLX, report.sThumbLY = state.lx, state.ly
        report.sThumbRX, report.sThumbRY = state.rx, state.ry
        self._pad.update()

    def close(self) -> None:
        self._pad.reset()
        self._pad.update()
        self._pad.unregister_notification()


class LogSink:
    """Prints state changes. For checking the link without ViGEmBus."""

    def set_feedback_handler(self, handler: Callable[[int, int], None]) -> None:
        pass

    def update(self, state: PadState) -> None:
        log.info("buttons=%05x L=(%6d,%6d) R=(%6d,%6d) LT=%3d RT=%3d", state.buttons, state.lx,
                 state.ly, state.rx, state.ry, state.lt, state.rt)

    def close(self) -> None:
        pass


@dataclass
class Session:
    addr: tuple
    device_id: bytes
    name: str
    session_id: int
    slot: int
    sink: GamepadSink
    last_rx: float
    last_seq: Optional[int] = None
    state: PadState = field(default_factory=PadState)
    battery_pct: int = 0
    received: int = 0
    lost: int = 0
    stale: int = 0
    rumble: tuple = (0, 0)
    feedback_seq: int = 0
    last_feedback: float = 0.0


class Receiver:
    MAX_PADS = 4  # XInput's limit

    def __init__(self, sink_factory: Callable[[], GamepadSink], bind: str = "0.0.0.0",
                 port: int = PORT, name: Optional[str] = None, rate_hz: int = 250,
                 timeout: float = 2.0, heartbeat: float = 0.25,
                 allow: Optional[set] = None) -> None:
        self._sink_factory = sink_factory
        self._bind = (bind, port)
        self.name = name or socket.gethostname()
        self.rate_hz = rate_hz
        self.timeout = timeout
        self.heartbeat = heartbeat
        self._allow = {a.lower().replace(":", "") for a in allow} if allow else None
        self._fx: dict = {FX_LEFT: None, FX_RIGHT: None}
        self._sessions: dict = {}
        self._lock = threading.RLock()
        self._sock: Optional[socket.socket] = None
        self._thread: Optional[threading.Thread] = None
        self._running = False
        self.bad_packets = 0

    @property
    def port(self) -> int:
        return self._sock.getsockname()[1]

    def sessions(self) -> list:
        with self._lock:
            return list(self._sessions.values())

    def start(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.bind(self._bind)
        self._sock.settimeout(0.02)
        self._running = True
        self._thread = threading.Thread(target=self._run, name="kestrel-receiver", daemon=True)
        self._thread.start()
        log.info("listening on %s:%d as %s", self._bind[0], self.port, self.name)

    def stop(self) -> None:
        self._running = False
        if self._thread:
            self._thread.join(timeout=2)
        with self._lock:
            for s in list(self._sessions.values()):
                self._close(s, "receiver stopped", send_bye=True)
        if self._sock:
            self._sock.close()

    def set_trigger_effect(self, side: int, effect: Optional[TriggerEffect]) -> None:
        """Sets the effect sent to every controller. None returns that trigger
        to the controller's own profile."""
        with self._lock:
            self._fx[side] = effect
            for s in self._sessions.values():
                self._send_feedback(s)

    def _run(self) -> None:
        while self._running:
            data = None
            try:
                data, addr = self._sock.recvfrom(256)
            except socket.timeout:
                pass
            except ConnectionResetError:
                pass  # Windows reports a closed peer port here; the timeout cleans up
            except OSError:
                if self._running:
                    log.exception("socket error")
                break
            now = time.monotonic()
            with self._lock:
                if data is not None:
                    self._handle(data, addr, now)
                self._housekeeping(now)

    def _handle(self, data: bytes, addr: tuple, now: float) -> None:
        pkt = decode(data)
        if pkt is None:
            self.bad_packets += 1
            return
        if pkt.type == DISCOVER:
            self._on_discover(pkt.body, addr, now)
            return
        s = self._sessions.get(addr)
        if s is None or pkt.session != s.session_id:
            return  # not from a controller we made an offer to
        if pkt.type == INPUT:
            self._on_input(s, pkt, now)
        elif pkt.type == BYE:
            self._close(s, "controller said goodbye", send_bye=False)

    def _on_discover(self, d: Discover, addr: tuple, now: float) -> None:
        device = d.device_id.hex()
        if self._allow is not None and device not in self._allow:
            log.warning("ignoring controller %s at %s: not in the allow list", device, addr[0])
            return
        s = self._sessions.get(addr)
        if s is None:
            # A controller that restarted comes back from a new port.
            for old in list(self._sessions.values()):
                if old.device_id == d.device_id:
                    self._close(old, "controller reconnected", send_bye=False)
            used = {x.slot for x in self._sessions.values()}
            slot = next((n for n in range(1, self.MAX_PADS + 1) if n not in used), None)
            if slot is None:
                log.warning("ignoring controller %s: all %d pads are in use", device, self.MAX_PADS)
                return
            try:
                sink = self._sink_factory()
            except Exception as exc:
                log.error("cannot create a pad for controller %s: %s", device, exc)
                return
            s = Session(addr, d.device_id, d.name, secrets.randbelow(0xFFFF) + 1, slot, sink, now)
            sink.set_feedback_handler(lambda large, small, s=s: self._on_rumble(s, large, small))
            self._sessions[addr] = s
            log.info("%s (%s) at %s is pad %d, battery %d%%", d.name, device, addr[0], slot,
                     d.battery_pct)
        s.last_rx = now
        self._send(addr, encode_offer(0, s.session_id, Offer(self.name, self.rate_hz, s.slot)))

    def _on_input(self, s: Session, pkt: Packet, now: float) -> None:
        if s.last_seq is not None:
            ahead = (pkt.seq - s.last_seq) & 0xFFFF
            if ahead == 0 or ahead >= 0x8000:
                s.stale += 1  # a duplicate, or older than what we already applied
                return
            s.lost += ahead - 1
        s.last_seq = pkt.seq
        s.last_rx = now
        s.received += 1
        s.battery_pct = pkt.body.battery_pct
        if pkt.body.state != s.state:
            s.state = pkt.body.state
            s.sink.update(s.state)

    def _on_rumble(self, s: Session, large: int, small: int) -> None:
        # Called from the sink's own thread.
        with self._lock:
            if self._sessions.get(s.addr) is s and s.rumble != (large, small):
                s.rumble = (large, small)
                self._send_feedback(s)

    def _send_feedback(self, s: Session) -> None:
        left, right = self._fx[FX_LEFT], self._fx[FX_RIGHT]
        valid = (FX_LEFT if left else 0) | (FX_RIGHT if right else 0)
        s.feedback_seq = (s.feedback_seq + 1) & 0xFFFF
        s.last_feedback = time.monotonic()
        self._send(s.addr, encode_feedback(s.feedback_seq, s.session_id, Feedback(
            s.rumble[0], s.rumble[1], valid, left or TriggerEffect(), right or TriggerEffect(),
            s.last_seq or 0)))

    def _housekeeping(self, now: float) -> None:
        for s in list(self._sessions.values()):
            if now - s.last_rx > self.timeout:
                self._close(s, "no packets for %.1f s" % self.timeout, send_bye=True)
            elif now - s.last_feedback >= self.heartbeat:
                self._send_feedback(s)  # also tells the controller we are still here

    def _close(self, s: Session, reason: str, send_bye: bool) -> None:
        self._sessions.pop(s.addr, None)
        if send_bye:
            self._send(s.addr, encode_bye(0, s.session_id))
        try:
            s.sink.update(PadState())  # release everything the game still sees held
            s.sink.close()
        except Exception:
            log.exception("closing pad %d failed", s.slot)
        log.info("pad %d (%s) closed: %s. %d packets, %d lost", s.slot, s.name, reason,
                 s.received, s.lost)

    def _send(self, addr: tuple, data: bytes) -> None:
        try:
            self._sock.sendto(data, addr)
        except OSError as exc:
            log.debug("send to %s failed: %s", addr, exc)


def main(argv: Optional[list] = None) -> int:
    ap = argparse.ArgumentParser(description="Receiver for Kestrel controllers in WiFi mode.")
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default: all)")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--rate", type=int, default=250, help="input packets per second to ask for")
    ap.add_argument("--sink", choices=("vigem", "log"), default="vigem",
                    help="vigem makes a virtual Xbox 360 pad; log prints the inputs")
    ap.add_argument("--allow", action="append", metavar="DEVICE_ID",
                    help="accept only this controller ID (12 hex digits); repeat for more")
    ap.add_argument("--left-trigger", type=parse_effect, metavar="EFFECT",
                    help="for example resistance:60:180, hair:70, click:60:200:24, pulse:90:128:12")
    ap.add_argument("--right-trigger", type=parse_effect, metavar="EFFECT")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    if not 50 <= args.rate <= 1000:
        ap.error("--rate must be between 50 and 1000")

    if args.sink == "vigem":
        try:
            ViGEmSink().close()  # fail now, with the reason, if the driver is missing
        except RuntimeError as exc:
            log.error("%s", exc)
            return 1
    factory = ViGEmSink if args.sink == "vigem" else LogSink
    receiver = Receiver(factory, bind=args.bind, port=args.port, rate_hz=args.rate,
                        allow=set(args.allow) if args.allow else None)
    receiver.set_trigger_effect(FX_LEFT, args.left_trigger)
    receiver.set_trigger_effect(FX_RIGHT, args.right_trigger)
    receiver.start()
    try:
        while True:
            time.sleep(5)
            for s in receiver.sessions():
                log.info("pad %d: %d packets, %d lost, battery %d%%", s.slot, s.received, s.lost,
                         s.battery_pct)
    except KeyboardInterrupt:
        pass
    finally:
        receiver.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
