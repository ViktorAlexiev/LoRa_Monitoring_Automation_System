import os
from pathlib import Path

from sqlalchemy import create_engine, event
from sqlalchemy.orm import declarative_base, sessionmaker

BASE_DIR = Path(__file__).resolve().parent.parent
DATA_DIR = BASE_DIR / "data"
DATA_DIR.mkdir(exist_ok=True)

# Defaults to a local SQLite file so the app runs with zero external setup.
# To point at the real MySQL deployment instead, set DATABASE_URL, e.g.:
#   mysql+pymysql://user:password@localhost:3306/dbname
DATABASE_URL = os.environ.get("DATABASE_URL", f"sqlite:///{DATA_DIR / 'app.db'}")
IS_SQLITE = DATABASE_URL.startswith("sqlite")

# SQLite-only: the demo/local-dev setup is several separate OS processes
# (web-api + reconciler + desired_state_setter + health_checker +
# mqtt_bridge), all hitting one file - SQLite's default rollback-journal
# mode holds an exclusive lock for the whole duration of a write, so two of
# those processes writing at once throws "database is locked" instead of
# waiting (found via live multi-daemon testing, worse the more sensors are
# reporting). WAL mode lets readers and one writer run concurrently; the
# busy_timeout below covers the remaining writer-vs-writer case by waiting
# up to 10s instead of failing immediately. Irrelevant for the real
# deployment (MySQL, a real client-server engine with its own locking).
connect_args = {"check_same_thread": False, "timeout": 10} if IS_SQLITE else {}
engine = create_engine(DATABASE_URL, connect_args=connect_args)

if IS_SQLITE:
    @event.listens_for(engine, "connect")
    def _sqlite_pragmas(dbapi_connection, _record):
        cur = dbapi_connection.cursor()
        cur.execute("PRAGMA journal_mode=WAL")
        cur.execute("PRAGMA busy_timeout=10000")
        cur.close()
        # SQLite's built-in lower()/upper() only fold ASCII, so a case-insensitive
        # search (SQLAlchemy's ilike -> lower(col) LIKE lower(x)) never matched
        # Cyrillic typed in another case ("аварийно" vs "АВАРИЙНО"). Replace them
        # with Unicode-aware versions. (MySQL's default collation already folds
        # Cyrillic, so the real deployment needs nothing.)
        dbapi_connection.create_function("lower", 1, lambda v: v.lower() if isinstance(v, str) else v, deterministic=True)
        dbapi_connection.create_function("upper", 1, lambda v: v.upper() if isinstance(v, str) else v, deterministic=True)
SessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)

Base = declarative_base()


def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()
