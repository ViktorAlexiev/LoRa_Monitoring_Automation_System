import datetime
from typing import Annotated, List, Literal, Optional

from pydantic import BaseModel, ConfigDict, Field

from .config import CONFIG


def _default(section: str, key: str):
    return Field(default_factory=lambda: CONFIG[section][key])

State = Literal["on", "off"]
Regime = Literal["manual", "clock", "threshold"]
Role = Literal["admin", "agronomist", "viewer"]
AccessLevel = Literal["view", "control"]
Param = Literal["S_T", "S_H", "A_T", "A_H"]

# Module IDs travel in URL paths (/api/sensors/{id}, /api/zones/{id}/sensors/{id}, ...),
# so they're restricted to characters that are safe there - letters, digits, - and _.
# In particular no backslash: browsers rewrite "\" to "/" in http(s) URLs, which silently
# breaks path-based routing for an id containing one.
DeviceId = Annotated[str, Field(pattern=r"^[A-Za-z0-9_-]{1,16}$")]


class ORMModel(BaseModel):
    # Every datetime in this app is written with datetime.datetime.utcnow()
    # (models.py's now()) - a naive value with no tzinfo, because that's
    # what's actually stored in the DB column. Serialized as-is, that comes
    # out as e.g. "2026-09-12T19:14:52" with no "Z"/offset - and a JS
    # `new Date(...)` on a date-time string with no timezone designator is
    # defined (ECMA-262) to parse it as LOCAL time, not UTC. For any viewer
    # not in UTC+0 this silently shows the wrong wall-clock time everywhere
    # (charts, "detected_at" on error banners, "last updated", ...) - found
    # via live bench testing with a UTC+3 browser showing timestamps a flat
    # 3 hours behind the actual local time. Appending "Z" here marks the
    # string as UTC explicitly, so `new Date(...)` on the frontend converts
    # it correctly instead of taking the numbers at face value.
    model_config = ConfigDict(
        from_attributes=True,
        json_encoders={datetime.datetime: lambda v: v.isoformat() + "Z"},
    )


# ---------------------------------------------------------------- zones ----

class ZoneCreate(BaseModel):
    name: str
    description: str = ""
    regime: Regime = "manual"
    humidity_warn_min: Optional[float] = None
    humidity_warn_max: Optional[float] = None


class ZoneUpdate(BaseModel):
    name: Optional[str] = None
    description: Optional[str] = None
    regime: Optional[Regime] = None
    is_active: Optional[bool] = None
    humidity_warn_min: Optional[float] = None
    humidity_warn_max: Optional[float] = None


class ZoneOut(ORMModel):
    id: int
    name: str
    description: str
    regime: Regime
    is_active: bool
    created_at: datetime.datetime
    transition_status: Literal["none", "waiting", "error"] = "none"
    pending_regime: Optional[Regime] = None
    pending_is_active: Optional[bool] = None
    humidity_warn_min: Optional[float] = None
    humidity_warn_max: Optional[float] = None


class GaugeReadings(BaseModel):
    soil_t: Optional[float] = None
    soil_h: Optional[float] = None
    air_t: Optional[float] = None
    air_h: Optional[float] = None
    # Magnus-Tetens approximation from air_t/air_h - see routers/zones.py's
    # _dew_point. Null only when air_t/air_h themselves are unavailable.
    dew_point: Optional[float] = None
    # Minutes since the newest reading from any sensor in the zone (None = never).
    data_age_minutes: Optional[int] = None


class ZoneErrorOut(ORMModel):
    id: int
    zone_id: Optional[int] = None
    sensor_id: Optional[str] = None
    valve_id: Optional[str] = None
    pump_id: Optional[str] = None
    executor_id: Optional[str] = None
    repeater_id: Optional[str] = None
    error_code: str
    severity: Literal["warning", "error", "critical"]
    description: str
    detected_at: datetime.datetime
    resolved_at: Optional[datetime.datetime] = None


class SensorReadingOut(ORMModel):
    id: int
    sensor_id: str
    soil_t: Optional[float] = None
    soil_h: Optional[float] = None
    air_t: Optional[float] = None
    air_h: Optional[float] = None
    recorded_at: datetime.datetime


class AuditOut(ORMModel):
    id: int
    at: datetime.datetime
    username: str = ""
    display_name: str = ""
    action: str
    zone_id: Optional[int] = None
    zone_name: str = ""
    detail: str = ""


class ModuleRef(BaseModel):
    kind: str
    id: str
    name: str
    state: Optional[str] = None


class ZoneSummary(ZoneOut):
    readings: GaugeReadings = GaugeReadings()
    modules: List[ModuleRef] = []
    open_error_count: int = 0
    worst_open_severity: Optional[Literal["warning", "error", "critical"]] = None


# ------------------------------------------------------------- devices ----

