# -*- coding: utf-8 -*-
"""Диференциален тест на СЪВМЕСТИМОСТ между новите ESP-IDF прошивки и старите Arduino прошивки.

Едни и същи кадри/съобщения се подават на ДВЕТЕ реализации (старата - през драйверите в tests/native/esp32,
новата - през драйверите в espidf/compat) и резултатите се сравняват. Арбитър на wire формата е независима
Python реализация (библиотеката `cryptography`: AES-CTR + AES-CMAC), т.е. нито едната прошивка, нито тестовата
AES база не се приемат на доверие.

Какво доказва: ESP-IDF и Arduino устройства могат да работят в ЕДНА LoRa мрежа (кадрите се разбират в двете
посоки) и бекендът получава еднакви MQTT съобщения от двата вида gateway."""
import json
import os
import struct
import subprocess
import sys
import unittest

from cryptography.hazmat.primitives import cmac
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

HERE = os.path.dirname(os.path.abspath(__file__))
OLD = os.path.normpath(os.path.join(HERE, "..", "esp32"))
BUILD = os.path.join(HERE, "build")
KEY_HEX = "00112233445566778899aabbccddeeff"
KEY = bytes.fromhex(KEY_HEX)

OLD_SENSOR = os.path.join(OLD, "sensor_capture.exe")
OLD_GATEWAY = os.path.join(OLD, "gateway_rx.exe")
OLD_EXECUTOR = os.path.join(OLD, "executor_rx.exe")
NEW_SENSOR = os.path.join(BUILD, "compat_sensor.exe")
NEW_GATEWAY = os.path.join(BUILD, "compat_gateway.exe")
NEW_EXECUTOR = os.path.join(BUILD, "compat_executor.exe")

T_SENSOR, T_REP_HB, T_EXEC_HB, T_ACK, T_CMD, T_STATE_REQ, T_STATE, T_STATE_RESTART, T_STATE_ACK = 1, 2, 3, 4, 5, 6, 7, 8, 9


# ----------------------------------------------------------------------------- независим еталон на протокола
def pad6(s):
    b = s.encode() if isinstance(s, str) else s
    return b + bytes(6 - len(b))


def ref_wire(t, sender, ctr, pt=b"", key=KEY):
    sender = pad6(sender)
    nonce = bytes([t]) + sender + struct.pack(">I", ctr) + bytes(5)
    enc = Cipher(algorithms.AES(key), modes.CTR(nonce)).encryptor()
    ct = enc.update(pt) + enc.finalize()
    c = cmac.CMAC(algorithms.AES(key))
    c.update(nonce + ct)
    return sender + ct + struct.pack(">I", ctr) + c.finalize()[:4]


def ref_parse(frame, t, key=KEY):
    """-> (sender:str, ctr:int, plaintext:bytes) или None при невалиден MAC"""
    if len(frame) < 14:
        return None
    sender, ct, ctr, tag = frame[:6], frame[6:-8], struct.unpack(">I", frame[-8:-4])[0], frame[-4:]
    nonce = bytes([t]) + sender + frame[-8:-4] + bytes(5)
    c = cmac.CMAC(algorithms.AES(key))
    c.update(nonce + ct)
    if c.finalize()[:4] != tag:
        return None
    dec = Cipher(algorithms.AES(key), modes.CTR(nonce)).decryptor()
    return sender.rstrip(b"\0").decode(), ctr, dec.update(ct) + dec.finalize()


def sensor_pt(v):
    return b"".join(struct.pack("<h", x) for x in v)


def downlink(target, t, sender, ctr, pt=b""):
    return pad6(target) + ref_wire(t, sender, ctr, pt)


def cmd_pt(cid, com):
    return cid.encode().ljust(4, b"\0") + bytes([com, 0xFF])


def ack_pt(cid, com, status):
    return cid.encode().ljust(4, b"\0") + bytes([com, status])


def state_pt(entries):
    return b"".join(c.encode().ljust(4, b"\0") + bytes([s]) for c, s in entries)


# ----------------------------------------------------------------------------- пускане на драйверите
def run(exe, args, lines, timeout=120):
    p = subprocess.run([exe] + [str(a) for a in args], input="\n".join(lines) + "\n", capture_output=True, text=True,
                       timeout=timeout)
    assert p.returncode == 0, "%s: %s" % (exe, p.stderr)
    return p.stdout.splitlines()


