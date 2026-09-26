from typing import List, Optional

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy.orm import Session

from .. import models, schemas
from ..auth import get_current_user, require_zone_control
from ..database import get_db

router = APIRouter(prefix="/api/audit", tags=["audit"])


@router.get("", response_model=List[schemas.AuditOut])
def list_audit(zone_id: Optional[int] = None, limit: int = 200, before_id: Optional[int] = None,
               db: Session = Depends(get_db), user: models.User = Depends(get_current_user)):
    """The history of human actions, newest first.

    Who sees what:
      - the whole history (all zones and system-wide actions): administrators only;
      - one zone's history (?zone_id=): anyone who may CONTROL that zone
        (agronomists and administrators) - viewers see no history.
    """
    if zone_id is None:
        if user.role != "admin":
            raise HTTPException(403, "Общата история е само за администратор")
    else:
        require_zone_control(db, user, zone_id)
    query = db.query(models.AuditLog)
    if zone_id is not None:
        query = query.filter(models.AuditLog.zone_id == zone_id)
    if before_id is not None:
        query = query.filter(models.AuditLog.id < before_id)
    return query.order_by(models.AuditLog.id.desc()).limit(min(max(limit, 1), 500)).all()