class ExecutorCreate(BaseModel):
    id: DeviceId
    name: str = ""


class ExecutorUpdate(BaseModel):
    name: Optional[str] = None
    is_active: Optional[bool] = None


class ExecutorOut(ORMModel):
    id: str
    name: str
    is_active: bool
    last_heartbeat_at: Optional[datetime.datetime] = None


class RepeaterCreate(BaseModel):
    id: DeviceId
    name: str = ""
    sensor_ids: List[str] = []


class RepeaterUpdate(BaseModel):
    name: Optional[str] = None
    is_active: Optional[bool] = None
    sensor_ids: Optional[List[str]] = None


class RepeaterOut(ORMModel):
    id: str
    name: str
    is_active: bool
    last_heartbeat_at: Optional[datetime.datetime] = None
    sensor_ids: List[str] = []


class GatewayCreate(BaseModel):
    id: DeviceId
    name: str = ""


class GatewayUpdate(BaseModel):
    name: Optional[str] = None
    is_active: Optional[bool] = None


class GatewayOut(ORMModel):
    id: str
    name: str
    is_active: bool
    last_heartbeat_at: Optional[datetime.datetime] = None


class SensorCreate(BaseModel):
    id: DeviceId
    name: str = ""
    zone_id: Optional[int] = None
    repeater_id: Optional[str] = None


class SensorUpdate(BaseModel):
    name: Optional[str] = None
    zone_id: Optional[int] = None
    repeater_id: Optional[str] = None
    is_active: Optional[bool] = None


class SensorOut(ORMModel):
    id: str
    name: str
    zone_id: Optional[int] = None
    is_active: bool
    repeater_id: Optional[str] = None


class PumpCreate(BaseModel):
    id: DeviceId
    name: str = ""
    executor_id: Optional[str] = None
    # A pump that can never open a valve (0) or a negative slot count is
    # never a valid configuration - previously unvalidated, so the admin
    # panel accepted 0/-1 and silently disabled irrigation through that pump
    # (found via live bench testing).
    max_simultaneous_valves: int = Field(default_factory=lambda: CONFIG["pump_defaults"]["max_simultaneous_valves"], ge=1)
    startup_time_s: int = _default("pump_defaults", "startup_time_s")
    shutdown_time_s: int = _default("pump_defaults", "shutdown_time_s")


class PumpUpdate(BaseModel):
    name: Optional[str] = None
    executor_id: Optional[str] = None
    max_simultaneous_valves: Optional[int] = Field(default=None, ge=1)
    startup_time_s: Optional[int] = None
    shutdown_time_s: Optional[int] = None
    is_active: Optional[bool] = None


class PumpOut(ORMModel):
    id: str
    name: str
    executor_id: Optional[str] = None
    max_simultaneous_valves: int
    startup_time_s: int
    shutdown_time_s: int
    desired_state: State
    current_state: State
    current_updated_at: Optional[datetime.datetime] = None
    is_active: bool


class ValveCreate(BaseModel):
    id: DeviceId
    name: str = ""
    pump_id: Optional[str] = None
    executor_id: Optional[str] = None
    zone_id: Optional[int] = None
    opening_time_s: int = _default("valve_defaults", "opening_time_s")
    closing_time_s: int = _default("valve_defaults", "closing_time_s")


class ValveUpdate(BaseModel):
    name: Optional[str] = None
    pump_id: Optional[str] = None
    executor_id: Optional[str] = None
    zone_id: Optional[int] = None
    is_active: Optional[bool] = None
    opening_time_s: Optional[int] = None
    closing_time_s: Optional[int] = None


class ValveOut(ORMModel):
    id: str
    name: str
    zone_id: Optional[int] = None
    pump_id: Optional[str] = None
    executor_id: Optional[str] = None
    opening_time_s: int
    closing_time_s: int
    desired_state: State
    current_state: State
    current_updated_at: Optional[datetime.datetime] = None
    is_active: bool


class ValveCommandCreate(BaseModel):
    requested_state: State


# ---------------------------------------------------------- zone regime ----

class SensorLayoutItem(BaseModel):
    sensor_id: str
    x: float = Field(ge=0, le=100)
    y: float = Field(ge=0, le=100)


class SensorLayoutSet(BaseModel):
    items: List[SensorLayoutItem] = []


class MapObjectItem(BaseModel):
    kind: str = Field(max_length=16)
    label: str = Field(default="", max_length=64)
    color: str = Field(default="", max_length=16)
    x: float = Field(ge=0, le=100)
    y: float = Field(ge=0, le=100)
    w: float = Field(ge=2, le=100)
    h: float = Field(ge=2, le=100)


class MapObjectsSet(BaseModel):
    objects: List[MapObjectItem] = []


