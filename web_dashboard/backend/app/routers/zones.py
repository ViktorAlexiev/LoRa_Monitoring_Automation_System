import math
from typing import List

from fastapi import APIRouter, Body, Depends, HTTPException
from sqlalchemy.orm import Session

from .. import models, pump_capacity, schemas
from ..zone_stats import zone_averages
from ..auth import accessible_zone_ids, get_current_user, require_role, require_zone_control, require_zone_view
from ..database import get_db

router = APIRouter(prefix="/api/zones", tags=["zones"], dependencies=[Depends(get_current_user)])


def _dew_point(air_t, air_h):
    """Magnus-Tetens approximation - standard meteorological formula, ±0.35°C
    accuracy over 0-60°C / 1-100% RH (well within this app's actual sensor
    range). Computed from AIR temperature/humidity specifically (not soil -
    dew point is a property of the air, condensation risk on leaves/surfaces
    exposed to it), so both must actually be available for the zone's
    current averaging window; either missing means "can't say", not 0."""
    if air_t is None or air_h is None or air_h <= 0:
        return None
    b, c = 17.62, 243.12
    gamma = math.log(air_h / 100.0) + (b * air_t) / (c + air_t)
    return round((c * gamma) / (b - gamma), 1)


def _detach_valve_from_zone(db: Session, valve: models.Valve):
    """A valve leaving its zone (unassigned, or its pump/executor got
    deleted elsewhere) can't stay a member of that zone's schedule/threshold
    rules - drop those links too instead of leaving them dangling."""
    if valve.zone_id is None:
        return
    db.query(models.ScheduleValve).filter_by(valve_id=valve.id).delete(synchronize_session=False)
    db.query(models.ThresholdValve).filter_by(valve_id=valve.id).delete(synchronize_session=False)
    valve.zone_id = None


def _zone_summary(zone: models.Zone) -> schemas.ZoneSummary:
    # The average itself is defined in app/zone_stats.py, shared with
    # desired_state_setter.py so both always see the same number.
    avgs = zone_averages(zone)

    modules: List[schemas.ModuleRef] = []
    pumps_seen = {}
    for v in zone.valves:
        modules.append(schemas.ModuleRef(kind="valve", id=v.id, name=v.name, state=v.current_state))
        if v.pump and v.pump_id not in pumps_seen:
            pumps_seen[v.pump_id] = v.pump
    for pump in pumps_seen.values():
        modules.append(schemas.ModuleRef(kind="pump", id=pump.id, name=pump.name, state=pump.current_state))

    open = [e for e in zone.errors if e.resolved_at is None]
    severity_rank = {"warning": 0, "error": 1, "critical": 2}
    worst = max(open, key=lambda e: severity_rank.get(e.severity, 0), default=None)

    summary = schemas.ZoneSummary.model_validate(zone)
    summary.readings = schemas.GaugeReadings(
        soil_t=avgs["soil_t"], soil_h=avgs["soil_h"], air_t=avgs["air_t"], air_h=avgs["air_h"],
        dew_point=_dew_point(avgs["air_t"], avgs["air_h"]),
    )
    summary.modules = modules
    summary.open_error_count = len(open)
    summary.worst_open_severity = worst.severity if worst else None
    return summary


