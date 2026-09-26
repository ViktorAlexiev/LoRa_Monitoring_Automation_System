import datetime

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy.orm import Session

from .. import config, models, pump_capacity, schemas
from ..auth import get_current_user, require_role, require_zone_control, require_zone_view, zone_access_level
from ..database import get_db

router = APIRouter(prefix="/api", tags=["devices"], dependencies=[Depends(get_current_user)])
_admin_only = [Depends(require_role("admin"))]


def _require_command_permission(db: Session, user: models.User, zone_id):
    """Manual valve/pump control: admin always, agronomist only for a zone
    they have control access to. A consumer not (yet) assigned to any zone
    has no agronomist to grant it through, so it's admin-only until it is."""
    if user.role == "admin":
        return
    if zone_id is None or zone_access_level(db, user, zone_id) != "control":
        raise HTTPException(403, "Нямаш права за управление на тоя консуматор")


def _detach_valve_from_zone(db: Session, valve: models.Valve):
    """A valve that loses its pump or executor no longer satisfies the
    zone-assignment gate (see description_updated.docx: a valve needs both
    set before it can join a zone) - drop it from its zone entirely, and
    with it any per-zone automation rule that referenced it."""
    if valve.zone_id is None:
        return
    db.query(models.ScheduleValve).filter_by(valve_id=valve.id).delete(synchronize_session=False)
    db.query(models.ThresholdValve).filter_by(valve_id=valve.id).delete(synchronize_session=False)
    valve.zone_id = None


# --------------------------------------------------------------- executors ----

@router.get("/executors", response_model=list[schemas.ExecutorOut])
def list_executors(db: Session = Depends(get_db)):
    return db.query(models.Executor).order_by(models.Executor.id).all()


@router.post("/executors", response_model=schemas.ExecutorOut, status_code=201, dependencies=_admin_only)
def create_executor(payload: schemas.ExecutorCreate, db: Session = Depends(get_db)):
    if db.get(models.Executor, payload.id):
        raise HTTPException(409, f"Executor {payload.id} already exists")
    obj = models.Executor(id=payload.id, name=payload.name)
    db.add(obj)
    db.commit()
    db.refresh(obj)
    return obj


@router.patch("/executors/{executor_id}", response_model=schemas.ExecutorOut, dependencies=_admin_only)
def update_executor(executor_id: str, payload: schemas.ExecutorUpdate, db: Session = Depends(get_db)):
    obj = db.get(models.Executor, executor_id)
    if not obj:
        raise HTTPException(404, "Not found")
    for field, value in payload.model_dump(exclude_unset=True).items():
        setattr(obj, field, value)
    db.commit()
    db.refresh(obj)
    return obj


@router.delete("/executors/{executor_id}", status_code=204, dependencies=_admin_only)
def delete_executor(executor_id: str, db: Session = Depends(get_db)):
    """Valves/pumps wired to this executor lose that link (executor_id=None)
    instead of being left pointing at a deleted row - a valve that loses its
    executor also gets dropped from its zone (see _detach_valve_from_zone),
    since it no longer satisfies the zone-assignment gate."""
    obj = db.get(models.Executor, executor_id)
    if not obj:
        raise HTTPException(404, "Not found")
    for valve in obj.valves:
        valve.executor_id = None
        _detach_valve_from_zone(db, valve)
    for pump in obj.pumps:
        pump.executor_id = None
    db.delete(obj)
    db.commit()


# --------------------------------------------------------------- repeaters ----

def _repeater_out(obj: models.Repeater) -> schemas.RepeaterOut:
    data = schemas.RepeaterOut.model_validate(obj)
    data.sensor_ids = [link.sensor_id for link in obj.sensors]
    return data


@router.get("/repeaters", response_model=list[schemas.RepeaterOut])
def list_repeaters(db: Session = Depends(get_db)):
    return [_repeater_out(r) for r in db.query(models.Repeater).order_by(models.Repeater.id).all()]


