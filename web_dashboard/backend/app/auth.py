"""Login sessions and role-based access checks for the demo dashboard.

Session model: a random token lives in an httpOnly cookie on the client and
as a row in SESSIONS server-side. SESSIONS.expires_at is a SLIDING window
(session_duration() from now), pushed forward on every authenticated request
via get_current_user - not a fixed expiry from login time. A token that
hasn't been used in over CONFIG["security"]["session_timeout_minutes"] is
simply gone next time it's checked. Editable live from the admin "Настройки"
tab, same as every other CONFIG value - no restart needed.

Roles (models.User.role): "admin" (everything, including the admin panel),
"agronomist" (regime/schedule/threshold/manual-control for zones they have
access to, no admin panel), "viewer" ("Наблюдател" - read-only, monitoring
tabs only, for zones they have access to). Access to a specific zone is
UserZoneAccess; its access_level is derived from role at assignment time
(admin/agronomist -> control, viewer -> view - see routers/users.py), not
picked independently per zone.
"""

import datetime
import hashlib
import hmac
import os
import secrets
from typing import Optional

from fastapi import Cookie, Depends, HTTPException, Response
from sqlalchemy.orm import Session as DBSession

from . import models
from .config import CONFIG
from .database import get_db

SESSION_COOKIE = "session_token"
PBKDF2_ITERATIONS = 200_000

# Brute-force protection on /api/auth/login (see models.User.failed_login_attempts).
# Per-username, not per-IP - simpler, no need to trust proxy headers from
# nginx, and it's the account itself (not a source address) that a
# real-world attacker is actually trying to break into.
LOGIN_MAX_ATTEMPTS = 5
LOGIN_LOCKOUT_MINUTES = 15


def session_duration() -> datetime.timedelta:
    return datetime.timedelta(minutes=CONFIG["security"]["session_timeout_minutes"])

# The session cookie only travels safely over plain http BECAUSE this is a
# local dev demo. A real deployment always sits behind nginx+certbot (see
# description_updated.docx, "Контейнеризация чрез Docker") and MUST set
# COOKIE_SECURE=true there - a Secure cookie is refused entirely by the
# browser on http, which is exactly why this defaults to false and isn't
# just hardcoded true.
COOKIE_SECURE = os.environ.get("COOKIE_SECURE", "false").lower() == "true"


def hash_password(raw: str) -> str:
    salt = secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac("sha256", raw.encode("utf-8"), bytes.fromhex(salt), PBKDF2_ITERATIONS)
    return f"{salt}${digest.hex()}"


def verify_password(raw: str, stored: str) -> bool:
    if not stored or "$" not in stored:
        return False
    salt, expected_hex = stored.split("$", 1)
    digest = hashlib.pbkdf2_hmac("sha256", raw.encode("utf-8"), bytes.fromhex(salt), PBKDF2_ITERATIONS)
    return hmac.compare_digest(digest.hex(), expected_hex)


def login_lock_remaining(user: models.User) -> Optional[datetime.timedelta]:
    """None if not locked; otherwise how much longer the lockout lasts."""
    if user.locked_until is None:
        return None
    remaining = user.locked_until - datetime.datetime.utcnow()
    return remaining if remaining.total_seconds() > 0 else None


def register_failed_login(db: DBSession, user: models.User):
    user.failed_login_attempts = (user.failed_login_attempts or 0) + 1
    if user.failed_login_attempts >= LOGIN_MAX_ATTEMPTS:
        user.locked_until = datetime.datetime.utcnow() + datetime.timedelta(minutes=LOGIN_LOCKOUT_MINUTES)
        user.failed_login_attempts = 0  # the lockout itself is now the deterrent, not a growing counter
    db.commit()


def register_successful_login(db: DBSession, user: models.User):
    user.failed_login_attempts = 0
    user.locked_until = None
    db.commit()


def create_session(db: DBSession, user: models.User) -> str:
    token = secrets.token_urlsafe(32)
    db.add(models.Session(
        token=token, user_id=user.id,
        expires_at=datetime.datetime.utcnow() + session_duration(),
    ))
    db.commit()
    return token


def destroy_session(db: DBSession, token: str):
    db.query(models.Session).filter_by(token=token).delete()
    db.commit()


def get_current_user(
    response: Response,
    session_token: str = Cookie(default=None, alias=SESSION_COOKIE),
    db: DBSession = Depends(get_db),
) -> models.User:
    if not session_token:
        raise HTTPException(401, "Не си логнат")
    sess = db.query(models.Session).filter_by(token=session_token).first()
    now = datetime.datetime.utcnow()
    if sess is None or sess.expires_at < now:
        raise HTTPException(401, "Сесията е изтекла — влез отново")
    user = db.get(models.User, sess.user_id)
    if user is None or not user.is_active:
        raise HTTPException(401, "Акаунтът е деактивиран")
    duration = session_duration()
    sess.expires_at = now + duration  # sliding window - this request counts as activity
    db.commit()
    # The cookie's own max_age is a hard ceiling the BROWSER enforces on its
    # own, independent of the server-side row above - without re-sending it
    # here on every request, the browser would drop the cookie after the
    # window from login regardless of activity, silently defeating the
    # sliding window for any session longer than one timeout period.
    response.set_cookie(
        SESSION_COOKIE, session_token,
        httponly=True, samesite="lax", secure=COOKIE_SECURE,
        max_age=int(duration.total_seconds()),
    )
    return user


def require_role(*roles: str):
    def _dep(user: models.User = Depends(get_current_user)) -> models.User:
        if user.role not in roles:
            raise HTTPException(403, "Нямаш права за това действие")
        return user
    return _dep


def zone_access_level(db: DBSession, user: models.User, zone_id: int):
    """None (no access), "view", or "control". Admin always has "control"."""
    if user.role == "admin":
        return "control"
    link = db.get(models.UserZoneAccess, {"user_id": user.id, "zone_id": zone_id})
    return link.access_level if link else None


def require_zone_view(db: DBSession, user: models.User, zone_id: int):
    if zone_access_level(db, user, zone_id) is None:
        raise HTTPException(403, "Нямаш достъп до тая зона")


def require_zone_control(db: DBSession, user: models.User, zone_id: int):
    if zone_access_level(db, user, zone_id) != "control":
        raise HTTPException(403, "Нямаш права за управление на тая зона")


def accessible_zone_ids(db: DBSession, user: models.User):
    """None means "all zones" (admin) - callers should treat that specially
    rather than querying an actual (huge/irrelevant) id list."""
    if user.role == "admin":
        return None
    return {
        link.zone_id for link in
        db.query(models.UserZoneAccess).filter_by(user_id=user.id).all()
    }
