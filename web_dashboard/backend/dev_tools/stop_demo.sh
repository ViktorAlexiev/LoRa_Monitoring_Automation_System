#!/bin/bash
# Stops everything start_demo.sh started (API, daemons, simulator, broker, Vite).
for pid in $(wmic process where "name='python.exe' or name='node.exe'" get ProcessId,CommandLine 2>/dev/null | grep -E "uvicorn|daemons|mini_broker|vite" | grep -o '[0-9]*[ ]*$'); do
  taskkill //F //PID $pid >/dev/null 2>&1
done
pkill -f "uvicorn app.main:app" 2>/dev/null; pkill -f "daemons/" 2>/dev/null; pkill -f mini_broker 2>/dev/null; pkill -f "vite --port 5173" 2>/dev/null
echo stopped