@router.post("/repeaters", response_model=schemas.RepeaterOut, status_code=201, dependencies=_admin_only)
def create_repeater(payload: schemas.RepeaterCreate, db: Session = Depends(get_db)):
    if db.get(models.Repeater, payload.id):
        raise HTTPException(409, f"Repeater {payload.id} already exists")
    obj = models.Repeater(id=payload.id, name=payload.name)
    db.add(obj)
    db.flush()
    for sensor_id in payload.sensor_ids:
        db.add(models.RepeaterSensor(repeater_id=obj.id, sensor_id=sensor_id))
    db.commit()
    db.refresh(obj)
    return _repeater_out(obj)


@router.patch("/repeaters/{repeater_id}", response_model=schemas.RepeaterOut, dependencies=_admin_only)
def update_repeater(repeater_id: str, payload: schemas.RepeaterUpdate, db: Session = Depends(get_db)):
    obj = db.get(models.Repeater, repeater_id)
    if not obj:
        raise HTTPException(404, "Not found")
    data = payload.model_dump(exclude_unset=True)
    sensor_ids = data.pop("sensor_ids", None)
    for field, value in data.items():
        setattr(obj, field, value)
    if sensor_ids is not None:
        db.query(models.RepeaterSensor).filter_by(repeater_id=repeater_id).delete()
        for sid in sensor_ids:
            db.add(models.RepeaterSensor(repeater_id=repeater_id, sensor_id=sid))
    db.commit()
    db.refresh(obj)
    return _repeater_out(obj)


@router.delete("/repeaters/{repeater_id}", status_code=204, dependencies=_admin_only)
def delete_repeater(repeater_id: str, db: Session = Depends(get_db)):
    obj = db.get(models.Repeater, repeater_id)
    if not obj:
        raise HTTPException(404, "Not found")
    db.delete(obj)
    db.commit()


# ----------------------------------------------------------------- gateway ----

@router.get("/gateway", response_model=list[schemas.GatewayOut])
def list_gateway(db: Session = Depends(get_db)):
    return db.query(models.Gateway).all()


@router.post("/gateway", response_model=schemas.GatewayOut, status_code=201, dependencies=_admin_only)
def create_gateway(payload: schemas.GatewayCreate, db: Session = Depends(get_db)):
    if db.get(models.Gateway, payload.id):
        raise HTTPException(409, f"Gateway {payload.id} already exists")
    obj = models.Gateway(id=payload.id, name=payload.name)
    db.add(obj)
    db.commit()
    db.refresh(obj)
    return obj


@router.patch("/gateway/{gateway_id}", response_model=schemas.GatewayOut, dependencies=_admin_only)
def update_gateway(gateway_id: str, payload: schemas.GatewayUpdate, db: Session = Depends(get_db)):
    obj = db.get(models.Gateway, gateway_id)
    if not obj:
        raise HTTPException(404, "Not found")
    for field, value in payload.model_dump(exclude_unset=True).items():
        setattr(obj, field, value)
    db.commit()
    db.refresh(obj)
    return obj


@router.delete("/gateway/{gateway_id}", status_code=204, dependencies=_admin_only)
def delete_gateway(gateway_id: str, db: Session = Depends(get_db)):
    obj = db.get(models.Gateway, gateway_id)
    if not obj:
        raise HTTPException(404, "Not found")
    db.delete(obj)
    db.commit()


# ------------------------------------------------------------------ sensors ----

@router.get("/sensors", response_model=list[schemas.SensorOut])
def list_sensors(db: Session = Depends(get_db)):
    out = []
    for s in db.query(models.Sensor).order_by(models.Sensor.id).all():
        link = db.query(models.RepeaterSensor).filter_by(sensor_id=s.id).first()
        item = schemas.SensorOut.model_validate(s)
        item.repeater_id = link.repeater_id if link else None
        out.append(item)
    return out


