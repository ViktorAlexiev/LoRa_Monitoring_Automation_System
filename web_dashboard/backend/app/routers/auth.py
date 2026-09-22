from fastapi import APIRouter, Cookie, Depends, HTTPException, Response
from sqlalchemy.orm import Session

from .. import models, schemas
from ..auth import (
    COOKIE_SECURE,
    SESSION_COOKIE,
    create_session,
    destroy_session,
    get_current_user,
    login_lock_remaining,
    register_failed_login,
    register_successful_login,
    session_duration,
    verify_password,
)
from ..database import get_db
from .users import _user_out  # reuse the same zone_access shaping as the admin Users screen

router = APIRouter(prefix="/api/auth", tags=["auth"])


@router.post("/login", response_model=schemas.UserOut)
def login(payload: schemas.LoginRequest, response: Response, db: Session = Depends(get_db)):
    user = db.query(models.User).filter_by(username=payload.username).first()

    if user is not None:
        remaining = login_lock_remaining(user)
        if remaining is not None:
            minutes = max(1, int(remaining.total_seconds() // 60) + 1)
            raise HTTPException(429, f"Прекалено много неуспешни опити — опитай пак след {minutes} мин")

    if user is None or not user.is_active or not verify_password(payload.password, user.password_hash):
        if user is not None and user.is_active:
            register_failed_login(db, user)
        raise HTTPException(401, "Грешно потребителско име или парола")

    register_successful_login(db, user)
    token = create_session(db, user)
    response.set_cookie(
        SESSION_COOKIE, token,
        httponly=True, samesite="lax", secure=COOKIE_SECURE,
        max_age=int(session_duration().total_seconds()),
    )
    return _user_out(user)


@router.post("/logout", status_code=204)
def logout(response: Response, db: Session = Depends(get_db), session_token: str = Cookie(default=None, alias=SESSION_COOKIE)):
    if session_token:
        destroy_session(db, session_token)
    response.delete_cookie(SESSION_COOKIE, httponly=True, samesite="lax", secure=COOKIE_SECURE)


@router.get("/me", response_model=schemas.UserOut)
def me(user: models.User = Depends(get_current_user)):
    return _user_out(user)
