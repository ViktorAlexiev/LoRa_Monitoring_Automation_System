"""Pump-capacity validation shared between two call sites: saving a new
schedule (routers/zones.py, at creation time) and editing something that
could retroactively invalidate a capacity guarantee already checked at
save-time - lowering a pump's max_simultaneous_valves, or reassigning a
valve's pump_id (routers/devices.py). Kept in one place so both paths agree
on what "fits" means."""

from fastapi import HTTPException
from sqlalchemy.orm import Session

from . import models


def minutes(hhmm: str) -> int:
    h, m = hhmm.split(":")
    return int(h) * 60 + int(m)


def times_overlap(start1: str, end1: str, start2: str, end2: str) -> bool:
    return minutes(start1) < minutes(end2) and minutes(start2) < minutes(end1)


def check_new_schedule_fits(
    db: Session,
    valve_ids,
    start_time: str,
    end_time: str,
    days_mask: int,
    exclude_schedule_id: int = None,
):
    """Validates a schedule being saved against every OTHER enabled schedule
    (any zone) that overlaps in days/time. Two checks:

    1. Same valve, overlapping window, anywhere - always an error. This is
       NOT a pump-capacity question (the same valve doesn't need "two slots"
       by being told to open twice) - it's a plain duplicate/conflicting
       instruction to one physical valve, and the old capacity-only check
       missed it entirely because it deduplicated by valve_id before
       comparing against other schedules of the very same valve.
    2. DIFFERENT valves sharing a pump - checked against the pump's
       max_simultaneous_valves, across every zone that pump serves (a pump
       can be shared by more than two zones; the count below is the true
       number of distinct valves that would want it open at once, not just
       this zone's).

    Pre-validation only; the real-time backstop is still the live retry /
    PUMP_QUEUE_WAIT mechanism in desired_state_setter.py for whatever slips
    past it (e.g. a manual-mode valve grabbing the slot at the exact
    moment)."""
    query = db.query(models.ZoneSchedule).filter(models.ZoneSchedule.enabled.is_(True))
    if exclude_schedule_id is not None:
        query = query.filter(models.ZoneSchedule.id != exclude_schedule_id)
    other_schedules = [
        s for s in query.all()
        if (s.days_mask & days_mask) and times_overlap(start_time, end_time, s.start_time, s.end_time)
    ]

    valve_id_set = set(valve_ids)
    for sched in other_schedules:
        for link in sched.valve_links:
            if link.valve_id in valve_id_set:
                zone_name = sched.zone.name if sched.zone else "?"
                raise HTTPException(
                    400,
                    f"Клапан {link.valve_id} вече има припокриващ се интервал "
                    f"{sched.start_time}–{sched.end_time} в „{zone_name}“ — "
                    f"редактирай или изтрий стария интервал вместо да добавяш дублиращ се.",
                )

    by_pump = {}
    for vid in valve_ids:
        valve = db.get(models.Valve, vid)
        if valve and valve.pump_id:
            by_pump.setdefault(valve.pump_id, set()).add(vid)

    for pump_id, vids in by_pump.items():
        pump = db.get(models.Pump, pump_id)
        if not pump:
            continue
        # Every DISTINCT valve (any zone) on this pump whose schedule
        # overlaps this one, plus the valves this new schedule itself wants -
        # a pump shared by 3+ zones is counted fully, not pairwise.
        concurrent = {vid: "тоя интервал" for vid in vids}
        for sched in other_schedules:
            for link in sched.valve_links:
                if link.valve and link.valve.pump_id == pump_id and link.valve_id not in concurrent:
                    zone_name = sched.zone.name if sched.zone else "?"
                    concurrent[link.valve_id] = f"{sched.start_time}–{sched.end_time} в „{zone_name}“"

        if len(concurrent) > pump.max_simultaneous_valves:
            detail = "; ".join(f"{vid} ({desc})" for vid, desc in concurrent.items())
            raise HTTPException(
                400,
                f"Помпа {pump_id} позволява максимум {pump.max_simultaneous_valves} "
                f"едновременно отворени клапана, а с припокриващи се интервали от други зони "
                f"ще са нужни {len(concurrent)}: {detail}",
            )


