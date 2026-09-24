"""desired_state_setter.py daemon (reference implementation - see
description_updated.docx).

The only process that decides desired_state for CLOCK- and THRESHOLD-regime
valves. Manual-regime zones are never touched here - those valves only ever
change on a human's direct command, via the website. Also the sole keeper
of PUMPS.desired_state, recomputed every tick as OR across all of a pump's
valves - regardless of which regime or code path changed a given valve.

Queueing model:
  - Both regimes check the pump's live capacity (max_simultaneous_valves)
    before turning a valve on. If the pump is full, the zone doesn't get
    the slot - a PUMP_QUEUE_WAIT row is opened in ZONE_ERRORS instead
    (severity=warning), and the same check runs again next tick.
  - Clock-mode waiting is bounded by the schedule's own end_time - if the
    interval ends before a slot frees, the valve just never opens for that
    occurrence. There is no catch-up; the queue wait is simply resolved
    (closed) once the interval passes.
  - Threshold-mode waiting is a real FIFO priority queue: ZONE_ERRORS'
    detected_at (set once, the first tick a rule starts waiting, and never
    touched again while it's open) IS the queue position - the oldest open
    PUMP_QUEUE_WAIT for a pump always gets first claim on a newly freed
    slot, checked before any zone is allowed to try for it fresh this tick.

Manual override during a clock interval: a human can still directly command
a clock-regime valve via the website (see routers/devices.py - only
threshold-regime blocks manual commands outright). When they do, that
endpoint sets Valve.manual_override=True. From the next tick on, this
daemon leaves that valve's desired_state exactly as the human set it -
whether the schedule currently says "on" or "off" - instead of fighting
them back to the schedule's own opinion. Valve.override_phase records
whether the valve was inside or outside its interval at the moment the
override was noticed; once _now_in_interval's answer for that valve flips
away from that snapshot (the interval boundary is crossed, in either
direction), the override expires and the schedule resumes normal control
from that tick - i.e. the override lasts for "the rest of this occurrence,"
never longer.

Zone average: every decision below (threshold rules, the overwatering
guard) reads the zone's average through app/zone_stats.py's zone_average -
the very same number the dashboard shows (windowed average, 255 markers and
outliers excluded), not a separate calculation.

Overwatering guard (clock regime only): before opening a scheduled valve,
checks the zone's humidity_warn_max (a zone-wide safety bound, independent
of regime - see models.Zone). If soil moisture is already at/above it, the
valve simply doesn't open for that occurrence. No separate error is raised
here - health_checker.py already raises SOIL_TOO_WET off the very same
threshold, for any regime, so this guard would just be duplicating it.

This file is a runnable reference implementation. It is NOT wired into the
web_dashboard demo app - see reconciler.py's module docstring for the same
caveat about running it against a database you're also poking through the
admin UI.
"""

import datetime
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from app import models  # noqa: E402
from app.database import SessionLocal  # noqa: E402
from app.zone_stats import zone_average  # noqa: E402

TICK_SECONDS = 5

PARAM_TO_READING_FIELD = {"S_H": "soil_h", "S_T": "soil_t", "A_H": "air_h", "A_T": "air_t"}


def _minutes(hhmm: str) -> int:
    h, m = hhmm.split(":")
    return int(h) * 60 + int(m)


def _now_in_interval(now: datetime.datetime, days_mask: int, start_time: str, end_time: str) -> bool:
    if not (days_mask & (1 << now.weekday())):  # Python Monday=0 == "Пн" bit 0, same convention as the frontend
        return False
    now_min = now.hour * 60 + now.minute
    return _minutes(start_time) <= now_min < _minutes(end_time)


def _pump_current_load(db, pump_id, exclude_valve_id=None):
    """Every DISTINCT valve (any zone) that currently wants this pump on."""
    valves = db.query(models.Valve).filter_by(pump_id=pump_id).all()
    return {v.id for v in valves if v.desired_state == "on" and v.id != exclude_valve_id}


