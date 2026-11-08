# -*- coding: utf-8 -*-
"""Тестове на LoRa executor-а на ESP32_PCB(temp_hum): РЕАЛНИЯТ executor.ino + radio_io.cpp + queues +
consumers + power_mgmt + config_storage, срещу фалшиви LoRa/NVS/сън/GPIO (драйвер executor_rx.exe).
Python играе ролята на Gateway: строи пакетите през драйвера (с реалния crypto) и чете отговорите.

Не се тества: реално радио, CAD върху SX127x регистри, реален сън/събуждане по DIO0 на чипа."""
import os
import subprocess
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
EXEC = os.path.join(HERE, "executor_rx.exe")
KEY = "00112233445566778899aabbccddeeff"
OTHER_KEY = "ffeeddccbbaa99887766554433221100"
CONSUMERS = "V01:25,V02:26,P01:27"


def run_exec(commands, mid="E001", key=KEY, freq=433000000, sf=7, bw=125000, consumers=CONSUMERS):
    out = subprocess.run([EXEC, mid, key, str(freq), str(sf), str(bw), consumers],
                         input="\n".join(commands) + "\n", capture_output=True, text=True, timeout=120)
    assert out.returncode == 0, out.stderr
    blocks = [{"cmd": "BOOT", "txs": [], "dec": [], "pins": None, "stats": None}]
    for line in out.stdout.splitlines():
        if line.startswith(">> "):
            blocks.append({"cmd": line[3:], "txs": [], "dec": [], "pins": None, "stats": None})
        elif line.startswith("TX "):
            _, t, h = line.split()
            blocks[-1]["txs"].append({"t": int(t[1:]), "hex": h})
        elif line.startswith("TXDEC "):
            parts = line.split()
            blocks[-1]["dec"].append({"type": parts[1], "sender": parts[2].split("=")[1],
                                      "pt": parts[3].split("=")[1] if len(parts) > 3 else ""})
        elif line.startswith("PINS"):
            blocks[-1]["pins"] = {int(kv.split("=")[0]): int(kv.split("=")[1]) for kv in line.split()[1:]}
        elif line.startswith("STATS"):
            blocks[-1]["stats"] = {kv.split("=")[0]: int(kv.split("=")[1]) for kv in line.split()[1:]}
    return blocks


def decs(blocks, typ=None):
    r = [d for b in blocks for d in b["dec"]]
    return [d for d in r if typ is None or d["type"] == typ]


def txs(blocks):
    return [t for b in blocks for t in b["txs"]]


def hexid(s, n=4):
    return s.encode().ljust(n, b"\x00").hex()


def last_pins(blocks):
    return [b["pins"] for b in blocks if b["pins"] is not None][-1]


def ack_pt(c_id, com, status):
    return hexid(c_id) + "%02x%02x" % (com, status)


SETTLE = "LOOP 3000"          # ~3 s будно - достатъчно за отложения ACK/NACK
ACK_RESTART = ["MKRESPACK E001 1", "LOOP 20"]


class TestBoot(unittest.TestCase):
    def test_all_consumers_are_forced_off_at_boot(self):
        b = run_exec(["PINS"])
        self.assertEqual(last_pins(b), {25: 0, 26: 0, 27: 0})

    def test_restart_state_response_is_sent_right_after_boot(self):
        b = run_exec(["LOOP 30"])
        d = decs(b, "STATE_RESP_RESTART")
        self.assertEqual(len(d), 1)
        self.assertEqual(d[0]["sender"], "E001")
        self.assertEqual(d[0]["pt"], hexid("V01") + "00" + hexid("V02") + "00" + hexid("P01") + "00")

    def test_restart_response_is_retried_twice_then_the_executor_gives_up_silently(self):
        b = run_exec(["LOOP 20000"])
        self.assertEqual(len(decs(b, "STATE_RESP_RESTART")), 3)       # 1 + 2 повторения
        self.assertTrue(len(set(t["hex"] for t in txs(b))) >= 3)                 # всеки опит е нов пакет (нов counter)

    def test_gateway_ack_stops_the_restart_retries(self):
        b = run_exec(["LOOP 30", "MKRESPACK E001 1", "LOOP 20000"])
        self.assertEqual(len(decs(b, "STATE_RESP_RESTART")), 1)

    def test_ack_addressed_to_another_executor_does_not_stop_the_retries(self):
        b = run_exec(["LOOP 30", "MKRESPACK E002 1", "LOOP 20000"])
        self.assertEqual(len(decs(b, "STATE_RESP_RESTART")), 3)

    def test_ack_encrypted_with_a_foreign_key_does_not_stop_the_retries(self):
        b = run_exec(["LOOP 30", "SETKEY " + OTHER_KEY, "MKRESPACK E001 1", "LOOP 20000"])
        self.assertEqual(len(decs(b, "STATE_RESP_RESTART")), 3)

    def test_tampered_ack_does_not_stop_the_retries(self):
        b = run_exec(["LOOP 30", "MKRESPACK E001 1", "TAMPER 12", "LOOP 20000"])
        self.assertEqual(len(decs(b, "STATE_RESP_RESTART")), 3)


