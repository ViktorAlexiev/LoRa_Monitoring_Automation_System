"""mqtt_bridge.py - see Документация на системата.docx section 4 (the MQTT
protocol manual) for the authoritative wire format this implements.

The passive half of the system's MQTT traffic: everything the radio network
reports on its own, without anyone having asked for it. Purely a listener -
it never publishes anything. The active, request/response half (sending
"commands" and reading "commands_status" back) is reconciler.py's job, kept
separate because that side needs to correlate replies against specific
pending database rows, not just append data.

Subscribes to two topics:

  sensors - one row per reading, appended to SENSOR_READINGS. The "ts" field
  in the payload is relative time since the Gateway's last boot, not a real
  timestamp (manual 4.3) - recorded_at is deliberately left to its own
  default (this bridge's own receipt time) instead of trusting "ts". This is
  the one simple timestamp a reading needs; nothing else in the payload is
  time-related and worth keeping.
  A value of 255.0 in any reading field means "this sensor didn't return a
  valid reading this cycle" (manual 2.1) - it is stored AS-IS, unconverted,
  because that's the literal marker health_checker.py's SENSOR_FAULT_255
  check (zone_errors_catalog.docx) looks for. Anything that averages
  readings (e.g. the dashboard's per-zone summary) needs to filter 255 out
  itself - this bridge's job is only to record what was actually sent.
  rssi/snr (radio signal quality), when present, are read only to help tell
  a sensor reading's origin apart from other traffic - their values aren't
  useful to this app and are deliberately not stored.

  heartbeat - shared by Repeater, Executor, and Gateway (manual 4.2), with
  no explicit field saying which. Disambiguated the way the manual itself
  suggests: presence of rssi/snr means Repeater (only Repeater's heartbeat
  carries them); everything else is looked up by id, trying Executor then
  Gateway. The rssi/snr VALUES themselves aren't kept - the only thing this
  bridge needs them for is that disambiguation.

Unknown ids (a message for a sensor/executor/repeater/gateway that isn't in
the database) are logged and dropped, not auto-created - a module has to be
registered through the admin panel first, same as everywhere else in this
app.

This file is a runnable reference implementation, not wired into the
web_dashboard demo app. Needs a real MQTT broker reachable per
config/system.ini - see daemons/system_config.py. Install paho-mqtt
(`pip install paho-mqtt`) if it's not already in the environment.
"""

import datetime
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import paho.mqtt.client as mqtt  # noqa: E402

from app import models  # noqa: E402
from app.database import SessionLocal  # noqa: E402
from daemons.system_config import mqtt_settings  # noqa: E402

TOPIC_SENSORS = "sensors"
TOPIC_HEARTBEAT = "heartbeat"


def _on_sensors(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
        sensor_id = payload["id"]
    except (ValueError, KeyError):
        print(f"mqtt_bridge: malformed {TOPIC_SENSORS} payload: {msg.payload!r}")
        return

    db = SessionLocal()
    try:
        sensor = db.get(models.Sensor, sensor_id)
        if sensor is None:
            print(f"mqtt_bridge: reading for unknown sensor {sensor_id!r} - ignored")
            return
        db.add(models.SensorReading(
            sensor_id=sensor_id,
            soil_t=payload.get("sT"),
            soil_h=payload.get("sH"),
            air_t=payload.get("aT"),
            air_h=payload.get("aH"),
            # recorded_at deliberately NOT set from payload["ts"] - see
            # module docstring. Falls back to the column default (now()).
            # rssi/snr, if present, are not stored - see module docstring.
        ))
        db.commit()
    finally:
        db.close()


def _on_heartbeat(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
        device_id = payload["id"]
    except (ValueError, KeyError):
        print(f"mqtt_bridge: malformed {TOPIC_HEARTBEAT} payload: {msg.payload!r}")
        return

    now = datetime.datetime.utcnow()
    has_radio_fields = "rssi" in payload or "snr" in payload

    db = SessionLocal()
    try:
        if has_radio_fields:
            device = db.get(models.Repeater, device_id)
            if device is None:
                print(f"mqtt_bridge: heartbeat (with rssi/snr) for unknown repeater {device_id!r} - ignored")
                return
            device.last_heartbeat_at = now  # rssi/snr values themselves are not stored - see module docstring
            db.commit()
            return

        # No rssi/snr - Executor or Gateway, indistinguishable by shape
        # alone (manual 4.2) - resolve by id lookup, as the manual suggests.
        device = db.get(models.Executor, device_id)
        if device is not None:
            device.last_heartbeat_at = now
            db.commit()
            return

        device = db.get(models.Gateway, device_id)
        if device is not None:
            device.last_heartbeat_at = now
            db.commit()
            return

        print(f"mqtt_bridge: heartbeat for unknown device {device_id!r} - ignored")
    finally:
        db.close()


def _on_message(client, userdata, msg):
    if msg.topic == TOPIC_SENSORS:
        _on_sensors(client, userdata, msg)
    elif msg.topic == TOPIC_HEARTBEAT:
        _on_heartbeat(client, userdata, msg)


def main():
    settings = mqtt_settings()
    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    if settings["username"]:
        client.username_pw_set(settings["username"], settings["password"])
    client.on_message = _on_message

    def _on_connect(client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            client.subscribe(TOPIC_SENSORS, qos=1)
            client.subscribe(TOPIC_HEARTBEAT, qos=1)
            print(f"mqtt_bridge: connected to MQTT broker at {settings['host']}:{settings['port']}, "
                  f"subscribed to {TOPIC_SENSORS}/{TOPIC_HEARTBEAT}")
        else:
            print(f"mqtt_bridge: MQTT connect failed: {reason_code}")

    client.on_connect = _on_connect
    client.connect(settings["host"], settings["port"], keepalive=30)
    print("mqtt_bridge.py started - listening")
    client.loop_forever()  # this daemon is pure I/O, no periodic tick needed


if __name__ == "__main__":
    main()
