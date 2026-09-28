"""Audit trail: one line per notable event - a human action (see log()) or a
system-detected one (see log_system()), such as a device restarting on its
own. Callers add the row to the SAME session/transaction as the change/
detection itself (so it succeeds or fails together with the row) and commit
as they already do."""

from . import models


def log(db, user, action, detail, zone=None, zone_id=None, zone_name="", executor_id=None):
    if zone is not None:
        zone_id, zone_name = zone.id, zone.name
    elif zone_id is not None and not zone_name:
        z = db.get(models.Zone, zone_id)
        zone_name = z.name if z else ""
    db.add(models.AuditLog(
        user_id=getattr(user, "id", None),
        username=getattr(user, "username", "") or "",
        display_name=(getattr(user, "full_name", "") or getattr(user, "username", "") or ""),
        action=action, detail=detail, zone_id=zone_id, zone_name=zone_name, executor_id=executor_id,
    ))


def log_system(db, action, detail, zone=None, zone_id=None, zone_name="", executor_id=None):
    """Same row shape as log(), but for something a daemon noticed rather
    than something a person did - user_id stays NULL and display_name reads
    "Системно" instead of blank, so it's obviously not attributed to
    whoever happens to view the history next."""
    if zone is not None:
        zone_id, zone_name = zone.id, zone.name
    elif zone_id is not None and not zone_name:
        z = db.get(models.Zone, zone_id)
        zone_name = z.name if z else ""
    db.add(models.AuditLog(
        user_id=None, username="", display_name="Системно",
        action=action, detail=detail, zone_id=zone_id, zone_name=zone_name, executor_id=executor_id,
    ))
