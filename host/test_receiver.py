"""Tests for kestrel_receiver against a simulated controller on loopback.

Run from this directory:  py -3.12 -m unittest -v test_receiver
No ViGEmBus or vgamepad needed. A fake sink stands in for the virtual pad.
"""
import pathlib
import socket
import sys
import time
import unittest

import kestrel_receiver as kr

VECTORS = pathlib.Path(__file__).resolve().parents[1] / "firmware" / "sim" / "out" / "wifi_vectors.txt"
DEVICE = bytes.fromhex("246f28aabbcc")


class FakeSink:
    def __init__(self):
        self.states = []
        self.closed = False
        self.handler = None

    def set_feedback_handler(self, handler):
        self.handler = handler

    def update(self, state):
        self.states.append(state)

    def close(self):
        self.closed = True


class SimController:
    """Speaks the controller's side of the protocol from a loopback socket."""

    def __init__(self, port, device_id=DEVICE):
        self.dest = ("127.0.0.1", port)
        self.device_id = device_id
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(1.0)
        self.session = 0
        self.seq = 0

    def close(self):
        self.sock.close()

    def discover(self, battery=76):
        self.sock.sendto(kr.encode_discover(1, kr.Discover(self.device_id, 0x0100, battery, 0, "Kestrel")),
                         self.dest)

    def recv(self, timeout=1.0):
        self.sock.settimeout(timeout)
        try:
            data, _ = self.sock.recvfrom(256)
        except (socket.timeout, ConnectionResetError):
            return None
        return kr.decode(data)

    def recv_type(self, ptype, timeout=1.0):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            pkt = self.recv(max(0.01, end - time.monotonic()))
            if pkt is not None and pkt.type == ptype:
                return pkt
        return None

    def connect(self):
        self.discover()
        offer = self.recv_type(kr.OFFER)
        assert offer is not None, "no offer"
        self.session = offer.session
        return offer

    def send_input(self, state, seq=None, session=None, battery=80):
        if seq is None:
            self.seq = (self.seq + 1) & 0xFFFF
            seq = self.seq
        report = kr.InputReport(state, battery, 0, 1234)
        self.sock.sendto(kr.encode_input(seq, self.session if session is None else session, report),
                         self.dest)


def wait_for(cond, timeout=2.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.005)
    return cond()