@router.post("/sensors", response_model=schemas.SensorOut, status_code=201, dependencies=_admin_only)
def create_sensor(payload: schemas.SensorCreate, db: Session = Depends(get_db)):
    if db.get(models.Sensor, payload.id):
        raise HTTPException(409, f"Sensor {payload.id} already exists")
    if payload.zone_id and not db.get(models.Zone, payload.zone_id):
        raise HTTPException(400, "Unknown zone_id")
    obj = models.Sensor(id=payload.id, name=payload.name, zone_id=payload.zone_id)
    db.add(obj)
    db.flush()
    if payload.repeater_id:
        db.add(models.RepeaterSensor(repeater_id=payload.repeater_id, sensor_id=obj.id))
    db.commit()
    db.refresh(obj)
    item = schemas.SensorOut.model_validate(obj)
    item.repeater_id = payload.repeater_id
    return item


@router.patch("/sensors/{sensor_id}", response_model=schemas.SensorOut, dependencies=_admin_only)
def update_sensor(sensor_id: str, payload: schemas.SensorUpdate, db: Session = Depends(get_db)):
    obj = db.get(models.Sensor, sensor_id)
    if not obj:
        raise HTTPException(404, "Not found")
    for field in ("name", "zone_id", "is_active"):
        value = getattr(payload, field)
        if value is not None:
            setattr(obj, field, value)
    if payload.repeater_id is not None:
        db.query(models.RepeaterSensor).filter_by(sensor_id=sensor_id).delete()
        if payload.repeater_id:
            db.add(models.RepeaterSensor(repeater_id=payload.repeater_id, sensor_id=sensor_id))
    db.commit()
    db.refresh(obj)
    link = db.query(models.RepeaterSensor).filter_by(sensor_id=sensor_id).first()
    item = schemas.SensorOut.model_validate(obj)
    item.repeater_id = link.repeater_id if link else None
    return item


@router.delete("/sensors/{sensor_id}", status_code=204, dependencies=_admin_only)
def delete_sensor(sensor_id: str, db: Session = Depends(get_db)):
    """Readings and repeater links cascade with the sensor (models.py
    relationships); ZONE_ERRORS rows referencing it don't (they're a log),
    so they're nulled out instead of left pointing at a deleted sensor_id."""
    obj = db.get(models.Sensor, sensor_id)
    if not obj:
        raise HTTPException(404, "Not found")
    db.query(models.ZoneError).filter_by(sensor_id=sensor_id).update({"sensor_id": None}, synchronize_session=False)
    db.delete(obj)
    db.commit()


# ------------------------------------------------------------------- pumps ----

@router.get("/pumps", response_model=list[schemas.PumpOut])
def list_pumps(db: Session = Depends(get_db)):
    return db.query(models.Pump).order_by(models.Pump.id).all()


@router.post("/pumps", response_model=schemas.PumpOut, status_code=201, dependencies=_admin_only)
def create_pump(payload: schemas.PumpCreate, db: Session = Depends(get_db)):
    if db.get(models.Pump, payload.id):
        raise HTTPException(409, f"Pump {payload.id} already exists")
    if payload.executor_id and not db.get(models.Executor, payload.executor_id):
        raise HTTPException(400, "Unknown executor_id")
    obj = models.Pump(**payload.model_dump())
    db.add(obj)
    db.commit()
    db.refresh(obj)
    return obj


@router.patch("/pumps/{pump_id}", response_model=schemas.PumpOut, dependencies=_admin_only)
def update_pump(pump_id: str, payload: schemas.PumpUpdate, db: Session = Depends(get_db)):
    obj = db.get(models.Pump, pump_id)
    if not obj:
        raise HTTPException(404, "Not found")
    data = payload.model_dump(exclude_unset=True)
    if data.get("executor_id") and not db.get(models.Executor, data["executor_id"]):
        raise HTTPException(400, "Unknown executor_id")

    new_max = data.get("max_simultaneous_valves")
    if new_max is not None and new_max < obj.max_simultaneous_valves:
        # Lowering capacity could retroactively break an already-saved
        # schedule's or threshold rule's guarantee - raising it never can,
        # so only check here.
        pump_capacity.check_all_existing_schedules_fit_pump(db, pump_id, new_max)
        pump_capacity.check_all_existing_thresholds_fit_pump(db, pump_id, new_max)

    for field, value in data.items():
        setattr(obj, field, value)
    db.commit()
    db.refresh(obj)
    return obj