def check_new_threshold_fits(db: Session, zone_id: int, param: str, valve_ids):
    """Threshold rules have no time window - a rule can fire whenever its
    sensor condition is met, at any hour - so unlike schedules (checked only
    against OTHER intervals that overlap in days/time), every threshold rule
    in the system is treated as permanently "overlapping" with every other
    one. Validates the total distinct compensating valves a shared pump
    would need, across every zone's threshold rules, against
    max_simultaneous_valves.

    This only checks threshold-vs-threshold. A threshold rule racing a
    clock-mode schedule for the same pump is a cross-regime timing question
    that has no fixed window to pre-validate against - that's exactly what
    the live retry / PUMP_QUEUE_WAIT mechanism in desired_state_setter.py
    is for (see description_updated.docx)."""
    by_pump = {}
    for vid in valve_ids:
        valve = db.get(models.Valve, vid)
        if valve and valve.pump_id:
            by_pump.setdefault(valve.pump_id, set()).add(vid)
    if not by_pump:
        return

    other_thresholds = (
        db.query(models.ZoneThreshold)
        .filter(~((models.ZoneThreshold.zone_id == zone_id) & (models.ZoneThreshold.param == param)))
        .all()
    )

    for pump_id, vids in by_pump.items():
        pump = db.get(models.Pump, pump_id)
        if not pump:
            continue
        concurrent = {vid: "тоя праг" for vid in vids}
        for t in other_thresholds:
            links = db.query(models.ThresholdValve).filter_by(zone_id=t.zone_id, param=t.param).all()
            for link in links:
                if link.valve and link.valve.pump_id == pump_id and link.valve_id not in concurrent:
                    zone_name = t.zone.name if t.zone else "?"
                    concurrent[link.valve_id] = f"праг по влажност в „{zone_name}“"

        if len(concurrent) > pump.max_simultaneous_valves:
            detail = "; ".join(f"{vid} ({desc})" for vid, desc in concurrent.items())
            raise HTTPException(
                400,
                f"Помпа {pump_id} позволява максимум {pump.max_simultaneous_valves} едновременно "
                f"отворени клапана. Праговите правила нямат времево ограничение (могат да се "
                f"задействат по всяко време), затова се броят винаги като потенциално едновременни "
                f"— а тук ще са нужни {len(concurrent)}: {detail}",
            )


def check_all_existing_thresholds_fit_pump(db: Session, pump_id: str, max_valves: int):
    """Threshold-rule counterpart to check_all_existing_schedules_fit_pump
    below - used at the same call site (lowering a pump's
    max_simultaneous_valves) so that guard can't be bypassed just because
    the pump happens to be shared by threshold rules instead of/alongside
    schedules. Without this, update_pump only ever re-validated
    ZoneSchedule rows, so lowering a pump's capacity below what its
    already-saved ZoneThreshold rules need was silently accepted (found via
    live bench testing) even though the equivalent schedule case was
    already correctly rejected. Threshold rules have no time window (see
    check_new_threshold_fits), so - same as there - every rule on this pump
    counts as permanently concurrent with every other."""
    rows = db.query(models.ZoneThreshold).all()
    by_pump = {}
    for t in rows:
        links = db.query(models.ThresholdValve).filter_by(zone_id=t.zone_id, param=t.param).all()
        for link in links:
            if link.valve and link.valve.pump_id == pump_id:
                zone_name = t.zone.name if t.zone else "?"
                by_pump.setdefault(link.valve_id, f"праг по влажност в „{zone_name}“")

    if len(by_pump) > max_valves:
        detail = "; ".join(f"{vid} ({desc})" for vid, desc in by_pump.items())
        raise HTTPException(
            400,
            f"Тая промяна би нарушила капацитета на помпа {pump_id} (макс. {max_valves} едновременно) — "
            f"прагови правила биха се нуждаели едновременно от {len(by_pump)}: {detail}",
        )


def check_all_existing_schedules_fit_pump(db: Session, pump_id: str, max_valves: int):
    """Re-validates every already-saved schedule against a pump's current
    max_simultaneous_valves - used when lowering a pump's capacity or
    reassigning a valve's pump_id, so an update can't silently break a
    capacity guarantee that create_schedule already checked once."""
    schedules = db.query(models.ZoneSchedule).filter(models.ZoneSchedule.enabled.is_(True)).all()
    relevant = []
    for s in schedules:
        vids = {link.valve_id for link in s.valve_links if link.valve and link.valve.pump_id == pump_id}
        if vids:
            relevant.append((s, vids))

    for i, (s1, v1) in enumerate(relevant):
        concurrent = {vid: s1 for vid in v1}
        for j, (s2, v2) in enumerate(relevant):
            if i == j:
                continue
            if (s1.days_mask & s2.days_mask) and times_overlap(s1.start_time, s1.end_time, s2.start_time, s2.end_time):
                for vid in v2:
                    concurrent.setdefault(vid, s2)
        if len(concurrent) > max_valves:
            detail = "; ".join(
                f"{vid} ({sched.start_time}–{sched.end_time} в „{sched.zone.name if sched.zone else '?'}“)"
                for vid, sched in concurrent.items()
            )
            raise HTTPException(
                400,
                f"Тая промяна би нарушила капацитета на помпа {pump_id} (макс. {max_valves} едновременно) — "
                f"в интервал, припокриващ се с „{s1.zone.name if s1.zone else '?'}“ ({s1.start_time}–{s1.end_time}), "
                f"биха се нуждаели едновременно {len(concurrent)}: {detail}",
            )
