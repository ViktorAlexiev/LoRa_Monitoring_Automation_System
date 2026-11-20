"""Fills a freshly seeded demo database so the dashboard looks lived-in:
arranges the whole-site map and the zone maps (incl. the external sensors) and
back-fills two days of readings so the charts are not empty. Run it once, with
the API already up:

    python dev_tools/demo_setup.py <path-to-demo.db> [api-base-url]

Uses only the standard library; admin login is admin / 123 (the demo default)."""
import datetime
import http.cookiejar
import json
import math
import random
import sqlite3
import sys
import urllib.request

DB = sys.argv[1]
BASE = sys.argv[2] if len(sys.argv) > 2 else "http://localhost:8010/api"
jar = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))


def call(method, path, body=None):
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers={"Content-Type": "application/json"})
    with op.open(req) as r:
        raw = r.read()
        return json.loads(raw) if raw else None


call("POST", "/auth/login", {"username": "admin", "password": "123"})
zones = {z["name"]: z["id"] for z in call("GET", "/zones")}
zid = {k: next(v for n, v in zones.items() if n.startswith(k)) for k in ("Зона 1", "Зона 2", "Оранжерия Б", "Открита")}

# --- whole-site map: zones, landmarks, external sensors -------------------------------
call("PUT", "/site/map", {
    "zones": [
        {"zone_id": zid["Зона 1"], "x": 24, "y": 58, "w": 30, "h": 24},
        {"zone_id": zid["Зона 2"], "x": 24, "y": 30, "w": 30, "h": 24},
        {"zone_id": zid["Оранжерия Б"], "x": 64, "y": 38, "w": 26, "h": 30},
        {"zone_id": zid["Открита"], "x": 62, "y": 80, "w": 44, "h": 26},
    ],
    "objects": [
        {"kind": "building", "label": "Склад", "color": "", "x": 48, "y": 8, "w": 18, "h": 12},
        {"kind": "gate", "label": "Вход", "color": "", "x": 8, "y": 92, "w": 9, "h": 9},
        {"kind": "line", "label": "Главен път", "color": "gray", "x": 40, "y": 98, "w": 80, "h": 3},
    ],
    "sensors": [
        {"sensor_id": "SX01", "x": 48, "y": 20},
        {"sensor_id": "SX02", "x": 62, "y": 68},
        {"sensor_id": "SX03", "x": 12, "y": 8},
    ],
})

# --- zone maps: own sensors + the external ones linked to the zone --------------------
ext_pos = {"SX01": (88, 14), "SX02": (88, 14), "SX03": (12, 14)}
sensors = call("GET", "/sensors")
for z in zid.values():
    items, n = [], 0
    for s in sensors:
        if s["kind"] == "external":
            if z in s["zone_ids"]:
                x, y = ext_pos[s["id"]]
                items.append({"sensor_id": s["id"], "x": x, "y": y})
        elif s["zone_id"] == z:
            items.append({"sensor_id": s["id"], "x": 30 + 25 * n, "y": 55})
            n += 1
    call("PUT", f"/zones/{z}/layout", {"items": items})

# --- two days of readings, every 10 minutes ---------------------------------------------
rnd = random.Random(7)
con = sqlite3.connect(DB)
now = datetime.datetime.utcnow()
base_air = {"S001": (24, 52), "S002": (22.5, 55), "S003": (20.5, 58), "S004": (18, 46)}
base_ext = {"SX01": (13, 80), "SX02": (12, 84), "SX03": (11, 87)}
con.execute("delete from sensor_readings")
for step in range(48 * 6, -1, -1):
    t = now - datetime.timedelta(minutes=10 * step)
    h = (t.hour + t.minute / 60 + 3) % 24  # local-ish
    swing = math.sin((h - 9) / 24 * 2 * math.pi)
    for sid, (bt, bh) in base_air.items():
        soil = 45 + 8 * math.sin(step / 40 + len(sid)) + rnd.uniform(-1, 1) - (step % 90) * 0.05
        con.execute("insert into sensor_readings (sensor_id,soil_t,soil_h,air_t,air_h,recorded_at) values (?,?,?,?,?,?)",
                    (sid, round(20 + 2 * swing + rnd.uniform(-.3, .3), 1), round(soil, 1),
                     round(bt + 4 * swing + rnd.uniform(-.4, .4), 1), round(bh - 10 * swing + rnd.uniform(-1, 1), 1), t))
    for sid, (bt, bh) in base_ext.items():
        con.execute("insert into sensor_readings (sensor_id,soil_t,soil_h,air_t,air_h,recorded_at) values (?,?,?,?,?,?)",
                    (sid, None, None, round(bt + 6 * swing + rnd.uniform(-.3, .3), 1),
                     round(max(20, min(99, bh - 18 * swing + rnd.uniform(-1, 1))), 1), t))
start_mv = {("sensor", "S001"): 4010, ("sensor", "S002"): 3920, ("sensor", "S003"): 3790, ("sensor", "S004"): 3345,
            ("sensor", "SX01"): 3735, ("sensor", "SX02"): 4090, ("sensor", "SX03"): 3880, ("repeater", "R001"): 3860}
con.execute("delete from battery_readings")
for (kind, dev), now_mv in start_mv.items():
    for hours_ago in range(7 * 24, -1, -3):   # one point every 3 h, draining about 2.5 mV an hour
        t = now - datetime.timedelta(hours=hours_ago)
        mv = min(4180, now_mv + 2.5 * hours_ago + rnd.uniform(-4, 4))
        con.execute("insert into battery_readings (kind,device_id,mv,recorded_at) values (?,?,?,?)", (kind, dev, int(mv), t))
con.commit()
con.close()
print("demo data ready")
