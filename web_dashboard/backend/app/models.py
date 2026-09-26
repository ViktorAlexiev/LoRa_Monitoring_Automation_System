import datetime

from sqlalchemy import (
    Boolean,
    Column,
    DateTime,
    Enum,
    Float,
    ForeignKey,
    Integer,
    String,
    Text,
    UniqueConstraint,
)
from sqlalchemy.orm import relationship

from .database import Base


def now():
    return datetime.datetime.utcnow()


# ---------------------------------------------------------------- zones ----

class Zone(Base):
    __tablename__ = "zones"

    id = Column(Integer, primary_key=True, autoincrement=True)
    name = Column(String(64), nullable=False)
    description = Column(String(255), default="")
    regime = Column(Enum("manual", "clock", "threshold", name="zone_regime"), default="manual")
    is_active = Column(Boolean, default=False)
    created_at = Column(DateTime, default=now)

    # Activation / regime-change transition (see description_updated.docx,
    # "Изчакване на изключване на консуматори при преход"). While
    # transition_status != "none", desired_state_setter.py must skip this
    # zone entirely - it's waiting on the admin to resolve a stuck consumer,
    # not running its regime's normal automation.
    transition_status = Column(Enum("none", "waiting", "error", name="zone_transition_status"), default="none")
    pending_regime = Column(Enum("manual", "clock", "threshold", name="zone_pending_regime"), nullable=True)
    pending_is_active = Column(Boolean, nullable=True)

    # Zone-wide soil-moisture safety bounds - independent of regime, unlike
    # ZONE_THRESHOLDS.min_val/max_val (which only exist for/drive threshold-
    # regime irrigation). These just raise a warning when soil moisture is
    # outside them, and clock-regime uses max as an overwatering guard
    # before opening a scheduled valve (see health_checker.py / daemons/
    # desired_state_setter.py).
    humidity_warn_min = Column(Float, nullable=True)
    humidity_warn_max = Column(Float, nullable=True)

    sensors = relationship("Sensor", back_populates="zone")
    valves = relationship("Valve", back_populates="zone")
    schedules = relationship("ZoneSchedule", back_populates="zone", cascade="all, delete-orphan")
    map_objects = relationship("ZoneMapObject", back_populates="zone", cascade="all, delete-orphan")
    thresholds = relationship("ZoneThreshold", back_populates="zone", cascade="all, delete-orphan")
    errors = relationship("ZoneError", back_populates="zone", cascade="all, delete-orphan")
    events = relationship("ZoneEvent", back_populates="zone", cascade="all, delete-orphan")


# ------------------------------------------------------- physical modules ----

class Executor(Base):
    __tablename__ = "executors"

    id = Column(String(6), primary_key=True)  # module_id, e.g. "E001"
    name = Column(String(255), default="")
    is_active = Column(Boolean, default=True)
    last_heartbeat_at = Column(DateTime, nullable=True)

    valves = relationship("Valve", back_populates="executor")
    pumps = relationship("Pump", back_populates="executor")


class Repeater(Base):
    __tablename__ = "repeaters"

    id = Column(String(6), primary_key=True)  # e.g. "R001"
    name = Column(String(255), default="")
    is_active = Column(Boolean, default=True)
    last_heartbeat_at = Column(DateTime, nullable=True)

    sensors = relationship("RepeaterSensor", back_populates="repeater", cascade="all, delete-orphan")


class Gateway(Base):
    __tablename__ = "gateway"

    id = Column(String(6), primary_key=True)  # e.g. "GW01"
    name = Column(String(255), default="")
    is_active = Column(Boolean, default=True)
    last_heartbeat_at = Column(DateTime, nullable=True)


class Sensor(Base):
    __tablename__ = "sensors"

    id = Column(String(6), primary_key=True)  # e.g. "S001"
    name = Column(String(255), default="")
    zone_id = Column(Integer, ForeignKey("zones.id"), nullable=True)
    is_active = Column(Boolean, default=True)

    zone = relationship("Zone", back_populates="sensors")
    readings = relationship("SensorReading", back_populates="sensor", cascade="all, delete-orphan")
    repeaters = relationship("RepeaterSensor", back_populates="sensor", cascade="all, delete-orphan")
    layout = relationship("SensorLayout", back_populates="sensor", uselist=False, cascade="all, delete-orphan")


class SensorLayout(Base):
    """Where a sensor sits on the zone's site map (admin arranges it to mirror
    the real layout of the site). x/y are percentages of the map, 0-100.
    A separate table (not columns on sensors) so existing databases pick it up
    through create_all without a migration."""

    __tablename__ = "sensor_layout"

    sensor_id = Column(String(6), ForeignKey("sensors.id"), primary_key=True)
    x = Column(Float, nullable=False)
    y = Column(Float, nullable=False)

    sensor = relationship("Sensor", back_populates="layout")