class TestCommands(unittest.TestCase):
    def test_on_command_sets_the_pin_and_is_acked(self):
        b = run_exec(ACK_RESTART + ["MKCMD E001 V01 A1 1", SETTLE, "PINS"])
        self.assertEqual(last_pins(b), {25: 1, 26: 0, 27: 0})
        acks = decs(b, "ACK")
        self.assertEqual(len(acks), 1)
        self.assertEqual(acks[0]["sender"], "E001")
        self.assertEqual(acks[0]["pt"], ack_pt("V01", 0xA1, 0))       # C_ID, com, status=ACK(0)

    def test_off_command_clears_the_pin(self):
        b = run_exec(ACK_RESTART + ["MKCMD E001 P01 A1 1", SETTLE, "MKCMD E001 P01 B2 2", SETTLE, "PINS"])
        self.assertEqual(last_pins(b), {25: 0, 26: 0, 27: 0})
        self.assertEqual([d["pt"] for d in decs(b, "ACK")], [ack_pt("P01", 0xA1, 0), ack_pt("P01", 0xB2, 0)])

    def test_unknown_consumer_gets_a_nack_and_no_pin_changes(self):
        b = run_exec(ACK_RESTART + ["MKCMD E001 V99 A1 1", SETTLE, "PINS"])
        self.assertEqual(last_pins(b), {25: 0, 26: 0, 27: 0})
        acks = decs(b, "ACK")
        self.assertEqual([d["pt"] for d in acks], [ack_pt("V99", 0xA1, 2)])          # status=NACK(2)

    def test_invalid_command_code_gets_a_nack_and_no_pin_changes(self):
        b = run_exec(ACK_RESTART + ["MKCMD E001 V01 C4 1", SETTLE, "PINS"])
        self.assertEqual(last_pins(b), {25: 0, 26: 0, 27: 0})
        self.assertEqual([d["pt"] for d in decs(b, "ACK")], [ack_pt("V01", 0xC4, 2)])

    def test_command_for_another_executor_is_ignored(self):
        b = run_exec(ACK_RESTART + ["MKCMD E002 V01 A1 1", SETTLE, "PINS"])
        self.assertEqual(last_pins(b), {25: 0, 26: 0, 27: 0})
        self.assertEqual(decs(b, "ACK"), [])

    def test_command_with_foreign_key_is_ignored(self):
        b = run_exec(ACK_RESTART + ["SETKEY " + OTHER_KEY, "MKCMD E001 V01 A1 1", SETTLE, "PINS"])
        self.assertEqual(last_pins(b), {25: 0, 26: 0, 27: 0})
        self.assertEqual(decs(b, "ACK"), [])

    def test_tampered_command_is_ignored(self):
        for pos in (6, 8, 14, 18, 22, 25):     # sender, ciphertext, counter, tag
            b = run_exec(ACK_RESTART + ["MKCMD E001 V01 A1 1", "TAMPER %d" % pos, SETTLE, "PINS"])
            self.assertEqual(last_pins(b), {25: 0, 26: 0, 27: 0}, "позиция %d" % pos)
            self.assertEqual(decs(b, "ACK"), [], "позиция %d" % pos)

    def test_random_garbage_packets_are_ignored(self):
        import random
        rnd = random.Random(3)
        cmds = ACK_RESTART + ["RAW " + bytes(rnd.randrange(256) for _ in range(n)).hex()
                              for n in (1, 5, 14, 19, 20, 25, 26, 27, 40)] + [SETTLE, "PINS"]
        b = run_exec(cmds)
        self.assertEqual(last_pins(b), {25: 0, 26: 0, 27: 0})
        self.assertEqual(decs(b, "ACK"), [])

    def test_command_is_executed_even_when_the_channel_is_busy_the_ack_waits(self):
        b = run_exec(ACK_RESTART + ["BUSY 1", "MKCMD E001 V01 A1 1", "LOOP 800", "PINS"])
        self.assertEqual(last_pins(b), {25: 1, 26: 0, 27: 0})     # изпълнението не зависи от канала
        self.assertEqual(decs(b, "ACK"), [])                       # ACK не се праща по зает канал

    def test_ack_is_sent_once_the_channel_becomes_free(self):
        b = run_exec(ACK_RESTART + ["BUSY 1", "MKCMD E001 V01 A1 1", "LOOP 300", "BUSY 0", SETTLE])
        self.assertEqual(len(decs(b, "ACK")), 1)

    def test_several_commands_in_a_row_are_all_executed_and_acked_in_order(self):
        cmds = ACK_RESTART + ["MKCMD E001 V01 A1 1", "MKCMD E001 V02 A1 2", "MKCMD E001 P01 A1 3", SETTLE, "PINS"]
        b = run_exec(cmds)
        self.assertEqual(last_pins(b), {25: 1, 26: 1, 27: 1})
        self.assertEqual([d["pt"] for d in decs(b, "ACK")],
                         [ack_pt("V01", 0xA1, 0), ack_pt("V02", 0xA1, 0), ack_pt("P01", 0xA1, 0)])

    def test_burst_over_queue_capacity_does_not_crash_and_state_stays_consistent(self):
        cmds = ACK_RESTART + ["MKCMD E001 V01 A1 %d" % i for i in range(1, 13)] + [SETTLE, "LOOP 5000", "PINS"]
        b = run_exec(cmds)
        self.assertEqual(last_pins(b), {25: 1, 26: 0, 27: 0})
        n = len(decs(b, "ACK"))
        self.assertTrue(1 <= n <= 12, n)

    def test_replayed_command_is_executed_again(self):
        # документира сегашното поведение: няма replay защита (прието ограничение - виж CHANGES.md)
        b = run_exec(ACK_RESTART + ["MKCMD E001 V01 A1 1", SETTLE, "MKCMD E001 V01 B2 2", SETTLE,
                                    "MKCMD E001 V01 A1 1", SETTLE, "PINS"])
        self.assertEqual(last_pins(b), {25: 1, 26: 0, 27: 0})
        self.assertEqual(len(decs(b, "ACK")), 3)

    def test_nonce_counter_never_repeats_across_acks_and_state_responses(self):
        cmds = ACK_RESTART + []
        for i in range(1, 7):
            cmds += ["MKCMD E001 V01 A1 %d" % i, SETTLE, "MKSTATEREQ E001 %d" % (100 + i), SETTLE]
        b = run_exec(cmds)
        frames = [t["hex"] for t in txs(b)]
        counters = [int(f[2 * (len(f) // 2 - 8):2 * (len(f) // 2 - 4)], 16) for f in frames]
        self.assertEqual(len(counters), len(set(counters)))


class TestStates(unittest.TestCase):
    def test_state_request_reports_the_real_pin_states(self):
        b = run_exec(ACK_RESTART + ["MKCMD E001 V02 A1 1", SETTLE, "MKSTATEREQ E001 2", SETTLE])
        d = decs(b, "STATE_RESP")
        self.assertEqual(len(d), 1)
        self.assertEqual(d[0]["pt"], hexid("V01") + "00" + hexid("V02") + "01" + hexid("P01") + "00")

    def test_state_request_for_another_executor_is_ignored(self):
        b = run_exec(ACK_RESTART + ["MKSTATEREQ E002 1", SETTLE])
        self.assertEqual(decs(b, "STATE_RESP"), [])

    def test_state_request_with_foreign_key_is_ignored(self):
        b = run_exec(ACK_RESTART + ["SETKEY " + OTHER_KEY, "MKSTATEREQ E001 1", SETTLE])
        self.assertEqual(decs(b, "STATE_RESP"), [])

    def test_state_response_is_not_the_restart_flavour(self):
        b = run_exec(ACK_RESTART + ["MKSTATEREQ E001 1", SETTLE])
        self.assertEqual(len(decs(b, "STATE_RESP")), 1)
        self.assertEqual(len(decs(b, "STATE_RESP_RESTART")), 1)    # само тази от boot

    def test_executor_with_ten_consumers(self):
        cons = ",".join("C%02d:%d" % (i + 1, 10 + i) for i in range(10))
        b = run_exec(["MKRESPACK E001 1", "LOOP 20", "MKSTATEREQ E001 2", SETTLE], consumers=cons)
        d = decs(b, "STATE_RESP")
        self.assertEqual(len(d), 1)
        self.assertEqual(len(d[0]["pt"]) // 2, 50)                 # 10 x (4 + 1)


class TestHeartbeatAndSleep(unittest.TestCase):
    def test_idle_executor_sleeps_and_sends_a_heartbeat_about_every_5_minutes(self):
        b = run_exec(ACK_RESTART + ["LOOP 130", "STATS"])
        hb = [t for blk in b for t in blk["txs"] if any(d["type"] == "HB" for d in blk["dec"])]
        hbs = decs(b, "HB")
        self.assertTrue(len(hbs) >= 3, len(hbs))
        times = [t["t"] for blk in b for t in blk["txs"]
                 if any(d["type"] == "HB" for d in blk["dec"])]
        gaps = [(times[i + 1] - times[i]) / 1000.0 for i in range(len(times) - 1)]
        for g in gaps:
            self.assertTrue(296 <= g <= 312, g)                    # 38 x 8 s = 304 s
        stats = [blk["stats"] for blk in b if blk["stats"]][-1]
        self.assertTrue(stats["sleeps"] >= 100)

    def test_no_sleep_while_work_is_pending(self):
        b = run_exec(["LOOP 100", "STATS"])                        # restart response още чака ACK
        stats = [blk["stats"] for blk in b if blk["stats"]][-1]
        self.assertEqual(stats["sleeps"], 0)

    def test_heartbeat_frames_decrypt_with_the_network_key_only(self):
        b = run_exec(ACK_RESTART + ["LOOP 60"])
        hbs = [t["hex"] for blk in b for t in blk["txs"] if any(d["type"] == "HB" for d in blk["dec"])]
        self.assertTrue(len(hbs) >= 1)
        self.assertTrue(all(len(h) // 2 == 14 for h in hbs))       # без данни: ID + counter + MAC


if __name__ == "__main__":
    unittest.main(verbosity=2)