def tx_frames(out):
    return [bytes.fromhex(l.split()[2]) for l in out if l.startswith("TX ")]


def pubs(out):
    r = []
    for l in out:
        if l.startswith("PUB "):
            _, t, topic, retained, payload = l.split(" ", 4)
            r.append((topic, retained == "1", payload))
    return r


GW_ARGS = ["G001", KEY_HEX, 433000000, 7, 125000]


def gateway(exe, cmds):
    return run(exe, GW_ARGS, cmds)


def old_gw_cmds(cmds):
    """старият драйвер: след всяка команда времето се върти допълнително (loop() работи на обиколки)"""
    r = []
    for c in cmds:
        r.append(c)
        if c.startswith("MQTT"):
            r.append("RUN 60 25")
    return r


def have_old():
    return all(os.path.exists(p) for p in (OLD_SENSOR, OLD_GATEWAY, OLD_EXECUTOR))


def have_new():
    return all(os.path.exists(p) for p in (NEW_SENSOR, NEW_GATEWAY, NEW_EXECUTOR))


def norm(payload):
    """JSON без полетата, които зависят от момента/ефира (rssi, snr, ts)"""
    d = json.loads(payload)
    for k in ("rssi", "snr", "ts"):
        d.pop(k, None)
    return d


# ============================================================================= sensor: стар и нов -> един и същ формат
@unittest.skipUnless(have_old() and have_new(), "липсват драйверите (виж ../esp32/run_esp32.sh и run_compat.sh)")
class TestSensorFrames(unittest.TestCase):
    CASES = [(22.5, 55.0, 1, 0), (-4.3, 88.7, 1, 0), (40.0, 10.0, 1, 0), (22.5, 55.0, 0, 0), (22.5, 55.0, 1, 1)]

    def decode(self, out):
        frames = [l for l in out if l.startswith("FRAME")]
        res = []
        for l in frames:
            h = l.split()[1]
            f = bytes.fromhex(h)
            self.assertEqual(len(f), 22)
            p = ref_parse(f, T_SENSOR)
            self.assertIsNotNone(p, "невалиден MAC на кадър " + h)
            res.append((p[0], struct.unpack("<4h", p[2]), l))
        return res

    def test_both_produce_the_same_sensor_values_and_sleep(self):
        lines = ["CYCLE %s %s %d %d" % c for c in self.CASES]
        old = self.decode(run(OLD_SENSOR, ["S001", KEY_HEX, 433000000, 7, 125000], lines))
        new = self.decode(run(NEW_SENSOR, ["S001", KEY_HEX, 433000000, 7, 125000], lines))
        self.assertEqual(len(old), len(new))
        for (so, vo, lo), (sn, vn, ln) in zip(old, new):
            self.assertEqual(so, sn)
            for a, b in zip(vo, vn):
                if a == -32768 or b == -32768:
                    self.assertEqual(a, b, "различно 'няма данни': %s / %s" % (lo, ln))
                else:
                    self.assertLessEqual(abs(a - b), 4, "стойностите се различават: %s / %s" % (vo, vn))
            self.assertIn("SLEEP=904", lo)
            self.assertIn("SLEEP=904", ln)

    def test_new_sensor_frame_decodes_with_the_independent_reference(self):
        new = self.decode(run(NEW_SENSOR, ["S777", KEY_HEX, 868100000, 9, 125000], ["CYCLE 21.0 60.0 1 0"]))
        self.assertEqual(new[0][0], "S777")
        v = new[0][1]
        self.assertAlmostEqual(v[0] / 100, 25.0, delta=0.05)
        self.assertAlmostEqual(v[1] / 100, 50.0, delta=0.05)
        self.assertAlmostEqual(v[2] / 100, 21.0, delta=0.05)
        self.assertAlmostEqual(v[3] / 100, 60.0, delta=0.1)

    def test_counters_increase_across_cycles_in_both(self):
        for exe in (OLD_SENSOR, NEW_SENSOR):
            out = run(exe, ["S001", KEY_HEX, 433000000, 7, 125000], ["CYCLE 20.0 50.0 1 0"] * 3)
            ctrs = [ref_parse(bytes.fromhex(l.split()[1]), T_SENSOR)[1] for l in out if l.startswith("FRAME")]
            self.assertEqual(len(ctrs), 3)
            self.assertEqual(ctrs, sorted(set(ctrs)), exe)