class ZoneMapObject(Base):
    """A rough landmark drawn on the zone's site map (building, gate, well,
    road, ...) so the worker can orient himself. Position/size are percentages
    of the map. Cosmetic only."""

    __tablename__ = "zone_map_objects"

    id = Column(Integer, primary_key=True, autoincrement=True)
    zone_id = Column(Integer, ForeignKey("zones.id"), nullable=False)
    kind = Column(String(16), nullable=False)
    label = Column(String(64), default="")
    color = Column(String(16), default="")  # palette key chosen by the admin; empty = the kind's own colour
    x = Column(Float, nullable=False)
    y = Column(Float, nullable=False)
    w = Column(Float, nullable=False)
    h = Column(Float, nullable=False)

    zone = relationship("Zone", back_populates="map_objects")


class AuditLog(Base):
    """Who did what, when - human actions only (manual valve/pump commands,
    mode changes, schedule/threshold edits, emergency stops). Names are
    snapshots so the history stays readable after a user or zone is
    renamed or deleted."""

    __tablename__ = "audit_log"

    id = Column(Integer, primary_key=True, autoincrement=True)
    at = Column(DateTime, default=now, index=True)
    user_id = Column(Integer, nullable=True)
    username = Column(String(64), default="")
    display_name = Column(String(128), default="")
    action = Column(String(32), nullable=False)
    zone_id = Column(Integer, nullable=True, index=True)
    zone_name = Column(String(128), default="")
    detail = Column(Text, default="")


class RepeaterSensor(Base):
    """Diagnostic-only link: which sensor is known to route through which repeater."""

    __tablename__ = "repeater_sensors"

    repeater_id = Column(String(6), ForeignKey("repeaters.id"), primary_key=True)
    sensor_id = Column(String(6), ForeignKey("sensors.id"), primary_key=True)

    repeater = relationship("Repeater", back_populates="sensors")
    sensor = relationship("Sensor", back_populates="repeaters")


class Pump(Base):
    __tablename__ = "pumps"

    id = Column(String(4), primary_key=True)  # e.g. "P01"
    name = Column(String(255), default="")
    executor_id = Column(String(6), ForeignKey("executors.id"), nullable=True)
    max_simultaneous_valves = Column(Integer, default=1)
    startup_time_s = Column(Integer, default=3)  # how long the pump itself takes to start
    shutdown_time_s = Column(Integer, default=5)  # how long the pump itself takes to stop
    desired_state = Column(Enum("on", "off", name="pump_desired_state"), default="off")
    current_state = Column(Enum("on", "off", name="pump_current_state"), default="off")
    current_updated_at = Column(DateTime, nullable=True)
    is_active = Column(Boolean, default=True)

    # Set by daemons/reconciler.py's _give_up() the instant a command
    # terminally fails (local timeout backstop, device NACK, or a
    # gateway-reported timeout) - reconciler resets desired_state to
    # current_state at the same moment, so the tick loop stops retrying.
    # health_checker.py reads this flag to open/resolve
    # PUMP_COMMAND_TIMEOUT (see its _check_command_failures) - it's the
    # reliable replacement for inferring failure from PumpCommand history,
    # which never actually worked (see reconciler.py's module docstring).
    # Cleared by reconciler.py the moment a later attempt actually succeeds.
    last_command_failed = Column(Boolean, default=False)

    executor = relationship("Executor", back_populates="pumps")
    valves = relationship("Valve", back_populates="pump")


class Valve(Base):
    __tablename__ = "valves"

    id = Column(String(4), primary_key=True)  # e.g. "V01"
    name = Column(String(255), default="")  # friendly name, e.g. "Мъглуване капково"
    zone_id = Column(Integer, ForeignKey("zones.id"), nullable=True)
    pump_id = Column(String(4), ForeignKey("pumps.id"), nullable=True)
    executor_id = Column(String(6), ForeignKey("executors.id"), nullable=True)
    opening_time_s = Column(Integer, default=3)  # how long the valve itself takes to open
    closing_time_s = Column(Integer, default=3)  # how long the valve itself takes to close
    desired_state = Column(Enum("on", "off", name="valve_desired_state"), default="off")
    current_state = Column(Enum("on", "off", name="valve_current_state"), default="off")
    current_updated_at = Column(DateTime, nullable=True)
    is_active = Column(Boolean, default=True)

    # Clock-regime manual override (see daemons/desired_state_setter.py's
    # _tick_clock_zones): set by a human command via the API, so the
    # scheduler leaves this valve alone for the rest of the CURRENT interval
    # occurrence instead of re-asserting its own schedule on the very next
    # tick. override_phase records whether the valve was inside or outside
    # its interval at the moment of override - once that flips (the
    # interval boundary is crossed either way), control returns to the
    # schedule automatically.
    manual_override = Column(Boolean, default=False)
    override_phase = Column(Boolean, nullable=True)

    # See Pump.last_command_failed above - same mechanism, VALVE_COMMAND_TIMEOUT.
    last_command_failed = Column(Boolean, default=False)

    zone = relationship("Zone", back_populates="valves")
    pump = relationship("Pump", back_populates="valves")
    executor = relationship("Executor", back_populates="valves")


