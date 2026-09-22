"""fake_field.py - test-only simulator standing in for the real LoRa radio
network (Gateway + sensors + executors + repeater) during a manual QA pass.

NOT part of the product - never referenced by docker-compose.yml or any
other app code. Publishes/subscribes on the exact MQTT topics/JSON shapes
documented in description_updated.docx section 4 (see mqtt_bridge.py /
reconciler.py docstrings for the authoritative wire format), so from the
backend+daemons' point of view this is indistinguishable from real hardware.

Time compression: the real sensors report every ~10 minutes (per the spec).
Waiting 10 real minutes per cycle would make a QA pass impractically slow, so
this simulator reports every SENSOR_PERIOD_S (default 12s) instead - fast
enough to watch zone averages/thresholds/graphs move in a normal test
session, while every daemon-side timing (reconciler's 2s tick, settle times,
health_checker's offline-detection minutes) still runs on its own real
wall-clock speed, unmodified. Soil humidity is walked as a slow random walk
that drains toward SOIL_DRY_TARGET between waterings and jumps up sharply
whenever this script sees (via a lightweight poll of the backend API) that a
valve feeding a zone is actually on - this is what makes threshold-regime
irrigation and OVERWATERING_DETECTED/SOIL_TOO_WET actually exercisable
without a human physically opening a real valve.

Usage: python daemons/fake_field.py
"""

import argparse
import json
import random
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import paho.mqtt.client as mqtt  # noqa: E402

from daemons.system_config import mqtt_settings  # noqa: E402

SENSOR_PERIOD_S = 12          # simulated "every ~10 minutes" sensor report
HEARTBEAT_PERIOD_S = 20       # Executor/Repeater/Gateway heartbeat cadence
COMMAND_ACK_DELAY_S = 0.4     # how long the fake executor takes to "flip the pin"

TOPIC_SENSORS = "sensors"
TOPIC_HEARTBEAT = "heartbeat"
TOPIC_COMMANDS = "commands"
TOPIC_COMMANDS_STATUS = "commands_status"
TOPIC_MODULE_STATES_REQUEST = "module_states_request"
TOPIC_MODULE_STATES_RESPONSE = "module_states_response"

COM_ON = "A1"
COM_OFF = "B2"
STATUS_ACK = 0
STATUS_TIMEOUT = 1
STATUS_NACK = 2

# --- fleet layout, matching app/seed.py's demo database exactly ------------

GATEWAY_ID = "GW01"
REPEATER_ID = "R001"
EXECUTORS = ["E001", "E002"]
# executor_id -> list of consumer ids it owns (valves + its pump)
EXECUTOR_CONSUMERS = {
    "E001": ["V01", "V02", "V03", "P01"],
    "E002": ["V04", "P02", "V05", "P03"],
}
SENSORS = ["S001", "S002", "S003", "S004"]
# sensor_id -> which valve(s), if opened, raise this sensor's soil humidity -
# mirrors seed.py's zone membership (S00x sits in the same zone as V0y).
SENSOR_ZONE_VALVES = {
    "S001": ["V01"],
    "S002": ["V02", "V03"],
    "S003": ["V04"],
    "S004": ["V05"],
}
REPEATER_ROUTES = {"S002", "S004"}  # per RepeaterSensor seed rows - these report "through" R001

SOIL_DRY_DRIFT = -0.06     # %RH per second, slow natural drying
SOIL_WATER_RISE = 0.9      # %RH per second while a feeding valve is open
AIR_BASE = {"S001": (24.0, 52.0), "S002": (22.5, 55.0), "S003": (20.5, 58.0), "S004": (18.0, 46.0)}


