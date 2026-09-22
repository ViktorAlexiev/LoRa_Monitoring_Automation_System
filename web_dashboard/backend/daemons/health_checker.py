"""health_checker.py daemon (reference implementation - see
description_updated.docx and zone_errors_catalog.docx for the authoritative
error_code list, severities, and open/resolve conditions).

The only process that decides when a monitoring condition becomes a row in
ZONE_ERRORS (or stops being one). It never writes desired_state or
current_state - it only reads other tables and raises/clears warnings.

Checks implemented so far:

1. Soil-moisture safety bounds (SOIL_TOO_DRY / SOIL_TOO_WET, warning).
   Zone.humidity_warn_min/max are zone-wide, independent of regime (unlike
   ZONE_THRESHOLDS.min_val/max_val, which only exist for/drive
   threshold-regime irrigation). Also the exact threshold clock-regime's
   overwatering guard checks (see daemons/desired_state_setter.py) - kept
   deliberately the same so both behaviors always agree.

2. SENSOR_OFFLINE (error) - zone_errors_catalog.docx: "Няма нов ред в
   sensor_readings за конкретен S_ID от Х минути". Threshold is
   config["health_checks"]["sensor_offline_minutes"]. Attributed to the
   sensor's own zone_id (nullable - an unassigned sensor still gets
   flagged, just with no zone to attach it to).

3. GATEWAY_OFFLINE / LORA_REPEATER_DOWN / EXECUTOR_OFFLINE (critical) - a
   heartbeat-based version of the same idea, for Executor/Repeater/Gateway.
   The catalog originally described GATEWAY_OFFLINE via a sensor_readings
   gap and marked LORA_REPEATER_DOWN as "not catchable - no heartbeat
   table" - both are now checked directly off last_heartbeat_at, since the
   schema has one. EXECUTOR_OFFLINE isn't in the catalog at all; added here
   for symmetry (a silent executor is exactly as serious). These are
   zone_id=None (network-wide) - a Gateway/Executor/Repeater typically
   serves more than one zone, so there's no single zone to blame it on.

4. SENSOR_FAULT_255 (error) - the sensor's own "invalid reading this cycle"
   marker (manual 2.1), checked against its latest reading row only.

5. VALVE_COMMAND_TIMEOUT / PUMP_COMMAND_TIMEOUT (critical) - the last
   command sent to a valve/pump came back nack or timeout AND the mismatch
   with desired_state is still live. reconciler.py already resolves a
   stuck-pending command to 'timeout' via its own local backstop; this
   check is what turns that into something the UI actually shows.

6. VALVE_NO_EFFECT (error) - the catalog originally split this into
   VALVE_NO_EFFECT (per-valve, expects a specific growth rate) and
   ZONE_NO_RESPONSE (zone-level, stricter "completely unchanged" variant).
   Merged into one here, using the simpler "completely unchanged" condition
   (there's no modeled "expected growth rate" to compare against): a valve
   has been current_state=on for config["health_checks"]
   ["no_effect_after_minutes"] and its zone's soil moisture hasn't moved AT
   ALL since it turned on - clogged line, broken pump, or a sensor that
   isn't actually in the wetted area.

7. SENSOR_STUCK_VALUE (warning) - separate from #6: this fires regardless
   of irrigation state. A sensor's readings have been byte-identical for
   config["health_checks"]["sensor_stuck_minutes"] - suggests a frozen
   sensor (stuck ADC reading, dead but still transmitting) rather than a
   genuinely stable environment, which realistically always drifts at
   least a little over that long.

8. SENSOR_OUT_OF_RANGE (error) - a reading outside physically possible
   bounds (hardcoded constants below, deliberately NOT admin-configurable -
   these describe sensor hardware limits, not a business preference).
   Distinct from SENSOR_FAULT_255, which is the sensor's own explicit
   "invalid" marker rather than a bogus real number.

9. PUMP_CAPACITY_EXCEEDED (error) - "shouldn't be able to happen" defensive
   check: more valves are current_state=on for a pump than its
   max_simultaneous_valves allows. Everything else in this app (schedule
   save-time validation, desired_state_setter.py's queue) is supposed to
   prevent this; this check exists purely as a just-in-case backstop
   against the actual physical/DB state ever disagreeing with that.

10. OVERWATERING_DETECTED (warning) - distinct from SOIL_TOO_WET: this
    fires specifically when the zone is too wet while EVERY one of its
    valves is current_state=off - i.e. nothing this system did caused it
    (rain, a leak, or a valve stuck open in a way nothing here can detect -
    see the VALVE_STUCK_OPEN omission note below).

11. SENSOR_OUTLIER (warning) - only meaningful with >=3 sensors reporting
    the same field in one zone (fewer than that and a standard deviation is
    too noisy to mean anything). A sensor whose latest value is more than 2
    standard deviations from that group's mean is flagged - it's still
    reporting, just implausibly different from its zone-mates.

12. THRESHOLD_MISCONFIGURED (warning) - a saved ZONE_THRESHOLDS row with
    min_val >= max_val. app/routers/zones.py's upsert_threshold rejects this
    at save time now; this is a defensive backstop for rows saved before
    that validation existed.

13. Zone transition progression (_progress_zone_transitions) - not a
    ZONE_ERRORS check by itself, but the only place that resolves
    Zone.transition_status="waiting" (see models.Zone and
    routers/zones.py's update_zone/_start_force_off): applies the pending
    regime/is_active once every zone valve's current_state confirms off, or
    flips to "error" (opening MODULE_UNREACHABLE) once any of them reports
    Valve.last_command_failed. Kept in this daemon because it's the same
    "translate raw state into a decision/alarm" role as every check above,
    not because it touches desired_state/current_state - it doesn't.

14. Data retention (_cleanup_old_sensor_readings) - not a ZONE_ERRORS check,
    same reasoning as #13 for why it lives here anyway: deletes
    SensorReading rows older than
    config["data_retention"]["sensor_readings_retention_days"], the
    highest-volume table in the schema. Throttled to once per
    CLEANUP_INTERVAL_SECONDS, not every tick.

Deliberately NOT implemented: VALVE_STUCK_OPEN (current_state=off while S_H
keeps rising) - there is no way to tell that apart from a legitimate
infiltration_wait_s tail-off or another zone's pump still running on a
shared line, without a flow/pressure sensor this system doesn't have.

Notification delivery (email/push) is intentionally out of scope - only the
ZONE_ERRORS row (which the UI already renders as a banner, per-zone or on
the dashboard for zone_id=None ones) is written here.
"""

