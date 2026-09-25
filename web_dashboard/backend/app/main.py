import os
import time
from typing import List

from fastapi import Depends, FastAPI
from fastapi.middleware.cors import CORSMiddleware
from sqlalchemy import inspect, text
from sqlalchemy.exc import OperationalError
from sqlalchemy.orm import Session

from . import models, schemas
from .auth import get_current_user, hash_password
from .database import Base, SessionLocal, engine, get_db
from .routers import auth, config, devices, users, zones
from .seed import seed

# MySQL's official image does its first-boot init in two phases with an
# internal restart in between (see docker-compose.yml's healthcheck) - the
# DB can refuse connections for a few seconds even after compose considers
# it "healthy". Retry instead of crashing the whole container on that one
# race - SQLite never hits this at all (connects to a local file instantly).
for attempt in range(10):
    try:
        engine.connect().close()
        break
    except OperationalError:
        if attempt == 9:
            raise
        time.sleep(2)

Base.metadata.create_all(bind=engine)

# SQLAlchemy's create_all only creates missing TABLES, not missing columns on
# tables that already exist - patch newly added Zone columns onto an
# existing dev database here instead of pulling in a full migration tool.
_existing_zone_cols = {c["name"] for c in inspect(engine).get_columns("zones")}
_zone_migrations = {
    "transition_status": "VARCHAR(16) DEFAULT 'none'",
    "pending_regime": "VARCHAR(16)",
    "pending_is_active": "BOOLEAN",
    "humidity_warn_min": "FLOAT",
    "humidity_warn_max": "FLOAT",
}
with engine.begin() as conn:
    for col, ddl_type in _zone_migrations.items():
        if col not in _existing_zone_cols:
            conn.execute(text(f"ALTER TABLE zones ADD COLUMN {col} {ddl_type}"))

# Pump's timing columns were renamed to match what they actually mean
# (pump startup/shutdown time, not "valve settle"/"min off") - rename the
# existing columns in place so existing data survives.
_existing_pump_cols = {c["name"] for c in inspect(engine).get_columns("pumps")}
_pump_renames = {
    "valve_settle_s": "startup_time_s",
    "min_off_time_s": "shutdown_time_s",
}
with engine.begin() as conn:
    for old_col, new_col in _pump_renames.items():
        if old_col in _existing_pump_cols and new_col not in _existing_pump_cols:
            conn.execute(text(f"ALTER TABLE pumps RENAME COLUMN {old_col} TO {new_col}"))

# reconciler.py's _give_up() (see its module docstring) flips this the
# instant a command terminally fails, so health_checker.py can reliably
# open/resolve PUMP_COMMAND_TIMEOUT from it.
if "last_command_failed" not in _existing_pump_cols:
    with engine.begin() as conn:
        conn.execute(text("ALTER TABLE pumps ADD COLUMN last_command_failed BOOLEAN DEFAULT 0"))

# Valves get their own opening/closing time columns (previously the pump's
# timing fields were edited via the valve form as a stand-in - now valves
# have real columns of their own).
_existing_valve_cols = {c["name"] for c in inspect(engine).get_columns("valves")}
_valve_migrations = {
    "opening_time_s": "INTEGER DEFAULT 3",
    "closing_time_s": "INTEGER DEFAULT 3",
    "manual_override": "BOOLEAN DEFAULT 0",
    "override_phase": "BOOLEAN",
    # See Pump.last_command_failed above - same mechanism, VALVE_COMMAND_TIMEOUT.
    "last_command_failed": "BOOLEAN DEFAULT 0",
}
with engine.begin() as conn:
    for col, ddl_type in _valve_migrations.items():
        if col not in _existing_valve_cols:
            conn.execute(text(f"ALTER TABLE valves ADD COLUMN {col} {ddl_type}"))

