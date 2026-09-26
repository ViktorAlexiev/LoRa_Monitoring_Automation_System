"""Audit trail: one line per human action. Callers add the row to the SAME
session/transaction as the change itself (so a change and its log entry
succeed or fail together) and commit as they already do."""

from . import models


def log(db, user, action, detail, zone=None, zone_id=None, zone_name=""):
    if zone is not None:
        zone_id, zone_name = zone.id, zone.name
    elif zone_id is not None and not zone_name:
        z = db.get(models.Zone, zone_id)
        zone_name = z.name if z else ""
    db.add(models.AuditLog(
        user_id=getattr(user, "id", None),
        username=getattr(user, "username", "") or "",
        display_name=(getattr(user, "full_name", "") or getattr(user, "username", "") or ""),
        action=action, detail=detail, zone_id=zone_id, zone_name=zone_name,
    ))