import sys
import time
import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app import models  # noqa: E402
from app.config import CONFIG  # noqa: E402
from app.database import SessionLocal  # noqa: E402


TICK_SECONDS = 30
CLEANUP_INTERVAL_SECONDS = 24 * 3600  # how often _cleanup_old_sensor_readings actually runs its DELETE


SENSOR_FAULT_VALUE = 255.0  # manual 2.1: "this cycle's reading is invalid" marker - never a real value

# Physically possible bounds per field - hardware limits, not a tunable
# business preference, so these stay as code constants rather than
# app/config.py entries. Wide on purpose (this flags impossible readings,
# not just "unusual" ones - that's SENSOR_OUTLIER's job).
PHYSICAL_RANGES = {
    "soil_t": (-10.0, 60.0),
    "soil_h": (0.0, 100.0),
    "air_t": (-20.0, 60.0),
    "air_h": (0.0, 100.0),
}


def _latest(sensor):
    """The chronologically latest reading - NOT sensor.readings[-1], which
    is just relationship/insertion order and can be wrong whenever a row
    arrives out of sequence (a delayed MQTT message, backfilled data, clock
    skew between readings inserted in the same tick, ...)."""
    return max(sensor.readings, key=lambda r: r.recorded_at) if sensor.readings else None


def _latest_reading_avg(zone, attr):
    values = []
    for sensor in zone.sensors:
        last = _latest(sensor)
        if last is not None:
            v = getattr(last, attr)
            if v is not None and v != SENSOR_FAULT_VALUE:
                values.append(v)
    return sum(values) / len(values) if values else None