# ============================================================================= gateway: стар и нов -> еднакви MQTT съобщения
@unittest.skipUnless(have_old() and have_new(), "липсват драйверите")
class TestGatewayUplink(unittest.TestCase):
    def feed(self, frames):
        cmds = ["RX " + f.hex() for f in frames]
        old = pubs(gateway(OLD_GATEWAY, old_gw_cmds(cmds)))
        new = pubs(gateway(NEW_GATEWAY, cmds))
        return old, new

    def compare(self, frames, expect_topics=None):
        old, new = self.feed(frames)
        old = [(t, r, norm(p)) for t, r, p in old if t != "heartbeat" or True]
        new = [(t, r, norm(p)) for t, r, p in new]
        # собствените heartbeat-и на gateway-я (по време) не се сравняват
        old = [x for x in old if not (x[0] == "heartbeat" and x[2].get("id") == "G001")]
        new = [x for x in new if not (x[0] == "heartbeat" and x[2].get("id") == "G001")]
        self.assertEqual(len(old), len(new), "\nстар: %s\nнов: %s" % (old, new))
        for o, n in zip(old, new):
            self.assertEqual(o[0], n[0])
            self.assertFalse(n[1], "ново съобщение е retained!")
            self.assertEqual(o[2], n[2])
        return new

    def test_sensor_frames_same_json(self):
        fr = [ref_wire(T_SENSOR, "S001", 10 + i, sensor_pt(v)) for i, v in enumerate(
            [(2250, 5500, 2000, 6000), (-4000, 0, 8500, 10000), (-32768, -32768, 2000, -32768), (-1, 1, 5, 99), (12345, -12345, 0, 0)])]
        new = self.compare(fr)
        self.assertEqual(len(new), 5)
        self.assertEqual(new[2][2]["soil_t"], -32768)

    def test_duplicates_and_garbage_same_behaviour(self):
        f = ref_wire(T_SENSOR, "S001", 1, sensor_pt((1, 2, 3, 4)))
        bad = bytearray(f); bad[10] ^= 1
        other = ref_wire(T_SENSOR, "S001", 2, sensor_pt((1, 2, 3, 4)), key=bytes(16))
        new = self.compare([f, f, bytes(bad), other, b"\x01" * 30, b"\x02" * 3, f])
        self.assertEqual(len(new), 1)

    def test_heartbeats_and_acks_and_states_same_json(self):
        fr = [ref_wire(T_REP_HB, "R001", 1), ref_wire(T_EXEC_HB, "E007", 2),
              ref_wire(T_STATE, "E007", 3, state_pt([("V01", 1), ("P01", 0)])),
              ref_wire(T_STATE_RESTART, "E007", 4, state_pt([("V01", 0)])),
              ref_wire(T_STATE, "E001", 5, b""),
              ref_wire(T_STATE, "E002", 6, state_pt([("C%d" % i, i % 2) for i in range(10)]))]
        new = self.compare(fr)
        self.assertEqual(len(new), 6)
        self.assertTrue(new[3][2].get("restart"))
        self.assertNotIn("restart", new[2][2])

    def test_restart_state_response_gets_the_same_radio_ack(self):
        f = ref_wire(T_STATE_RESTART, "E007", 4, state_pt([("V01", 0)]))
        for exe, mk in ((OLD_GATEWAY, old_gw_cmds), (NEW_GATEWAY, lambda c: c)):
            out = gateway(exe, mk(["RX " + f.hex(), "TICK 500"]))
            txs = tx_frames(out)
            self.assertEqual(len(txs), 1, exe)
            self.assertEqual(len(txs[0]), 20)
            self.assertEqual(txs[0][:6], pad6("E007"))
            p = ref_parse(txs[0][6:], T_STATE_ACK)
            self.assertIsNotNone(p, exe)
            self.assertEqual(p[0], "G001")