class FakeExecutor:
    """One physical Executor module: owns some valves/pumps, tracks their
    ACTUAL on/off state independently of the backend DB (this is the
    'ground truth' the real relay board would hold), and answers commands/
    module_states_request the way the real firmware does (manual 3.2)."""

    def __init__(self, executor_id, consumer_ids):
        self.executor_id = executor_id
        self.states = {cid: "OFF" for cid in consumer_ids}
        self.lock = threading.Lock()
        # Test-only fault injection, toggled at runtime via inject_fault() -
        # lets a QA pass exercise NACK/timeout handling on demand instead of
        # only ever seeing the happy path.
        self.fault_mode = {}  # consumer_id -> "nack" | "timeout" | None

    def handle_command(self, client, consumer_id, com):
        if consumer_id not in self.states:
            print(f"fake_field: {self.executor_id} got command for unknown consumer {consumer_id} - NACK")
            client.publish(TOPIC_COMMANDS_STATUS, json.dumps({
                "M_ID": self.executor_id, "C_ID": consumer_id, "com": com, "status": STATUS_NACK,
            }), qos=1)
            return

        fault = self.fault_mode.get(consumer_id)
        if fault == "timeout":
            print(f"fake_field: {self.executor_id}/{consumer_id} fault-injected TIMEOUT (dropping command silently)")
            return  # simulate an unreachable device - no reply at all
        if fault == "nack":
            print(f"fake_field: {self.executor_id}/{consumer_id} fault-injected NACK")
            client.publish(TOPIC_COMMANDS_STATUS, json.dumps({
                "M_ID": self.executor_id, "C_ID": consumer_id, "com": com, "status": STATUS_NACK,
            }), qos=1)
            return

        def _ack_after_delay():
            time.sleep(COMMAND_ACK_DELAY_S)
            with self.lock:
                self.states[consumer_id] = "ON" if com == COM_ON else "OFF"
            client.publish(TOPIC_COMMANDS_STATUS, json.dumps({
                "M_ID": self.executor_id, "C_ID": consumer_id, "com": com, "status": STATUS_ACK,
            }), qos=1)
            print(f"fake_field: {self.executor_id}/{consumer_id} -> {self.states[consumer_id]} (ACK sent)")

        threading.Thread(target=_ack_after_delay, daemon=True).start()

    def handle_states_request(self, client):
        with self.lock:
            states = [{"id": cid, "state": s} for cid, s in self.states.items()]
        client.publish(TOPIC_MODULE_STATES_RESPONSE, json.dumps({
            "id": self.executor_id, "states": states,
        }), qos=1)

    def is_on(self, consumer_id):
        with self.lock:
            return self.states.get(consumer_id) == "ON"


