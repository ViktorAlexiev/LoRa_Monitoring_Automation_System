"""Reconciler daemon - see description_updated.docx and Документация на
системата.docx (section 4, the MQTT protocol manual) for the authoritative
wire format.

The only process in the real system allowed to write current_state,
current_updated_at, and rows in VALVE_COMMANDS/PUMP_COMMANDS. It never
touches desired_state on its own initiative - that belongs to
desired_state_setter.py (and to the website, for manual toggles and the
activation/regime-change/deactivation force-off sequence) - WITH ONE
DELIBERATE EXCEPTION: when a command definitively fails (see "Giving up"
below), reconciler resets desired_state back to current_state itself,
so a genuinely unreachable device doesn't get hammered with the same
command forever while silently reporting nothing.

Giving up: a command can end terminally failed three ways - our own local
LOCAL_TIMEOUT_SECONDS backstop (_timeout_stale_pending, no reply at all),
an explicit NACK from the device (status=2), or the gateway's own
"status": "timeout" report (status=1, its 2 internal retries per manual
3.1 exhausted). All three now go through _give_up(): desired_state is
reset to current_state (so the tick loop stops retrying - continuing to
retry a device that just told us it can't be reached helps no one), and
Valve.last_command_failed / Pump.last_command_failed is set True - a plain
state flag, NOT a ZONE_ERRORS write. Opening/resolving
VALVE_COMMAND_TIMEOUT / PUMP_COMMAND_TIMEOUT from that flag is still
health_checker.py's job, same as every other error code (see its
_check_command_failures) - reconciler only ever owns current_state/
desired_state/*_COMMANDS, never ZONE_ERRORS. The flag replaced an earlier
attempt where health_checker inferred failure from "the latest
ValveCommand's status": that never actually worked, because this same
retry-suppressing reset means a fresh 'pending' row always supersedes the
'timeout'/'nack' one in the same commit, before health_checker's own tick
ever gets to read it (found via live bench testing). last_command_failed
is cleared the same way it was set - by reconciler, in
_finalize_acked_commands, the moment a later attempt on this valve/pump
actually succeeds. Until then, the desired/current reset means the
mismatch is gone but the ORIGINAL intent (someone wanted this valve on) is
not silently retried again on its own - a human has to notice the alarm
and re-send the command.

Three moving parts:

1. An MQTT client (background thread via paho-mqtt's loop_start()):
   - Publishes to "commands": {"M_ID": executor_id, "C_ID": consumer_id,
     "com": "A1"|"B2"} - "A1" = turn on, "B2" = turn off (see manual 4.2).
   - Subscribes to "commands_status": {"M_ID","C_ID","com","status"} where
     status is 0=ACK, 1=TIMEOUT, 2=NACK (manual 4.2). The gateway has
     already done its own up-to-2 retries before this arrives (manual
     3.1) - reconciler does not retry on top of that, it just records the
     outcome.

2. A one-time startup resync (see _resync_all_executors): the tick loop only
   ever reacts to desired != current, so a state drift that happened while
   this daemon was down (crash, manual intervention, an executor losing
   power and resetting its relays) would otherwise sit uncorrected forever.
   On startup, publishes "module_states_request": {"M_ID": executor_id} to
   every executor with an attached valve/pump, and the resulting
   "module_states_response": {"id","states":[{"id","state"}]} (or
   {"id","status":"timeout"} - manual 4.2) is applied straight to
   current_state, bypassing the normal command/ack/settle-time flow since
   this is a report of already-settled physical fact, not a transition in
   progress. Skipped for any consumer with a command already in flight, to
   avoid racing its own resolution.

3. A tick loop (main thread) that:
   a. Finalizes anything the MQTT thread marked acked but not yet
      resolved, once its physical settle time has elapsed (see below).
   b. Times out any command that's been "pending" far longer than the
      gateway's own worst case (up to ~32s at SF12) with no reply at all -
      a defensive local backstop in case a commands_status message never
      arrives.
   c. Issues new commands for any valve/pump where desired != current and
      nothing is already in flight for it - respecting the physical
      sequencing rule (valve opens before its pump starts; pump stops
      before its valve closes, unless the pump isn't actually stopping).

ACK vs "physically settled" - an important distinction: per the manual,
Executor "изпълнява ги физически (превключва съответния пин), и връща
потвърждение" - it sends ACK AFTER flipping the pin, not before. But
flipping a pin is instantaneous; the valve/pump actually finishing its
physical motion (water pressure, motor spin-up) still takes real time that
the device never reports back on its own. So: ValveCommand/PumpCommand.
acked_at records when the MQTT round-trip actually resolved; resolved_at
(and the current_state update) only happens once opening_time_s/
closing_time_s/startup_time_s/shutdown_time_s has ALSO elapsed since then -
our own model of "the physical motion is done", independent of the wire
protocol.

This file is a runnable reference implementation, not wired into the
web_dashboard demo app (which still simulates convergence synchronously by
design). Needs a real MQTT broker reachable per config/system.ini - see
daemons/system_config.py. Install paho-mqtt (`pip install paho-mqtt`) if
it's not already in the environment.
"""