class CodecTests(unittest.TestCase):
    def test_input_round_trip(self):
        state = kr.PadState(kr.BTN_A | kr.BTN_RB, 1000, -1000, 32767, -32768, 7, 255)
        data = kr.encode_input(0x0102, 0x1234, kr.InputReport(state, 88, kr.FLAG_CHARGING, 0x01020304))
        self.assertEqual(data.hex(), "4b5301030201341221000000e80318fcff7f008007ff580104030201")
        pkt = kr.decode(data)
        self.assertEqual((pkt.type, pkt.seq, pkt.session), (kr.INPUT, 0x0102, 0x1234))
        self.assertEqual(pkt.body.state, state)
        self.assertEqual((pkt.body.battery_pct, pkt.body.flags, pkt.body.time_ms), (88, 1, 0x01020304))

    def test_rejects_bad_packets(self):
        good = kr.encode_bye(1, 2)
        self.assertIsNotNone(kr.decode(good))
        self.assertIsNone(kr.decode(b""))
        self.assertIsNone(kr.decode(good[:-1]))
        self.assertIsNone(kr.decode(b"XS" + good[2:]))
        self.assertIsNone(kr.decode(good[:2] + b"\x02" + good[3:]))  # version 2
        self.assertIsNone(kr.decode(good[:3] + b"\x09" + good[4:]))  # unknown type
        short = kr.encode_input(1, 2, kr.InputReport(kr.PadState(), 0, 0, 0))[:-1]
        self.assertIsNone(kr.decode(short))
        fb = bytearray(kr.encode_feedback(1, 2, kr.Feedback(0, 0, 0, kr.TriggerEffect(), kr.TriggerEffect(), 0)))
        fb[kr.HEADER.size + 4] = 5  # effect number out of range
        self.assertIsNone(kr.decode(bytes(fb)))

    def test_ignores_trailing_bytes(self):
        data = kr.encode_offer(1, 7, kr.Offer("PC", 250, 1)) + b"\xee" * 6
        self.assertEqual(kr.decode(data).body, kr.Offer("PC", 250, 1))

    def test_names_are_cut_to_sixteen_bytes(self):
        pkt = kr.decode(kr.encode_offer(1, 7, kr.Offer("A" * 30, 250, 1)))
        self.assertEqual(pkt.body.name, "A" * 16)

    @unittest.skipUnless(VECTORS.exists(), "run firmware/sim's kestrel_tests first")
    def test_firmware_vectors(self):
        """Packets encoded by the firmware's C code decode to the same values here."""
        vec = dict(line.split() for line in VECTORS.read_text().splitlines() if line.strip())
        raw = {k: bytes.fromhex(v) for k, v in vec.items()}

        d = kr.decode(raw["discover"])
        self.assertEqual((d.type, d.seq, d.session), (kr.DISCOVER, 1, 0))
        self.assertEqual(d.body, kr.Discover(DEVICE, 0x0100, 76, 0, "Kestrel"))
        self.assertEqual(kr.encode_discover(1, d.body), raw["discover"])

        o = kr.decode(raw["offer"])
        self.assertEqual((o.seq, o.session, o.body), (1, 0xBEEF, kr.Offer("GAMING-PC", 250, 1)))
        self.assertEqual(kr.encode_offer(1, 0xBEEF, o.body), raw["offer"])

        i = kr.decode(raw["input"])
        self.assertEqual((i.seq, i.session), (0x0102, 0x1234))
        self.assertEqual(i.body.state, kr.PadState(kr.BTN_A | kr.BTN_RB, 1000, -1000, 32767, -32768, 7, 255))
        self.assertEqual((i.body.battery_pct, i.body.flags, i.body.time_ms), (88, kr.FLAG_CHARGING, 0x01020304))
        self.assertEqual(kr.encode_input(0x0102, 0x1234, i.body), raw["input"])

        f = kr.decode(raw["feedback"])
        want = kr.Feedback(200, 100, kr.FX_LEFT | kr.FX_RIGHT, kr.TriggerEffect(1, 60, 180, 0),
                           kr.TriggerEffect(4, 90, 128, 12), 0x0102)
        self.assertEqual((f.seq, f.session, f.body), (9, 0xBEEF, want))
        self.assertEqual(kr.encode_feedback(9, 0xBEEF, want), raw["feedback"])

        b = kr.decode(raw["bye"])
        self.assertEqual((b.type, b.seq, b.session), (kr.BYE, 10, 0xBEEF))
        self.assertEqual(kr.encode_bye(10, 0xBEEF), raw["bye"])

    def test_xusb_buttons(self):
        self.assertEqual(kr.xusb_buttons(0), 0)
        self.assertEqual(kr.xusb_buttons(kr.BTN_A), 0x1000)
        self.assertEqual(kr.xusb_buttons(kr.BTN_Y | kr.BTN_DUP | kr.BTN_HOME), 0x8401)
        self.assertEqual(kr.xusb_buttons(kr.BTN_VIEW | kr.BTN_MENU | kr.BTN_L3 | kr.BTN_R3), 0x00F0)
        self.assertEqual(kr.xusb_buttons(kr.BTN_LB | kr.BTN_RB | kr.BTN_B | kr.BTN_X), 0x6300)
        # LT, RT, and the extra buttons have no XInput button bit.
        self.assertEqual(kr.xusb_buttons(kr.BTN_LT | kr.BTN_RT | kr.BTN_EX1 | kr.BTN_EX3), 0)

    def test_parse_effect(self):
        self.assertEqual(kr.parse_effect("off"), kr.TriggerEffect(0, 60, 160, 0))
        self.assertEqual(kr.parse_effect("hair:70"), kr.TriggerEffect(2, 70, 160, 0))
        self.assertEqual(kr.parse_effect("pulse:90:128:12"), kr.TriggerEffect(4, 90, 128, 12))
        for bad in ("wobble", "hair:300", "hair:1:2:3:4", "hair:x"):
            with self.assertRaises(ValueError):
                kr.parse_effect(bad)

    def test_vgamepad_is_not_imported_at_load(self):
        self.assertNotIn("vgamepad", sys.modules)