class FieldSimulator:
    def __init__(self):
        settings = mqtt_settings()
        self.client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
        if settings["username"]:
            self.client.username_pw_set(settings["username"], settings["password"])
        self.client.on_message = self._on_message
        self.client.on_connect = self._on_connect
        self.settings = settings

        self.executors = {eid: FakeExecutor(eid, cids) for eid, cids in EXECUTOR_CONSUMERS.items()}
        self.consumer_owner = {cid: eid for eid, cids in EXECUTOR_CONSUMERS.items() for cid in cids}
        self.soil_h = {sid: random.uniform(35, 55) for sid in SENSORS}
        self.stop_event = threading.Event()

        # Fault-injection / stale-sensor toggles, flippable at runtime by
        # editing these sets from the interactive console (see main()).
        self.sensors_paused = set()      # sensor ids to stop reporting (-> SENSOR_OFFLINE)
        self.sensors_faulty = set()      # sensor ids to report 255.0 (-> SENSOR_FAULT_255)
        self.sensors_stuck = set()       # sensor ids to freeze their last value (-> SENSOR_STUCK_VALUE)
        self.executors_paused = set()    # executor ids to stop heartbeating (-> device offline)
        self.repeater_paused = False
        self.gateway_paused = False

    # --------------------------------------------------------------- MQTT ----

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            client.subscribe(TOPIC_COMMANDS, qos=1)
            client.subscribe(TOPIC_MODULE_STATES_REQUEST, qos=1)
            print(f"fake_field: connected to {self.settings['host']}:{self.settings['port']}, "
                  f"simulating {list(self.executors)} + sensors {SENSORS} + {REPEATER_ID} + {GATEWAY_ID}")
        else:
            print(f"fake_field: MQTT connect failed: {reason_code}")

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except ValueError:
            return
        if msg.topic == TOPIC_COMMANDS:
            executor_id = payload.get("M_ID")
            consumer_id = payload.get("C_ID")
            com = payload.get("com")
            ex = self.executors.get(executor_id)
            if ex is None:
                print(f"fake_field: command for unknown executor {executor_id} - ignored")
                return
            if executor_id in self.executors_paused:
                print(f"fake_field: {executor_id} is paused (offline) - dropping command silently")
                return
            ex.handle_command(client, consumer_id, com)
        elif msg.topic == TOPIC_MODULE_STATES_REQUEST:
            executor_id = payload.get("M_ID")
            ex = self.executors.get(executor_id)
            if ex is not None and executor_id not in self.executors_paused:
                ex.handle_states_request(client)

    def connect(self):
        self.client.connect(self.settings["host"], self.settings["port"], keepalive=30)
        self.client.loop_start()

    # ------------------------------------------------------------ sensors ----

    def _any_feeding_valve_open(self, sensor_id):
        for valve_id in SENSOR_ZONE_VALVES.get(sensor_id, []):
            owner = self.consumer_owner.get(valve_id)
            ex = self.executors.get(owner)
            if ex and ex.is_on(valve_id):
                return True
        return False

    def _step_soil(self, sensor_id):
        if self._any_feeding_valve_open(sensor_id):
            self.soil_h[sensor_id] += SOIL_WATER_RISE * SENSOR_PERIOD_S * random.uniform(0.7, 1.1)
        else:
            self.soil_h[sensor_id] += SOIL_DRY_DRIFT * SENSOR_PERIOD_S * random.uniform(0.7, 1.3)
        self.soil_h[sensor_id] = max(5.0, min(95.0, self.soil_h[sensor_id]))

    def _sensor_loop(self):
        while not self.stop_event.is_set():
            for sensor_id in SENSORS:
                if sensor_id in self.sensors_paused:
                    continue  # simulates SENSOR_OFFLINE - no packet sent at all
                if sensor_id not in self.sensors_stuck:
                    self._step_soil(sensor_id)
                soil_t_base, air_t_base = 21.0, AIR_BASE[sensor_id][0]
                air_h_base = AIR_BASE[sensor_id][1]

                if sensor_id in self.sensors_faulty:
                    payload = {"id": sensor_id, "sT": 255.0, "sH": 255.0, "aT": 255.0, "aH": 255.0,
                               "rssi": -60, "snr": 8.0, "ts": int(time.time())}
                else:
                    payload = {
                        "id": sensor_id,
                        "sT": round(soil_t_base + random.uniform(-0.8, 0.8), 1),
                        "sH": round(self.soil_h[sensor_id], 1),
                        "aT": round(air_t_base + random.uniform(-1.0, 1.0), 1),
                        "aH": round(air_h_base + random.uniform(-3.0, 3.0), 1),
                        "rssi": random.randint(-90, -40),
                        "snr": round(random.uniform(2.0, 12.0), 1),
                        "ts": int(time.time()),
                    }
                self.client.publish(TOPIC_SENSORS, json.dumps(payload), qos=0)
            time.sleep(SENSOR_PERIOD_S)

    # ---------------------------------------------------------- heartbeats ----

    def _heartbeat_loop(self):
        while not self.stop_event.is_set():
            now = int(time.time())
            if not self.gateway_paused:
                self.client.publish(TOPIC_HEARTBEAT, json.dumps({"id": GATEWAY_ID, "ts": now}), qos=0)
            if not self.repeater_paused:
                self.client.publish(TOPIC_HEARTBEAT, json.dumps({
                    "id": REPEATER_ID, "ts": now,
                    "rssi": random.randint(-95, -50), "snr": round(random.uniform(1.0, 10.0), 1),
                }), qos=0)
            for executor_id in self.executors:
                if executor_id not in self.executors_paused:
                    self.client.publish(TOPIC_HEARTBEAT, json.dumps({"id": executor_id, "ts": now}), qos=0)
            time.sleep(HEARTBEAT_PERIOD_S)

    def start(self):
        self.connect()
        time.sleep(1)
        threading.Thread(target=self._sensor_loop, daemon=True).start()
        threading.Thread(target=self._heartbeat_loop, daemon=True).start()


CONTROL_PORT = 9099


