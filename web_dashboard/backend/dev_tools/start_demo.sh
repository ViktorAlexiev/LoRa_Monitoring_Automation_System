#!/bin/bash
# Runs the whole dashboard locally WITHOUT Docker: mini MQTT broker, API (8010),
# the four daemons, the field simulator (live readings) and the Vite dev server (5173).
#   bash dev_tools/start_demo.sh [work-dir]      (default: dev_tools/.demo - holds the demo DB and logs)
# Stop everything:  bash dev_tools/stop_demo.sh
HERE="$(cd "$(dirname "$0")" && pwd)"
BACK="$(dirname "$HERE")"
WORK="${1:-$HERE/.demo}"
mkdir -p "$WORK"
if [ -x "$BACK/venv/Scripts/python.exe" ]; then PY="$BACK/venv/Scripts/python.exe"; else PY="$BACK/venv/bin/python"; fi
export DATABASE_URL="sqlite:///$WORK/demo.db"
export MQTT_HOST=localhost MQTT_PORT=1884
export SEED_DEMO_DATA=true SYSTEM_ADMIN_USERNAME=admin SYSTEM_ADMIN_PASSWORD=123
export PYTHONIOENCODING=utf-8
cd "$BACK"
FIRST=0; [ -f "$WORK/demo.db" ] || FIRST=1
"$PY" dev_tools/mini_broker.py 1884 > "$WORK/broker.out" 2>&1 &
sleep 1
"$PY" -m uvicorn app.main:app --port 8010 > "$WORK/api.out" 2>&1 &
sleep 8
if [ "$FIRST" = 1 ]; then "$PY" dev_tools/demo_setup.py "$WORK/demo.db" > "$WORK/setup.out" 2>&1; fi
"$PY" daemons/fake_field.py > "$WORK/sim.out" 2>&1 &
"$PY" daemons/mqtt_bridge.py > "$WORK/bridge.out" 2>&1 &
"$PY" daemons/reconciler.py > "$WORK/reconciler.out" 2>&1 &
"$PY" daemons/desired_state_setter.py > "$WORK/dss.out" 2>&1 &
"$PY" daemons/health_checker.py > "$WORK/hc.out" 2>&1 &
cd "$BACK/../frontend"
VITE_API_URL=http://localhost:8010/api npx vite --port 5173 > "$WORK/vite.out" 2>&1 &
sleep 6
echo "started - open http://localhost:5173 (admin / 123)"