def _open_or_refresh(db, error_code, severity, description, zone_id=None, sensor_id=None, valve_id=None,
                      pump_id=None, executor_id=None, repeater_id=None):
    # zone_id is NOT part of the identity key - it's a derived attribute of
    # whichever sensor/valve/pump/executor/repeater the error is about (or
    # None for a network-wide one), and that attribute can change (a sensor
    # gets assigned to/moved between zones) while the error is still open.
    # Keying the lookup on it too meant reassigning a sensor orphaned its
    # already-open error under the old zone_id forever - health_checker's
    # own resolve query (using the sensor's CURRENT zone_id) would never
    # find it again, so it stayed open with a stale description even after
    # the sensor started reporting fine (found via live bench testing).
    existing = (
        db.query(models.ZoneError)
        .filter_by(error_code=error_code, resolved_at=None, sensor_id=sensor_id,
                   valve_id=valve_id, pump_id=pump_id, executor_id=executor_id, repeater_id=repeater_id)
        .first()
    )
    if existing:
        existing.description = description  # keep it fresh; detected_at (duration since first seen) stays put
        existing.severity = severity  # a check's severity can change across code updates too - don't let a row opened under the old rule outlive it
        existing.zone_id = zone_id  # follow the device's current zone, same reasoning as above
    else:
        db.add(models.ZoneError(
            zone_id=zone_id, sensor_id=sensor_id, valve_id=valve_id, pump_id=pump_id, executor_id=executor_id,
            repeater_id=repeater_id, error_code=error_code, severity=severity, description=description,
        ))


def _resolve(db, error_code, zone_id=None, sensor_id=None, valve_id=None, pump_id=None, executor_id=None,
             repeater_id=None):
    # See _open_or_refresh above - zone_id is deliberately excluded from the
    # lookup here too, for the same reason (a stale zone_id must not stop a
    # genuinely-resolved error from resolving).
    rows = (
        db.query(models.ZoneError)
        .filter_by(error_code=error_code, resolved_at=None, sensor_id=sensor_id,
                   valve_id=valve_id, pump_id=pump_id, executor_id=executor_id, repeater_id=repeater_id)
        .all()
    )
    for row in rows:
        row.resolved_at = datetime.datetime.utcnow()


def _check_soil_moisture(db):
    for zone in db.query(models.Zone).filter_by(is_active=True).all():
        if zone.humidity_warn_min is None and zone.humidity_warn_max is None:
            continue
        reading = _latest_reading_avg(zone, "soil_h")
        if reading is None:
            continue

        too_dry = zone.humidity_warn_min is not None and reading < zone.humidity_warn_min
        too_wet = zone.humidity_warn_max is not None and reading > zone.humidity_warn_max

        if too_dry:
            _open_or_refresh(
                db, "SOIL_TOO_DRY", "error",
                f"Недостатъчно поливане — влажността на почвата е {reading:.1f}%, под зададения минимум {zone.humidity_warn_min}%",
                zone_id=zone.id,
            )
        else:
            _resolve(db, "SOIL_TOO_DRY", zone_id=zone.id)

        if too_wet:
            _open_or_refresh(
                db, "SOIL_TOO_WET", "error",
                f"Преполиване — влажността на почвата е {reading:.1f}%, над зададения максимум {zone.humidity_warn_max}%",
                zone_id=zone.id,
            )
        else:
            _resolve(db, "SOIL_TOO_WET", zone_id=zone.id)


def _check_sensor_offline(db, now):
    limit_minutes = CONFIG["health_checks"]["sensor_offline_minutes"]
    cutoff = now - datetime.timedelta(minutes=limit_minutes)
    for sensor in db.query(models.Sensor).filter_by(is_active=True).all():
        latest = _latest(sensor)
        last = latest.recorded_at if latest else None
        if last is None or last < cutoff:
            age = "никога" if last is None else f"последно в {last.strftime('%Y-%m-%d %H:%M')}"
            _open_or_refresh(
                db, "SENSOR_OFFLINE", "error",
                f"Сензор {sensor.id} няма ново показание повече от {limit_minutes} мин ({age})",
                zone_id=sensor.zone_id, sensor_id=sensor.id,
            )
        else:
            _resolve(db, "SENSOR_OFFLINE", zone_id=sensor.zone_id, sensor_id=sensor.id)