def _make_control_handler(sim: FieldSimulator):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass  # quiet - fake_field's own prints are enough

        def _json(self, obj, code=200):
            body = json.dumps(obj).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _state(self):
            return {
                "soil_h": {k: round(v, 1) for k, v in sim.soil_h.items()},
                "executor_states": {eid: dict(ex.states) for eid, ex in sim.executors.items()},
                "sensors_paused": sorted(sim.sensors_paused),
                "sensors_faulty": sorted(sim.sensors_faulty),
                "sensors_stuck": sorted(sim.sensors_stuck),
                "executors_paused": sorted(sim.executors_paused),
                "repeater_paused": sim.repeater_paused,
                "gateway_paused": sim.gateway_paused,
                "fault_mode": {eid: ex.fault_mode for eid, ex in sim.executors.items()},
            }

        def do_GET(self):
            if self.path == "/state":
                self._json(self._state())
            else:
                self._json({"error": "unknown path"}, 404)

        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except ValueError:
                self._json({"error": "bad json"}, 400)
                return

            # /sensor/<id>/pause|resume|fault|clear_fault|stick|unstick
            # /executor/<id>/pause|resume
            # /executor/<id>/consumer/<cid>/fault?mode=nack|timeout|clear
            # /repeater/pause|resume  /gateway/pause|resume
            # /soil/<id>/set  {"value": 42.0}
            parts = [p for p in self.path.split("/") if p]
            try:
                if parts[:1] == ["sensor"] and len(parts) == 3:
                    sid, action = parts[1], parts[2]
                    {
                        "pause": lambda: sim.sensors_paused.add(sid),
                        "resume": lambda: sim.sensors_paused.discard(sid),
                        "fault": lambda: sim.sensors_faulty.add(sid),
                        "clear_fault": lambda: sim.sensors_faulty.discard(sid),
                        "stick": lambda: sim.sensors_stuck.add(sid),
                        "unstick": lambda: sim.sensors_stuck.discard(sid),
                    }[action]()
                    self._json({"ok": True, "state": self._state()})
                elif parts[:1] == ["executor"] and len(parts) == 3:
                    eid, action = parts[1], parts[2]
                    {"pause": lambda: sim.executors_paused.add(eid),
                     "resume": lambda: sim.executors_paused.discard(eid)}[action]()
                    self._json({"ok": True, "state": self._state()})
                elif parts[:1] == ["executor"] and len(parts) == 5 and parts[2] == "consumer" and parts[4] == "fault":
                    eid, cid = parts[1], parts[3]
                    mode = body.get("mode")
                    ex = sim.executors.get(eid)
                    if ex is None:
                        self._json({"error": "unknown executor"}, 404)
                        return
                    ex.fault_mode[cid] = None if mode == "clear" else mode
                    self._json({"ok": True, "state": self._state()})
                elif parts == ["repeater", "pause"]:
                    sim.repeater_paused = True
                    self._json({"ok": True})
                elif parts == ["repeater", "resume"]:
                    sim.repeater_paused = False
                    self._json({"ok": True})
                elif parts == ["gateway", "pause"]:
                    sim.gateway_paused = True
                    self._json({"ok": True})
                elif parts == ["gateway", "resume"]:
                    sim.gateway_paused = False
                    self._json({"ok": True})
                elif parts[:1] == ["soil"] and len(parts) == 3 and parts[2] == "set":
                    sid = parts[1]
                    sim.soil_h[sid] = float(body["value"])
                    self._json({"ok": True, "state": self._state()})
                else:
                    self._json({"error": "unknown action"}, 404)
            except (KeyError, IndexError) as exc:
                self._json({"error": str(exc)}, 400)

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.parse_args()
    sim = FieldSimulator()
    sim.start()

    server = HTTPServer(("127.0.0.1", CONTROL_PORT), _make_control_handler(sim))
    threading.Thread(target=server.serve_forever, daemon=True).start()

    print(f"fake_field.py running - control API on http://127.0.0.1:{CONTROL_PORT} "
          f"(GET /state, POST /sensor/<id>/pause|resume|fault|clear_fault|stick|unstick, "
          f"/executor/<id>/pause|resume, /executor/<id>/consumer/<cid>/fault {{mode}}, "
          f"/repeater|gateway/pause|resume, /soil/<id>/set {{value}}). Ctrl+C to stop.")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