def _open_queue_wait(db, zone, valve, pump, occupants):
    existing = (
        db.query(models.ZoneError)
        .filter_by(zone_id=zone.id, valve_id=valve.id, pump_id=pump.id, error_code="PUMP_QUEUE_WAIT", resolved_at=None)
        .first()
    )
    occupant_desc = ", ".join(sorted(occupants)) or "?"
    description = (
        f"Изчаква слот на помпа {pump.id} "
        f"({len(occupants)}/{pump.max_simultaneous_valves} заети от: {occupant_desc})"
    )
    if existing:
        existing.description = description  # keep the occupant list fresh; detected_at (queue position) stays put
    else:
        db.add(models.ZoneError(
            zone_id=zone.id, valve_id=valve.id, pump_id=pump.id,
            error_code="PUMP_QUEUE_WAIT", severity="warning", description=description,
        ))


def _resolve_queue_wait(db, zone_id, valve_id):
    for row in (
        db.query(models.ZoneError)
        .filter_by(zone_id=zone_id, valve_id=valve_id, error_code="PUMP_QUEUE_WAIT", resolved_at=None)
        .all()
    ):
        row.resolved_at = datetime.datetime.utcnow()


def _try_claim_slot(db, zone, valve, pump) -> bool:
    """If the pump has room, claims it (sets desired_state=on) and closes
    any open wait for this valve; otherwise opens/refreshes a
    PUMP_QUEUE_WAIT row and returns False."""
    occupants = _pump_current_load(db, pump.id, exclude_valve_id=valve.id)
    if len(occupants) < pump.max_simultaneous_valves:
        valve.desired_state = "on"
        _resolve_queue_wait(db, zone.id, valve.id)
        return True
    _open_queue_wait(db, zone, valve, pump, occupants)
    return False


def _tick_clock_zones(db, now):
    # transition_status != "none" (see models.Zone) means the zone is mid
    # activation/regime-change, waiting to confirm its consumers are really
    # off (routers/zones.py) - touching desired_state here would fight that
    # force-off (e.g. re-opening a valve the transition just closed because
    # the schedule still says "on").
    for zone in db.query(models.Zone).filter_by(regime="clock", is_active=True, transition_status="none").all():
        should_be_on = set()
        for sched in zone.schedules:
            if sched.enabled and _now_in_interval(now, sched.days_mask, sched.start_time, sched.end_time):
                should_be_on.update(link.valve_id for link in sched.valve_links)

        for valve in zone.valves:
            if not valve.pump_id:
                continue
            in_interval = valve.id in should_be_on

            if valve.manual_override:
                if valve.override_phase is None:
                    # first tick since the human's command - snapshot which
                    # side of the interval boundary we're on right now and
                    # don't second-guess them yet this same tick
                    valve.override_phase = in_interval
                    continue
                if in_interval == valve.override_phase:
                    continue  # still the same occurrence they overrode - leave it exactly as they set it
                # boundary crossed since the override - hand control back to
                # the schedule starting this tick, same as if it had never happened
                valve.manual_override = False
                valve.override_phase = None

            if not in_interval:
                if valve.desired_state == "on":
                    valve.desired_state = "off"
                _resolve_queue_wait(db, zone.id, valve.id)  # interval over - no catch-up, wait is moot
                continue
            if valve.desired_state == "on":
                continue  # already has its slot for this interval

            # Overwatering guard: even though the schedule says "on" now,
            # skip it if the soil is already at/above the zone's warning
            # ceiling - see health_checker.py, which raises SOIL_TOO_WET for
            # the same threshold, so no separate error is written here.
            if zone.humidity_warn_max is not None:
                reading = zone_average(zone, "soil_h")
                if reading is not None and reading >= zone.humidity_warn_max:
                    continue

            _try_claim_slot(db, zone, valve, valve.pump)