def _check_sensor_fault_255(db):
    """zone_errors_catalog.docx: SENSOR_FAULT_255 (error) - a reading row
    contains the "invalid this cycle" marker in one of its fields. Checked
    against the sensor's LATEST reading only - an old 255 that's since been
    superseded by a real value shouldn't keep the error open."""
    fields = {"soil_t": "почвена температура", "soil_h": "почвена влажност",
              "air_t": "въздушна температура", "air_h": "въздушна влажност"}
    for sensor in db.query(models.Sensor).filter_by(is_active=True).all():
        last = _latest(sensor)
        if last is None:
            continue
        bad_fields = [label for attr, label in fields.items() if getattr(last, attr) == SENSOR_FAULT_VALUE]
        if bad_fields:
            _open_or_refresh(
                db, "SENSOR_FAULT_255", "error",
                f"Сензор {sensor.id} върна невалидно показание (255) за: {', '.join(bad_fields)}",
                zone_id=sensor.zone_id, sensor_id=sensor.id,
            )
        else:
            _resolve(db, "SENSOR_FAULT_255", zone_id=sensor.zone_id, sensor_id=sensor.id)


def _check_command_failures(db):
    """zone_errors_catalog.docx: VALVE_COMMAND_TIMEOUT / PUMP_COMMAND_TIMEOUT
    (critical). Driven off Valve.last_command_failed / Pump.last_command_failed
    - a plain flag reconciler.py's _give_up() sets the instant a command
    terminally fails (local backstop timeout, device NACK, or a
    gateway-reported timeout) and clears the instant a later attempt
    actually succeeds (see reconciler.py's module docstring). This
    replaced an earlier version of this check that tried to infer the same
    failure from "the latest ValveCommand/PumpCommand row's status" -  that
    never actually worked, because reconciler's own give-up logic queues a
    fresh 'pending' retry in the very same commit that produced the
    'timeout'/'nack' row, so the "last command" this check used to look at
    was always 'pending', never 'timeout' (found via live bench testing,
    not just review). reconciler.py never writes ZONE_ERRORS itself - it
    only owns current_state/desired_state/*_COMMANDS/last_command_failed;
    opening and resolving the actual alarm here keeps that ownership split
    the same as every other error code in this file.

    Also resolves MODULE_UNREACHABLE (raised by the activation/regime-
    change/deactivation force-off sequence in routers/zones.py when a
    consumer couldn't be confirmed off) - it has no other check that ever
    clears it, so once desired catches up to current again (the retry
    eventually succeeded), the old failure is no longer real."""
    for valve in db.query(models.Valve).all():
        if valve.last_command_failed:
            _open_or_refresh(
                db, "VALVE_COMMAND_TIMEOUT", "critical",
                f"Последната команда към клапан {valve.id} не е достигнала целта — desired_state е "
                f"върнат към текущото състояние от reconciler-а, за да спре безкрайният retry",
                zone_id=valve.zone_id, valve_id=valve.id,
            )
        else:
            _resolve(db, "VALVE_COMMAND_TIMEOUT", zone_id=valve.zone_id, valve_id=valve.id)
        if valve.desired_state == valve.current_state:
            _resolve(db, "MODULE_UNREACHABLE", zone_id=valve.zone_id, valve_id=valve.id, executor_id=valve.executor_id)

    for pump in db.query(models.Pump).all():
        if pump.last_command_failed:
            _open_or_refresh(
                db, "PUMP_COMMAND_TIMEOUT", "critical",
                f"Последната команда към помпа {pump.id} не е достигнала целта — desired_state е "
                f"върнат към текущото състояние от reconciler-а, за да спре безкрайният retry",
                pump_id=pump.id,
            )
        else:
            _resolve(db, "PUMP_COMMAND_TIMEOUT", pump_id=pump.id)
        if pump.desired_state == pump.current_state:
            # Unlike a valve, a pump isn't itself tied to one zone (see
            # models.Pump) - a failed force-off can log MODULE_UNREACHABLE
            # for this pump under any of the zones that share it, so clear
            # all of them once it actually converges, regardless of zone_id.
            for row in (
                db.query(models.ZoneError)
                .filter_by(error_code="MODULE_UNREACHABLE", pump_id=pump.id, executor_id=pump.executor_id, resolved_at=None)
                .all()
            ):
                row.resolved_at = datetime.datetime.utcnow()


