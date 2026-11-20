"""Battery voltage -> percent / level. One definition for the API, the
dashboard and health_checker.py's BATTERY_* checks.

The device reports millivolts (-32768 = no data, e.g. the node has no battery
or the measurement failed - such a value is never stored). Percent is linear
between config battery.empty_mv and battery.full_mv: crude for a Li-ion cell
(its curve is flat in the middle) but easy to explain and tune."""

from .config import CONFIG

NO_DATA = -32768


def valid_mv(value):
    """A usable millivolt reading, or None. Rejects the no-data marker and nonsense."""
    try:
        mv = int(value)
    except (TypeError, ValueError):
        return None
    return mv if 500 <= mv <= 20000 else None


# Resting (open-circuit) voltage of a Li-ion 18650 cell -> state of charge, as
# (mV, percent) points from empty to full; between two points it is a straight
# line. The curve is flat around 3.7-3.9 V, so a linear scale would be wrong there.
LIION_18650 = [
    (3270, 0), (3610, 5), (3690, 10), (3710, 15), (3730, 20), (3750, 25), (3770, 30), (3790, 35), (3800, 40),
    (3820, 45), (3840, 50), (3850, 55), (3870, 60), (3910, 65), (3950, 70), (3980, 75), (4020, 80), (4080, 85),
    (4110, 90), (4150, 95), (4200, 100),
]


def percent(mv):
    cfg = CONFIG["battery"]
    if cfg.get("curve", "liion_18650") == "liion_18650":
        if mv <= LIION_18650[0][0]:
            return 0
        if mv >= LIION_18650[-1][0]:
            return 100
        for (v0, p0), (v1, p1) in zip(LIION_18650, LIION_18650[1:]):
            if v0 <= mv <= v1:
                return round(p0 + (mv - v0) * (p1 - p0) / (v1 - v0))
    lo, hi = cfg["empty_mv"], cfg["full_mv"]
    if hi <= lo:
        return None
    return max(0, min(100, round((mv - lo) * 100 / (hi - lo))))


def level(pct):
    """"ok" | "low" | "critical" | "nodata" (pct None)."""
    if pct is None:
        return "nodata"
    cfg = CONFIG["battery"]
    if pct <= cfg["critical_percent"]:
        return "critical"
    if pct <= cfg["warn_percent"]:
        return "low"
    return "ok"
