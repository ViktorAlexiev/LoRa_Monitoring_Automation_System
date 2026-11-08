# -*- coding: utf-8 -*-
"""Системна симулация на ESP-IDF прошивките.

РЕАЛНИТЕ приложения (sensor, repeater, gateway, executor, executor_wifi) работят като ОТДЕЛНИ процеси върху
симулатора (виж system/sys_common.h) в ОБЩ виртуален час. Този файл е координаторът: държи часа, "ефира" (кой кого чува,
колизии, заетост на канала за CAD) и "бекенда" (MQTT към/от gateway-я и Wi-Fi executor-а). Така се проверява
целият път, не само отделните модули:

    sensor -> (repeater) -> gateway -> MQTT           и           MQTT -> gateway -> executor -> ACK -> MQTT

Какво НЕ е тествано: реален ефир (затихване, мултипът, capture ефект), реален ток, реални Wi-Fi/MQTT закъснения."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

from test_compat import (T_ACK, T_CMD, T_EXEC_HB, T_REP_HB, T_SENSOR, T_STATE, T_STATE_ACK, T_STATE_REQ, T_STATE_RESTART,
                         KEY_HEX, pad6, ref_parse)

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
EXE = {k: os.path.join(BUILD, "sys_%s.exe" % k) for k in ("sensor", "repeater", "gateway", "executor", "executor_wifi")}
START_US = 1_000_000
S = 1_000_000          # микросекунди в секунда


class Frame:
    def __init__(self, src, t0, t1, freq, hexs):
        self.src, self.t0, self.t1, self.freq, self.hex = src, t0, t1, freq, hexs
        self.data = bytes.fromhex(hexs)
        self.delivered = False
        self.recv = {}


class Pub:
    def __init__(self, t, node, topic, qos, retain, payload):
        self.t, self.node, self.topic, self.qos, self.retain, self.payload = t, node, topic, qos, retain, payload

    @property
    def json(self):
        return json.loads(self.payload)


class Node:
    def __init__(self, world, name, kind, **args):
        self.world, self.name, self.kind = world, name, kind
        cmd = [EXE[kind]]
        for k, v in args.items():
            cmd += ["--" + k, str(v)]
        cmd += ["--t0", str(world.now)]
        self.err = tempfile.TemporaryFile()
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err, text=True,
                                  encoding="utf-8", bufsize=1)
        self.next = world.now
        self.now = world.now
        self._read()

    def _read(self):
        res = {}
        while True:
            line = self.p.stdout.readline()
            if not line:
                self.err.seek(0)
                raise RuntimeError("%s: процесът приключи неочаквано: %s" % (self.name, self.err.read().decode("utf8", "replace")[-500:]))
            line = line.rstrip("\n")
            if line == "END":
                return res
            tag, _, rest = line.partition(" ")
            if tag == "S":
                t0, t1, freq, h = rest.split()
                self.world.on_frame(Frame(self.name, int(t0), int(t1), int(freq), h))
            elif tag == "P":
                topic, qos, retain, payload = rest.split(" ", 3)
                self.world.pubs.append(Pub(self.world.now, self.name, topic, int(qos), retain == "1", payload))
            elif tag == "N":
                self.next = int(rest)
            elif tag == "T":
                self.now = int(rest)
            elif tag == "R":
                res["R"] = rest == "1"
            elif tag in ("STAT", "PINS", "LOGCOUNT"):
                res[tag] = rest

    def cmd(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self._read()

    def step(self, us):
        return self.cmd("STEP %d" % us)

    def stat(self):
        d = {}
        for kv in self.cmd("STAT")["STAT"].split():
            k, v = kv.split("=")
            d[k] = int(v)
        return d

    def pins(self):
        return {int(a): int(b) for a, b in (kv.split("=") for kv in self.cmd("PINS")["PINS"].split())}

    def logcount(self, needle):
        return int(self.cmd("LOG " + needle)["LOGCOUNT"])

    def close(self):
        try:
            self.p.stdin.write("QUIT\n")
            self.p.stdin.flush()
        except Exception:
            pass
        try:
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()
        self.err.close()


class World:
    def __init__(self, collisions=True):
        self.now = START_US
        self.nodes = {}
        self.links = set()
        self.frames = []
        self.pubs = []
        self.actions = []
        self.collisions = collisions
        self.sent = {}

    # ---- топология ----
    def add(self, name, kind, **args):
        n = Node(self, name, kind, **args)
        self.nodes[name] = n
        return n

    def link(self, a, b, both=False):
        self.links.add((a, b))
        if both:
            self.links.add((b, a))

    def hears(self, src, dst):
        return (src, dst) in self.links

    # ---- ефир ----
    def on_frame(self, f):
        self.sent.setdefault(f.src, []).append(f.hex)
        self.frames.append(f)
        for name, n in self.nodes.items():
            if name != f.src and self.hears(f.src, name):
                n.cmd("ENV %d %d %d" % (f.freq, f.t0, f.t1))          # CAD на слушателите вижда активността

    def collided(self, f, dst):
        if not self.collisions:
            return False
        for g in self.frames:
            if g is f or abs(g.freq - f.freq) >= 15000:
                continue
            if self.hears(g.src, dst) and g.t0 < f.t1 and g.t1 > f.t0:
                return True
        return False

    def deliver(self, f):
        f.delivered = True
        for name, n in list(self.nodes.items()):
            if name == f.src or not self.hears(f.src, name):
                continue
            crc = not self.collided(f, name)
            r = n.cmd("RX %d %d %s" % (f.freq, 1 if crc else 0, f.hex))
            f.recv[name] = (crc, r.get("R", False))
        if len(self.frames) > 400:
            self.frames = [g for g in self.frames if g.t1 > self.now - 60 * S]

    # ---- бекенд ----
    def mq(self, node, topic, payload):
        if isinstance(payload, dict):
            payload = json.dumps(payload)
        return self.nodes[node].cmd("MQ %s %s" % (topic, payload)).get("R", False)

    def at(self, sec, fn):
        self.actions.append((START_US + int(sec * S), fn))
        self.actions.sort(key=lambda x: x[0])

    # ---- време ----
    def run(self, seconds):
        self.run_until(self.now + int(seconds * S))

    def run_until(self, end):
        while self.now < end:
            cands = [end]
            cands += [n.next for n in self.nodes.values() if n.next > self.now]
            if any(n.next <= self.now for n in self.nodes.values()):
                cands.append(self.now + 100)                         # устройство с готова работа: даваме му време
            cands += [f.t1 for f in self.frames if not f.delivered and f.t1 > self.now]
            cands += [t for t, _ in self.actions if t > self.now]
            nxt = min(cands)
            if nxt <= self.now:
                nxt = self.now + 200
            step = nxt - self.now
            for n in self.nodes.values():
                n.step(step)
            self.now = nxt
            for f in list(self.frames):
                if not f.delivered and f.t1 <= self.now:
                    self.deliver(f)
            while self.actions and self.actions[0][0] <= self.now:
                _, fn = self.actions.pop(0)
                fn()

    def close(self):
        for n in self.nodes.values():
            n.close()

    # ---- помощници ----
    def published(self, topic, node=None):
        return [p for p in self.pubs if p.topic == topic and (node is None or p.node == node)]

    def t_rel(self, t):
        return (t - START_US) / S


def sensor_frames(world, src=None):
    return [f for f in world.frames if len(f.data) == 22 and (src is None or f.src == src)]


def have_exes():
    return all(os.path.exists(p) for p in EXE.values())


@unittest.skipUnless(have_exes(), "няма драйвери (пусни ./run_system.sh)")
class SystemCase(unittest.TestCase):
    def world(self, **kw):
        w = World(**kw)
        self.addCleanup(w.close)
        return w

    def gateway(self, w, name="GW", gid="G001", freq=433000000, **extra):
        return w.add(name, "gateway", id=gid, key=KEY_HEX, freq=freq, net=1, seed=11, **extra)

    def sensor(self, w, name, sid, freq=433000000, seed=21, **extra):
        return w.add(name, "sensor", id=sid, key=KEY_HEX, freq=freq, seed=seed, **extra)

    def repeater(self, w, name, rid="R001", rx=434000000, tx=433000000, seed=31):
        return w.add(name, "repeater", id=rid, key=KEY_HEX, freq=rx, freqtx=tx, seed=seed)

    def executor(self, w, name="EX", eid="E007", freq=433000000, consumers="V01:4,P01:5", seed=41):
        return w.add(name, "executor", id=eid, key=KEY_HEX, freq=freq, consumers=consumers, seed=seed)


# ============================================================================= sensor -> gateway -> MQTT
class TestUplink(SystemCase):
    def test_sensor_direct_to_gateway_cycle_after_cycle(self):
        w = self.world()
        gw = self.gateway(w)
        w.run(3)                                              # gateway готов: Wi-Fi + MQTT
        sn = self.sensor(w, "S1", "S001")
        sn.cmd("SET air_t 21.5")
        w.link("S1", "GW")
        w.run(2000)
        pubs = w.published("sensors")
        self.assertEqual(len(pubs), 3)                        # ~0 s, ~905 s, ~1810 s
        for p in pubs:
            j = p.json
            self.assertEqual(j["id"], "S001")
            self.assertAlmostEqual(j["soil_t"], 25.0, delta=0.05)
            self.assertAlmostEqual(j["soil_h"], 50.0, delta=0.05)
            self.assertAlmostEqual(j["air_t"], 21.5, delta=0.05)
            self.assertFalse(p.retain)
            self.assertEqual(p.qos, 0)
        gaps = [(b.t - a.t) / S for a, b in zip(pubs, pubs[1:])]
        for g in gaps:
            self.assertTrue(903 <= g <= 908, gaps)            # 113 x 8 s + работата на цикъла
        # първото показание пристига до ~1.5 s след старта на сензора (jitter <= 1 s + airtime)
        self.assertLess((pubs[0].t - (START_US + 3 * S)) / S, 1.6)

    def test_sensor_behind_a_repeater_reaches_the_gateway_and_the_gateway_never_hears_it_directly(self):
        w = self.world()
        self.gateway(w)
        self.repeater(w, "R1")
        w.run(3)
        self.sensor(w, "S1", "S001", freq=434000000)
        w.link("S1", "R1")
        w.link("R1", "GW")
        w.run(1000)
        pubs = w.published("sensors")
        self.assertEqual(len(pubs), 2)
        sf = sensor_frames(w)
        # 1 кадър от сензора на 434 MHz и препратен (байт по байт същият) на 433 MHz
        for cyc in range(2):
            orig = [f for f in sf if f.src == "S1"][cyc]
            fwd = [f for f in sf if f.src == "R1" and f.hex == orig.hex]
            self.assertEqual(len(fwd), 1)
            self.assertEqual(fwd[0].freq // 1000000, 433)
            self.assertEqual(orig.freq // 1000000, 434)
        self.assertFalse(any(r[1] for f in sf if f.src == "S1" for k, r in f.recv.items() if k == "GW"))

    def test_two_repeaters_give_duplicates_that_the_gateway_publishes_once(self):
        w = self.world()
        self.gateway(w)
        self.repeater(w, "R1", "R001", seed=31)
        self.repeater(w, "R2", "R002", seed=32)
        w.run(3)
        self.sensor(w, "S1", "S001", freq=434000000)
        for r in ("R1", "R2"):
            w.link("S1", r)
            w.link(r, "GW")
        w.link("R1", "R2", both=True)
        w.run(2000)
        sf = sensor_frames(w)
        fwd = [f for f in sf if f.src in ("R1", "R2")]
        pubs = w.published("sensors")
        cycles = len([f for f in sf if f.src == "S1"])
        self.assertEqual(cycles, 3)
        self.assertGreaterEqual(len(fwd), 3)                  # поне един препращач за цикъл (колизия на двата е възможна)
        self.assertEqual(len(pubs), cycles)                    # дубликатите не стигат до MQTT
        hexes = [p.payload for p in pubs]
        self.assertEqual(len(set(hexes)), len(hexes))

    def test_heartbeats_of_repeater_and_executor_reach_mqtt(self):
        w = self.world()
        self.gateway(w)
        self.repeater(w, "R1", "R001")
        self.executor(w, "EX", "E007")
        w.link("R1", "GW")
        w.link("EX", "GW", both=True)
        w.run(1000)
        hb = w.published("heartbeat", "GW")
        ids = [p.json["id"] for p in hb]
        self.assertGreaterEqual(ids.count("R001"), 3)
        self.assertGreaterEqual(ids.count("E007"), 3)
        self.assertGreaterEqual(ids.count("G001"), 30)         # собствен на gateway-я: на всеки 30 s
        rep = [p for p in hb if p.json["id"] == "R001"][0].json
        self.assertIn("rssi", rep)
        self.assertNotIn("rssi", [p for p in hb if p.json["id"] == "E007"][0].json)


# ============================================================================= MQTT -> gateway -> executor -> ACK -> MQTT
class TestDownlink(SystemCase):
    def setup_pair(self, w, linked=True, t_gateway=0, t_exec=3):
        gw = self.gateway(w)
        w.run(3)
        ex = self.executor(w)
        if linked:
            w.link("GW", "EX", both=True)
        w.run(4)                                               # boot отговорът на executor-а е минал
        return gw, ex

    def test_command_switches_the_relay_and_ack_comes_back_as_status(self):
        w = self.world()
        gw, ex = self.setup_pair(w)
        t0 = w.now
        self.assertTrue(w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "V01", "com": "A1"}))
        w.run(3)
        self.assertEqual(ex.pins()[4], 1)
        self.assertEqual(ex.pins()[5], 0)
        st = w.published("commands_status")
        self.assertEqual(len(st), 1)
        self.assertEqual(st[0].json, {"M_ID": "E007", "C_ID": "V01", "com": "A1", "status": 0})
        self.assertEqual(st[0].qos, 1)
        self.assertFalse(st[0].retain)
        # ACK след паузата на executor-а (500 ms) + airtime-овете: < 1 s общо
        self.assertGreaterEqual((st[0].t - t0) / S, 0.5)
        self.assertLess((st[0].t - t0) / S, 1.0)
        # команда и ACK са в ефира еднократно (без повторения)
        cmds = [f for f in w.frames if len(f.data) == 26]
        acks = [f for f in w.frames if f.src == "EX" and len(f.data) == 20]
        self.assertEqual(len(cmds), 1)
        self.assertEqual(len(acks), 1)

    def test_off_command_nack_and_unknown_consumer(self):
        w = self.world()
        gw, ex = self.setup_pair(w)
        w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "P01", "com": "A1"}); w.run(2)
        w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "P01", "com": "B2"}); w.run(2)
        w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "XXX", "com": "A1"}); w.run(2)
        w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "V01", "com": "55"}); w.run(2)
        st = [p.json["status"] for p in w.published("commands_status")]
        self.assertEqual(st, [0, 0, 2, 2])
        self.assertEqual(ex.pins(), {4: 0, 5: 0})

    def test_executor_out_of_range_gives_timeout_after_two_retries(self):
        w = self.world()
        gw, ex = self.setup_pair(w, linked=False)
        t0 = w.now
        w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "V01", "com": "A1"})
        w.run(15)
        st = w.published("commands_status")
        self.assertEqual(len(st), 1)
        self.assertEqual(st[0].json["status"], 1)              # TIMEOUT
        self.assertEqual(len([f for f in w.frames if len(f.data) == 26]), 3)      # 1 + 2 повторения
        self.assertEqual(ex.pins()[4], 0)
        self.assertGreater((st[0].t - t0) / S, 2.0)             # 3 x ACK таймаут (~1.1 s при SF7)

    def test_lost_ack_retry_still_completes_the_command_once(self):
        w = self.world()
        gw, ex = self.setup_pair(w)
        w.links.discard(("EX", "GW"))                          # ACK-ът не стига до gateway-я
        w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "V01", "com": "A1"})
        w.run(1.5)
        w.link("EX", "GW")                                     # връзката се възстановява преди повторението
        w.run(10)
        st = w.published("commands_status")
        self.assertEqual(len(st), 1)
        self.assertEqual(st[0].json["status"], 0)              # повторената команда е потвърдена
        self.assertEqual(ex.pins()[4], 1)

    def test_restart_response_is_acked_by_the_gateway_and_retries_stop(self):
        w = self.world()
        gw = self.gateway(w)
        w.link("GW", "EX", both=True)
        w.run(3)
        ex = self.executor(w)
        w.run(10)
        rs = w.published("module_states_response")
        self.assertEqual(len(rs), 1)
        self.assertEqual(rs[0].json, {"id": "E007", "restart": True, "states": [{"id": "V01", "state": "OFF"}, {"id": "P01", "state": "OFF"}]})
        restart_frames = [f for f in w.frames if f.src == "EX" and len(f.data) == 24]
        self.assertEqual(len(restart_frames), 1)               # един, не три: gateway-ят го потвърди по радиото
        self.assertEqual(len([f for f in w.frames if f.src == "GW" and len(f.data) == 20]), 1)

    def test_executor_without_a_gateway_retries_three_times_and_gives_up(self):
        w = self.world()
        ex = self.executor(w)
        w.run(20)
        self.assertEqual(len([f for f in w.frames if f.src == "EX" and len(f.data) == 24]), 3)

    def test_executor_reboot_is_visible_to_the_backend_with_restart_true_and_all_off(self):
        w = self.world()
        gw, ex = self.setup_pair(w)
        w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "V01", "com": "A1"})
        w.run(2)
        self.assertEqual(ex.pins()[4], 1)
        n0 = len(w.published("module_states_response"))
        ex.cmd("REBOOT")
        w.run(5)
        self.assertEqual(ex.pins(), {4: 0, 5: 0})              # безопасно състояние след рестарт
        rs = w.published("module_states_response")
        self.assertEqual(len(rs), n0 + 1)
        self.assertTrue(rs[-1].json["restart"])
        self.assertEqual([s["state"] for s in rs[-1].json["states"]], ["OFF", "OFF"])

    def test_state_request_roundtrip_with_real_pin_states(self):
        w = self.world()
        gw, ex = self.setup_pair(w)
        w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "P01", "com": "A1"})
        w.run(2)
        n0 = len(w.published("module_states_response"))
        w.mq("GW", "module_states_requests", {"M_ID": "E007"})
        w.run(3)
        rs = w.published("module_states_response")
        self.assertEqual(len(rs), n0 + 1)
        self.assertNotIn("restart", rs[-1].json)
        self.assertEqual([s["state"] for s in rs[-1].json["states"]], ["OFF", "ON"])

    def test_state_request_for_a_missing_module_times_out(self):
        w = self.world()
        gw, ex = self.setup_pair(w, linked=False)
        n0 = len(w.published("module_states_response"))
        w.mq("GW", "module_states_requests", {"M_ID": "E007"})
        w.run(15)
        rs = w.published("module_states_response")
        self.assertEqual(len(rs), n0 + 1)
        self.assertEqual(rs[-1].json, {"id": "E007", "status": "timeout"})

    def test_two_executors_on_the_same_frequency_are_addressed_independently(self):
        w = self.world()
        gw = self.gateway(w)
        w.run(3)
        e1 = self.executor(w, "EX1", "E001", consumers="V01:4", seed=41)
        e2 = self.executor(w, "EX2", "E002", consumers="V01:4", seed=42)
        for e in ("EX1", "EX2"):
            w.link("GW", e, both=True)
        w.run(15)
        w.mq("GW", "commands", {"M_ID": "E002", "C_ID": "V01", "com": "A1"})
        w.run(3)
        self.assertEqual(e1.pins()[4], 0)
        self.assertEqual(e2.pins()[4], 1)
        st = w.published("commands_status")
        self.assertEqual(st[-1].json["M_ID"], "E002")

    def test_executor_behind_sf12_slow_link_still_works(self):
        w = self.world()
        gw = w.add("GW", "gateway", id="G001", key=KEY_HEX, freq=433000000, net=1, seed=11, sf=12, bw=62500)
        w.run(3)
        ex = w.add("EX", "executor", id="E007", key=KEY_HEX, freq=433000000, consumers="V01:4", seed=41, sf=12, bw=62500)
        w.link("GW", "EX", both=True)
        w.run(40)
        w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "V01", "com": "A1"})
        w.run(40)
        self.assertEqual(ex.pins()[4], 1)
        self.assertEqual(w.published("commands_status")[-1].json["status"], 0)


# ============================================================================= Wi-Fi executor
class TestWifiExecutor(SystemCase):
    def test_wifi_executor_takes_commands_over_mqtt_and_is_independent_of_the_lora_gateway(self):
        w = self.world()
        gw = self.gateway(w)
        we = w.add("WE", "executor_wifi", id="E010", key=KEY_HEX, consumers="V01:4,P01:5", net=1, seed=51)
        w.run(5)
        online = w.published("executors/E010/lwt", "WE")
        self.assertEqual(online[0].json, {"id": "E010", "online": True})
        self.assertTrue(online[0].retain)
        rs = w.published("module_states_response", "WE")
        self.assertTrue(rs[0].json["restart"])
        w.mq("WE", "wifi_commands", {"M_ID": "E010", "C_ID": "V01", "com": "A1"})
        w.run(1)
        self.assertEqual(we.pins()[4], 1)
        self.assertEqual(w.published("commands_status", "WE")[-1].json, {"M_ID": "E010", "C_ID": "V01", "com": "A1", "status": 0})
        self.assertEqual(len(w.published("commands_status", "GW")), 0)       # LoRa gateway-ят не е засегнат
        self.assertEqual(len([f for f in w.frames]), 0)                      # нищо по радиото
        w.mq("WE", "wifi_commands", {"M_ID": "E999", "C_ID": "V01", "com": "B2"})   # чужда команда
        w.run(1)
        self.assertEqual(we.pins()[4], 1)
        w.mq("WE", "wifi_commands", {"M_ID": "E010", "com": "C3"})
        w.run(1)
        self.assertEqual([s["state"] for s in w.published("module_states_response", "WE")[-1].json["states"]], ["ON", "OFF"])


# ============================================================================= дълго изпълнение, много устройства
class TestLongRun(SystemCase):
    def test_six_hours_with_three_sensors_a_repeater_and_an_executor(self):
        w = self.world()
        gw = self.gateway(w)
        rp = self.repeater(w, "R1", "R001")
        ex = self.executor(w, "EX", "E007")
        w.run(3)
        sens = []
        for i, sid in enumerate(("S001", "S002", "S003")):
            sens.append(self.sensor(w, "S%d" % i, sid, freq=434000000 if i < 2 else 433000000, seed=100 + i))
        for i in range(2):
            w.link("S%d" % i, "R1")
        w.link("S2", "GW")
        w.link("R1", "GW")
        w.link("GW", "EX", both=True)
        w.link("EX", "R1")                 # executor-ът чува repeater-а, но не го слуша на неговата честота
        w.at(4000, lambda: w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "V01", "com": "A1"}))
        w.at(8000, lambda: w.mq("GW", "commands", {"M_ID": "E007", "C_ID": "V01", "com": "B2"}))
        w.run(6 * 3600)
        pubs = w.published("sensors")
        by = {}
        for p in pubs:
            by.setdefault(p.json["id"], []).append(p)
        for sid in ("S001", "S002", "S003"):
            n = len(by.get(sid, []))
            self.assertGreaterEqual(n, 22, "%s: само %d показания за 6 часа" % (sid, n))      # ~24 възможни
            self.assertLessEqual(n, 25)
        st = [p.json["status"] for p in w.published("commands_status")]
        self.assertEqual(st, [0, 0])
        self.assertEqual(ex.pins()[4], 0)
        hb = w.published("heartbeat", "GW")
        ids = [p.json["id"] for p in hb]
        self.assertGreaterEqual(ids.count("R001"), 68)         # 21600 / 304 = 71
        self.assertGreaterEqual(ids.count("E007"), 68)
        for n in (gw, rp, ex):
            s = n.stat()
            self.assertEqual(s["restarted"], 0, n.name)
            self.assertEqual(s["overrun"], 0, n.name)
        for n in (rp, ex):
            s = n.stat()
            self.assertGreater(s["idle_us"] / s["total_us"], 0.995, n.name)    # спи >99.5 % от шест часа
            self.assertGreater(s["sleeps"], 100)
        # никога един и същ шифрован кадър два пъти от един подател (няма повторен nonce), през целите 6 часа
        for src, hexes in w.sent.items():
            if src in ("EX", "R1", "S0", "S1", "S2", "GW"):
                self.assertEqual(len(hexes), len(set(hexes)), "повторен кадър от " + src)


if __name__ == "__main__":
    unittest.main(verbosity=2)