import datetime
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import paho.mqtt.client as mqtt  # noqa: E402

from app import models  # noqa: E402
from app.database import SessionLocal  # noqa: E402
from daemons.system_config import mqtt_settings  # noqa: E402

TICK_SECONDS = 2
# Backstop, not a retry. Must stay ABOVE the gateway's worst case: (1 + MAX_RETRIES) x ACK timeout,
# which grows with the LoRa spreading factor (~3.6s at SF7 ... ~32s at SF12, 3 x ~10.8s). Firing
# earlier than the gateway gives up risks a command that DID execute being recorded as failed
# (desired reset to current, late commands_status then ignored) - a DB/physical mismatch.
LOCAL_TIMEOUT_SECONDS = 40
COM_ON = "A1"
COM_OFF = "B2"
STATUS_ACK = 0
STATUS_TIMEOUT = 1
STATUS_NACK = 2

TOPIC_COMMANDS = "commands"
TOPIC_COMMANDS_STATUS = "commands_status"
TOPIC_MODULE_STATES_REQUEST = "module_states_request"
TOPIC_MODULE_STATES_RESPONSE = "module_states_response"


# --------------------------------------------------------------- MQTT in ----

def _find_pending_command(db, consumer_id: str, requested_state: str):
    """C_ID is either a valve_id or a pump_id (both up to 4 chars per the
    manual) - by convention they don't collide (V.../P... prefixes), so
    trying valve first and falling back to pump is enough to disambiguate
    in practice."""
    cmd = (
        db.query(models.ValveCommand)
        .filter_by(valve_id=consumer_id, requested_state=requested_state, status="pending")
        .order_by(models.ValveCommand.created_at.desc())
        .first()
    )
    if cmd:
        return cmd, "valve"
    cmd = (
        db.query(models.PumpCommand)
        .filter_by(pump_id=consumer_id, requested_state=requested_state, status="pending")
        .order_by(models.PumpCommand.created_at.desc())
        .first()
    )
    return (cmd, "pump") if cmd else (None, None)


def _give_up(db, kind: str, obj, reason: str, now):
    """A command for obj (a Valve or Pump) has terminally failed - see the
    module docstring's "Giving up" section. Resets desired_state to
    current_state (stop retrying) and flips last_command_failed=True - a
    plain state flag, not a ZONE_ERRORS write. reconciler.py owns
    current_state/desired_state/*_COMMANDS rows (see module docstring);
    ZONE_ERRORS stays health_checker.py's job, same as every other error
    code - it reads this flag on its own tick and opens/resolves
    VALVE_COMMAND_TIMEOUT/PUMP_COMMAND_TIMEOUT from it, the same way it
    already reacts to any other column reconciler/desired_state_setter
    write. The flag beats the previous approach of health_checker inferring
    failure from "the latest ValveCommand's status": that never actually
    worked, because this same function's retry-suppressing reset means a
    fresh 'pending' row always supersedes the 'timeout'/'nack' one in the
    same commit, before anyone else gets to read it."""
    obj.desired_state = obj.current_state
    obj.last_command_failed = True
    print(f"reconciler: giving up on {kind} {obj.id} - {reason}")