@unittest.skipUnless(have_old() and have_new(), "липсват драйверите")
class TestGatewayDownlink(unittest.TestCase):
    def first_tx(self, exe, cmds):
        out = gateway(exe, cmds)
        return tx_frames(out), pubs(out)

    def test_command_frame_has_the_same_content(self):
        cmd = ["MQTT commands " + json.dumps({"M_ID": "E007", "C_ID": "V01", "com": "A1"})]
        old, _ = self.first_tx(OLD_GATEWAY, old_gw_cmds(cmd))
        new, _ = self.first_tx(NEW_GATEWAY, cmd + ["TICK 300"])
        for frames in (old, new):
            self.assertGreaterEqual(len(frames), 1)
        for f in (old[0], new[0]):
            self.assertEqual(len(f), 26)
            self.assertEqual(f[:6], pad6("E007"))
        po, pn = ref_parse(old[0][6:], T_CMD), ref_parse(new[0][6:], T_CMD)
        self.assertIsNotNone(po)
        self.assertIsNotNone(pn)
        self.assertEqual(po[0], pn[0])             # sender G001
        self.assertEqual(po[2], pn[2])             # C_ID, com, 0xFF - еднакво

    def test_state_request_frame_has_the_same_content(self):
        cmd = ["MQTT module_states_requests " + json.dumps({"M_ID": "E007"})]
        old, _ = self.first_tx(OLD_GATEWAY, old_gw_cmds(cmd))
        new, _ = self.first_tx(NEW_GATEWAY, cmd + ["TICK 300"])
        for f in (old[0], new[0]):
            self.assertEqual(len(f), 20)
            self.assertEqual(f[:6], pad6("E007"))
            self.assertIsNotNone(ref_parse(f[6:], T_STATE_REQ))

    def test_ack_to_command_gives_the_same_status_json_and_timeout_the_same(self):
        cmd = ["MQTT commands " + json.dumps({"M_ID": "E007", "C_ID": "V01", "com": "A1"})]
        ack = ref_wire(T_ACK, "E007", 50, ack_pt("V01", 0xA1, 0))
        nack = ref_wire(T_ACK, "E007", 51, ack_pt("V01", 0xA1, 2))
        for a, status in ((ack, 0), (nack, 2)):
            old = pubs(gateway(OLD_GATEWAY, old_gw_cmds(cmd + ["RX " + a.hex()])))
            new = pubs(gateway(NEW_GATEWAY, cmd + ["TICK 300", "RX " + a.hex()]))
            so = [json.loads(p) for t, r, p in old if t == "commands_status"]
            sn = [json.loads(p) for t, r, p in new if t == "commands_status"]
            self.assertEqual(so, sn)
            self.assertEqual(sn[0]["status"], status)
        # без ACK: 2 повторения и TIMEOUT (status 1) и в двата
        old = gateway(OLD_GATEWAY, old_gw_cmds(cmd) + ["RUN 800 50"])
        new = gateway(NEW_GATEWAY, cmd + ["TICK 30000"])
        self.assertEqual(len(tx_frames(old)), len(tx_frames(new)), "брой предавания (1 + 2 повторения)")
        self.assertEqual(len(tx_frames(new)), 3)
        so = [json.loads(p) for t, r, p in pubs(old) if t == "commands_status"]
        sn = [json.loads(p) for t, r, p in pubs(new) if t == "commands_status"]
        self.assertEqual(so, sn)
        self.assertEqual(sn[0]["status"], 1)

    def test_subscriptions_and_retain(self):
        out = gateway(NEW_GATEWAY, ["TICK 100"])
        subs = [tuple(l.split()[1:]) for l in out if l.startswith("SUB ")]
        self.assertIn(("commands", "1"), subs)
        self.assertIn(("module_states_requests", "1"), subs)


