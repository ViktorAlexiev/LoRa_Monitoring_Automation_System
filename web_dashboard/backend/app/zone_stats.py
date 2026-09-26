"""The one place a zone's "average reading" is defined.

The dashboard (routers/zones.py's _zone_summary) and desired_state_setter.py
must agree on it: when a tile says the soil is at 38.4%, the threshold rule
deciding whether to irrigate has to be looking at that same 38.4%, not at
some other number computed a different way. Both call zone_averages() here.

Definition (unchanged from what the dashboard always showed):
  - each sensor contributes its NEWEST valid reading, and only if that reading
    is within the last config sensor_readings.averaging_window_minutes (a
    sensor with nothing in that window contributes nothing - a zone with no
    fresh data at all averages to None, "-" on the dashboard). So the zone
    number always matches what the individual sensor tiles show right now;
  - the sensor's 255.0 "invalid reading" marker is ignored;
  - a leave-one-out outlier among the remaining values is excluded before
    averaging (robust_mean);
  - the result is rounded to 1 decimal.
"""

import datetime

from sqlalchemy import func
from sqlalchemy.orm import object_session

from . import models
from .config import CONFIG

SENSOR_FAULT_VALUE = 255.0  # manual 2.1: "this cycle's reading is invalid" marker - never a real value
ATTRS = ("soil_t", "soil_h", "air_t", "air_h")


def robust_mean(values, z_thresh=2.0):
    """Arithmetic mean, but a value that's a leave-one-out outlier against
    the rest is excluded first - the exact same test health_checker.py's
    SENSOR_OUTLIER check uses (each value's deviation from the mean/stddev
    of the OTHER values, not a population stat that includes itself and can
    mask its own extremity). This means: a single miscalibrated/glitching
    sensor can't drag the zone's average off, but as long as everything
    agrees (the normal case), this is just a plain mean - real legitimate
    variation between sensors is NOT smoothed away, unlike a straight
    median would. Needs >=3 values to judge anything (with 2, a "deviation"
    is meaningless - either one is as valid as the other), so falls back to
    a plain mean below that."""
    if len(values) < 3:
        return sum(values) / len(values)
    keep = []
    for i, v in enumerate(values):
        others = values[:i] + values[i + 1:]
        mean_other = sum(others) / len(others)
        var_other = sum((o - mean_other) ** 2 for o in others) / len(others)
        std_other = var_other ** 0.5
        deviates = abs(v - mean_other) > 0.01 if std_other == 0 else abs(v - mean_other) > z_thresh * std_other
        if not deviates:
            keep.append(v)
    if not keep:  # degenerate case (shouldn't happen in practice) - don't return nothing
        return sum(values) / len(values)
    return sum(keep) / len(keep)


def zone_averages(zone):
    """{attr: average or None} for soil_t/soil_h/air_t/air_h - see module
    docstring for the exact definition. One query per call (only the window's
    rows for this zone's sensors), not every reading ever recorded."""
    sensor_ids = [s.id for s in zone.sensors]
    if not sensor_ids:
        return {attr: None for attr in ATTRS}

    window_min = CONFIG["sensor_readings"]["averaging_window_minutes"]
    cutoff = datetime.datetime.utcnow() - datetime.timedelta(minutes=window_min)
    rows = (
        object_session(zone)
        .query(models.SensorReading)
        .filter(models.SensorReading.sensor_id.in_(sensor_ids), models.SensorReading.recorded_at >= cutoff)
        .all()
    )

    # Each sensor counts ONCE, with its newest valid value inside the window.
    # (Averaging every reading in the window instead would drag old values
    # into the number: after the soil went from 15% to 95% the zone would
    # still show ~50% while every sensor tile said 95%.)
    rows.sort(key=lambda r: r.recorded_at, reverse=True)
    out = {}
    for attr in ATTRS:
        latest = {}
        for r in rows:
            v = getattr(r, attr)
            if r.sensor_id not in latest and v is not None and v != SENSOR_FAULT_VALUE:
                latest[r.sensor_id] = v
        values = list(latest.values())
        out[attr] = round(robust_mean(values), 1) if values else None
    return out


def zone_average(zone, attr):
    return zone_averages(zone)[attr]


def zone_data_age_minutes(zone):
    """Whole minutes since the newest reading from any of the zone's sensors
    (None if there has never been one). The dashboard shows it next to the
    averages so a stale number can't pass for a fresh one."""
    sensor_ids = [s.id for s in zone.sensors]
    if not sensor_ids:
        return None
    newest = (
        object_session(zone)
        .query(func.max(models.SensorReading.recorded_at))
        .filter(models.SensorReading.sensor_id.in_(sensor_ids))
        .scalar()
    )
    if newest is None:
        return None
    return max(0, int((datetime.datetime.utcnow() - newest).total_seconds() // 60))