@router.delete("/pumps/{pump_id}", status_code=204, dependencies=_admin_only)
def delete_pump(pump_id: str, db: Session = Depends(get_db)):
    """Valves wired to this pump lose that link (pump_id=None) and, since
    they no longer satisfy the zone-assignment gate, get dropped from their
    zone too - see _detach_valve_from_zone."""
    obj = db.get(models.Pump, pump_id)
    if not obj:
        raise HTTPException(404, "Not found")
    for valve in obj.valves:
        valve.pump_id = None
        _detach_valve_from_zone(db, valve)
    db.delete(obj)
    db.commit()


# ------------------------------------------------------------------ valves ----

@router.get("/valves", response_model=list[schemas.ValveOut])
def list_valves(db: Session = Depends(get_db)):
    return db.query(models.Valve).order_by(models.Valve.id).all()


@router.post("/valves", response_model=schemas.ValveOut, status_code=201, dependencies=_admin_only)
def create_valve(payload: schemas.ValveCreate, db: Session = Depends(get_db)):
    if db.get(models.Valve, payload.id):
        raise HTTPException(409, f"Valve {payload.id} already exists")
    if payload.pump_id and not db.get(models.Pump, payload.pump_id):
        raise HTTPException(400, "Unknown pump_id")
    if payload.executor_id and not db.get(models.Executor, payload.executor_id):
        raise HTTPException(400, "Unknown executor_id")
    obj = models.Valve(**payload.model_dump())
    db.add(obj)
    db.commit()
    db.refresh(obj)
    return obj


@router.patch("/valves/{valve_id}", response_model=schemas.ValveOut, dependencies=_admin_only)
def update_valve(valve_id: str, payload: schemas.ValveUpdate, db: Session = Depends(get_db)):
    obj = db.get(models.Valve, valve_id)
    if not obj:
        raise HTTPException(404, "Not found")
    data = payload.model_dump(exclude_unset=True)
    if data.get("pump_id") and not db.get(models.Pump, data["pump_id"]):
        raise HTTPException(400, "Unknown pump_id")
    if data.get("executor_id") and not db.get(models.Executor, data["executor_id"]):
        raise HTTPException(400, "Unknown executor_id")
    if data.get("zone_id") and not db.get(models.Zone, data["zone_id"]):
        raise HTTPException(400, "Unknown zone_id")

    pump_changing = "pump_id" in data and data["pump_id"] != obj.pump_id
    for field, value in data.items():
        setattr(obj, field, value)

    if pump_changing and obj.pump_id:
        # Reassigning this valve to a new pump could push an already-saved
        # schedule OR threshold rule of ITS over its capacity - re-check the
        # same way create_schedule/upsert_threshold does, and roll back if
        # it would.
        new_pump = db.get(models.Pump, obj.pump_id)
        try:
            pump_capacity.check_all_existing_schedules_fit_pump(db, obj.pump_id, new_pump.max_simultaneous_valves)
            pump_capacity.check_all_existing_thresholds_fit_pump(db, obj.pump_id, new_pump.max_simultaneous_valves)
        except HTTPException:
            db.rollback()
            raise

    db.commit()
    db.refresh(obj)
    return obj


@router.delete("/valves/{valve_id}", status_code=204, dependencies=_admin_only)
def delete_valve(valve_id: str, db: Session = Depends(get_db)):
    """Schedule/threshold links referencing this valve don't cascade on
    their own (they key off valve_id, not a relationship with a delete
    rule) - drop them explicitly. ZONE_ERRORS rows are a log, so they're
    nulled out instead of deleted."""
    obj = db.get(models.Valve, valve_id)
    if not obj:
        raise HTTPException(404, "Not found")
    db.query(models.ScheduleValve).filter_by(valve_id=valve_id).delete(synchronize_session=False)
    db.query(models.ThresholdValve).filter_by(valve_id=valve_id).delete(synchronize_session=False)
    db.query(models.ZoneError).filter_by(valve_id=valve_id).update({"valve_id": None}, synchronize_session=False)
    db.delete(obj)
    db.commit()


