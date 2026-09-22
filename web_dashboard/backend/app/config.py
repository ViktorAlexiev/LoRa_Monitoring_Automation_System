"""Editable defaults/placeholders for values that don't have a real source
yet (pump/valve timing, sensor-reading averaging window, ...). Editable two
ways: directly in config/defaults.json (needs a backend restart to pick up),
or live from the admin panel's "Настройки" tab (POST /api/config - mutates
CONFIG in place and rewrites the file immediately, no restart needed)."""

import json
from pathlib import Path

CONFIG_PATH = Path(__file__).resolve().parent.parent / "config" / "defaults.json"

_DEFAULTS = {
    "pump_defaults": {
        "max_simultaneous_valves": 1,
        "startup_time_s": 3,
        "shutdown_time_s": 5,
    },
    "valve_defaults": {
        "opening_time_s": 3,
        "closing_time_s": 3,
    },
    "threshold_defaults": {
        "irrigation_duration_s": 300,
        "infiltration_wait_s": 900,
    },
    "sensor_readings": {
        "averaging_window_minutes": 15,
        "history_limit": 100,
    },
    "refresh_intervals": {
        "dashboard_seconds": 15,
        "zone_detail_seconds": 20,
    },
    "health_checks": {
        # SENSOR_OFFLINE (see zone_errors_catalog.docx): no new sensor_readings
        # row for this many minutes, while the sensor is active.
        "sensor_offline_minutes": 30,
        # GATEWAY_OFFLINE / LORA_REPEATER_DOWN / EXECUTOR_OFFLINE: no heartbeat
        # for this many minutes, while the device is active.
        "device_offline_minutes": 10,
        # VALVE_NO_EFFECT: a valve has been current_state=on for this many
        # minutes but its zone's soil moisture hasn't moved at all.
        "no_effect_after_minutes": 15,
        # SENSOR_STUCK_VALUE: a sensor's readings haven't changed AT ALL
        # (byte-identical) across this many minutes' worth of readings,
        # regardless of whether anything is irrigating.
        "sensor_stuck_minutes": 60,
    },
    "security": {
        # Sliding window (see app/auth.py) - a logged-in user's session is
        # extended by this many minutes on every request, so someone
        # actively using the dashboard is never logged out mid-session; it
        # only expires after this long with NO requests at all.
        "session_timeout_minutes": 60,
    },
    "data_retention": {
        # daemons/health_checker.py's _cleanup_old_sensor_readings deletes
        # SensorReading rows older than this - by far the highest-volume
        # table (one row per sensor per report cycle, continuously), the
        # one actually worth bounding. 90 days is enough for the Преглед
        # tab's historical trend charts to stay meaningful without the
        # table growing forever on the Pi's limited storage.
        "sensor_readings_retention_days": 90,
    },
}


def _load():
    if not CONFIG_PATH.exists():
        return {k: dict(v) for k, v in _DEFAULTS.items()}
    with open(CONFIG_PATH, encoding="utf-8") as f:
        data = json.load(f)
    merged = {k: dict(v) for k, v in _DEFAULTS.items()}
    for key, value in data.items():
        if isinstance(value, dict) and key in merged:
            merged[key].update(value)
        else:
            merged[key] = value
    return merged


# A single shared dict, mutated in place (never reassigned) - callers that
# read CONFIG["section"]["key"] at request time (schemas.py's Field
# default_factory, _zone_summary, pump_capacity.py) always see the latest
# saved values without needing a process restart.
CONFIG = _load()


def save():
    CONFIG_PATH.parent.mkdir(exist_ok=True)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(CONFIG, f, ensure_ascii=False, indent=2)
