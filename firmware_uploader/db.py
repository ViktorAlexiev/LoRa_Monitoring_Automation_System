import sqlite3
import json
from datetime import datetime


def get_conn(db_path):
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    return conn


def init_db(db_path):
    conn = get_conn(db_path)
    cur = conn.cursor()
    cur.execute("""
        CREATE TABLE IF NOT EXISTS upload_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT,
            action TEXT,
            device_type TEXT,
            module_id TEXT,
            parameters TEXT,
            latitude REAL,
            longitude REAL
        )
    """)
    cur.execute("""
        CREATE TABLE IF NOT EXISTS registry (
            module_id TEXT PRIMARY KEY,
            device_type TEXT,
            status TEXT,
            parameters TEXT,
            last_upload TEXT,
            latitude REAL,
            longitude REAL,
            retire_notes TEXT
        )
    """)
    conn.commit()
    conn.close()


def add_log(db_path, device_type, module_id, parameters, lat, lon, action="upload"):
    conn = get_conn(db_path)
    conn.execute(
        "INSERT INTO upload_log (timestamp, action, device_type, module_id, parameters, latitude, longitude) "
        "VALUES (?,?,?,?,?,?,?)",
        (datetime.now().isoformat(timespec='seconds'), action, device_type, module_id,
         json.dumps(parameters), lat, lon)
    )
    conn.commit()
    conn.close()


def get_all_logs(db_path):
    conn = get_conn(db_path)
    rows = conn.execute("SELECT * FROM upload_log ORDER BY id DESC").fetchall()
    conn.close()
    return rows


def get_registry_entry(db_path, module_id):
    conn = get_conn(db_path)
    row = conn.execute("SELECT * FROM registry WHERE module_id=?", (module_id,)).fetchone()
    conn.close()
    return row


def get_all_registry(db_path):
    conn = get_conn(db_path)
    rows = conn.execute("SELECT * FROM registry ORDER BY module_id").fetchall()
    conn.close()
    return rows


def upsert_registry(db_path, module_id, device_type, parameters, lat, lon, status="active"):
    conn = get_conn(db_path)
    conn.execute("""
        INSERT INTO registry (module_id, device_type, status, parameters, last_upload, latitude, longitude, retire_notes)
        VALUES (?,?,?,?,?,?,?,'')
        ON CONFLICT(module_id) DO UPDATE SET
            device_type=excluded.device_type,
            status=excluded.status,
            parameters=excluded.parameters,
            last_upload=excluded.last_upload,
            latitude=excluded.latitude,
            longitude=excluded.longitude
    """, (module_id, device_type, status, json.dumps(parameters),
          datetime.now().isoformat(timespec='seconds'), lat, lon))
    conn.commit()
    conn.close()


def retire_device(db_path, module_id, notes=""):
    entry = get_registry_entry(db_path, module_id)
    device_type = entry['device_type'] if entry else ""
    conn = get_conn(db_path)
    conn.execute("UPDATE registry SET status='retired', retire_notes=? WHERE module_id=?", (notes, module_id))
    conn.commit()
    conn.close()
    add_log(db_path, device_type, module_id, {"notes": notes}, None, None, action="retire")


def unretire_device(db_path, module_id):
    entry = get_registry_entry(db_path, module_id)
    device_type = entry['device_type'] if entry else ""
    conn = get_conn(db_path)
    conn.execute("UPDATE registry SET status='active' WHERE module_id=?", (module_id,))
    conn.commit()
    conn.close()
    add_log(db_path, device_type, module_id, {}, None, None, action="unretire")


def is_module_id_active(db_path, module_id):
    entry = get_registry_entry(db_path, module_id)
    return entry is not None and entry['status'] == 'active'


def clear_log(db_path):
    conn = get_conn(db_path)
    conn.execute("DELETE FROM upload_log")
    conn.commit()
    conn.close()


def clear_registry(db_path):
    conn = get_conn(db_path)
    conn.execute("DELETE FROM registry")
    conn.commit()
    conn.close()


def get_all_active_consumer_ids(db_path, exclude_module_id=None):
    """Consumer IDs used by currently active executors (global uniqueness check)."""
    conn = get_conn(db_path)
    rows = conn.execute("SELECT module_id, device_type, parameters FROM registry WHERE status='active'").fetchall()
    conn.close()
    ids = set()
    for r in rows:
        if r['device_type'] != 'executor':
            continue
        if exclude_module_id and r['module_id'] == exclude_module_id:
            continue
        try:
            params = json.loads(r['parameters'])
            for c in params.get('consumers', []):
                ids.add(c['id'])
        except Exception:
            pass
    return ids