def _check_device_offline(db, now):
    """Executor/Repeater/Gateway heartbeat staleness. zone_id is left NULL -
    these devices are typically shared across zones, so there's no single
    zone to attach the problem to (see zone_errors.zone_id nullability).
    Each device type keys off its own FK column (executor_id/repeater_id) so
    multiple repeaters, say, don't collide into one shared error row.
    Gateway has no such column - there is architecturally only ever one (see
    Документация на системата.docx: "централният мост"), so a single
    id-less GATEWAY_OFFLINE row is the correct model, not a bug."""
    limit_minutes = CONFIG["health_checks"]["device_offline_minutes"]
    cutoff = now - datetime.timedelta(minutes=limit_minutes)

    device_checks = [
        (models.Executor, "EXECUTOR_OFFLINE", "изпълнител", "executor_id"),
        (models.Repeater, "LORA_REPEATER_DOWN", "повторител", "repeater_id"),
        (models.Gateway, "GATEWAY_OFFLINE", "gateway", None),
    ]
    for model, error_code, label, id_field in device_checks:
        for device in db.query(model).filter_by(is_active=True).all():
            last = device.last_heartbeat_at
            kwargs = {id_field: device.id} if id_field else {}
            if last is None or last < cutoff:
                age = "никога" if last is None else f"последно в {last.strftime('%Y-%m-%d %H:%M')}"
                _open_or_refresh(
                    db, error_code, "critical",
                    f"Няма heartbeat от {label} {device.id} повече от {limit_minutes} мин ({age})",
                    **kwargs,
                )
            else:
                _resolve(db, error_code, **kwargs)


def _check_valve_no_effect(db, now):
    """Combined VALVE_NO_EFFECT / ZONE_NO_RESPONSE - see module docstring."""
    limit_minutes = CONFIG["health_checks"]["no_effect_after_minutes"]
    for valve in db.query(models.Valve).all():
        if valve.current_state != "on" or valve.current_updated_at is None:
            _resolve(db, "VALVE_NO_EFFECT", zone_id=valve.zone_id, valve_id=valve.id)
            continue
        on_since = valve.current_updated_at
        if (now - on_since).total_seconds() < limit_minutes * 60:
            continue
        zone = valve.zone
        if zone is None:
            continue
        readings = [
            r.soil_h for sensor in zone.sensors for r in sensor.readings
            if r.recorded_at >= on_since and r.soil_h is not None and r.soil_h != SENSOR_FAULT_VALUE
        ]
        if len(readings) < 2:
            continue  # not enough data since it turned on to judge anything
        if max(readings) - min(readings) < 0.05:  # effectively unchanged, allow tiny float noise
            _open_or_refresh(
                db, "VALVE_NO_EFFECT", "error",
                f"Клапан {valve.id} е включен от над {limit_minutes} мин, но влажността на почвата в "
                f"„{zone.name}“ не се е променила изобщо (~{readings[-1]}%)",
                zone_id=zone.id, valve_id=valve.id,
            )
        else:
            _resolve(db, "VALVE_NO_EFFECT", zone_id=zone.id, valve_id=valve.id)