# zone_errors.zone_id became nullable (a network-wide problem, e.g. the
# Gateway itself offline, isn't "on" any one zone). SQLite can't relax a
# NOT NULL constraint with a plain ALTER, so rebuild the table when needed.
_zone_error_cols = {c["name"]: c for c in inspect(engine).get_columns("zone_errors")}
if _zone_error_cols and not _zone_error_cols["zone_id"]["nullable"]:
    with engine.begin() as conn:
        conn.execute(text("""
            CREATE TABLE zone_errors_new (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                zone_id INTEGER,
                sensor_id VARCHAR(6),
                valve_id VARCHAR(4),
                pump_id VARCHAR(4),
                executor_id VARCHAR(6),
                error_code VARCHAR(32),
                severity VARCHAR(16),
                description VARCHAR(255),
                detected_at DATETIME,
                resolved_at DATETIME
            )
        """))
        conn.execute(text("INSERT INTO zone_errors_new SELECT * FROM zone_errors"))
        conn.execute(text("DROP TABLE zone_errors"))
        conn.execute(text("ALTER TABLE zone_errors_new RENAME TO zone_errors"))

_existing_zone_error_cols = {c["name"] for c in inspect(engine).get_columns("zone_errors")}
if "repeater_id" not in _existing_zone_error_cols:
    with engine.begin() as conn:
        conn.execute(text("ALTER TABLE zone_errors ADD COLUMN repeater_id VARCHAR(6)"))

# acked_at: when the real MQTT commands_status reply arrives, separate from
# resolved_at (which also waits for the physical settle time) - see
# ValveCommand.acked_at.
for table in ("valve_commands", "pump_commands"):
    _cols = {c["name"] for c in inspect(engine).get_columns(table)}
    if "acked_at" not in _cols:
        with engine.begin() as conn:
            conn.execute(text(f"ALTER TABLE {table} ADD COLUMN acked_at DATETIME"))

# rssi/snr turned out not to be worth keeping - a reading only needs the one
# simple recorded_at timestamp, and the radio-quality columns just sat there
# unused. SQLite here predates ALTER TABLE DROP COLUMN (3.35+), so drop them
# by rebuilding the table instead - same pattern as zone_errors above.
_existing_reading_cols = {c["name"] for c in inspect(engine).get_columns("sensor_readings")}
if "rssi" in _existing_reading_cols or "snr" in _existing_reading_cols:
    with engine.begin() as conn:
        conn.execute(text("""
            CREATE TABLE sensor_readings_new (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                sensor_id VARCHAR(6) NOT NULL,
                soil_t FLOAT,
                soil_h FLOAT,
                air_t FLOAT,
                air_h FLOAT,
                recorded_at DATETIME
            )
        """))
        conn.execute(text(
            "INSERT INTO sensor_readings_new (id, sensor_id, soil_t, soil_h, air_t, air_h, recorded_at) "
            "SELECT id, sensor_id, soil_t, soil_h, air_t, air_h, recorded_at FROM sensor_readings"
        ))
        conn.execute(text("DROP TABLE sensor_readings"))
        conn.execute(text("ALTER TABLE sensor_readings_new RENAME TO sensor_readings"))

# recorded_at is now indexed (see models.py) - every "readings since X" query
# in this app filters on it, and so does the retention cleanup
# (daemons/health_checker.py). Existing databases predate the index.
_existing_reading_indexes = {ix["name"] for ix in inspect(engine).get_indexes("sensor_readings")}
if "ix_sensor_readings_recorded_at" not in _existing_reading_indexes:
    with engine.begin() as conn:
        conn.execute(text("CREATE INDEX ix_sensor_readings_recorded_at ON sensor_readings (recorded_at)"))

_existing_repeater_cols = {c["name"] for c in inspect(engine).get_columns("repeaters")}
if "last_rssi" in _existing_repeater_cols or "last_snr" in _existing_repeater_cols:
    with engine.begin() as conn:
        conn.execute(text("""
            CREATE TABLE repeaters_new (
                id VARCHAR(6) PRIMARY KEY,
                name VARCHAR(255),
                is_active BOOLEAN,
                last_heartbeat_at DATETIME
            )
        """))
        conn.execute(text(
            "INSERT INTO repeaters_new (id, name, is_active, last_heartbeat_at) "
            "SELECT id, name, is_active, last_heartbeat_at FROM repeaters"
        ))
        conn.execute(text("DROP TABLE repeaters"))
        conn.execute(text("ALTER TABLE repeaters_new RENAME TO repeaters"))