class LinkTests(unittest.TestCase):
    def setUp(self):
        self.sinks = []
        self.receiver = kr.Receiver(self._make_sink, bind="127.0.0.1", port=0, name="TEST-PC",
                                    rate_hz=500, timeout=0.4, heartbeat=0.05)
        self.receiver.start()
        self.pad = SimController(self.receiver.port)
        self.addCleanup(self.receiver.stop)
        self.addCleanup(self.pad.close)

    def _make_sink(self):
        self.sinks.append(FakeSink())
        return self.sinks[-1]

    def test_discovery_gets_an_offer(self):
        offer = self.pad.connect()
        self.assertEqual(offer.body, kr.Offer("TEST-PC", 500, 1))
        self.assertNotEqual(offer.session, 0)
        self.assertEqual(len(self.sinks), 1)
        s = self.receiver.sessions()[0]
        self.assertEqual((s.device_id, s.name, s.slot), (DEVICE, "Kestrel", 1))

    def test_repeated_discovery_keeps_the_session(self):
        first = self.pad.connect()
        second = self.pad.connect()  # as if the first offer had been lost
        self.assertEqual(first.session, second.session)
        self.assertEqual(len(self.sinks), 1)

    def test_input_reaches_the_sink(self):
        self.pad.connect()
        a = kr.PadState(kr.BTN_A, 100, -200, 300, -400, 10, 20)
        b = kr.PadState(kr.BTN_A | kr.BTN_DUP, 32767, -32768, 0, 0, 255, 0)
        self.pad.send_input(a)
        self.pad.send_input(a)  # unchanged: no second update
        self.pad.send_input(b)
        self.assertTrue(wait_for(lambda: len(self.sinks[0].states) == 2))
        self.assertEqual(self.sinks[0].states, [a, b])
        s = self.receiver.sessions()[0]
        self.assertEqual((s.received, s.lost, s.battery_pct), (3, 0, 80))

    def test_stale_and_duplicate_packets_are_dropped(self):
        self.pad.connect()
        new, old = kr.PadState(kr.BTN_B), kr.PadState(kr.BTN_X)
        self.pad.send_input(kr.PadState(kr.BTN_A), seq=10)
        self.pad.send_input(new, seq=14)  # 11 to 13 never arrive
        self.pad.send_input(old, seq=12)  # arrives late
        self.pad.send_input(old, seq=14)  # duplicate
        self.assertTrue(wait_for(lambda: self.receiver.sessions()[0].stale == 2))
        s = self.receiver.sessions()[0]
        self.assertEqual(self.sinks[0].states[-1], new)
        self.assertEqual((s.received, s.lost), (2, 3))

    def test_sequence_numbers_wrap(self):
        self.pad.connect()
        self.pad.send_input(kr.PadState(kr.BTN_A), seq=65535)
        self.pad.send_input(kr.PadState(kr.BTN_B), seq=0)
        self.assertTrue(wait_for(lambda: len(self.sinks[0].states) == 2))
        self.assertEqual(self.receiver.sessions()[0].lost, 0)

    def test_wrong_session_and_strangers_are_ignored(self):
        self.pad.connect()
        self.pad.send_input(kr.PadState(kr.BTN_A), session=self.pad.session ^ 0x5555)
        stranger = SimController(self.receiver.port)
        self.addCleanup(stranger.close)
        stranger.session = self.pad.session
        stranger.send_input(kr.PadState(kr.BTN_B))  # right session, wrong address
        stranger.sock.sendto(b"not a kestrel packet", stranger.dest)
        self.assertTrue(wait_for(lambda: self.receiver.bad_packets == 1))
        self.pad.send_input(kr.PadState(kr.BTN_Y))
        self.assertTrue(wait_for(lambda: len(self.sinks[0].states) == 1))
        self.assertEqual(self.sinks[0].states, [kr.PadState(kr.BTN_Y)])

    def test_rumble_goes_back_on_the_same_socket(self):
        self.pad.connect()
        self.pad.send_input(kr.PadState(kr.BTN_A), seq=77)
        self.assertTrue(wait_for(lambda: len(self.sinks[0].states) == 1))
        self.sinks[0].handler(200, 100)  # a game turns the motors on
        end = time.monotonic() + 1
        fb = None
        while time.monotonic() < end:
            pkt = self.pad.recv_type(kr.FEEDBACK, 0.5)
            if pkt and pkt.body.rumble_left == 200:
                fb = pkt
                break
        self.assertIsNotNone(fb)
        self.assertEqual(fb.session, self.pad.session)
        self.assertEqual((fb.body.rumble_left, fb.body.rumble_right, fb.body.ack_seq), (200, 100, 77))
        self.assertEqual(fb.body.fx_valid, 0)  # no effect set: the pad keeps its profile's

    def test_heartbeat_arrives_without_rumble(self):
        self.pad.connect()
        first = self.pad.recv_type(kr.FEEDBACK, 1.0)
        second = self.pad.recv_type(kr.FEEDBACK, 1.0)
        self.assertIsNotNone(first)
        self.assertIsNotNone(second)
        self.assertEqual((first.body.rumble_left, first.body.rumble_right), (0, 0))
        self.assertEqual((second.seq - first.seq) & 0xFFFF, 1)

    def test_trigger_effects_are_sent(self):
        self.receiver.set_trigger_effect(kr.FX_RIGHT, kr.parse_effect("hair:70"))
        self.pad.connect()
        fb = self.pad.recv_type(kr.FEEDBACK, 1.0)
        self.assertEqual(fb.body.fx_valid, kr.FX_RIGHT)
        self.assertEqual(fb.body.fx_right, kr.TriggerEffect(2, 70, 160, 0))
        self.receiver.set_trigger_effect(kr.FX_LEFT, kr.TriggerEffect(4, 90, 128, 12))
        self.assertTrue(wait_for(lambda: (p := self.pad.recv_type(kr.FEEDBACK, 0.2)) is not None
                                 and p.body.fx_valid == 3 and p.body.fx_left.effect == 4))

    def test_silence_times_out_and_releases_the_pad(self):
        self.pad.connect()
        held = kr.PadState(kr.BTN_A, 0, 0, 0, 0, 255, 255)
        self.pad.send_input(held)
        self.assertTrue(wait_for(lambda: self.sinks[0].states == [held]))
        self.assertTrue(wait_for(lambda: self.sinks[0].closed, timeout=2))
        self.assertEqual(self.sinks[0].states[-1], kr.PadState())  # nothing left held down
        self.assertEqual(self.receiver.sessions(), [])
        self.assertIsNotNone(self.pad.recv_type(kr.BYE, 1.0))  # the receiver said so
        # The controller goes back to discovery and gets a fresh pad.
        self.pad.connect()
        self.assertEqual(len(self.sinks), 2)
        self.assertFalse(self.sinks[1].closed)
        self.assertEqual(len(self.receiver.sessions()), 1)

    def test_bye_closes_the_session(self):
        self.pad.connect()
        self.pad.sock.sendto(kr.encode_bye(5, self.pad.session), self.pad.dest)
        self.assertTrue(wait_for(lambda: self.sinks[0].closed))
        self.assertEqual(self.receiver.sessions(), [])

    def test_restarted_controller_replaces_its_session(self):
        self.pad.connect()
        again = SimController(self.receiver.port)  # same device ID, new port
        self.addCleanup(again.close)
        again.connect()
        self.assertTrue(self.sinks[0].closed)
        self.assertEqual(len(self.receiver.sessions()), 1)
        self.assertEqual(self.receiver.sessions()[0].slot, 1)

    def test_at_most_four_pads(self):
        pads = [self.pad] + [SimController(self.receiver.port, bytes([i] * 6)) for i in range(1, 5)]
        for p in pads[1:]:
            self.addCleanup(p.close)
        slots = []
        for p in pads[:4]:
            slots.append(p.connect().body.slot)
        self.assertEqual(slots, [1, 2, 3, 4])
        pads[4].discover()
        self.assertIsNone(pads[4].recv_type(kr.OFFER, 0.3))
        self.assertEqual(len(self.receiver.sessions()), 4)


class AllowListTests(unittest.TestCase):
    def test_only_listed_controllers_get_an_offer(self):
        receiver = kr.Receiver(FakeSink, bind="127.0.0.1", port=0, allow={"24:6F:28:AA:BB:CC"})
        receiver.start()
        self.addCleanup(receiver.stop)
        known, unknown = SimController(receiver.port), SimController(receiver.port, bytes(6))
        self.addCleanup(known.close)
        self.addCleanup(unknown.close)
        unknown.discover()
        self.assertIsNone(unknown.recv_type(kr.OFFER, 0.3))
        self.assertIsNotNone(known.connect())

    def test_a_failing_sink_does_not_stop_the_receiver(self):
        def broken():
            raise RuntimeError("no driver")
        receiver = kr.Receiver(broken, bind="127.0.0.1", port=0)
        receiver.start()
        self.addCleanup(receiver.stop)
        pad = SimController(receiver.port)
        self.addCleanup(pad.close)
        pad.discover()
        self.assertIsNone(pad.recv_type(kr.OFFER, 0.3))
        self.assertEqual(receiver.sessions(), [])


if __name__ == "__main__":
    unittest.main()