def _check_sensor_stuck(db, now):
    limit_minutes = CONFIG["health_checks"]["sensor_stuck_minutes"]
    cutoff = now - datetime.timedelta(minutes=limit_minutes)
    fields = {"soil_t": "почвена температура", "soil_h": "почвена влажност",
              "air_t": "въздушна температура", "air_h": "въздушна влажност"}
    for sensor in db.query(models.Sensor).filter_by(is_active=True).all():
        window = [r for r in sensor.readings if r.recorded_at >= cutoff]
        # Need at least 3 points AND a window that actually spans close to
        # the full limit - otherwise a sensor that just started reporting
        # would trivially "match itself" on 1-2 points. Span is computed via
        # min/max over recorded_at, NOT window[0]/window[-1] - the list
        # comes from sensor.readings, whose iteration order is NOT
        # guaranteed chronological (see _latest()'s docstring).
        if len(window) < 3:
            _resolve(db, "SENSOR_STUCK_VALUE", zone_id=sensor.zone_id, sensor_id=sensor.id)
            continue
        span = (max(r.recorded_at for r in window) - min(r.recorded_at for r in window)).total_seconds()
        if span < limit_minutes * 60 * 0.8:
            _resolve(db, "SENSOR_STUCK_VALUE", zone_id=sensor.zone_id, sensor_id=sensor.id)
            continue
        stuck_fields = []
        for attr, label in fields.items():
            values = {getattr(r, attr) for r in window if getattr(r, attr) is not None and getattr(r, attr) != SENSOR_FAULT_VALUE}
            if len(values) == 1:
                stuck_fields.append(label)
        if stuck_fields:
            _open_or_refresh(
                db, "SENSOR_STUCK_VALUE", "warning",
                f"Сензор {sensor.id} показва напълно непроменена стойност повече от {limit_minutes} мин за: "
                f"{', '.join(stuck_fields)}",
                zone_id=sensor.zone_id, sensor_id=sensor.id,
            )
        else:
            _resolve(db, "SENSOR_STUCK_VALUE", zone_id=sensor.zone_id, sensor_id=sensor.id)


def _check_sensor_out_of_range(db):
    fields = {"soil_t": "почвена температура", "soil_h": "почвена влажност",
              "air_t": "въздушна температура", "air_h": "въздушна влажност"}
    for sensor in db.query(models.Sensor).filter_by(is_active=True).all():
        last = _latest(sensor)
        if last is None:
            continue
        bad = []
        for attr, label in fields.items():
            v = getattr(last, attr)
            if v is None or v == SENSOR_FAULT_VALUE:
                continue
            lo, hi = PHYSICAL_RANGES[attr]
            if v < lo or v > hi:
                bad.append(f"{label}={v}")
        if bad:
            _open_or_refresh(
                db, "SENSOR_OUT_OF_RANGE", "error",
                f"Сензор {sensor.id} върна физически невъзможна стойност: {', '.join(bad)}",
                zone_id=sensor.zone_id, sensor_id=sensor.id,
            )
        else:
            _resolve(db, "SENSOR_OUT_OF_RANGE", zone_id=sensor.zone_id, sensor_id=sensor.id)


def _check_pump_capacity_exceeded(db):
    for pump in db.query(models.Pump).all():
        open_count = sum(1 for v in pump.valves if v.current_state == "on")
        if open_count > pump.max_simultaneous_valves:
            _open_or_refresh(
                db, "PUMP_CAPACITY_EXCEEDED", "error",
                f"Помпа {pump.id} обслужва {open_count} отворени клапана едновременно, "
                f"а лимитът ѝ е {pump.max_simultaneous_valves}",
                pump_id=pump.id,
            )
        else:
            _resolve(db, "PUMP_CAPACITY_EXCEEDED", pump_id=pump.id)


def _check_overwatering(db):
    for zone in db.query(models.Zone).filter_by(is_active=True).all():
        if zone.humidity_warn_max is None:
            continue
        if any(v.current_state == "on" for v in zone.valves):
            # something IS actively watering - that's SOIL_TOO_WET's territory,
            # not this. Still resolve any previously-open row: it may have
            # been raised while everything was off and no longer applies.
            _resolve(db, "OVERWATERING_DETECTED", zone_id=zone.id)
            continue
        reading = _latest_reading_avg(zone, "soil_h")
        if reading is None:
            continue
        if reading > zone.humidity_warn_max:
            _open_or_refresh(
                db, "OVERWATERING_DETECTED", "warning",
                f"Почвата в „{zone.name}“ е над максимума ({reading:.1f}% > {zone.humidity_warn_max}%), "
                f"въпреки че всички клапани са изключени",
                zone_id=zone.id,
            )
        else:
            _resolve(db, "OVERWATERING_DETECTED", zone_id=zone.id)


