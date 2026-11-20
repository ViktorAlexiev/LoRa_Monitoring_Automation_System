"""Battery monitoring feeds: the latest voltage of every device that has a
battery, and the history of one device."""

import datetime
from typing import List, Optional

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel
from sqlalchemy.orm import Session

from .. import battery, models
from ..auth import get_current_user
from ..database import get_db

router = APIRouter(prefix="/api/battery", tags=["battery"], dependencies=[Depends(get_current_user)])

KIND_MODEL = {"sensor": models.Sensor, "executor": models.Executor, "repeater": models.Repeater}


class BatteryItem(BaseModel):
    kind: str
    id: str
    external: bool = False      # a sensor of kind "external" (outside, reference)
    name: str = ""
    zones: List[str] = []       # sensors: the zone(s) they belong to
    mv: Optional[int] = None
    percent: Optional[int] = None
    level: str = "nodata"       # ok | low | critical | nodata
    age_minutes: Optional[int] = None
    is_active: bool = True


class BatteryPoint(BaseModel):
    mv: int
    percent: Optional[int] = None
    recorded_at: datetime.datetime


def _zone_names(db: Session, device) -> List[str]:
    if isinstance(device, models.Sensor):
        if device.kind == "external":
            names = {z.id: z.name for z in db.query(models.Zone).all()}
            return [names[i] for i in device.zone_ids if i in names]
        return [device.zone.name] if device.zone else []
    return []


@router.get("", response_model=List[BatteryItem])
def list_batteries(db: Session = Depends(get_db)):
    """Every device that is marked as having a battery, with its latest valid
    voltage (null = nothing received yet). Lowest battery first."""
    out = []
    for kind, model in KIND_MODEL.items():
        for dev in db.query(model).filter_by(has_battery=True).order_by(model.id).all():
            last = (
                db.query(models.BatteryReading).filter_by(kind=kind, device_id=dev.id)
                .order_by(models.BatteryReading.recorded_at.desc()).first()
            )
            pct = battery.percent(last.mv) if last else None
            out.append(BatteryItem(
                kind=kind, id=dev.id, name=dev.name or "", zones=_zone_names(db, dev),
                external=getattr(dev, "kind", "") == "external",
                mv=last.mv if last else None, percent=pct, level=battery.level(pct),
                age_minutes=max(0, int((datetime.datetime.utcnow() - last.recorded_at).total_seconds() // 60)) if last else None,
                is_active=bool(dev.is_active),
            ))
    out.sort(key=lambda i: (i.percent is None, i.percent if i.percent is not None else 0, i.id))
    return out


@router.get("/{kind}/{device_id}/history", response_model=List[BatteryPoint])
def battery_history(kind: str, device_id: str, hours: int = 24 * 7, db: Session = Depends(get_db)):
    if kind not in KIND_MODEL:
        raise HTTPException(404, "Unknown device type")
    since = datetime.datetime.utcnow() - datetime.timedelta(hours=min(max(hours, 1), 24 * 90))
    rows = (
        db.query(models.BatteryReading)
        .filter(models.BatteryReading.kind == kind, models.BatteryReading.device_id == device_id,
                models.BatteryReading.recorded_at >= since)
        .order_by(models.BatteryReading.recorded_at.asc()).limit(20000).all()
    )
    return [BatteryPoint(mv=r.mv, percent=battery.percent(r.mv), recorded_at=r.recorded_at) for r in rows]