# ------------------------------------------------------------- telemetry ----

class SensorReading(Base):
    __tablename__ = "sensor_readings"

    id = Column(Integer, primary_key=True, autoincrement=True)
    sensor_id = Column(String(6), ForeignKey("sensors.id"), nullable=False)
    soil_t = Column(Float, nullable=True)
    soil_h = Column(Float, nullable=True)
    air_t = Column(Float, nullable=True)
    air_h = Column(Float, nullable=True)
    # Indexed: every "readings since X" query in this app (VALVE_NO_EFFECT,
    # SENSOR_STUCK_VALUE, the Преглед tab's trend charts, ...) filters on
    # this column, and daemons/health_checker.py's retention cleanup
    # deletes by it too - by far the highest-volume table in the schema.
    recorded_at = Column(DateTime, default=now, index=True)

    sensor = relationship("Sensor", back_populates="readings")


# ----------------------------------------------------------- zone regime ----

class ZoneSchedule(Base):
    __tablename__ = "zone_schedules"

    id = Column(Integer, primary_key=True, autoincrement=True)
    zone_id = Column(Integer, ForeignKey("zones.id"), nullable=False)
    start_time = Column(String(5))  # "HH:MM"
    end_time = Column(String(5))
    days_mask = Column(Integer, default=127)  # bitmask Mon..Sun
    enabled = Column(Boolean, default=True)

    zone = relationship("Zone", back_populates="schedules")
    valve_links = relationship("ScheduleValve", back_populates="schedule", cascade="all, delete-orphan")


class ScheduleValve(Base):
    __tablename__ = "schedule_valves"

    schedule_id = Column(Integer, ForeignKey("zone_schedules.id"), primary_key=True)
    valve_id = Column(String(4), ForeignKey("valves.id"), primary_key=True)

    schedule = relationship("ZoneSchedule", back_populates="valve_links")
    valve = relationship("Valve")


class ZoneThreshold(Base):
    __tablename__ = "zone_thresholds"

    zone_id = Column(Integer, ForeignKey("zones.id"), primary_key=True)
    param = Column(Enum("S_T", "S_H", "A_T", "A_H", name="threshold_param"), primary_key=True)
    min_val = Column(Float, nullable=True)
    max_val = Column(Float, nullable=True)
    irrigation_duration_s = Column(Integer, default=300)
    infiltration_wait_s = Column(Integer, default=900)

    zone = relationship("Zone", back_populates="thresholds")


class ThresholdValve(Base):
    """Which valves a given (zone, param) threshold rule waters.

    zone_id/param intentionally aren't a DB-level composite FK to keep the
    demo schema simple across SQLite/MySQL; the app layer only ever writes
    pairs that already exist in zone_thresholds.
    """

    __tablename__ = "threshold_valves"

    zone_id = Column(Integer, ForeignKey("zones.id"), primary_key=True)
    param = Column(String(4), primary_key=True)
    valve_id = Column(String(4), ForeignKey("valves.id"), primary_key=True)

    valve = relationship("Valve")


# -------------------------------------------------------- health / audit ----

class ZoneError(Base):
    __tablename__ = "zone_errors"

    id = Column(Integer, primary_key=True, autoincrement=True)
    # Nullable: a network-wide problem (e.g. the Gateway itself is offline)
    # isn't "on" any one zone - it affects all of them. Per-zone problems
    # still set this normally.
    zone_id = Column(Integer, ForeignKey("zones.id"), nullable=True)
    sensor_id = Column(String(6), ForeignKey("sensors.id"), nullable=True)
    valve_id = Column(String(4), ForeignKey("valves.id"), nullable=True)
    pump_id = Column(String(4), ForeignKey("pumps.id"), nullable=True)
    executor_id = Column(String(6), ForeignKey("executors.id"), nullable=True)
    repeater_id = Column(String(6), ForeignKey("repeaters.id"), nullable=True)
    error_code = Column(String(32))
    severity = Column(Enum("warning", "error", "critical", name="error_severity"), default="warning")
    description = Column(String(255), default="")
    detected_at = Column(DateTime, default=now)
    resolved_at = Column(DateTime, nullable=True)

    zone = relationship("Zone", back_populates="errors")


