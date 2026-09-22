from typing import List

from fastapi import APIRouter, Depends, HTTPException
from sqlalchemy.orm import Session

from .. import models, schemas
from ..auth import hash_password, require_role
from ..database import get_db

router = APIRouter(prefix="/api/users", tags=["users"], dependencies=[Depends(require_role("admin"))])


def _user_out(user: models.User) -> schemas.UserOut:
    data = schemas.UserOut.model_validate(user)
    data.zones = [
        {"zone_id": link.zone_id, "zone_name": link.zone.name, "access_level": link.access_level}
        for link in user.zone_access
        if link.zone is not None  # zone was deleted; UserZoneAccess.zone_id can't be nulled (it's part of the PK)
    ]
    return data


@router.get("", response_model=List[schemas.UserOut])
def list_users(db: Session = Depends(get_db)):
    # The always-present system admin (seed.py) never shows up here - it's
    # not a real account to manage, just the guaranteed way in.
    return [_user_out(u) for u in db.query(models.User).filter_by(is_system=False).order_by(models.User.id).all()]


@router.post("", response_model=schemas.UserOut, status_code=201)
def create_user(payload: schemas.UserCreate, db: Session = Depends(get_db)):
    if db.query(models.User).filter_by(username=payload.username).first():
        raise HTTPException(409, "Username already taken")
    user = models.User(
        username=payload.username,
        password_hash=hash_password(payload.password),
        role=payload.role,
        full_name=payload.full_name,
        email=payload.email,
    )
    db.add(user)
    db.flush()
    for zone_id in payload.zone_ids:
        if not db.get(models.Zone, zone_id):
            raise HTTPException(400, f"Unknown zone_id {zone_id}")
        level = "control" if payload.role in ("admin", "agronomist") else "view"
        db.add(models.UserZoneAccess(user_id=user.id, zone_id=zone_id, access_level=level))
    db.commit()
    db.refresh(user)
    return _user_out(user)


@router.patch("/{user_id}", response_model=schemas.UserOut)
def update_user(user_id: int, payload: schemas.UserUpdate, db: Session = Depends(get_db)):
    user = db.get(models.User, user_id)
    if not user or user.is_system:
        raise HTTPException(404, "Not found")

    data = payload.model_dump(exclude_unset=True)
    zone_ids = data.pop("zone_ids", None)
    password = data.pop("password", None)
    for field, value in data.items():
        setattr(user, field, value)
    if password:
        user.password_hash = hash_password(password)

    if zone_ids is not None:
        db.query(models.UserZoneAccess).filter_by(user_id=user_id).delete()
        level = "control" if user.role in ("admin", "agronomist") else "view"
        for zone_id in zone_ids:
            if not db.get(models.Zone, zone_id):
                raise HTTPException(400, f"Unknown zone_id {zone_id}")
            db.add(models.UserZoneAccess(user_id=user_id, zone_id=zone_id, access_level=level))

    db.commit()
    db.refresh(user)
    return _user_out(user)


@router.delete("/{user_id}", status_code=204)
def delete_user(user_id: int, db: Session = Depends(get_db)):
    user = db.get(models.User, user_id)
    if not user or user.is_system:
        raise HTTPException(404, "Not found")
    db.delete(user)
    db.commit()