def _check_threshold_misconfigured(db):
    """THRESHOLD_MISCONFIGURED (zone_errors_catalog.docx, warning): a saved
    ZONE_THRESHOLDS row with min_val >= max_val. app/routers/zones.py's
    upsert_threshold now rejects this at save time (found unvalidated via
    live bench testing), but this check stays as a defensive backstop for
    rows that predate that validation or were written directly - same
    "prevent AND detect" pattern as PUMP_CAPACITY_EXCEEDED above.

    Doesn't use _open_or_refresh/_resolve: those key identity off a device
    id (sensor/valve/pump/executor/repeater), deliberately excluding
    zone_id so a device's error follows it across a zone reassignment. A
    threshold rule has no device id of its own - only (zone_id, param) - so
    keying on zone_id directly here is correct, not a workaround. A zone can
    have up to 4 rules (one per param); like _check_sensor_out_of_range does
    for a sensor's multiple bad fields, every misconfigured param for a zone
    is collected into that zone's single row instead of each param fighting
    over the same row (which would silently drop all but the last one)."""
    bad_by_zone = {}
    for rule in db.query(models.ZoneThreshold).all():
        if rule.min_val is not None and rule.max_val is not None and rule.min_val >= rule.max_val:
            bad_by_zone.setdefault(rule.zone_id, []).append(
                f"{rule.param} (долна {rule.min_val} ≥ горна {rule.max_val})"
            )

    for zone in db.query(models.Zone).all():
        existing = (
            db.query(models.ZoneError)
            .filter_by(error_code="THRESHOLD_MISCONFIGURED", zone_id=zone.id, resolved_at=None)
            .first()
        )
        bad = bad_by_zone.get(zone.id)
        if bad:
            description = f"Неправилно зададени прагове в „{zone.name}“: {'; '.join(bad)}"
            if existing:
                existing.description = description
            else:
                db.add(models.ZoneError(
                    zone_id=zone.id, error_code="THRESHOLD_MISCONFIGURED",
                    severity="warning", description=description,
                ))
        elif existing:
            existing.resolved_at = datetime.datetime.utcnow()


def _check_sensor_outlier(db):
    """Leave-one-out, not a plain population stddev: with only 3-5 sensors
    typical per zone, including the candidate itself in the mean/stddev it's
    being judged against lets an extreme value inflate its own stddev
    enough to "mask" itself out of detection. Comparing each sensor against
    the OTHERS' mean/stddev (excluding itself) avoids that."""
    fields = {"soil_t": "почвена температура", "soil_h": "почвена влажност",
              "air_t": "въздушна температура", "air_h": "въздушна влажност"}
    for zone in db.query(models.Zone).all():
        sensors_with_readings = [s for s in zone.sensors if s.readings]
        outlier_desc = {}  # sensor_id -> [field descriptions]
        if len(sensors_with_readings) >= 3:
            for attr, label in fields.items():
                pairs = [(s, getattr(_latest(s), attr)) for s in sensors_with_readings]
                pairs = [(s, v) for s, v in pairs if v is not None and v != SENSOR_FAULT_VALUE]
                if len(pairs) < 3:
                    continue
                for target_s, target_v in pairs:
                    others = [v for s, v in pairs if s is not target_s]
                    other_mean = sum(others) / len(others)
                    other_var = sum((v - other_mean) ** 2 for v in others) / len(others)
                    other_std = other_var ** 0.5
                    deviates = (
                        abs(target_v - other_mean) > 0.01 if other_std == 0
                        else abs(target_v - other_mean) > 2 * other_std
                    )
                    if deviates:
                        outlier_desc.setdefault(target_s.id, []).append(
                            f"{label} {target_v} (останалите: средно {other_mean:.1f})"
                        )

        for s in sensors_with_readings:
            if s.id in outlier_desc:
                _open_or_refresh(
                    db, "SENSOR_OUTLIER", "warning",
                    f"Сензор {s.id} се отклонява силно от останалите в „{zone.name}“: "
                    f"{'; '.join(outlier_desc[s.id])}",
                    zone_id=zone.id, sensor_id=s.id,
                )
            else:
                _resolve(db, "SENSOR_OUTLIER", zone_id=zone.id, sensor_id=s.id)