class ValveCommand(Base):
    __tablename__ = "valve_commands"

    id = Column(Integer, primary_key=True, autoincrement=True)
    valve_id = Column(String(4), ForeignKey("valves.id"), nullable=False)
    requested_state = Column(Enum("on", "off", name="valve_cmd_state"))
    status = Column(Enum("pending", "success", "nack", "timeout", name="valve_cmd_status"), default="pending")
    created_at = Column(DateTime, default=now)
    # Set when the MQTT commands_status ACK/NACK/timeout actually arrives -
    # separate from resolved_at, which only fires once the physical settle
    # time (opening_time_s/closing_time_s) has ALSO elapsed since this
    # moment. An ACK means "the executor flipped the pin", not "the valve
    # has finished physically moving" - see daemons/reconciler.py.
    acked_at = Column(DateTime, nullable=True)
    resolved_at = Column(DateTime, nullable=True)


class PumpCommand(Base):
    __tablename__ = "pump_commands"

    id = Column(Integer, primary_key=True, autoincrement=True)
    pump_id = Column(String(4), ForeignKey("pumps.id"), nullable=False)
    requested_state = Column(Enum("on", "off", name="pump_cmd_state"))
    status = Column(Enum("pending", "success", "nack", "timeout", name="pump_cmd_status"), default="pending")
    created_at = Column(DateTime, default=now)
    acked_at = Column(DateTime, nullable=True)  # see ValveCommand.acked_at
    resolved_at = Column(DateTime, nullable=True)


class ZoneEvent(Base):
    __tablename__ = "zone_events"

    id = Column(Integer, primary_key=True, autoincrement=True)
    zone_id = Column(Integer, ForeignKey("zones.id"), nullable=False)
    valve_id = Column(String(4), ForeignKey("valves.id"), nullable=True)
    pump_id = Column(String(4), ForeignKey("pumps.id"), nullable=True)
    event_type = Column(String(32))
    triggered_by = Column(String(64), default="")
    detail = Column(String(255), default="")
    created_at = Column(DateTime, default=now)

    zone = relationship("Zone", back_populates="events")


# ------------------------------------------------------------------ users ----

class User(Base):
    __tablename__ = "users"

    id = Column(Integer, primary_key=True, autoincrement=True)
    username = Column(String(64), unique=True, nullable=False)
    password_hash = Column(String(255), default="")
    role = Column(Enum("admin", "agronomist", "viewer", name="user_role"), default="viewer")
    full_name = Column(String(128), default="")
    email = Column(String(255), default="")
    is_active = Column(Boolean, default=True)
    created_at = Column(DateTime, default=now)
    # The one always-present admin account (seed.py) - never shown in the
    # Users admin table and never editable/deletable through that screen, so
    # there's always at least one way into the system regardless of what an
    # admin does to the other accounts.
    is_system = Column(Boolean, default=False)
    # Brute-force lockout (see app/auth.py) - counts consecutive failed
    # logins for THIS username; a correct password resets it to 0. Once it
    # reaches the threshold, locked_until is set and further attempts are
    # rejected outright (without even checking the password) until it
    # passes, regardless of how many more attempts arrive in the meantime.
    failed_login_attempts = Column(Integer, default=0)
    locked_until = Column(DateTime, nullable=True)

    zone_access = relationship("UserZoneAccess", back_populates="user", cascade="all, delete-orphan")
    sessions = relationship("Session", back_populates="user", cascade="all, delete-orphan")


class Session(Base):
    """Login session - see app/auth.py. token lives in an httpOnly cookie on
    the client; expires_at is a sliding window, pushed forward on every
    authenticated request, not a fixed expiry from login time."""

    __tablename__ = "sessions"

    id = Column(Integer, primary_key=True, autoincrement=True)
    token = Column(String(64), unique=True, nullable=False, index=True)
    user_id = Column(Integer, ForeignKey("users.id"), nullable=False)
    created_at = Column(DateTime, default=now)
    expires_at = Column(DateTime, nullable=False)

    user = relationship("User", back_populates="sessions")


class UserZoneAccess(Base):
    __tablename__ = "user_zone_access"

    user_id = Column(Integer, ForeignKey("users.id"), primary_key=True)
    zone_id = Column(Integer, ForeignKey("zones.id"), primary_key=True)
    access_level = Column(Enum("view", "control", name="access_level"), default="view")
    assigned_by = Column(String(64), default="")
    assigned_at = Column(DateTime, default=now)

    user = relationship("User", back_populates="zone_access")
    zone = relationship("Zone")