@router.post("/valves/{valve_id}/command", status_code=202)
def send_valve_command(valve_id: str, payload: schemas.ValveCommandCreate, db: Session = Depends(get_db),
                        user: models.User = Depends(get_current_user)):
    """Records a manual on/off request by writing desired_state only -
    reconciler.py picks up the mismatch and drives it for real over MQTT
    (see the comment below). 202 Accepted reflects that literally: this
    endpoint hands off the request, it doesn't confirm the valve actually
    moved."""
    obj = db.get(models.Valve, valve_id)
    if not obj:
        raise HTTPException(404, "Not found")
    _require_command_permission(db, user, obj.zone_id)

    if obj.zone and obj.zone.transition_status != "none":
        raise HTTPException(
            409,
            "Зоната изчаква изключване на консуматори от предишен преход — "
            "продължи или деактивирай зоната от банера, преди да я управляваш",
        )

    if obj.zone and obj.zone.regime == "threshold":
        raise HTTPException(
            409,
            "Зоната е в режим по прагове — ръчното управление е забранено, докато не смениш режима",
        )

    if payload.requested_state == "on" and obj.pump:
        occupants = [v for v in obj.pump.valves if v.id != obj.id and v.current_state == "on"]
        if len(occupants) + 1 > obj.pump.max_simultaneous_valves:
            REGIME_LABEL = {"manual": "ръчен", "clock": "по време", "threshold": "по прагове"}
            occupant_desc = "; ".join(
                f"{v.id} ({v.zone.name if v.zone else '—'}, режим: {REGIME_LABEL.get(v.zone.regime, v.zone.regime) if v.zone else '—'})"
                for v in occupants
            )
            raise HTTPException(
                409,
                f"Няма свободен слот за клапан {obj.id} — помпа {obj.pump.id} е заета от: {occupant_desc}. "
                f"Изчакай малко или изключи ръчно някой от тях, за да освободи слот.",
            )

    # Only WRITE desired_state here - never current_state, and never a
    # ValveCommand row directly. reconciler.py is the sole writer of both
    # (see its module docstring): its tick loop sees this desired != current
    # mismatch, sends the real MQTT command, and only advances current_state
    # once a real ACK plus the physical settle time have actually elapsed.
    # If reconciler.py isn't running (e.g. plain local dev with no daemons),
    # this simply never converges - which is honest: nothing configured
    # this executor for real, so nothing should claim it did.
    obj.desired_state = payload.requested_state
    # Clock-regime schedules are enforced by desired_state_setter.py, not
    # this endpoint - flag that a human just overrode this valve so that
    # daemon leaves it alone for the rest of the current interval occurrence
    # instead of re-asserting the schedule on its very next tick. Harmless
    # for manual/threshold zones, which don't read this flag.
    obj.manual_override = True
    obj.override_phase = None
    # Mirror desired_state_setter.py's _recompute_all_pumps for this one pump,
    # synchronously, right now - otherwise there's a race window (up to its
    # 5s tick interval) where reconciler.py's next 2s tick still sees the
    # pump's OLD desired_state, decides it isn't stopping, and closes the
    # valve before the pump - backwards from the intended
    # pump-off -> valve-closes / valve-opens -> pump-on sequencing (found via
    # live bench testing: switching a valve off left the pump running until
    # a stray later tick caught up).
    if obj.pump:
        obj.pump.desired_state = "on" if any(v.desired_state == "on" for v in obj.pump.valves) else "off"
    db.commit()
    return {"ok": True, "pending": obj.desired_state != obj.current_state}