class ZoneScheduleCreate(BaseModel):
    start_time: str
    end_time: str
    days_mask: int = 127
    enabled: bool = True
    valve_ids: List[str] = []


class ZoneScheduleOut(ORMModel):
    id: int
    zone_id: int
    start_time: str
    end_time: str
    days_mask: int
    enabled: bool
    valve_ids: List[str] = []


class ZoneThresholdCreate(BaseModel):
    param: Param
    min_val: Optional[float] = None
    max_val: Optional[float] = None
    irrigation_duration_s: int = _default("threshold_defaults", "irrigation_duration_s")
    infiltration_wait_s: int = _default("threshold_defaults", "infiltration_wait_s")
    valve_ids: List[str] = []


class ZoneThresholdOut(ORMModel):
    zone_id: int
    param: Param
    min_val: Optional[float] = None
    max_val: Optional[float] = None
    irrigation_duration_s: int
    infiltration_wait_s: int
    valve_ids: List[str] = []


# ----------------------------------------------------------------- config ----

class PumpDefaults(BaseModel):
    max_simultaneous_valves: int
    startup_time_s: int
    shutdown_time_s: int


class ValveDefaults(BaseModel):
    opening_time_s: int
    closing_time_s: int


class ThresholdDefaults(BaseModel):
    irrigation_duration_s: int
    infiltration_wait_s: int


class SensorReadingsConfig(BaseModel):
    averaging_window_minutes: int
    history_limit: int


class RefreshIntervals(BaseModel):
    dashboard_seconds: int
    zone_detail_seconds: int


class HealthChecksConfig(BaseModel):
    sensor_offline_minutes: int
    device_offline_minutes: int
    no_effect_after_minutes: int
    sensor_stuck_minutes: int


class SecurityConfig(BaseModel):
    session_timeout_minutes: int


class DataRetentionConfig(BaseModel):
    sensor_readings_retention_days: int


class SystemConfig(BaseModel):
    pump_defaults: PumpDefaults
    valve_defaults: ValveDefaults
    threshold_defaults: ThresholdDefaults
    sensor_readings: SensorReadingsConfig
    refresh_intervals: RefreshIntervals
    health_checks: HealthChecksConfig
    security: SecurityConfig
    data_retention: DataRetentionConfig


class PumpDefaultsUpdate(BaseModel):
    max_simultaneous_valves: Optional[int] = None
    startup_time_s: Optional[int] = None
    shutdown_time_s: Optional[int] = None


class ValveDefaultsUpdate(BaseModel):
    opening_time_s: Optional[int] = None
    closing_time_s: Optional[int] = None


class ThresholdDefaultsUpdate(BaseModel):
    irrigation_duration_s: Optional[int] = None
    infiltration_wait_s: Optional[int] = None


class SensorReadingsConfigUpdate(BaseModel):
    averaging_window_minutes: Optional[int] = None
    history_limit: Optional[int] = None


class RefreshIntervalsUpdate(BaseModel):
    dashboard_seconds: Optional[int] = None
    zone_detail_seconds: Optional[int] = None


class HealthChecksConfigUpdate(BaseModel):
    sensor_offline_minutes: Optional[int] = None
    device_offline_minutes: Optional[int] = None
    no_effect_after_minutes: Optional[int] = None
    sensor_stuck_minutes: Optional[int] = None


class SecurityConfigUpdate(BaseModel):
    session_timeout_minutes: Optional[int] = None


class DataRetentionConfigUpdate(BaseModel):
    sensor_readings_retention_days: Optional[int] = None


class SystemConfigUpdate(BaseModel):
    pump_defaults: Optional[PumpDefaultsUpdate] = None
    valve_defaults: Optional[ValveDefaultsUpdate] = None
    threshold_defaults: Optional[ThresholdDefaultsUpdate] = None
    sensor_readings: Optional[SensorReadingsConfigUpdate] = None
    refresh_intervals: Optional[RefreshIntervalsUpdate] = None
    health_checks: Optional[HealthChecksConfigUpdate] = None
    security: Optional[SecurityConfigUpdate] = None
    data_retention: Optional[DataRetentionConfigUpdate] = None


# ------------------------------------------------------------------ users ----

class LoginRequest(BaseModel):
    username: str
    password: str


class UserCreate(BaseModel):
    username: str
    password: str
    role: Role = "viewer"
    full_name: str = ""
    email: str = ""
    zone_ids: List[int] = []


class UserUpdate(BaseModel):
    role: Optional[Role] = None
    full_name: Optional[str] = None
    email: Optional[str] = None
    is_active: Optional[bool] = None
    password: Optional[str] = None
    zone_ids: Optional[List[int]] = None


class UserOut(ORMModel):
    id: int
    username: str
    role: Role
    full_name: str
    email: str
    is_active: bool
    zones: List[dict] = []
