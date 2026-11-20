"""Whole-site map (all zones on one picture) and the admin diagnostics feeds."""

from typing import List, Optional

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy.orm import Session

from .. import audit, models, schemas
from ..auth import get_current_user, require_role
from ..database import get_db
from ..zone_stats import sensor_air_now
from .zones import _dew_point

router = APIRouter(prefix="/api/site", tags=["site"])


@router.get("/map", response_model=schemas.SiteMapOut)
def get_site_map(db: Session = Depends(get_db), _user: models.User = Depends(get_current_user)):
    """Everyone sees the same picture (zone boxes + landmarks + external
    sensors with their live outdoor values). What a user may actually open
    is decided on the dashboard from the zone list's `accessible` flag."""
    zones = [
        schemas.SiteZoneItem(zone_id=p.zone_id, x=p.x, y=p.y, w=p.w, h=p.h)
        for p in db.query(models.SiteMapZone).all()
    ]
    objects = [
        schemas.MapObjectItem(kind=o.kind, label=o.label or "", color=o.color or "", x=o.x, y=o.y, w=o.w, h=o.h)
        for o in db.query(models.SiteMapObject).order_by(models.SiteMapObject.id).all()
    ]
    sensors = []
    for p in db.query(models.SiteMapSensor).all():
        s = p.sensor
        if s is None or s.kind != "external":
            continue
        now = sensor_air_now(s)
        sensors.append(schemas.SiteSensorOut(
            sensor_id=s.id, x=p.x, y=p.y, name=s.name or "", is_active=bool(s.is_active),
            air_t=now["air_t"], air_h=now["air_h"], dew_point=_dew_point(now["air_t"], now["air_h"]),
            age_minutes=now["age_minutes"],
        ))
    return schemas.SiteMapOut(zones=zones, objects=objects, sensors=sensors)


@router.get("/external-sensors", response_model=List[schemas.ExternalSensorOut])
def list_external_sensors(db: Session = Depends(get_db), _user: models.User = Depends(get_current_user)):
    """All external (reference) sensors with their latest outdoor values and
    the zones they take part in - the dashboard's "Външни" tab. Outdoor air
    readings are not sensitive, so every signed-in user sees them (a zone
    the user may not open is only named, like on the zone list)."""
    zone_names = {z.id: z.name for z in db.query(models.Zone).all()}
    out = []
    for s in db.query(models.Sensor).filter_by(kind="external").order_by(models.Sensor.id).all():
        now = sensor_air_now(s)
        out.append(schemas.ExternalSensorOut(
            id=s.id, name=s.name or "", is_active=bool(s.is_active),
            zones=[{"id": zid, "name": zone_names.get(zid, f"Зона #{zid}")} for zid in s.zone_ids],
            air_t=now["air_t"], air_h=now["air_h"], dew_point=_dew_point(now["air_t"], now["air_h"]),
            age_minutes=now["age_minutes"],
        ))
    return out


@router.put("/map", response_model=schemas.SiteMapOut)
def set_site_map(payload: schemas.SiteMapIn, db: Session = Depends(get_db),
                 user: models.User = Depends(require_role("admin"))):
    """Replaces the whole picture. Zones not listed are taken off the map."""
    if len(payload.objects) > 100:
        raise HTTPException(400, "Твърде много обекти на картата (максимум 100)")
    existing = {z.id for z in db.query(models.Zone.id).all()}
    seen = set()
    for item in payload.zones:
        if item.zone_id not in existing:
            raise HTTPException(400, f"Зона {item.zone_id} не съществува")
        if item.zone_id in seen:
            raise HTTPException(400, f"Зона {item.zone_id} е сложена два пъти")
        seen.add(item.zone_id)

    db.query(models.SiteMapZone).delete(synchronize_session=False)
    for item in payload.zones:
        db.add(models.SiteMapZone(zone_id=item.zone_id, x=item.x, y=item.y, w=item.w, h=item.h))
    db.query(models.SiteMapObject).delete(synchronize_session=False)
    for o in payload.objects:
        db.add(models.SiteMapObject(**o.model_dump()))
    external_ids = {s.id for s in db.query(models.Sensor).filter_by(kind="external").all()}
    placed = set()
    for item in payload.sensors:
        if item.sensor_id not in external_ids:
            raise HTTPException(400, f"Сензор {item.sensor_id} не е външен сензор")
        if item.sensor_id in placed:
            raise HTTPException(400, f"Сензор {item.sensor_id} е сложен два пъти")
        placed.add(item.sensor_id)
    db.query(models.SiteMapSensor).delete(synchronize_session=False)
    for item in payload.sensors:
        db.add(models.SiteMapSensor(sensor_id=item.sensor_id, x=item.x, y=item.y))
    audit.log(db, user, "site_map_change",
              f"Обновена карта на целия обект: {len(payload.zones)} зони, {len(payload.objects)} ориентира, "
              f"{len(payload.sensors)} външни сензора")
    db.commit()
    return get_site_map(db, user)


@router.get("/errors/history", response_model=List[schemas.ZoneErrorOut],
            dependencies=[Depends(require_role("admin"))])
def errors_history(limit: int = 200, before_id: Optional[int] = None, db: Session = Depends(get_db)):
    """Problems that have already been resolved (newest first) - the
    diagnostics page shows them next to the open ones, so a problem that came
    and went while nobody was looking isn't lost."""
    query = db.query(models.ZoneError).filter(models.ZoneError.resolved_at.isnot(None))
    if before_id is not None:
        query = query.filter(models.ZoneError.id < before_id)
    return query.order_by(models.ZoneError.id.desc()).limit(min(max(limit, 1), 500)).all()