def _on_commands_status(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
        consumer_id = payload["C_ID"]
        requested_state = "on" if payload.get("com") == COM_ON else "off"
        status = payload["status"]
    except (ValueError, KeyError):
        print(f"reconciler: malformed {TOPIC_COMMANDS_STATUS} payload: {msg.payload!r}")
        return

    db = SessionLocal()
    try:
        cmd, kind = _find_pending_command(db, consumer_id, requested_state)
        if cmd is None:
            return  # nothing pending matches - stale/duplicate reply, ignore
        now = datetime.datetime.utcnow()
        if status == STATUS_ACK:
            cmd.status = "success"
            cmd.acked_at = now
            # resolved_at / current_state still wait for the physical settle
            # time - see _finalize_acked_commands in the tick loop.
        elif status == STATUS_NACK:
            cmd.status = "nack"
            cmd.resolved_at = now  # nothing physically happened - finalize immediately
            obj = db.get(models.Valve if kind == "valve" else models.Pump, consumer_id)
            if obj is not None:
                _give_up(db, kind, obj, "отхвърлена от изпълнителя (NACK)", now)
        else:  # STATUS_TIMEOUT - the gateway's own retries (manual 3.1) are exhausted
            cmd.status = "timeout"
            cmd.resolved_at = now
            obj = db.get(models.Valve if kind == "valve" else models.Pump, consumer_id)
            if obj is not None:
                _give_up(db, kind, obj, "без отговор от gateway-я (timeout)", now)
        db.commit()
    finally:
        db.close()


def _apply_consumer_state(db, consumer_id: str, state: str, now):
    """Drift correction from module_states_response - we're being told the
    actual physical state directly, not confirming a command we issued, so
    this writes current_state straight through (no settle-time wait, no
    ValveCommand/PumpCommand row). Skipped when a command is already
    in-flight for that consumer, to avoid racing its own resolution."""
    if _has_pending(db, models.ValveCommand, "valve_id", consumer_id) or \
            _has_pending(db, models.PumpCommand, "pump_id", consumer_id):
        return
    valve = db.get(models.Valve, consumer_id)
    if valve is not None:
        if valve.current_state != state:
            print(f"reconciler: resync drift on valve {consumer_id}: db had "
                  f"'{valve.current_state}', device reports '{state}' - correcting")
            valve.current_state = state
            valve.current_updated_at = now
        return
    pump = db.get(models.Pump, consumer_id)
    if pump is not None and pump.current_state != state:
        print(f"reconciler: resync drift on pump {consumer_id}: db had "
              f"'{pump.current_state}', device reports '{state}' - correcting")
        pump.current_state = state
        pump.current_updated_at = now


def _on_module_states_response(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
        executor_id = payload["id"]
    except (ValueError, KeyError):
        print(f"reconciler: malformed {TOPIC_MODULE_STATES_RESPONSE} payload: {msg.payload!r}")
        return

    if payload.get("status") == "timeout":
        print(f"reconciler: module_states_request to executor {executor_id} timed out - "
              "no resync possible this round, will retry on next restart")
        return

    db = SessionLocal()
    try:
        now = datetime.datetime.utcnow()
        for entry in payload.get("states", []):
            _apply_consumer_state(db, entry["id"], entry["state"].lower(), now)
        db.commit()
    except (KeyError, AttributeError):
        print(f"reconciler: malformed states[] in {TOPIC_MODULE_STATES_RESPONSE} payload: {msg.payload!r}")
    finally:
        db.close()


def _on_message(client, userdata, msg):
    if msg.topic == TOPIC_COMMANDS_STATUS:
        _on_commands_status(client, userdata, msg)
    elif msg.topic == TOPIC_MODULE_STATES_RESPONSE:
        _on_module_states_response(client, userdata, msg)


def _make_mqtt_client():
    settings = mqtt_settings()
    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    if settings["username"]:
        client.username_pw_set(settings["username"], settings["password"])
    client.on_message = _on_message

    def _on_connect(client, userdata, flags, reason_code, properties):
        if reason_code == 0:
            client.subscribe(TOPIC_COMMANDS_STATUS, qos=1)
            client.subscribe(TOPIC_MODULE_STATES_RESPONSE, qos=1)
            print(f"reconciler: connected to MQTT broker at {settings['host']}:{settings['port']}")
        else:
            print(f"reconciler: MQTT connect failed: {reason_code}")

    client.on_connect = _on_connect
    client.connect(settings["host"], settings["port"], keepalive=30)
    client.loop_start()
    return client


def _resync_all_executors(client):
    """Startup-only resync: ask every executor that has at least one valve or
    pump attached what its consumers' real states are, so a gap while this
    daemon was down (crash, manual intervention, an executor losing power and
    resetting its relays) doesn't leave current_state stale forever. Replies
    land asynchronously via _on_module_states_response on the MQTT thread."""
    db = SessionLocal()
    try:
        executor_ids = {v.executor_id for v in db.query(models.Valve).all() if v.executor_id}
        executor_ids |= {p.executor_id for p in db.query(models.Pump).all() if p.executor_id}
    finally:
        db.close()
    for executor_id in executor_ids:
        client.publish(TOPIC_MODULE_STATES_REQUEST, json.dumps({"M_ID": executor_id}), qos=1)
    if executor_ids:
        print(f"reconciler: sent startup module_states_request to {len(executor_ids)} executor(s)")


# -------------------------------------------------------------- MQTT out ----

def _publish_command(client, executor_id: str, consumer_id: str, requested_state: str):
    com = COM_ON if requested_state == "on" else COM_OFF
    payload = json.dumps({"M_ID": executor_id, "C_ID": consumer_id, "com": com})
    client.publish(TOPIC_COMMANDS, payload, qos=1)


# ------------------------------------------------------------- tick loop ----

def _finalize_acked_commands(db, now):
    for cmd in db.query(models.ValveCommand).filter_by(status="success", resolved_at=None).all():
        if cmd.acked_at is None:
            continue
        valve = db.get(models.Valve, cmd.valve_id)
        if valve is None:
            cmd.resolved_at = now
            continue
        settle = valve.opening_time_s if cmd.requested_state == "on" else valve.closing_time_s
        if (now - cmd.acked_at).total_seconds() >= settle:
            valve.current_state = cmd.requested_state
            valve.current_updated_at = now
            valve.last_command_failed = False
            cmd.resolved_at = now

    for cmd in db.query(models.PumpCommand).filter_by(status="success", resolved_at=None).all():
        if cmd.acked_at is None:
            continue
        pump = db.get(models.Pump, cmd.pump_id)
        if pump is None:
            cmd.resolved_at = now
            continue
        settle = pump.startup_time_s if cmd.requested_state == "on" else pump.shutdown_time_s
        if (now - cmd.acked_at).total_seconds() >= settle:
            pump.current_state = cmd.requested_state
            pump.current_updated_at = now
            pump.last_command_failed = False
            cmd.resolved_at = now


def _timeout_stale_pending(db, now):
    """Backstop for a commands_status message that never arrives at all -
    the gateway's own envelope (manual 3.1) tops out around 12s at SF7 (~32s at SF12) including
    its 2 retries, so anything still 'pending' well past that is presumed
    lost, not just slow."""
    cutoff = now - datetime.timedelta(seconds=LOCAL_TIMEOUT_SECONDS)
    for cmd in db.query(models.ValveCommand).filter(
        models.ValveCommand.status == "pending", models.ValveCommand.created_at < cutoff
    ).all():
        cmd.status = "timeout"
        cmd.resolved_at = now
        valve = db.get(models.Valve, cmd.valve_id)
        if valve is not None:
            _give_up(db, "valve", valve, "без отговор (local timeout)", now)
    for cmd in db.query(models.PumpCommand).filter(
        models.PumpCommand.status == "pending", models.PumpCommand.created_at < cutoff
    ).all():
        cmd.status = "timeout"
        cmd.resolved_at = now
        pump = db.get(models.Pump, cmd.pump_id)
        if pump is not None:
            _give_up(db, "pump", pump, "без отговор (local timeout)", now)


def _has_pending(db, model, id_field, obj_id):
    """'In flight' means status='pending' (still waiting on the wire) OR
    status='success' with resolved_at still NULL (ACKed, but its physical
    settle time - opening_time_s/closing_time_s/startup_time_s/
    shutdown_time_s - hasn't elapsed yet, see _finalize_acked_commands).
    Checking status='pending' alone left that settle-time window
    unaccounted for: on every 2s tick during it, _drive_valves/_drive_pumps
    saw desired_state still != current_state (current_state only flips once
    the row is actually finalized) and no 'pending' row to explain why, so
    they issued a brand new duplicate command on top of the one already
    ACKed and quietly waiting out its settle time (found via live bench
    testing - watching a single valve-off produce 2-3 redundant commands to
    its pump before the real state change landed)."""
    return (
        db.query(model)
        .filter_by(**{id_field: obj_id})
        .filter(model.resolved_at.is_(None))
        .first()
        is not None
    )


def _drive_valves(db, client):
    for valve in db.query(models.Valve).all():
        if valve.desired_state == valve.current_state:
            continue
        if _has_pending(db, models.ValveCommand, "valve_id", valve.id):
            continue
        if not valve.executor_id:
            continue  # nowhere to send the command - not this daemon's problem to flag

        if valve.desired_state == "on":
            db.add(models.ValveCommand(valve_id=valve.id, requested_state="on", status="pending"))
            db.flush()
            _publish_command(client, valve.executor_id, valve.id, "on")
            continue

        pump = valve.pump
        pump_is_stopping = pump is not None and pump.desired_state == "off" and pump.current_state == "on"
        if pump_is_stopping:
            continue
        db.add(models.ValveCommand(valve_id=valve.id, requested_state="off", status="pending"))
        db.flush()
        _publish_command(client, valve.executor_id, valve.id, "off")


def _drive_pumps(db, client):
    for pump in db.query(models.Pump).all():
        if pump.desired_state == pump.current_state:
            continue
        if _has_pending(db, models.PumpCommand, "pump_id", pump.id):
            continue
        if not pump.executor_id:
            continue

        if pump.desired_state == "off":
            db.add(models.PumpCommand(pump_id=pump.id, requested_state="off", status="pending"))
            db.flush()
            _publish_command(client, pump.executor_id, pump.id, "off")
            continue

        a_valve_is_open = any(v.current_state == "on" for v in pump.valves)
        if not a_valve_is_open:
            continue
        db.add(models.PumpCommand(pump_id=pump.id, requested_state="on", status="pending"))
        db.flush()
        _publish_command(client, pump.executor_id, pump.id, "on")


def tick(client):
    db = SessionLocal()
    try:
        now = datetime.datetime.utcnow()
        _finalize_acked_commands(db, now)
        _timeout_stale_pending(db, now)
        db.flush()
        _drive_valves(db, client)
        db.flush()
        _drive_pumps(db, client)
        db.commit()
    finally:
        db.close()


def main():
    client = _make_mqtt_client()
    _resync_all_executors(client)
    print(f"reconciler.py started (tick every {TICK_SECONDS}s)")
    while True:
        try:
            tick(client)
        except Exception as exc:  # a daemon must not die on one bad tick
            print(f"reconciler tick failed: {exc}")
        time.sleep(TICK_SECONDS)


if __name__ == "__main__":
    main()
