"""Populates a fresh database with sample data so the UI has something to
show immediately after `docker`-free first run. Only runs when the zones
table is empty."""

import datetime

from sqlalchemy.orm import Session

from . import models
from .auth import hash_password


def seed(db: Session) -> None:
    if db.query(models.Zone).count() > 0:
        return

    z1 = models.Zone(name="Зона 1 · Домати", description="Оранжерия A, южен ред", regime="threshold", is_active=True)
    z2 = models.Zone(name="Зона 2 · Краставици", description="Оранжерия A, северен ред", regime="clock", is_active=True)
    z3 = models.Zone(name="Оранжерия Б", description="Разсад, южна стена", regime="manual", is_active=True)
    z4 = models.Zone(name="Открита площ В", description="Полски участък, зад склада", regime="threshold", is_active=False)
    db.add_all([z1, z2, z3, z4])
    db.flush()

    e1 = models.Executor(id="E001", name="Разпределително — Оранжерия A")
    e2 = models.Executor(id="E002", name="Разпределително — Оранжерия Б / В")
    db.add_all([e1, e2])

    r1 = models.Repeater(id="R001", name="Граничен, покрив Оранжерия A")
    db.add(r1)

    gw = models.Gateway(id="GW01", name="Централен gateway — техническо помещение")
    db.add(gw)

    p1 = models.Pump(id="P01", name="Главна помпа A", executor_id="E001", max_simultaneous_valves=2, current_state="on")
    p2 = models.Pump(id="P02", name="Помпа Б", executor_id="E002", max_simultaneous_valves=1, current_state="off")
    p3 = models.Pump(id="P03", name="Помпа В (кладенец)", executor_id="E002", max_simultaneous_valves=1, current_state="on")
    db.add_all([p1, p2, p3])
    db.flush()

    s1 = models.Sensor(id="S001", name="Почвен сензор — ред 1", zone_id=z1.id)
    s2 = models.Sensor(id="S002", name="Почвен сензор — ред 2", zone_id=z2.id)
    s3 = models.Sensor(id="S003", name="Почвен сензор — маси разсад", zone_id=z3.id)
    s4 = models.Sensor(id="S004", name="Почвен сензор — парцел В", zone_id=z4.id)
    db.add_all([s1, s2, s3, s4])
    db.flush()

    db.add_all([
        models.RepeaterSensor(repeater_id="R001", sensor_id="S002"),
        models.RepeaterSensor(repeater_id="R001", sensor_id="S004"),
    ])

    readings = [
        (s1, 24.1, 38, 27.4, 52),
        (s2, 22.6, 44, 26.9, 55),
        (s3, 20.8, 61, 24.2, 58),
        (s4, 18.3, 29, 23.1, 46),
    ]
    for sensor, st, sh, at, ah in readings:
        db.add(models.SensorReading(sensor_id=sensor.id, soil_t=st, soil_h=sh, air_t=at, air_h=ah))

    v1 = models.Valve(id="V01", name="Капково поливане — ред 1", zone_id=z1.id, pump_id="P01", executor_id="E001", current_state="off")
    v2 = models.Valve(id="V02", name="Капково поливане — ред 2", zone_id=z2.id, pump_id="P01", executor_id="E001", current_state="on")
    v3 = models.Valve(id="V03", name="Мъглуване", zone_id=z2.id, pump_id="P01", executor_id="E001", current_state="off")
    v4 = models.Valve(id="V04", name="Ръчно поливане разсад", zone_id=z3.id, pump_id="P02", executor_id="E002", current_state="off")
    v5 = models.Valve(id="V05", name="Капково — парцел В", zone_id=z4.id, pump_id="P03", executor_id="E002", current_state="on")
    db.add_all([v1, v2, v3, v4, v5])

    db.add(models.ZoneError(
        zone_id=z2.id, sensor_id="S002", error_code="SENSOR_STALE",
        severity="warning", description="Сензор S002 без промяна повече от 3 часа",
        detected_at=datetime.datetime.utcnow(),
    ))

    # Demo accounts, all with password "123" for now - a real deployment
    # would force a password reset before first use.
    admin = models.User(username="ivo", password_hash=hash_password("123"), role="admin", full_name="Ивайло П.", email="ivo@example.com")
    agro = models.User(username="agronom", password_hash=hash_password("123"), role="agronomist", full_name="Мария Иванова", email="maria@example.com")
    viewer = models.User(username="viewer", password_hash=hash_password("123"), role="viewer", full_name="Георги Тодоров", email="georgi@example.com")
    # The one always-present, never-shown-in-the-admin-table account - see
    # models.User.is_system. Guarantees a way in regardless of what happens
    # to the named accounts above.
    system_admin = models.User(
        username="admin", password_hash=hash_password("123"), role="admin",
        full_name="System Administrator", is_system=True,
    )
    db.add_all([admin, agro, viewer, system_admin])
    db.flush()

    db.add_all([
        models.UserZoneAccess(user_id=agro.id, zone_id=z1.id, access_level="control"),
        models.UserZoneAccess(user_id=agro.id, zone_id=z2.id, access_level="control"),
        models.UserZoneAccess(user_id=viewer.id, zone_id=z3.id, access_level="view"),
    ])

    db.commit()
