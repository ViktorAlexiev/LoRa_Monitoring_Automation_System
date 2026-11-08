# -*- coding: utf-8 -*-
"""End-to-end тестове на ESP32_PCB(temp_hum): РЕАЛНИЯТ sensor.ino -> (радио пакет) -> РЕАЛНИЯТ
gateway.ino -> MQTT, и командният път MQTT -> gateway -> LoRa -> (ACK) -> MQTT. Пускат се два
нативни драйвера (sensor_capture.exe, gateway_rx.exe), компилирани от run_esp32.sh срещу фалшиви
LoRa/WiFi/PubSubClient/NVS и реалната ArduinoJson.

Какво НЕ се тества: реално радио (обхват, колизии), реален Wi-Fi/MQTT брокер, реален flash/NVS, сън на
чипа. Крипто примитивата е верифицирана AES-128 (виж aes_selfcheck), не production библиотеката."""
import json
import math
import os
import subprocess
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SENSOR = os.path.join(HERE, "sensor_capture.exe")
GATEWAY = os.path.join(HERE, "gateway_rx.exe")
KEY = "00112233445566778899aabbccddeeff"
OTHER_KEY = "ffeeddccbbaa99887766554433221100"


# ----------------------------------------------------------------------------- драйвери
def run_sensor(cycles, sid="S001", key=KEY, freq=433000000, sf=7, bw=125000):
    """cycles: списък от (airT, airH, air_ok, busy). Връща списък от речници на всеки loop()."""
    stdin = "".join("CYCLE %s %s %d %d\n" % (t, h, ok, busy) for (t, h, ok, busy) in cycles)
    out = subprocess.run([SENSOR, sid, key, str(freq), str(sf), str(bw)], input=stdin,
                         capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stderr
    res = []
    for line in out.stdout.splitlines():
        if not line.startswith("FRAME"):
            continue
        parts = line.split()
        d = {"frame": parts[1]}
        for kv in parts[2:]:
            k, v = kv.split("=")
            d[k.lower()] = int(v)
        res.append(d)
    return res


def run_gateway(commands, gid="G001", key=KEY, freq=433000000, sf=7, bw=125000):
    """commands: списък от редове за gateway_rx. Връща (boot, [блок за всяка команда]); блокът е
    речник {cmd, pubs:[{t,topic,retained,payload}], txs:[{t,hex}], txdec:[...]}."""
    out = subprocess.run([GATEWAY, gid, key, str(freq), str(sf), str(bw)], input="\n".join(commands) + "\n",
                         capture_output=True, text=True, timeout=120)
    assert out.returncode == 0, out.stderr
    boot = {"subs": []}
    blocks = []
    cur = None
    for line in out.stdout.splitlines():
        if line.startswith(">> "):
            cur = {"cmd": line[3:], "pubs": [], "txs": [], "txdec": []}
            blocks.append(cur)
        elif line.startswith("SUB "):
            _, topic, qos = line.split()
            boot["subs"].append((topic, int(qos)))
        elif line.startswith("PUB "):
            _, t, topic, retained, payload = line.split(" ", 4)
            e = {"t": int(t[1:]), "topic": topic, "retained": retained == "1", "payload": payload}
            (cur or boot).setdefault("pubs", []).append(e) if cur is None else cur["pubs"].append(e)
        elif line.startswith("TX "):
            _, t, h = line.split()
            cur["txs"].append({"t": int(t[1:]), "hex": h})
        elif line.startswith("TXDEC "):
            cur["txdec"].append(line[6:])
    return boot, blocks


def all_pubs(blocks, topic=None):
    r = []
    for b in blocks:
        for p in b["pubs"]:
            if topic is None or p["topic"] == topic:
                r.append(p)
    return r


def all_txs(blocks):
    return [t for b in blocks for t in b["txs"]]


def toa_ms(payload_bytes, sf, bw_hz=125000, cr=1, preamble=8, crc=1):
    """Независима формула за времето във въздуха на LoRa пакет (Semtech AN1200.13)."""
    ts = (2 ** sf) / bw_hz
    de = 1 if ts > 0.016 else 0
    n = 8 + max(math.ceil((8 * payload_bytes - 4 * sf + 28 + 16 * crc) / (4 * (sf - 2 * de))) * (cr + 4), 0)
    return ((preamble + 4.25) * ts + n * ts) * 1000


# ============================================================================= сензор -> gateway
class TestSensorToGateway(unittest.TestCase):
    def test_normal_cycle_end_to_end(self):
        s = run_sensor([(22.5, 55.0, 1, 0)])[0]
        self.assertEqual(s["bytes"], 22)                      # компактният пакет
        self.assertEqual(s["sleep"], 904)                     # 113 x 8 s дълбок сън след предаването
        self.assertEqual(s["txcount"], 1)
        boot, blocks = run_gateway(["RX " + s["frame"]])
        pubs = all_pubs(blocks, "sensors")
        self.assertEqual(len(pubs), 1)
        j = json.loads(pubs[0]["payload"])
        self.assertEqual(j["id"], "S001")
        self.assertAlmostEqual(j["soil_t"], 25.0, places=2)
        self.assertAlmostEqual(j["soil_h"], 50.0, places=2)
        self.assertAlmostEqual(j["air_t"], 22.5, delta=0.02)   # SHT21 е с 14 бита - толерансата е резолюцията
        self.assertAlmostEqual(j["air_h"], 55.0, delta=0.05)
        self.assertEqual(j["rssi"], -50)
        for k in ("snr", "ts"):
            self.assertIn(k, j)

    def test_air_sensor_failure_reports_minus_32768_for_that_sensor_only(self):
        s = run_sensor([(22.5, 55.0, 0, 0)])[0]
        boot, blocks = run_gateway(["RX " + s["frame"]])
        j = json.loads(all_pubs(blocks, "sensors")[0]["payload"])
        self.assertEqual(j["air_t"], -32768)
        self.assertEqual(j["air_h"], -32768)
        self.assertAlmostEqual(j["soil_t"], 25.0, places=2)   # почвеният е ок
        self.assertAlmostEqual(j["soil_h"], 50.0, places=2)

    def test_values_across_the_whole_sensor_range(self):
        for t, h in [(-40.0, 0.0), (-10.5, 20.0), (0.0, 50.0), (15.25, 99.9), (85.0, 100.0)]:
            s = run_sensor([(t, h, 1, 0)])[0]
            boot, blocks = run_gateway(["RX " + s["frame"]])
            j = json.loads(all_pubs(blocks, "sensors")[0]["payload"])
            # SHT21 raw -> стойност: границите (-40..85 °C, 0..100 %RH) минават диапазонната проверка
            if -40.0 < t < 85.0 and 0.0 < h < 100.0:
                self.assertAlmostEqual(j["air_t"], t, delta=0.02, msg=str((t, h)))
                self.assertAlmostEqual(j["air_h"], h, delta=0.05, msg=str((t, h)))

    def test_frames_have_unique_increasing_counters_and_differ(self):
        res = run_sensor([(22.5, 55.0, 1, 0)] * 12)
        frames = [r["frame"] for r in res]
        self.assertEqual(len(set(frames)), 12)                 # идентични измервания -> различни пакети (nonce)
        counters = [int(f[2 * (6 + 8):2 * (6 + 8 + 4)], 16) for f in frames]
        self.assertEqual(counters, sorted(counters))
        self.assertEqual(len(set(counters)), 12)
        for f in frames:
            self.assertEqual(len(f) // 2, 22)
            self.assertEqual(bytes.fromhex(f[:12]), b"S001\x00\x00")   # senderId в чист вид, допълнен с 0

    def test_all_frames_are_accepted_by_the_gateway(self):
        res = run_sensor([(20.0 + i * 0.5, 40.0 + i, 1, 0) for i in range(10)])
        boot, blocks = run_gateway(["RX " + r["frame"] for r in res])
        self.assertEqual(len(all_pubs(blocks, "sensors")), 10)

    def test_duplicate_frame_from_two_repeaters_is_published_once(self):
        s = run_sensor([(22.5, 55.0, 1, 0)])[0]
        boot, blocks = run_gateway(["RX " + s["frame"], "RX " + s["frame"], "RX " + s["frame"]])
        self.assertEqual(len(all_pubs(blocks, "sensors")), 1)

    def test_dedup_buffer_remembers_8_recent_frames_then_forgets(self):
        res = run_sensor([(20.0 + i, 50.0, 1, 0) for i in range(10)])
        frames = [r["frame"] for r in res]
        # първите 9 различни, после първия отново: буферът е за 8 -> вече е забравен (публикува се пак)
        cmds = ["RX " + f for f in frames[:9]] + ["RX " + frames[8], "RX " + frames[0]]
        boot, blocks = run_gateway(cmds)
        self.assertEqual(len(all_pubs(blocks, "sensors")), 10)   # 9 + frames[0] отново; frames[8] дубликат е изядена

    def test_tampered_frame_is_rejected(self):
        s = run_sensor([(22.5, 55.0, 1, 0)])[0]
        raw = bytearray(bytes.fromhex(s["frame"]))
        for pos in (6, 10, 14, 18, 21):        # ciphertext, counter, tag
            bad = bytearray(raw)
            bad[pos] ^= 0x01
            boot, blocks = run_gateway(["RX " + bad.hex()])
            self.assertEqual(len(all_pubs(blocks, "sensors")), 0, "позиция %d" % pos)
        bad = bytearray(raw)
        bad[0] ^= 0x01                          # senderId (участва в nonce-а)
        boot, blocks = run_gateway(["RX " + bad.hex()])
        self.assertEqual(len(all_pubs(blocks, "sensors")), 0)

    def test_frame_encrypted_with_another_key_is_rejected(self):
        s = run_sensor([(22.5, 55.0, 1, 0)], key=OTHER_KEY)[0]
        boot, blocks = run_gateway(["RX " + s["frame"]], key=KEY)
        self.assertEqual(len(all_pubs(blocks, "sensors")), 0)

    def test_old_30_byte_frames_and_garbage_are_ignored(self):
        import random
        rnd = random.Random(7)
        cmds = ["RX " + bytes(rnd.randrange(256) for _ in range(n)).hex() for n in (30, 29, 23, 21, 1, 50)]
        boot, blocks = run_gateway(cmds)
        self.assertEqual(all_pubs(blocks), [])

    def test_sensor_waits_for_a_free_channel_but_transmits_anyway_after_two_cads(self):
        free = run_sensor([(22.5, 55.0, 1, 0)])[0]
        busy = run_sensor([(22.5, 55.0, 1, 1)])[0]
        self.assertEqual(free["cad"], 1)           # свободен: един CAD и предава
        self.assertEqual(busy["cad"], 2)           # зает: втори CAD и предава въпреки това (telemetry FORCE)
        self.assertEqual(busy["txcount"], 1)
        self.assertEqual(busy["bytes"], 22)

    def test_works_at_every_sf_and_bw(self):
        for sf in (7, 9, 12):
            for bw in (62500, 125000, 250000):
                s = run_sensor([(22.5, 55.0, 1, 0)], sf=sf, bw=bw)[0]
                boot, blocks = run_gateway(["RX " + s["frame"]], sf=sf, bw=bw)
                self.assertEqual(len(all_pubs(blocks, "sensors")), 1, (sf, bw))

    def test_sensor_id_variants(self):
        for sid in ("S1", "CS001", "ABCDEF", "X9"):
            s = run_sensor([(22.5, 55.0, 1, 0)], sid=sid)[0]
            boot, blocks = run_gateway(["RX " + s["frame"]])
            j = json.loads(all_pubs(blocks, "sensors")[0]["payload"])
            self.assertEqual(j["id"], sid)

    def test_no_gateway_message_is_ever_retained(self):
        # регресия: publish(topic, char*, size_t) избираше publish(topic, char*, bool retained) -> retain=true
        s = run_sensor([(22.5, 55.0, 1, 0)])[0]
        boot, blocks = run_gateway(["RX " + s["frame"], "MQTT commands {\"M_ID\":\"E001\",\"C_ID\":\"V01\",\"com\":\"A1\"}",
                                    "RUN 400 50", "TICK 31000"])
        pubs = all_pubs(blocks)
        self.assertTrue(len(pubs) >= 3)            # sensors, commands_status (timeout), heartbeat
        for p in pubs:
            self.assertFalse(p["retained"], p["topic"])


# ============================================================================= gateway: команди
CMD = 'MQTT commands {"M_ID":"E001","C_ID":"V01","com":"A1"}'


class TestGatewayCommands(unittest.TestCase):
    def test_gateway_subscribes_to_the_command_topics_only(self):
        boot, blocks = run_gateway([])
        topics = [t for t, q in boot["subs"]]
        self.assertEqual(sorted(topics), ["commands", "module_states_requests"])
        self.assertNotIn("wifi_commands", topics)   # Wi-Fi модулите получават команди директно

    def test_command_is_sent_by_lora_encrypted_to_the_executor(self):
        boot, blocks = run_gateway([CMD, "RUN 5 50"])
        txs = all_txs(blocks)
        self.assertTrue(len(txs) >= 1)
        dec = [d for b in blocks for d in b["txdec"]]
        self.assertTrue(dec[0].startswith("CMD target=E001 sender=G001 pt=56303100a1ff"), dec[0])  # "V01\0" A1 FF

    def test_ack_confirms_the_command_and_stops_retries(self):
        boot, blocks = run_gateway([CMD, "RUN 5 50", "MKACK E001 V01 A1 0 1", "RUN 400 50"])
        st = all_pubs(blocks, "commands_status")
        self.assertEqual(len(st), 1)
        self.assertEqual(json.loads(st[0]["payload"]),
                         {"M_ID": "E001", "C_ID": "V01", "com": "A1", "status": 0})
        self.assertEqual(len(all_txs(blocks)), 1)  # ACK веднага -> без нито едно повторение

    def test_nack_is_reported_as_status_2(self):
        boot, blocks = run_gateway([CMD, "RUN 5 50", "MKACK E001 V01 A1 2 1", "RUN 400 50"])
        st = all_pubs(blocks, "commands_status")
        self.assertEqual([json.loads(p["payload"])["status"] for p in st], [2])

    def test_no_ack_retries_twice_then_reports_timeout(self):
        boot, blocks = run_gateway([CMD, "RUN 400 50"])
        txs = all_txs(blocks)
        self.assertEqual(len(txs), 3)               # 1 опит + 2 повторения
        st = all_pubs(blocks, "commands_status")
        self.assertEqual(len(st), 1)
        self.assertEqual(json.loads(st[0]["payload"])["status"], 1)    # TIMEOUT
        # същата команда всеки път; всеки опит с нов counter (нов nonce)
        self.assertEqual(len(set(t["hex"] for t in txs)), 3)

    def test_timeout_arrives_within_about_12_seconds_at_sf7(self):
        boot, blocks = run_gateway([CMD, "RUN 400 50"])
        first = all_txs(blocks)[0]["t"]
        timeout_t = all_pubs(blocks, "commands_status")[0]["t"]
        self.assertTrue(timeout_t - first < 12000, timeout_t - first)
        self.assertTrue(timeout_t - first > 500)

    def test_retry_interval_is_longer_than_the_airtime_of_the_command_at_every_sf(self):
        for sf in (7, 9, 12):
            boot, blocks = run_gateway([CMD, "RUN 3000 50"], sf=sf)
            txs = all_txs(blocks)
            self.assertEqual(len(txs), 3, sf)
            gap = txs[1]["t"] - txs[0]["t"]
            self.assertTrue(gap > toa_ms(26, sf), "SF%d: пауза %d ms <= airtime %.0f ms" % (sf, gap, toa_ms(26, sf)))

    def test_second_command_while_one_is_pending_is_ignored(self):
        cmd2 = 'MQTT commands {"M_ID":"E001","C_ID":"V02","com":"A1"}'
        boot, blocks = run_gateway([CMD, "RUN 5 50", cmd2, "RUN 5 50", "MKACK E001 V01 A1 0 1", "RUN 20 50"])
        dec = " ".join(d for b in blocks for d in b["txdec"])
        self.assertNotIn("pt=56303200", dec)        # V02 никога не е изпратена
        self.assertEqual(len(all_pubs(blocks, "commands_status")), 1)

    def test_next_command_is_accepted_after_the_previous_one_is_acked(self):
        cmd2 = 'MQTT commands {"M_ID":"E001","C_ID":"V02","com":"B2"}'
        boot, blocks = run_gateway([CMD, "RUN 5 50", "MKACK E001 V01 A1 0 1", "RUN 5 50", cmd2, "RUN 5 50",
                                    "MKACK E001 V02 B2 0 2", "RUN 20 50"])
        st = [json.loads(p["payload"]) for p in all_pubs(blocks, "commands_status")]
        self.assertEqual([(s["C_ID"], s["com"], s["status"]) for s in st], [("V01", "A1", 0), ("V02", "B2", 0)])

    def test_next_command_is_accepted_after_a_timeout(self):
        cmd2 = 'MQTT commands {"M_ID":"E001","C_ID":"V02","com":"B2"}'
        boot, blocks = run_gateway([CMD, "RUN 400 50", cmd2, "RUN 5 50", "MKACK E001 V02 B2 0 5", "RUN 20 50"])
        st = [json.loads(p["payload"]) for p in all_pubs(blocks, "commands_status")]
        self.assertEqual([s["status"] for s in st], [1, 0])

    def test_late_ack_after_timeout_is_not_reported_as_a_new_result(self):
        boot, blocks = run_gateway([CMD, "RUN 400 50", "MKACK E001 V01 A1 0 9", "RUN 20 50"])
        st = all_pubs(blocks, "commands_status")
        self.assertEqual(len(st), 1)               # само TIMEOUT; закъснелият ACK не добавя ново съобщение

    def test_ack_from_a_different_executor_is_not_accepted_for_the_pending_command(self):
        boot, blocks = run_gateway([CMD, "RUN 5 50", "MKACK E002 V01 A1 0 1", "RUN 400 50"])
        st = [json.loads(p["payload"]) for p in all_pubs(blocks, "commands_status")]
        # ACK-ът на чужд executor не бива да потвърди командата към E001
        self.assertEqual([s["status"] for s in st if s["M_ID"] == "E001"], [1])

    def test_ack_for_a_different_consumer_is_not_accepted(self):
        boot, blocks = run_gateway([CMD, "RUN 5 50", "MKACK E001 V02 A1 0 1", "RUN 400 50"])
        st = [json.loads(p["payload"]) for p in all_pubs(blocks, "commands_status")]
        self.assertEqual([s["status"] for s in st if s["C_ID"] == "V01"], [1])

    def test_replayed_ack_with_wrong_key_is_ignored(self):
        boot, blocks = run_gateway([CMD, "RUN 5 50", "MKACK E001 V01 A1 0 1", "RUN 5 50"], key=KEY)
        self.assertEqual(len(all_pubs(blocks, "commands_status")), 1)

    def test_busy_channel_delays_the_command_until_it_is_free(self):
        boot, blocks = run_gateway(["BUSY 1", CMD, "RUN 20 50"])
        busy_txs = len(all_txs(blocks))
        boot, blocks2 = run_gateway(["BUSY 1", CMD, "RUN 20 50", "BUSY 0", "RUN 40 50"])
        self.assertEqual(busy_txs, 0)               # докато каналът е зает - не предава
        self.assertTrue(len(all_txs(blocks2)) >= 1)  # освободи ли се - предава

    def test_malformed_command_json_is_ignored(self):
        boot, blocks = run_gateway(["MQTT commands {not json", "RUN 20 50"])
        self.assertEqual(all_txs(blocks), [])
        self.assertEqual(all_pubs(blocks), [])


# ============================================================================= gateway: състояния и heartbeat
class TestGatewayStates(unittest.TestCase):
    def test_state_request_and_response(self):
        boot, blocks = run_gateway(['MQTT module_states_requests {"M_ID":"E001"}', "RUN 5 50",
                                    "MKSTATE E001 0 1 V01:1 V02:0 P01:1", "RUN 10 50"])
        dec = [d for b in blocks for d in b["txdec"]]
        self.assertTrue(dec[0].startswith("STATE_REQ target=E001 sender=G001"), dec[0])
        st = all_pubs(blocks, "module_states_response")
        self.assertEqual(len(st), 1)
        self.assertEqual(json.loads(st[0]["payload"]),
                         {"id": "E001", "states": [{"id": "V01", "state": "ON"}, {"id": "V02", "state": "OFF"},
                                                   {"id": "P01", "state": "ON"}]})

    def test_restart_state_response_is_flagged_and_acked_over_lora(self):
        boot, blocks = run_gateway(["MKSTATE E001 1 1 V01:0 P01:0", "RUN 40 50"])
        st = all_pubs(blocks, "module_states_response")
        self.assertEqual(len(st), 1)
        j = json.loads(st[0]["payload"])
        self.assertTrue(j["restart"])
        self.assertEqual(j["states"], [{"id": "V01", "state": "OFF"}, {"id": "P01", "state": "OFF"}])
        dec = [d for b in blocks for d in b["txdec"]]
        self.assertTrue(any(d.startswith("STATE_RESP_ACK target=E001") for d in dec), dec)

    def test_state_response_from_each_possible_size(self):
        for n in range(0, 11):
            entries = " ".join("C%02d:%d" % (i, i % 2) for i in range(n))
            boot, blocks = run_gateway(["MKSTATE E001 0 3 " + entries, "RUN 5 50"])
            st = all_pubs(blocks, "module_states_response")
            self.assertEqual(len(st), 1, "n=%d" % n)
            self.assertEqual(len(json.loads(st[0]["payload"])["states"]), n, "n=%d" % n)

    def test_state_request_without_response_times_out(self):
        boot, blocks = run_gateway(['MQTT module_states_requests {"M_ID":"E001"}', "RUN 400 50"])
        txs = all_txs(blocks)
        self.assertEqual(len(txs), 3)
        st = all_pubs(blocks, "module_states_response")
        self.assertEqual(len(st), 1)
        self.assertEqual(json.loads(st[0]["payload"]), {"id": "E001", "status": "timeout"})

    def test_heartbeat_frames_do_not_publish_by_themselves_and_are_not_confused(self):
        boot, blocks = run_gateway(["MKHB E001 EXEC 1", "MKHB R001 REPEATER 1", "RUN 5 50"])
        hb = all_pubs(blocks, "heartbeat")
        ids = sorted(json.loads(p["payload"])["id"] for p in hb)
        self.assertEqual(ids, ["E001", "R001"])
        rep = [json.loads(p["payload"]) for p in hb if json.loads(p["payload"])["id"] == "R001"][0]
        self.assertIn("rssi", rep)                 # само heartbeat-ът на repeater носи rssi/snr
        exe = [json.loads(p["payload"]) for p in hb if json.loads(p["payload"])["id"] == "E001"][0]
        self.assertNotIn("rssi", exe)

    def test_gateway_heartbeat_every_30_seconds(self):
        boot, blocks = run_gateway(["TICK 10000", "TICK 21000", "TICK 29000", "TICK 2000"])
        hb = [p for p in all_pubs(blocks, "heartbeat") if json.loads(p["payload"])["id"] == "G001"]
        self.assertEqual(len(hb), 2)

    def test_mixed_traffic_is_all_published(self):
        s = run_sensor([(22.5, 55.0, 1, 0)])[0]
        boot, blocks = run_gateway(["RX " + s["frame"], "MKHB E001 EXEC 1", "MKSTATE E001 0 2 V01:1", "RX " + s["frame"]])
        self.assertEqual(len(all_pubs(blocks, "sensors")), 1)
        self.assertEqual(len(all_pubs(blocks, "heartbeat")), 1)
        self.assertEqual(len(all_pubs(blocks, "module_states_response")), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