# ============================================================================= executor: стар и нов -> същото поведение
@unittest.skipUnless(have_old() and have_new(), "липсват драйверите")
class TestExecutor(unittest.TestCase):
    ARGS = ["E007", KEY_HEX, 433000000, 7, 125000, "V01:4,P01:5"]

    def scenario(self, exe, cmds):
        out = run(exe, self.ARGS, cmds)
        return out

    @staticmethod
    def pins(out):
        p = [l for l in out if l.startswith("PINS")]
        return p[-1].split()[1:] if p else None

    @staticmethod
    def decode_tx(out):
        res = []
        for f in tx_frames(out):
            for name, t in (("ACK", T_ACK), ("HB", T_EXEC_HB), ("STATE", T_STATE), ("RESTART", T_STATE_RESTART)):
                p = ref_parse(f, t)
                if p:
                    res.append((name, p[0], p[2]))
                    break
            else:
                res.append(("?", None, f))
        return res

    def test_boot_restart_response_and_its_ack(self):
        ack = downlink("E007", T_STATE_ACK, "G001", 900)
        for exe in (OLD_EXECUTOR, NEW_EXECUTOR):
            out = self.scenario(exe, ["RAW " + ack.hex(), "LOOP 1500", "PINS"])
            d = self.decode_tx(out)
            r = [x for x in d if x[0] == "RESTART"]                                # (старият драйвер върти и heartbeat-и при сън)
            self.assertEqual(len(r), 1, exe)                                        # без ACK бяха 3 пъти; с ACK - един
            self.assertEqual(r[0][1], "E007")
            self.assertEqual(r[0][2], state_pt([("V01", 0), ("P01", 0)]))
            self.assertEqual(self.pins(out), ["4=0", "5=0"])

    def test_commands_same_pins_and_same_acks(self):
        ack = downlink("E007", T_STATE_ACK, "G001", 900)
        cmds = ["RAW " + ack.hex(), "LOOP 100",
                "RAW " + downlink("E007", T_CMD, "G001", 1000, cmd_pt("V01", 0xA1)).hex(), "LOOP 900", "PINS",
                "RAW " + downlink("E007", T_CMD, "G001", 1001, cmd_pt("P01", 0xA1)).hex(), "LOOP 900",
                "RAW " + downlink("E007", T_CMD, "G001", 1002, cmd_pt("V01", 0xB2)).hex(), "LOOP 900", "PINS",
                "RAW " + downlink("E007", T_CMD, "G001", 1003, cmd_pt("V01", 0x55)).hex(), "LOOP 900",      # невалидна команда
                "RAW " + downlink("E007", T_CMD, "G001", 1004, cmd_pt("ZZZ", 0xA1)).hex(), "LOOP 900",      # няма такъв консуматор
                "RAW " + downlink("E999", T_CMD, "G001", 1005, cmd_pt("V01", 0xA1)).hex(), "LOOP 900",      # чужд модул
                "PINS"]
        res = []
        for exe in (OLD_EXECUTOR, NEW_EXECUTOR):
            out = self.scenario(exe, cmds)
            d = [x for x in self.decode_tx(out) if x[0] == "ACK"]
            res.append(([x[2] for x in d], [l for l in out if l.startswith("PINS")]))
        self.assertEqual(res[0][1], res[1][1])                                    # еднакви пинове на всяка стъпка
        self.assertEqual(res[0][0], res[1][0])                                    # еднакви ACK/NACK (C_ID, com, status)
        self.assertEqual(len(res[1][0]), 5)
        statuses = [x[5] for x in res[1][0]]
        self.assertEqual(statuses, [0, 0, 0, 2, 2])

    def test_state_request_same_response(self):
        ack = downlink("E007", T_STATE_ACK, "G001", 900)
        cmds = ["RAW " + ack.hex(), "LOOP 100",
                "RAW " + downlink("E007", T_CMD, "G001", 1000, cmd_pt("P01", 0xA1)).hex(), "LOOP 900",
                "RAW " + downlink("E007", T_STATE_REQ, "G001", 1001).hex(), "LOOP 900"]
        res = []
        for exe in (OLD_EXECUTOR, NEW_EXECUTOR):
            d = [x for x in self.decode_tx(self.scenario(exe, cmds)) if x[0] == "STATE"]
            res.append([x[2] for x in d])
        self.assertEqual(res[0], res[1])
        self.assertEqual(res[1], [state_pt([("V01", 0), ("P01", 1)])])

    def test_tampered_or_foreign_key_frames_are_ignored_by_both(self):
        ack = downlink("E007", T_STATE_ACK, "G001", 900)
        good = bytearray(downlink("E007", T_CMD, "G001", 1000, cmd_pt("V01", 0xA1)))
        good[-1] ^= 1
        foreign = pad6("E007") + ref_wire(T_CMD, "G001", 1001, cmd_pt("V01", 0xA1), key=bytes(16))
        for exe in (OLD_EXECUTOR, NEW_EXECUTOR):
            out = self.scenario(exe, ["RAW " + ack.hex(), "RAW " + bytes(good).hex(), "LOOP 900", "RAW " + foreign.hex(), "LOOP 900", "PINS"])
            self.assertEqual(self.pins(out), ["4=0", "5=0"], exe)
            self.assertEqual([x[0] for x in self.decode_tx(out) if x[0] == "ACK"], [], exe)


if __name__ == "__main__":
    if not have_old():
        print("ЗАБЕЛЕЖКА: старите драйвери липсват - събери ги с ../esp32/run_esp32.sh; тестовете се пропускат")
    unittest.main(verbosity=2)
