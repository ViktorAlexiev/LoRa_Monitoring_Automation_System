# One image, four containers (see ../deploy/docker-compose.yml): web-api and
# the three daemons (reconciler, desired_state_setter, health_checker,
# mqtt_bridge) all share this exact image and codebase - only the CMD/command
# differs per service. No reason to build four separate images for code that
# lives in one repo with one requirements.txt.
FROM python:3.11-slim

# Without this, Python block-buffers stdout when it isn't a TTY (i.e.
# always, under Docker) - the daemons' own print()s (reconciler.py giving
# up on a command, health_checker.py's retention cleanup, ...) can sit
# unflushed for a long time, making `docker logs` look silent when it
# isn't. uvicorn's own logging is unaffected either way (it flushes itself).
ENV PYTHONUNBUFFERED=1

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

# Runs as a plain user, not root - none of the 5 services (web-api or the 4
# daemons) need root inside the container; MySQL/MQTT are reached over the
# network, and the only local file write is config/defaults.json (see
# app/config.py), which lives on the config_data volume mounted at /app/config
# - chown it too so a non-root process can still write there.
RUN useradd --create-home --uid 1000 appuser && chown -R appuser:appuser /app
USER appuser

# Overridden per-service in docker-compose.yml (daemons run `python
# daemons/<name>.py` instead) - this default is what web-api actually uses.
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