_existing_user_cols = {c["name"] for c in inspect(engine).get_columns("users")}
if "is_system" not in _existing_user_cols:
    with engine.begin() as conn:
        conn.execute(text("ALTER TABLE users ADD COLUMN is_system BOOLEAN DEFAULT 0"))
_user_lockout_migrations = {
    "failed_login_attempts": "INTEGER DEFAULT 0",
    "locked_until": "DATETIME",
}
for col, ddl_type in _user_lockout_migrations.items():
    if col not in _existing_user_cols:
        with engine.begin() as conn:
            conn.execute(text(f"ALTER TABLE users ADD COLUMN {col} {ddl_type}"))

with SessionLocal() as db:
    # SEED_DEMO_DATA=false for a real deployment (see docker-compose.yml) -
    # a fresh production database should be empty except for the guaranteed
    # hidden admin account (created unconditionally below, regardless of
    # this flag). Defaults to true so local/dev runs still get sample data.
    if os.environ.get("SEED_DEMO_DATA", "true").lower() == "true":
        seed(db)

    # Real login is new - a database that predates it has a demo user
    # literally named "admin" (used only for topbar display, not auth),
    # which collides with the always-present hidden system account this
    # feature needs. Rename the old one out of the way exactly once - after
    # this, no non-system row will ever have username "admin" again.
    legacy_admin = db.query(models.User).filter_by(username="admin", is_system=False).first()
    if legacy_admin:
        legacy_admin.username = "ivo"

    # Demo accounts predating login had no real password - give them one so
    # the demo is actually usable, without touching accounts created since.
    for u in db.query(models.User).filter_by(is_system=False).all():
        if not u.password_hash:
            u.password_hash = hash_password("123")

    if not db.query(models.User).filter_by(is_system=True).first():
        # Configurable via .env (see docker-compose.yml) - defaults match
        # local/dev only; a real deployment should always override both.
        db.add(models.User(
            username=os.environ.get("SYSTEM_ADMIN_USERNAME", "admin"),
            password_hash=hash_password(os.environ.get("SYSTEM_ADMIN_PASSWORD", "123")),
            role="admin", full_name="System Administrator", is_system=True,
        ))
    db.commit()

app = FastAPI(title="АгроМонитор API", version="0.1.0")

app.add_middleware(
    CORSMiddleware,
    # Matches any localhost/127.0.0.1 port - Vite falls back to 5174, 5175...
    # if 5173 is already taken, so pinning one exact origin was too brittle.
    allow_origin_regex=r"http://(localhost|127\.0\.0\.1):\d+",
    allow_credentials=True,  # the session cookie needs this to travel cross-origin (5173 -> 8000)
    allow_methods=["*"],
    allow_headers=["*"],
)

app.include_router(auth.router)
app.include_router(zones.router)
app.include_router(devices.router)
app.include_router(users.router)
app.include_router(config.router)


@app.get("/api/health")
def health():
    return {"status": "ok"}


@app.get("/api/errors/open", response_model=List[schemas.ZoneErrorOut])
def list_open_errors_all(db: Session = Depends(get_db), _user: models.User = Depends(get_current_user)):
    """Every open problem (any zone or none) - the admin device tables use it
    to show which devices are actually in trouble."""
    return (
        db.query(models.ZoneError)
        .filter(models.ZoneError.resolved_at.is_(None))
        .order_by(models.ZoneError.detected_at.desc())
        .all()
    )


@app.get("/api/errors/network", response_model=List[schemas.ZoneErrorOut])
def list_network_errors(db: Session = Depends(get_db), _user: models.User = Depends(get_current_user)):
    """Open problems not attributable to any single zone (zone_id IS NULL) -
    e.g. the Gateway itself being offline. Per-zone problems stay reachable
    the usual way, via GET /api/zones/{id}/errors."""
    return (
        db.query(models.ZoneError)
        .filter(models.ZoneError.zone_id.is_(None), models.ZoneError.resolved_at.is_(None))
        .order_by(models.ZoneError.detected_at.desc())
        .all()
    )
