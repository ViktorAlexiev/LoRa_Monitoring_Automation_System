# -*- coding: utf-8 -*-
"""Тестове на repeater-а на ESP32_PCB(temp_hum): РЕАЛНИЯТ repeater.ino + radio_io.cpp + dedup.cpp +
power_mgmt.cpp + config_storage.cpp, срещу фалшиви LoRa/NVS/сън (драйвер repeater_rx.exe).
Входните пакети са истински криптирани sensor пакети от РЕАЛНИЯ sensor.ino (sensor_capture.exe).

Не се тества: реално радио и CAD, реално препращане между ленти във въздуха."""
import os
import subprocess
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPEATER = os.path.join(HERE, "repeater_rx.exe")
SENSOR = os.path.join(HERE, "sensor_capture.exe")
KEY = "00112233445566778899aabbccddeeff"
RX_HZ = 434000000
TX_HZ = 433000000


def sensor_frames(n, sid="S001", freq=RX_HZ):
    stdin = "".join("CYCLE %s 55.0 1 0\n" % (20.0 + i * 0.5) for i in range(n))
    out = subprocess.run([SENSOR, sid, KEY, str(freq), "7", "125000"], input=stdin,
                         capture_output=True, text=True, timeout=60)
    assert out.returncode == 0, out.stderr
    return [line.split()[1] for line in out.stdout.splitlines() if line.startswith("FRAME")]


def run_repeater(commands, mid="R001", rx=RX_HZ, tx=TX_HZ, sf=7, bw=125000):
    out = subprocess.run([REPEATER, mid, KEY, str(rx), str(tx), str(sf), str(bw)],
                         input="\n".join(commands) + "\n", capture_output=True, text=True, timeout=120)
    assert out.returncode == 0, out.stderr
    ready = None
    blocks = [{"cmd": "BOOT", "txs": [], "hb": 0, "stats": None}]
    for line in out.stdout.splitlines():
        if line.startswith("READY"):
            ready = int(line.split("radio_hz=")[1])
        elif line.startswith(">> "):
            blocks.append({"cmd": line[3:], "txs": [], "hb": 0, "stats": None})
        elif line.startswith("TX "):
            _, t, f, h = line.split()
            blocks[-1]["txs"].append({"t": int(t[1:]), "f": int(f[2:]), "hex": h})
        elif line.startswith("TXDEC HB"):
            blocks[-1]["hb"] += 1
        elif line.startswith("STATS"):
            blocks[-1]["stats"] = {kv.split("=")[0]: int(kv.split("=")[1]) for kv in line.split()[1:]}
    return ready, blocks


def all_txs(blocks):
    return [t for b in blocks for t in b["txs"]]


def forwarded(blocks):
    """всичко, което не е heartbeat на repeater-а (т.е. препратените пакети)"""
    r = []
    for b in blocks:
        hb_left = b["hb"]
        for t in b["txs"]:
            if len(t["hex"]) // 2 == 14 and hb_left > 0:
                hb_left -= 1
                continue
            r.append(t)
    return r


