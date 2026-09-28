"""Whole-site map (all zones on one picture) and the admin diagnostics feeds."""

from typing import List, Optional

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy.orm import Session

from .. import audit, models, schemas
from ..auth import get_current_user, require_role
from ..database import get_db

router = APIRouter(prefix="/api/site", tags=["site"])


@router.get("/map", response_model=schemas.SiteMapOut)
def get_site_map(db: Session = Depends(get_db), _user: models.User = Depends(get_current_user)):
    """Everyone sees the same picture (zone boxes + landmarks). What a user
    may actually open is decided on the dashboard from the zone list's
    `accessible` flag - the map itself carries no readings."""
    zones = [
        schemas.SiteZoneItem(zone_id=p.zone_id, x=p.x, y=p.y, w=p.w, h=p.h)
        for p in db.query(models.SiteMapZone).all()
    ]
    objects = [
        schemas.MapObjectItem(kind=o.kind, label=o.label or "", color=o.color or "", x=o.x, y=o.y, w=o.w, h=o.h)
        for o in db.query(models.SiteMapObject).order_by(models.SiteMapObject.id).all()
    ]
    return schemas.SiteMapOut(zones=zones, objects=objects)


@router.put("/map", response_model=schemas.SiteMapOut)
def set_site_map(payload: schemas.SiteMapOut, db: Session = Depends(get_db),
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
    audit.log(db, user, "site_map_change",
              f"Обновена карта на целия обект: {len(payload.zones)} зони, {len(payload.objects)} ориентира")
    db.commit()
    return payload


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