def _claim_with_priority(db, zone, valve, pump):
    """Before letting THIS zone try for a slot, check whether another zone
    has an older open PUMP_QUEUE_WAIT for the same pump - if so, that zone
    keeps priority this tick even though it hasn't been re-evaluated yet.
    This is what makes the queue FIFO across zones instead of "whichever
    zone's loop iteration happens to run first"."""
    my_wait = (
        db.query(models.ZoneError)
        .filter_by(zone_id=zone.id, valve_id=valve.id, pump_id=pump.id, error_code="PUMP_QUEUE_WAIT", resolved_at=None)
        .first()
    )
    my_since = my_wait.detected_at if my_wait else datetime.datetime.utcnow()
    older_waiter_exists = (
        db.query(models.ZoneError)
        .filter_by(pump_id=pump.id, error_code="PUMP_QUEUE_WAIT", resolved_at=None)
        .filter(models.ZoneError.zone_id != zone.id)
        .filter(models.ZoneError.detected_at < my_since)
        .first()
        is not None
    )
    if older_waiter_exists:
        if not my_wait:
            _open_queue_wait(db, zone, valve, pump, _pump_current_load(db, pump.id, exclude_valve_id=valve.id))
        return
    _try_claim_slot(db, zone, valve, pump)


def _tick_threshold_zones(db, now):
    for zone in db.query(models.Zone).filter_by(regime="threshold", is_active=True, transition_status="none").all():
        for rule in zone.thresholds:
            attr = PARAM_TO_READING_FIELD.get(rule.param)
            if attr is None:
                continue

            rule_valve_ids = {
                link.valve_id for link in
                db.query(models.ThresholdValve).filter_by(zone_id=zone.id, param=rule.param).all()
            }
            rule_valves = [v for v in zone.valves if v.id in rule_valve_ids]

            currently_irrigating = any(v.current_state == "on" for v in rule_valves)
            if currently_irrigating:
                started = min(
                    (v.current_updated_at for v in rule_valves if v.current_state == "on" and v.current_updated_at),
                    default=None,
                )
                if started and (now - started).total_seconds() >= rule.irrigation_duration_s:
                    for v in rule_valves:
                        v.desired_state = "off"
                continue

            last_off = max(
                (v.current_updated_at for v in rule_valves if v.current_state == "off" and v.current_updated_at),
                default=None,
            )
            if last_off and (now - last_off).total_seconds() < rule.infiltration_wait_s:
                continue  # still settling from the last cycle - don't re-check yet

            reading = zone_average(zone, attr)
            needs_water = reading is not None and rule.min_val is not None and reading < rule.min_val

            if not needs_water:
                for v in rule_valves:
                    _resolve_queue_wait(db, zone.id, v.id)
                continue

            for v in rule_valves:
                if not v.pump_id or v.desired_state == "on":
                    continue
                _claim_with_priority(db, zone, v, v.pump)


def _recompute_all_pumps(db):
    for pump in db.query(models.Pump).all():
        still_needed = any(v.desired_state == "on" for v in pump.valves)
        pump.desired_state = "on" if still_needed else "off"


def tick():
    db = SessionLocal()
    try:
        now = datetime.datetime.utcnow()
        # Clock-regime schedules (start_time/end_time) are wall-clock times
        # an admin typed expecting THEIR local morning/evening, not a UTC
        # instant - comparing them against utcnow() silently runs the
        # schedule N hours off from what was configured (N = the site's UTC
        # offset). datetime.now() (naive, no tzinfo) resolves through the
        # OS's local timezone, which docker-compose.yml now points at the
        # host's own /etc/localtime - so this follows the Pi's actual
        # configured local time instead of a hardcoded offset. Every other
        # timestamp in the app (recorded_at, detected_at, session expiry,
        # ...) stays on utcnow() - only wall-clock-of-day schedule matching
        # needs this.
        local_now = datetime.datetime.now()
        _tick_clock_zones(db, local_now)
        _tick_threshold_zones(db, now)
        _recompute_all_pumps(db)
        db.commit()
    finally:
        db.close()


def main():
    print(f"desired_state_setter.py started (tick every {TICK_SECONDS}s)")
    while True:
        try:
            tick()
        except Exception as exc:  # a daemon must not die on one bad tick
            print(f"desired_state_setter tick failed: {exc}")
        time.sleep(TICK_SECONDS)


if __name__ == "__main__":
    main()