class TestForwarding(unittest.TestCase):
    def test_radio_starts_on_the_rx_frequency(self):
        ready, blocks = run_repeater([])
        self.assertEqual(ready, RX_HZ)

    def test_sensor_frame_is_forwarded_unchanged_on_the_tx_frequency(self):
        f = sensor_frames(1)[0]
        ready, blocks = run_repeater(["LOOP 3", "RX " + f, "LOOP 400"])
        fw = forwarded(blocks)
        self.assertEqual(len(fw), 1)
        self.assertEqual(fw[0]["hex"], f)             # байт по байт същият пакет (без маркер)
        self.assertEqual(fw[0]["f"], TX_HZ)

    def test_radio_returns_to_the_rx_frequency_after_forwarding(self):
        f = sensor_frames(1)[0]
        ready, blocks = run_repeater(["LOOP 3", "RX " + f, "LOOP 400", "STATS"])
        stats = [b["stats"] for b in blocks if b["stats"]][-1]
        self.assertEqual(stats["radio_hz"], RX_HZ)

    def test_duplicate_is_forwarded_once(self):
        f = sensor_frames(1)[0]
        ready, blocks = run_repeater(["LOOP 3", "RX " + f, "LOOP 400", "RX " + f, "LOOP 400", "RX " + f, "LOOP 400"])
        self.assertEqual(len(forwarded(blocks)), 1)

    def test_dedup_remembers_6_frames_then_forgets_the_oldest(self):
        fr = sensor_frames(8)
        cmds = ["LOOP 3"]
        for f in fr[:7]:                              # 7 различни: първият изпада от буфера (6 записа)
            cmds += ["RX " + f, "LOOP 400"]
        cmds += ["RX " + fr[6], "LOOP 400"]           # още е в буфера -> дубликат
        cmds += ["RX " + fr[0], "LOOP 400"]           # изпаднал -> препраща се пак
        ready, blocks = run_repeater(cmds)
        fw = forwarded(blocks)
        self.assertEqual(len(fw), 8)
        self.assertEqual(fw[-1]["hex"], fr[0])

    def test_many_sensors_all_forwarded_in_order(self):
        fr = sensor_frames(5)
        cmds = ["LOOP 3"]
        for f in fr:
            cmds += ["RX " + f, "LOOP 100"]
        cmds += ["LOOP 400"]
        ready, blocks = run_repeater(cmds)
        self.assertEqual([t["hex"] for t in forwarded(blocks)], fr)

    def test_burst_of_frames_in_one_go_none_lost_up_to_queue_size(self):
        fr = sensor_frames(4)
        ready, blocks = run_repeater(["LOOP 3"] + ["RX " + f for f in fr] + ["LOOP 2000"])
        self.assertEqual(sorted(t["hex"] for t in forwarded(blocks)), sorted(fr))

    def test_burst_over_queue_size_does_not_crash(self):
        fr = sensor_frames(12)
        ready, blocks = run_repeater(["LOOP 3"] + ["RX " + f for f in fr] + ["LOOP 3000"])
        n = len(forwarded(blocks))
        self.assertTrue(4 <= n <= 12, n)

    def test_forwards_without_validating_the_content_documented_behaviour(self):
        # repeater-ът не проверява MAC/формат - препраща всичко, чуто на RX лентата (дублирането се
        # хваща по байтове). Документира се, за да е видимо: боклук на лентата отнема ефирно време.
        junk = bytes(range(22)).hex()
        ready, blocks = run_repeater(["LOOP 3", "RX " + junk, "LOOP 400"])
        fw = forwarded(blocks)
        self.assertEqual(len(fw), 1)
        self.assertEqual(fw[0]["hex"], junk)

    def test_busy_channel_the_frame_is_forwarded_anyway_after_two_cads(self):
        f = sensor_frames(1)[0]
        ready, blocks = run_repeater(["LOOP 3", "BUSY 1", "RX " + f, "LOOP 2000"])
        fw = forwarded(blocks)
        self.assertEqual(len(fw), 1)                  # telemetry FORCE: по-добре колизия, отколкото загуба
        self.assertEqual(fw[0]["hex"], f)

    def test_oversized_packet_is_truncated_not_crashing(self):
        big = bytes(range(60)).hex()
        ready, blocks = run_repeater(["LOOP 3", "RX " + big, "LOOP 400"])
        fw = forwarded(blocks)
        self.assertEqual(len(fw), 1)
        self.assertEqual(len(fw[0]["hex"]) // 2, 32)  # буферът е DEDUP_MAX_LEN = 32


class TestHeartbeat(unittest.TestCase):
    def test_heartbeat_every_about_5_minutes_on_the_tx_frequency(self):
        ready, blocks = run_repeater(["LOOP 150"])
        hb = [t for b in blocks for t in b["txs"] if len(t["hex"]) // 2 == 14]
        self.assertTrue(len(hb) >= 3, len(hb))
        for t in hb:
            self.assertEqual(t["f"], TX_HZ)
        gaps = [(hb[i + 1]["t"] - hb[i]["t"]) / 1000.0 for i in range(len(hb) - 1)]
        for g in gaps:
            self.assertTrue(296 <= g <= 312, g)       # 38 x 8 s
        self.assertEqual(len(set(t["hex"] for t in hb)), len(hb))      # всеки HB с нов counter

    def test_heartbeat_is_skipped_on_a_busy_channel(self):
        ready, blocks = run_repeater(["BUSY 1", "LOOP 150"])
        hb = [t for b in blocks for t in b["txs"] if len(t["hex"]) // 2 == 14]
        self.assertEqual(hb, [])                      # телеметрия/SKIP: при зает канал се пропуска

    def test_idle_repeater_sleeps(self):
        ready, blocks = run_repeater(["LOOP 40", "STATS"])
        stats = [b["stats"] for b in blocks if b["stats"]][-1]
        self.assertTrue(stats["sleeps"] >= 30)
        # тиковете се броят по реално време: 40 съня x 8 s = 320 s -> 40 тика, от които 38 са "изядени" от
        # един heartbeat (wdt_ticks -= 38)
        self.assertEqual(stats["ticks"], 40 - 38)

    def test_no_sleep_while_a_forward_is_pending(self):
        f = sensor_frames(1)[0]
        ready, blocks = run_repeater(["LOOP 3", "STATS", "BUSY 1", "RX " + f, "LOOP 1", "STATS", "LOOP 400"])
        before, during = [b["stats"] for b in blocks if b["stats"]][:2]
        self.assertEqual(during["sleeps"], before["sleeps"])      # чакащ forward -> без сън
        self.assertEqual(len(forwarded(blocks)), 1)                # и накрая е препратен


if __name__ == "__main__":
    unittest.main(verbosity=2)