def _progress_zone_transitions(db):
    """Resolves Zone.transition_status="waiting" (see models.Zone and
    routers/zones.py's update_zone) - the only place that decides whether a
    zone's activation/regime-change force-off actually converged. Started
    by routers/zones.py's _start_force_off (desired_state="off" on every
    zone valve); driven for real by daemons/reconciler.py, the same as any
    other desired_state write. Two outcomes, both read straight off the
    valves reconciler.py already maintains:
      - every valve's current_state is "off": the real off succeeded -
        apply pending_regime/pending_is_active and clear the transition.
      - any valve has last_command_failed=True (reconciler.py's _give_up -
        see its module docstring): that valve's off attempt terminally
        failed - flip to "error" (the admin resolves it via
        /transition/continue or /transition/deactivate) and open
        MODULE_UNREACHABLE for it, same shape as VALVE_COMMAND_TIMEOUT
        (_check_command_failures already raises that one generically off
        the same flag) but scoped to "this is why your regime-change is
        stuck", not just "a command failed".
    Otherwise (some valve still current_state="on" with last_command_failed
    still False): reconciler.py hasn't finished trying yet - leave it as
    "waiting" and check again next tick."""
    for zone in db.query(models.Zone).filter_by(transition_status="waiting").all():
        valves = zone.valves
        if all(v.current_state == "off" for v in valves):
            if zone.pending_regime is not None:
                zone.regime = zone.pending_regime
            if zone.pending_is_active is not None:
                zone.is_active = zone.pending_is_active
            zone.transition_status = "none"
            zone.pending_regime = None
            zone.pending_is_active = None
            continue
        failed = [v for v in valves if v.last_command_failed]
        if failed:
            zone.transition_status = "error"
            for valve in failed:
                _open_or_refresh(
                    db, "MODULE_UNREACHABLE", "error",
                    f"Няма връзка с модул {valve.executor_id or '?'} — не може да се изключи {valve.id} "
                    f"за преход на зона „{zone.name}“",
                    zone_id=zone.id, valve_id=valve.id, executor_id=valve.executor_id,
                )
        # else: still genuinely converging - leave as "waiting", recheck next tick.


_last_retention_cleanup = None  # in-memory, per-process - fine, this daemon is the only writer


def _cleanup_old_sensor_readings(db, now):
    """config["data_retention"]["sensor_readings_retention_days"] (admin-
    editable from Настройки, same PATCH /api/config mechanism as every other
    tunable here - see app/config.py) - deletes SensorReading rows older
    than that. By far the highest-volume table in the schema (one row per
    sensor per report cycle, forever) - the one actually worth bounding on
    the Pi's limited storage. Throttled to once per CLEANUP_INTERVAL_SECONDS
    rather than every TICK_SECONDS - a bounded DELETE is still a real table
    scan (even indexed - see models.SensorReading.recorded_at), no reason to
    repeat it every 30s."""
    global _last_retention_cleanup
    if _last_retention_cleanup is not None and (now - _last_retention_cleanup).total_seconds() < CLEANUP_INTERVAL_SECONDS:
        return
    _last_retention_cleanup = now
    days = CONFIG["data_retention"]["sensor_readings_retention_days"]
    cutoff = now - datetime.timedelta(days=days)
    deleted = (
        db.query(models.SensorReading)
        .filter(models.SensorReading.recorded_at < cutoff)
        .delete(synchronize_session=False)
    )
    if deleted:
        print(f"health_checker: retention cleanup deleted {deleted} sensor_readings row(s) older than {days}d")


def tick():
    db = SessionLocal()
    try:
        now = datetime.datetime.utcnow()
        _cleanup_old_sensor_readings(db, now)
        _check_soil_moisture(db)
        _check_sensor_offline(db, now)
        _check_sensor_fault_255(db)
        _check_command_failures(db)
        _progress_zone_transitions(db)
        _check_valve_no_effect(db, now)
        _check_sensor_stuck(db, now)
        _check_sensor_out_of_range(db)
        _check_pump_capacity_exceeded(db)
        _check_overwatering(db)
        _check_sensor_outlier(db)
        _check_threshold_misconfigured(db)
        _check_device_offline(db, now)
        db.commit()
    finally:
        db.close()


def main():
    print(f"health_checker.py started (tick every {TICK_SECONDS}s)")
    while True:
        try:
            tick()
        except Exception as exc:  # a daemon must not die on one bad tick
            print(f"health_checker tick failed: {exc}")
        time.sleep(TICK_SECONDS)


if __name__ == "__main__":
    main()