@router.get("/sensors/{sensor_id}/readings", response_model=list[schemas.SensorReadingOut])
def get_sensor_readings(sensor_id: str, limit: int = None, hours: int = None, db: Session = Depends(get_db),
                         user: models.User = Depends(get_current_user)):
    sensor = db.get(models.Sensor, sensor_id)
    if not sensor:
        raise HTTPException(404, "Not found")
    if sensor.zone_id is not None:
        require_zone_view(db, user, sensor.zone_id)
    if hours is not None:
        # time-window mode (chart periods 24 h / 7 d / 30 d): everything in the
        # window, oldest first, capped so a runaway request can't dump the table
        since = datetime.datetime.utcnow() - datetime.timedelta(hours=min(max(hours, 1), 24 * 90))
        return (
            db.query(models.SensorReading)
            .filter(models.SensorReading.sensor_id == sensor_id, models.SensorReading.recorded_at >= since)
            .order_by(models.SensorReading.recorded_at.asc())
            .limit(50000)
            .all()
        )
    if limit is None:
        limit = config.CONFIG["sensor_readings"]["history_limit"]
    rows = (
        db.query(models.SensorReading)
        .filter_by(sensor_id=sensor_id)
        .order_by(models.SensorReading.recorded_at.desc())
        .limit(limit)
        .all()
    )
    return list(reversed(rows))


@router.post("/pumps/{pump_id}/command", status_code=202)
def send_pump_command(pump_id: str, payload: schemas.ValveCommandCreate, db: Session = Depends(get_db),
                       user: models.User = Depends(get_current_user)):
    """Manual pump toggle. In the real system a pump only ever turns on/off
    as a side effect of its valves (see reconciler.py's _recompute_all_pumps)
    - this direct toggle exists so the zone view has something to call for
    a pump with no valve currently demanding it. Like send_valve_command,
    this only writes desired_state - reconciler.py drives it for real.

    Turning a pump ON is refused unless at least one of its valves is
    already physically open (current_state='on') - see the guard below.
    reconciler.py's _drive_pumps never actually sends an "on" command
    without that (a pump must not run dry), and desired_state_setter.py's
    _recompute_all_pumps resets desired_state back to 'off' within its own
    5s tick regardless - so without this check, clicking "Включи" on an
    idle pump silently did nothing for real (no MQTT command ever sent) while
    the UI still showed "Изчаква потвърждение от устройството…", misleading
    the user into thinking something was in progress (found via live bench
    testing)."""
    obj = db.get(models.Pump, pump_id)
    if not obj:
        raise HTTPException(404, "Not found")
    if user.role != "admin":
        served_zones = {v.zone_id for v in obj.valves if v.zone_id}
        if not served_zones or not any(zone_access_level(db, user, zid) == "control" for zid in served_zones):
            raise HTTPException(403, "Нямаш права за управление на тая помпа")
    stuck_zone = next((v.zone for v in obj.valves if v.zone and v.zone.transition_status != "none"), None)
    if stuck_zone:
        raise HTTPException(
            409,
            f"Зона „{stuck_zone.name}“ изчаква изключване на консуматори от предишен преход — "
            "довърши го, преди да управляваш тая помпа",
        )
    threshold_zone = next((v.zone for v in obj.valves if v.zone and v.zone.regime == "threshold"), None)
    if threshold_zone:
        raise HTTPException(
            409,
            f"Зона „{threshold_zone.name}“ е в режим по прагове — ръчното управление на тая помпа е "
            "забранено, докато не смениш режима",
        )
    if payload.requested_state == "on" and not any(v.current_state == "on" for v in obj.valves):
        raise HTTPException(
            409,
            f"Помпа {obj.id} няма нито един физически отворен клапан в момента — включването ѝ без "
            "поне един отворен клапан е забранено (риск от работа на сухо). Отвори клапан от тая помпа първо.",
        )
    obj.desired_state = payload.requested_state
    db.commit()
    return {"ok": True, "pending": obj.desired_state != obj.current_state}