@router.get("", response_model=List[schemas.ZoneSummary])
def list_zones(db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    ids = accessible_zone_ids(db, user)  # None = admin, sees everything
    query = db.query(models.Zone)
    if ids is not None:
        query = query.filter(models.Zone.id.in_(ids))
    return [_zone_summary(z) for z in query.order_by(models.Zone.id).all()]


@router.get("/{zone_id}", response_model=schemas.ZoneSummary)
def get_zone(zone_id: int, db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Not found")
    require_zone_view(db, user, zone_id)
    return _zone_summary(zone)


@router.post("", response_model=schemas.ZoneOut, status_code=201, dependencies=[Depends(require_role("admin"))])
def create_zone(payload: schemas.ZoneCreate, db: Session = Depends(get_db)):
    # A brand-new zone has no valves/schedules/thresholds yet, so it can only
    # start in manual mode - "по време"/"по прагове" are switched on later,
    # once they're set up (see _require_regime_configured).
    data = payload.model_dump()
    data["regime"] = "manual"
    zone = models.Zone(**data, is_active=False)
    db.add(zone)
    db.commit()
    db.refresh(zone)
    return zone


def _start_force_off(zone: models.Zone):
    """Zone activation / regime-change/deactivation safety sequence (see
    description_updated.docx, "Изчакване на изключване на консуматори при
    преход"): only STARTS a real off attempt - sets desired_state="off" on
    every valve in the zone and clears any manual clock-regime override
    (a systemic force-off should not be treated as "the schedule's own
    business", see Valve.manual_override), same as any other desired_state
    write. reconciler.py picks up the mismatch and drives it for real over
    MQTT, exactly like a manual command (see routers/devices.py) - this
    function does NOT wait for the result and does NOT know whether it will
    succeed. desired_state_setter.py recomputes each pump's desired_state
    as OR across ALL of its valves (any zone) on its own next tick, so a
    pump another zone is still legitimately using is never stolen here.

    health_checker.py's _progress_zone_transitions is what actually decides
    later whether the zone's "waiting" transition converged (every valve's
    current_state caught up) or failed (Valve.last_command_failed - see
    daemons/reconciler.py's _give_up) - see caller in update_zone below.
    A previous version of this function tried to decide success/failure
    SYNCHRONOUSLY, checking only Executor.is_active (a manually-toggled
    admin flag, not real reachability) - it never actually attempted MQTT
    at all, so it could report "success" while a real device was genuinely
    unreachable (found via live bench testing)."""
    for valve in zone.valves:
        valve.desired_state = "off"
        valve.manual_override = False
        valve.override_phase = None


def _require_no_transition(zone: models.Zone):
    """A zone stuck waiting for a stuck consumer to confirm off must not
    accept any other action - see 'Изчакване на изключване на консуматори
    при преход' in description_updated.docx. The admin has to resolve it
    first (continue with what's working, or deactivate the zone)."""
    if zone.transition_status != "none":
        raise HTTPException(
            409,
            "Зоната изчаква изключване на консуматори от предишен преход — "
            "продължи или деактивирай зоната от банера, преди да правиш нещо друго с нея",
        )


def _require_regime_configured(db: Session, zone: models.Zone, regime: str):
    """A zone must not run in "по време" / "по прагове" with nothing set up -
    it would just sit there doing nothing while looking like it's working."""
    if regime in ("clock", "threshold") and not zone.valves:
        raise HTTPException(
            400,
            "Тази зона няма клапани, затова не може да работи „По време“ или „По прагове“. "
            "Първо добави клапани към нея.",
        )
    if regime == "clock":
        ok = any(sch.enabled and sch.valve_links for sch in zone.schedules)
        if not ok:
            raise HTTPException(
                400,
                "Не може да се включи режим „По време“ — няма зададен график. "
                "Първо добави поне един график с клапан.",
            )
    elif regime == "threshold":
        linked = {
            (t.zone_id, t.param) for t in db.query(models.ThresholdValve).filter_by(zone_id=zone.id).all()
        }
        ok = any(
            (t.zone_id, t.param) in linked and (t.min_val is not None)
            for t in zone.thresholds
        )
        if not ok:
            raise HTTPException(
                400,
                "Не може да се включи режим „По прагове“ — няма зададен праг. "
                "Първо задай долна граница и клапан за поне един показател.",
            )


@router.patch("/{zone_id}", response_model=schemas.ZoneOut)
def update_zone(zone_id: int, payload: schemas.ZoneUpdate, db: Session = Depends(get_db),
                 user: models.User = Depends(get_current_user)):
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Not found")
    require_zone_control(db, user, zone_id)
    _require_no_transition(zone)

    activating = payload.is_active is True and not zone.is_active
    changing_regime_while_active = (
        payload.regime is not None and payload.regime != zone.regime and zone.is_active
    )
    deactivating = payload.is_active is False and zone.is_active

    final_active = payload.is_active if payload.is_active is not None else zone.is_active
    final_regime = payload.regime if payload.regime is not None else zone.regime
    # Checked whether or not the zone is active right now: an inactive zone
    # switched to "по време"/"по прагове" with nothing configured would just
    # fail later, on activation.
    if final_regime != "manual" and (activating or (payload.regime is not None and payload.regime != zone.regime)):
        _require_regime_configured(db, zone, final_regime)

    # A zone with no sensors (e.g. a purely manual valve someone flips by
    # eye) or no consumers (e.g. a monitoring-only zone with no irrigation)
    # is a real-world setup, not a misconfiguration - both are allowed.

    data = payload.model_dump(exclude_unset=True)
    target_regime = data.pop("regime", None)
    target_is_active = data.pop("is_active", None)
    for field, value in data.items():
        setattr(zone, field, value)

    if activating or changing_regime_while_active:
        # Real off attempt, but we do NOT know the outcome yet (reconciler.py
        # drives it for real over MQTT - see _start_force_off) - so the new
        # regime/is_active is NOT applied here. transition_status="waiting"
        # holds the zone (and blocks any other action on it, see
        # _require_no_transition above) until health_checker.py's
        # _progress_zone_transitions later either applies pending_regime/
        # pending_is_active (every valve confirmed off for real) or flips to
        # "error" (Valve.last_command_failed - see daemons/reconciler.py) for
        # the admin to resolve via /transition/continue or /transition/deactivate.
        _start_force_off(zone)
        zone.transition_status = "waiting"
        zone.pending_regime = target_regime if target_regime is not None else zone.regime
        zone.pending_is_active = target_is_active if target_is_active is not None else zone.is_active
    elif deactivating:
        # Deactivation applies immediately, unlike activation/regime-change -
        # there's no new regime waiting to take over, so nothing else is
        # racing this valve for its slot; is_active=False alone is enough to
        # stop every daemon loop from touching this zone again. Still starts
        # a real off attempt - if it fails, Valve.last_command_failed (see
        # daemons/reconciler.py) surfaces it exactly like a failed manual
        # command (VALVE_COMMAND_TIMEOUT, via health_checker.py), instead of
        # silently leaving a consumer in whatever state it was.
        _start_force_off(zone)
        zone.is_active = False
        if target_regime is not None:
            zone.regime = target_regime
    else:
        if target_regime is not None:
            zone.regime = target_regime
        if target_is_active is not None:
            zone.is_active = target_is_active

    db.commit()
    db.refresh(zone)
    return zone


@router.post("/{zone_id}/transition/continue", response_model=schemas.ZoneOut)
def continue_transition(zone_id: int, db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    """Admin chose to proceed with whatever DID turn off, leaving the stuck
    consumer's ZONE_ERRORS row open as an ongoing problem to fix later."""
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Not found")
    require_zone_control(db, user, zone_id)
    if zone.transition_status != "error":
        # "waiting" isn't stuck yet - reconciler.py may still genuinely
        # converge it any moment (see health_checker.py's
        # _progress_zone_transitions); jumping straight to "continue" here
        # would apply the pending regime/is_active before we actually know
        # whether the real off succeeded.
        raise HTTPException(400, "Зоната не чака решение (все още се опитва да се изключи, или не е в преход)")

    if zone.pending_regime is not None:
        zone.regime = zone.pending_regime
    if zone.pending_is_active is not None:
        zone.is_active = zone.pending_is_active
    zone.transition_status = "none"
    zone.pending_regime = None
    zone.pending_is_active = None
    db.commit()
    db.refresh(zone)
    return zone


@router.post("/{zone_id}/transition/deactivate", response_model=schemas.ZoneOut)
def deactivate_from_transition(zone_id: int, db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    """Admin gave up on the transition and just wants the zone off instead."""
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Not found")
    require_zone_control(db, user, zone_id)
    if zone.transition_status != "error":
        raise HTTPException(400, "Зоната не чака решение (все още се опитва да се изключи, или не е в преход)")

    zone.is_active = False
    for valve in zone.valves:
        valve.desired_state = "off"
    zone.transition_status = "none"
    zone.pending_regime = None
    zone.pending_is_active = None
    db.commit()
    db.refresh(zone)
    return zone


@router.get("/{zone_id}/errors", response_model=List[schemas.ZoneErrorOut])
def list_open_errors(zone_id: int, db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    if not db.get(models.Zone, zone_id):
        raise HTTPException(404, "Not found")
    require_zone_view(db, user, zone_id)
    return (
        db.query(models.ZoneError)
        .filter_by(zone_id=zone_id, resolved_at=None)
        .order_by(models.ZoneError.detected_at.desc())
        .all()
    )


@router.delete("/{zone_id}", status_code=204, dependencies=[Depends(require_role("admin"))])
def delete_zone(zone_id: int, db: Session = Depends(get_db)):
    """Sensors and valves aren't deleted with their zone - they just become
    unassigned (zone_id=None), same as an explicit unassign. Only the zone's
    own schedules/thresholds/errors/events (cascade="all, delete-orphan" on
    the Zone relationships) go away with it. UserZoneAccess isn't a Zone
    relationship (it's declared one-way, from User) and zone_id is part of
    its primary key so it can't be nulled - delete those rows explicitly."""
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Not found")
    for sensor in zone.sensors:
        sensor.zone_id = None
        sensor.layout = None
    for valve in list(zone.valves):
        _detach_valve_from_zone(db, valve)
    db.query(models.UserZoneAccess).filter_by(zone_id=zone_id).delete(synchronize_session=False)
    db.delete(zone)
    db.commit()


# --------------------------------------------------------- module assignment ----

def _require_inactive(zone: models.Zone):
    """Membership (which sensors/valves belong to a zone) can only change
    while the zone is deactivated - flipping it mid-operation could yank a
    module out from under a running schedule/threshold rule. Admin must
    explicitly deactivate first."""
    _require_no_transition(zone)
    if zone.is_active:
        raise HTTPException(
            409,
            "Зоната е активна - деактивирай я, преди да добавяш или махаш сензори/клапани",
        )


def _require_source_zone_inactive(source_zone: "models.Zone | None"):
    """A sensor/valve already sitting in a DIFFERENT zone can't be silently
    reassigned out from under that zone while it's still active - same
    reasoning as _require_inactive above, just checked against the module's
    CURRENT zone instead of the target one. Without this, assign_sensors/
    assign_valves only ever checked the target zone's is_active, so a module
    could be yanked out of an active, currently-running zone (breaking its
    schedule/threshold rule with no force-off, no confirmation, no error) as
    long as the TARGET zone happened to be inactive - found via live bench
    testing. A source zone that's the SAME as the target isn't checked here
    (that's already covered by _require_inactive on the target)."""
    if source_zone is None:
        return
    if source_zone.transition_status != "none" or source_zone.is_active:
        raise HTTPException(
            409,
            f"Зона „{source_zone.name}“ е активна - деактивирай я, преди да местиш сензори/клапани от нея",
        )


@router.post("/{zone_id}/sensors", dependencies=[Depends(require_role("admin"))])
def assign_sensors(zone_id: int, sensor_ids: List[str] = Body(embed=True), db: Session = Depends(get_db)):
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    _require_inactive(zone)
    for sid in sensor_ids:
        sensor = db.get(models.Sensor, sid)
        if not sensor:
            raise HTTPException(400, f"Unknown sensor {sid}")
        if sensor.zone_id is not None and sensor.zone_id != zone_id:
            _require_source_zone_inactive(sensor.zone)
        if sensor.zone_id != zone_id:
            sensor.layout = None  # its spot on the old zone's map means nothing here
        sensor.zone_id = zone_id
    db.commit()
    return {"ok": True}


@router.delete("/{zone_id}/sensors/{sensor_id}", dependencies=[Depends(require_role("admin"))])
def unassign_sensor(zone_id: int, sensor_id: str, db: Session = Depends(get_db)):
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    _require_inactive(zone)
    sensor = db.get(models.Sensor, sensor_id)
    if not sensor or sensor.zone_id != zone_id:
        raise HTTPException(404, "Not found in this zone")
    sensor.zone_id = None
    sensor.layout = None
    db.commit()
    return {"ok": True}


@router.post("/{zone_id}/valves", dependencies=[Depends(require_role("admin"))])
def assign_valves(zone_id: int, valve_ids: List[str] = Body(embed=True), db: Session = Depends(get_db)):
    """Only valves that already have both a pump and an executor set can be
    dropped into a zone - see the validation-gate discussion in
    description_updated.docx (nullable P_ID/M_ID + gate on zone assignment)."""
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    _require_inactive(zone)
    for vid in valve_ids:
        valve = db.get(models.Valve, vid)
        if not valve:
            raise HTTPException(400, f"Unknown valve {vid}")
        if not valve.pump_id or not valve.executor_id:
            raise HTTPException(
                400,
                f"Valve {vid} needs a pump and an executor assigned before it can join a zone",
            )
        if valve.zone_id is not None and valve.zone_id != zone_id:
            _require_source_zone_inactive(valve.zone)
        valve.zone_id = zone_id
    db.commit()
    return {"ok": True}


@router.delete("/{zone_id}/valves/{valve_id}", dependencies=[Depends(require_role("admin"))])
def unassign_valve(zone_id: int, valve_id: str, db: Session = Depends(get_db)):
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    _require_inactive(zone)
    valve = db.get(models.Valve, valve_id)
    if not valve or valve.zone_id != zone_id:
        raise HTTPException(404, "Not found in this zone")
    _detach_valve_from_zone(db, valve)
    db.commit()
    return {"ok": True}


# ---------------------------------------------------------------- schedules ----

@router.get("/{zone_id}/schedules", response_model=List[schemas.ZoneScheduleOut])
def list_schedules(zone_id: int, db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    require_zone_view(db, user, zone_id)
    out = []
    for s in db.query(models.ZoneSchedule).filter_by(zone_id=zone_id).all():
        item = schemas.ZoneScheduleOut.model_validate(s)
        item.valve_ids = [link.valve_id for link in s.valve_links]
        out.append(item)
    return out


@router.post("/{zone_id}/schedules", response_model=schemas.ZoneScheduleOut, status_code=201)
def create_schedule(zone_id: int, payload: schemas.ZoneScheduleCreate, db: Session = Depends(get_db),
                     user: models.User = Depends(get_current_user)):
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    require_zone_control(db, user, zone_id)
    _require_no_transition(zone)
    if not payload.valve_ids:
        raise HTTPException(400, "Интервалът трябва да отваря поне един клапан")
    if payload.start_time >= payload.end_time:
        raise HTTPException(400, "Краят на интервала трябва да е след началото")
    zone_valve_ids = {v.id for v in zone.valves}
    for vid in payload.valve_ids:
        if vid not in zone_valve_ids:
            raise HTTPException(400, f"Valve {vid} is not assigned to this zone")
    pump_capacity.check_new_schedule_fits(db, payload.valve_ids, payload.start_time, payload.end_time, payload.days_mask)

    obj = models.ZoneSchedule(
        zone_id=zone_id,
        start_time=payload.start_time,
        end_time=payload.end_time,
        days_mask=payload.days_mask,
        enabled=payload.enabled,
    )
    db.add(obj)
    db.flush()
    for vid in payload.valve_ids:
        db.add(models.ScheduleValve(schedule_id=obj.id, valve_id=vid))
    db.commit()
    db.refresh(obj)
    item = schemas.ZoneScheduleOut.model_validate(obj)
    item.valve_ids = payload.valve_ids
    return item


@router.put("/{zone_id}/schedules/{schedule_id}", response_model=schemas.ZoneScheduleOut)
def update_schedule(zone_id: int, schedule_id: int, payload: schemas.ZoneScheduleCreate,
                     db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    """Edit an existing interval in place (same checks as creating one, but it
    doesn't conflict with itself)."""
    zone = db.get(models.Zone, zone_id)
    obj = db.get(models.ZoneSchedule, schedule_id)
    if not zone or not obj or obj.zone_id != zone_id:
        raise HTTPException(404, "Not found")
    require_zone_control(db, user, zone_id)
    _require_no_transition(zone)
    if not payload.valve_ids:
        raise HTTPException(400, "Интервалът трябва да отваря поне един клапан")
    if payload.start_time >= payload.end_time:
        raise HTTPException(400, "Краят на интервала трябва да е след началото")
    zone_valve_ids = {v.id for v in zone.valves}
    for vid in payload.valve_ids:
        if vid not in zone_valve_ids:
            raise HTTPException(400, f"Valve {vid} is not assigned to this zone")
    pump_capacity.check_new_schedule_fits(
        db, payload.valve_ids, payload.start_time, payload.end_time, payload.days_mask,
        exclude_schedule_id=obj.id,
    )
    obj.start_time = payload.start_time
    obj.end_time = payload.end_time
    obj.days_mask = payload.days_mask
    obj.enabled = payload.enabled
    db.query(models.ScheduleValve).filter_by(schedule_id=obj.id).delete()
    for vid in payload.valve_ids:
        db.add(models.ScheduleValve(schedule_id=obj.id, valve_id=vid))
    db.commit()
    db.refresh(obj)
    item = schemas.ZoneScheduleOut.model_validate(obj)
    item.valve_ids = payload.valve_ids
    return item


@router.delete("/{zone_id}/schedules/{schedule_id}", status_code=204)
def delete_schedule(zone_id: int, schedule_id: int, db: Session = Depends(get_db),
                     user: models.User = Depends(get_current_user)):
    require_zone_control(db, user, zone_id)
    obj = db.get(models.ZoneSchedule, schedule_id)
    if not obj or obj.zone_id != zone_id:
        raise HTTPException(404, "Not found")
    zone = db.get(models.Zone, zone_id)
    if zone and zone.is_active and zone.regime == "clock":
        others = [x for x in zone.schedules if x.id != obj.id and x.enabled and x.valve_links]
        if not others:
            raise HTTPException(
                400,
                "Това е последният график на зоната, която работи „По време“. "
                "Първо смени режима или добави друг график.",
            )
    db.delete(obj)
    db.commit()


# --------------------------------------------------------------- thresholds ----

@router.get("/{zone_id}/thresholds", response_model=List[schemas.ZoneThresholdOut])
def list_thresholds(zone_id: int, db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    require_zone_view(db, user, zone_id)
    out = []
    for t in db.query(models.ZoneThreshold).filter_by(zone_id=zone_id).all():
        links = db.query(models.ThresholdValve).filter_by(zone_id=zone_id, param=t.param).all()
        item = schemas.ZoneThresholdOut.model_validate(t)
        item.valve_ids = [link.valve_id for link in links]
        out.append(item)
    return out


@router.post("/{zone_id}/thresholds", response_model=schemas.ZoneThresholdOut, status_code=201)
def upsert_threshold(zone_id: int, payload: schemas.ZoneThresholdCreate, db: Session = Depends(get_db),
                      user: models.User = Depends(get_current_user)):
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    require_zone_control(db, user, zone_id)
    _require_no_transition(zone)
    zone_valve_ids = {v.id for v in zone.valves}
    for vid in payload.valve_ids:
        if vid not in zone_valve_ids:
            raise HTTPException(400, f"Valve {vid} is not assigned to this zone")
    # THRESHOLD_MISCONFIGURED (zone_errors_catalog.docx): min_val >= max_val
    # is never meaningful (needs_water = reading < min_val would fire
    # permanently unless the reading is at/above max_val too) - reject it at
    # the source instead of only flagging it later (previously unvalidated,
    # found via live bench testing).
    if payload.min_val is not None and payload.max_val is not None and payload.min_val >= payload.max_val:
        raise HTTPException(
            400,
            f"Долната граница ({payload.min_val}) трябва да е по-малка от горната ({payload.max_val})",
        )
    pump_capacity.check_new_threshold_fits(db, zone_id, payload.param, payload.valve_ids)

    obj = db.get(models.ZoneThreshold, (zone_id, payload.param))
    if obj is None:
        obj = models.ZoneThreshold(zone_id=zone_id, param=payload.param)
        db.add(obj)
    obj.min_val = payload.min_val
    obj.max_val = payload.max_val
    obj.irrigation_duration_s = payload.irrigation_duration_s
    obj.infiltration_wait_s = payload.infiltration_wait_s

    db.query(models.ThresholdValve).filter_by(zone_id=zone_id, param=payload.param).delete()
    for vid in payload.valve_ids:
        db.add(models.ThresholdValve(zone_id=zone_id, param=payload.param, valve_id=vid))

    db.commit()
    item = schemas.ZoneThresholdOut.model_validate(obj)
    item.valve_ids = payload.valve_ids
    return item


@router.delete("/{zone_id}/thresholds/{param}", status_code=204)
def delete_threshold(zone_id: int, param: str, db: Session = Depends(get_db),
                      user: models.User = Depends(get_current_user)):
    zone = db.get(models.Zone, zone_id)
    obj = db.get(models.ZoneThreshold, (zone_id, param))
    if not zone or not obj:
        raise HTTPException(404, "Not found")
    require_zone_control(db, user, zone_id)
    _require_no_transition(zone)
    if zone.is_active and zone.regime == "threshold" and len(zone.thresholds) <= 1:
        raise HTTPException(
            400,
            "Това е единственият праг на зоната, която работи „По прагове“. "
            "Първо смени режима или задай друг праг.",
        )
    db.query(models.ThresholdValve).filter_by(zone_id=zone_id, param=param).delete()
    db.delete(obj)
    db.commit()


# ------------------------------------------------------------- sensor map ----

@router.get("/{zone_id}/layout", response_model=List[schemas.SensorLayoutItem])
def get_layout(zone_id: int, db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    require_zone_view(db, user, zone_id)
    return [
        schemas.SensorLayoutItem(sensor_id=s.id, x=s.layout.x, y=s.layout.y)
        for s in zone.sensors if s.layout is not None
    ]


@router.put("/{zone_id}/layout", response_model=List[schemas.SensorLayoutItem],
            dependencies=[Depends(require_role("admin"))])
def set_layout(zone_id: int, payload: schemas.SensorLayoutSet, db: Session = Depends(get_db)):
    """Replaces the whole map of the zone: sensors listed get that position,
    zone sensors not listed are taken off the map. Purely cosmetic, so it is
    allowed whether or not the zone is active."""
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    by_id = {s.id: s for s in zone.sensors}
    wanted = {}
    for item in payload.items:
        if item.sensor_id not in by_id:
            raise HTTPException(400, f"Сензор {item.sensor_id} не е в тази зона")
        wanted[item.sensor_id] = item
    for sid, sensor in by_id.items():
        if sid in wanted:
            if sensor.layout is None:
                sensor.layout = models.SensorLayout(x=wanted[sid].x, y=wanted[sid].y)
            else:
                sensor.layout.x = wanted[sid].x
                sensor.layout.y = wanted[sid].y
        elif sensor.layout is not None:
            sensor.layout = None
    db.commit()
    return list(wanted.values())


@router.get("/{zone_id}/objects", response_model=List[schemas.MapObjectItem])
def get_map_objects(zone_id: int, db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    require_zone_view(db, user, zone_id)
    return [schemas.MapObjectItem(kind=o.kind, label=o.label or "", x=o.x, y=o.y, w=o.w, h=o.h) for o in zone.map_objects]


@router.put("/{zone_id}/objects", response_model=List[schemas.MapObjectItem],
            dependencies=[Depends(require_role("admin"))])
def set_map_objects(zone_id: int, payload: schemas.MapObjectsSet, db: Session = Depends(get_db)):
    """Replaces all landmarks of the zone's map (cosmetic, always allowed)."""
    zone = db.get(models.Zone, zone_id)
    if not zone:
        raise HTTPException(404, "Zone not found")
    if len(payload.objects) > 60:
        raise HTTPException(400, "Твърде много обекти на картата (максимум 60)")
    zone.map_objects = [models.ZoneMapObject(**o.model_dump()) for o in payload.objects]
    db.commit()
    return payload.objects
