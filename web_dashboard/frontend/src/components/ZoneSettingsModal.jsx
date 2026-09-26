import { useEffect, useState } from "react";
import { api } from "../api.js";
import Modal from "./Modal.jsx";
import ConfirmDialog from "./ConfirmDialog.jsx";

const DAYS = ["Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Нд"];
// Автоматичното поливане по прагове винаги следи само влажността на почвата.
const PARAMS = [
  { key: "S_H", label: "Влажност на почвата", unit: "%", bounds: [0, 100] },
];

function daysToLabel(mask) {
  const on = DAYS.filter((_, i) => mask & (1 << i));
  return on.length === 7 ? "всеки ден" : on.join(", ");
}

// Used both for adding a new interval (existing = undefined) and for
// editing one in place.
function ScheduleForm({ zone, zoneValves, existing, onSaved, onCancel }) {
  const [start, setStart] = useState(existing?.start_time ?? "06:00");
  const [end, setEnd] = useState(existing?.end_time ?? "06:15");
  const [days, setDays] = useState(existing?.days_mask ?? 127);
  const [valveIds, setValveIds] = useState(existing?.valve_ids ?? []);
  const [error, setError] = useState(null);

  function toggleDay(i) {
    setDays((d) => (d & (1 << i) ? d & ~(1 << i) : d | (1 << i)));
  }
  function toggleValve(id) {
    setValveIds((v) => (v.includes(id) ? v.filter((x) => x !== id) : [...v, id]));
  }

  async function submit(e) {
    e.preventDefault();
    if (days === 0) { setError("Избери поне един ден."); return; }
    const body = { start_time: start, end_time: end, days_mask: days, enabled: existing?.enabled ?? true, valve_ids: valveIds };
    try {
      if (existing) await api.zones.updateSchedule(zone.id, existing.id, body);
      else await api.zones.createSchedule(zone.id, body);
      setError(null);
      onSaved();
    } catch (err) {
      setError(err.message);
    }
  }

  return (
    <form className="form-grid" onSubmit={submit}>
      <div className="field"><label>Начало</label><input type="time" value={start} onChange={(e) => setStart(e.target.value)} required /></div>
      <div className="field"><label>Край</label><input type="time" value={end} onChange={(e) => setEnd(e.target.value)} required /></div>
      <div className="field full">
        <label>Дни</label>
        <div className="days-row">
          {DAYS.map((d, i) => (
            <button type="button" key={d} className={`day-toggle ${days & (1 << i) ? "on" : ""}`} onClick={() => toggleDay(i)}>{d}</button>
          ))}
        </div>
      </div>
      <div className="field full">
        <label>Кои клапани отваря</label>
        <div className="checks">
          {zoneValves.map((v) => (
            <label className="check-chip" key={v.id}>
              <input type="checkbox" checked={valveIds.includes(v.id)} onChange={() => toggleValve(v.id)} />
              {v.name || v.id}
            </label>
          ))}
          {zoneValves.length === 0 && <span className="muted">Няма клапани в зоната</span>}
        </div>
      </div>
      {error && <div className="error-note field full">{error}</div>}
      <div className="field full">
        <button className="btn btn-primary btn-sm" type="submit">Запази</button>
        <button className="btn btn-sm" type="button" onClick={onCancel}>Отказ</button>
      </div>
    </form>
  );
}

function ScheduleCard({ zone, zoneValves, schedule, onSaved, onDelete }) {
  const [editing, setEditing] = useState(false);
  const names = schedule.valve_ids.map((id) => {
    const v = zoneValves.find((x) => x.id === id);
    return v?.name ? `${v.name} (${id})` : id;
  });
  if (editing) {
    return (
      <div className="threshold-row">
        <h4>Редакция на интервал</h4>
        <ScheduleForm
          zone={zone} zoneValves={zoneValves} existing={schedule}
          onSaved={() => { setEditing(false); onSaved(); }} onCancel={() => setEditing(false)}
        />
      </div>
    );
  }
  return (
    <div className="threshold-row">
      <h4>{schedule.start_time} – {schedule.end_time}</h4>
      <div className="muted">{daysToLabel(schedule.days_mask)} · клапани: {names.join(", ") || "—"}</div>
      <div className="row-actions">
        <button className="btn btn-sm" onClick={() => setEditing(true)}>Редактирай</button>
        <button className="btn btn-sm" onClick={onDelete}>Изтрий</button>
      </div>
    </div>
  );
}

function NewScheduleButton({ zone, zoneValves, onSaved }) {
  const [open, setOpen] = useState(false);
  if (!open) return <button className="btn btn-sm" onClick={() => setOpen(true)}>+ Нов интервал</button>;
  return (
    <div className="threshold-row">
      <h4>Нов интервал</h4>
      <ScheduleForm zone={zone} zoneValves={zoneValves} onSaved={() => { setOpen(false); onSaved(); }} onCancel={() => setOpen(false)} />
    </div>
  );
}

function ThresholdRow({ zone, param, label, unit, bounds, zoneValves, existing, onSaved, onDelete }) {
  const [editing, setEditing] = useState(false);
  const [minVal, setMinVal] = useState(existing?.min_val ?? bounds[0]);
  const [maxVal, setMaxVal] = useState(existing?.max_val ?? bounds[1]);
  const [duration, setDuration] = useState(existing?.irrigation_duration_s ?? 300);
  const [wait, setWait] = useState(existing?.infiltration_wait_s ?? 900);
  const [valveIds, setValveIds] = useState(existing?.valve_ids ?? []);
  const [error, setError] = useState(null);

  function toggleValve(id) {
    setValveIds((v) => (v.includes(id) ? v.filter((x) => x !== id) : [...v, id]));
  }

  async function save() {
    if (Number(minVal) >= Number(maxVal)) {
      setError("Минималната стойност трябва да е по-малка от максималната.");
      return;
    }
    try {
      await api.zones.upsertThreshold(zone.id, {
        param,
        min_val: Number(minVal),
        max_val: Number(maxVal),
        irrigation_duration_s: Number(duration),
        infiltration_wait_s: Number(wait),
        valve_ids: valveIds,
      });
      setEditing(false);
      setError(null);
      onSaved();
    } catch (err) {
      setError(err.message);
    }
  }

  return (
    <div className="threshold-row">
      <h4>{label}</h4>
      {!editing && (
        <div>
          {existing ? (
            <>
              <div className="muted">
                Диапазон: {existing.min_val ?? "—"} – {existing.max_val ?? "—"} · поливане {existing.irrigation_duration_s}s ·
                {" "}изчакване {existing.infiltration_wait_s}s · клапани: {existing.valve_ids.map((id) => {
                  const v = zoneValves.find((x) => x.id === id);
                  return v?.name ? `${v.name} (${id})` : id;
                }).join(", ") || "—"}
              </div>
              <div className="row-actions">
                <button className="btn btn-sm" onClick={() => setEditing(true)}>Редактирай</button>
                <button className="btn btn-sm" onClick={onDelete}>Изтрий</button>
              </div>
            </>
          ) : (
            <button className="btn btn-sm" onClick={() => setEditing(true)}>+ Настрой праг</button>
          )}
        </div>
      )}
      {editing && (
        <div className="form-grid">
          <div className="field full">
            <label>Мин. стойност ({bounds[0]}{unit} – {bounds[1]}{unit})</label>
            <div>
              <input
                type="range" min={bounds[0]} max={bounds[1]} step="0.5"
                value={minVal} onChange={(e) => setMinVal(e.target.value)}
                
              />
              <span className="mono">{Number(minVal)}{unit}</span>
            </div>
          </div>
          <div className="field full">
            <label>Макс. стойност ({bounds[0]}{unit} – {bounds[1]}{unit})</label>
            <div>
              <input
                type="range" min={bounds[0]} max={bounds[1]} step="0.5"
                value={maxVal} onChange={(e) => setMaxVal(e.target.value)}
                
              />
              <span className="mono">{Number(maxVal)}{unit}</span>
            </div>
          </div>
          <div className="field"><label>Продължителност на поливане, сек</label><input type="number" value={duration} onChange={(e) => setDuration(e.target.value)} /></div>
          <div className="field"><label>Изчакване преди прецена, сек</label><input type="number" value={wait} onChange={(e) => setWait(e.target.value)} /></div>
          <div className="field full">
            <label>Кои клапани компенсират</label>
            <div className="checks">
              {zoneValves.map((v) => (
                <label className="check-chip" key={v.id}>
                  <input type="checkbox" checked={valveIds.includes(v.id)} onChange={() => toggleValve(v.id)} />
                  {v.name || v.id}
                </label>
              ))}
              {zoneValves.length === 0 && <span className="muted">Няма клапани в зоната</span>}
            </div>
          </div>
          {error && <div className="error-note field full">{error}</div>}
          <div className="field full">
            <button className="btn btn-primary btn-sm" onClick={save}>Запази</button>
            <button className="btn btn-sm" onClick={() => setEditing(false)}>Отказ</button>
          </div>
        </div>
      )}
    </div>
  );
}

function HumidityGuardTab({ zone, onSaved }) {
  const [minVal, setMinVal] = useState(zone.humidity_warn_min ?? "");
  const [maxVal, setMaxVal] = useState(zone.humidity_warn_max ?? "");
  const [error, setError] = useState(null);
  const [saved, setSaved] = useState(false);

  async function save() {
    if (minVal !== "" && maxVal !== "" && Number(minVal) >= Number(maxVal)) {
      setError("Минималната влажност трябва да е по-малка от максималната.");
      return;
    }
    try {
      await api.zones.update(zone.id, {
        humidity_warn_min: minVal === "" ? null : Number(minVal),
        humidity_warn_max: maxVal === "" ? null : Number(maxVal),
      });
      setError(null);
      setSaved(true);
      onSaved();
    } catch (err) {
      setError(err.message);
    }
  }

  return (
    <>
      <div className="section-title">Защита от прекалено ниска/висока влажност</div>
      <p className="muted">
        Важи независимо от режима на зоната. При режим „по време“ се ползва и като защита срещу
        преполиване — планиран интервал не отваря клапан, ако почвата вече е на/над максимума.
      </p>
      <div className="form-grid">
        <div className="field">
          <label>Мин. влажност на почвата, %</label>
          <input type="number" min="0" max="100" value={minVal} onChange={(e) => { setMinVal(e.target.value); setSaved(false); }} />
        </div>
        <div className="field">
          <label>Макс. влажност на почвата, %</label>
          <input type="number" min="0" max="100" value={maxVal} onChange={(e) => { setMaxVal(e.target.value); setSaved(false); }} />
        </div>
        {error && <div className="error-note field full">{error}</div>}
        {saved && !error && <div className="card-note field full">Записано.</div>}
        <div className="field full">
          <button className="btn btn-primary btn-sm" onClick={save}>Запази</button>
        </div>
      </div>
    </>
  );
}

export default function ZoneSettingsModal({ zone, valves, onClose, onSaved = () => {} }) {
  const [activeTab, setActiveTab] = useState(zone.regime === "threshold" ? "threshold" : "clock");
  const [schedules, setSchedules] = useState([]);
  const [thresholds, setThresholds] = useState([]);
  const [confirmDeleteSchedule, setConfirmDeleteSchedule] = useState(null);
  const [confirmDeleteThreshold, setConfirmDeleteThreshold] = useState(null); // param key
  const [deleteError, setDeleteError] = useState(null);

  const zoneValves = valves.filter((v) => v.zone_id === zone.id);

  async function load() {
    const [s, t] = await Promise.all([api.zones.schedules(zone.id), api.zones.thresholds(zone.id)]);
    setSchedules(s);
    setThresholds(t);
  }

  useEffect(() => {
    load();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [zone.id]);

  async function deleteSchedule() {
    try {
      await api.zones.deleteSchedule(zone.id, confirmDeleteSchedule);
      setConfirmDeleteSchedule(null);
      setDeleteError(null);
      load();
    } catch (err) {
      setDeleteError(err.message);
    }
  }

  async function deleteThreshold() {
    try {
      await api.zones.deleteThreshold(zone.id, confirmDeleteThreshold);
      setConfirmDeleteThreshold(null);
      setDeleteError(null);
      load();
    } catch (err) {
      setDeleteError(err.message);
    }
  }

  return (
    <Modal title={`Настройки на автоматичните режими — ${zone.name}`} onClose={onClose} width="560px">
      <div className="subtabs">
        <button className={`subtab ${activeTab === "clock" ? "active" : ""}`} onClick={() => setActiveTab("clock")}>
          По време {zone.regime === "clock" && "· активен"}
        </button>
        <button className={`subtab ${activeTab === "threshold" ? "active" : ""}`} onClick={() => setActiveTab("threshold")}>
          По прагове {zone.regime === "threshold" && "· активен"}
        </button>
        <button className={`subtab ${activeTab === "humidity" ? "active" : ""}`} onClick={() => setActiveTab("humidity")}>
          Защита от влажност
        </button>
      </div>

      {activeTab === "clock" && (
        <>
          <div className="section-title">Часови интервали</div>
          {schedules.map((sch) => (
            <ScheduleCard
              key={sch.id} zone={zone} zoneValves={zoneValves} schedule={sch}
              onSaved={load} onDelete={() => setConfirmDeleteSchedule(sch.id)}
            />
          ))}
          {schedules.length === 0 && <p className="muted">Все още няма зададени интервали.</p>}
          <NewScheduleButton zone={zone} zoneValves={zoneValves} onSaved={load} />
        </>
      )}

      {activeTab === "threshold" && (
        <>
          <div className="section-title">Прагови правила</div>
          {PARAMS.map((p) => (
            <ThresholdRow
              key={p.key}
              zone={zone}
              param={p.key}
              label={p.label}
              unit={p.unit}
              bounds={p.bounds}
              zoneValves={zoneValves}
              existing={thresholds.find((t) => t.param === p.key)}
              onSaved={load}
              onDelete={() => setConfirmDeleteThreshold(p.key)}
            />
          ))}
        </>
      )}

      {activeTab === "humidity" && <HumidityGuardTab zone={zone} onSaved={onSaved} />}

      {confirmDeleteSchedule && (
        <ConfirmDialog
          title="Изтриване на интервал"
          message="Сигурен ли си, че искаш да изтриеш тоя часови интервал?"
          confirmLabel="Изтрий"
          danger
          error={deleteError}
          onConfirm={deleteSchedule}
          onCancel={() => { setConfirmDeleteSchedule(null); setDeleteError(null); }}
        />
      )}
      {confirmDeleteThreshold && (
        <ConfirmDialog
          title="Изтриване на праг"
          message="Сигурен ли си, че искаш да изтриеш това прагово правило?"
          confirmLabel="Изтрий"
          danger
          error={deleteError}
          onConfirm={deleteThreshold}
          onCancel={() => { setConfirmDeleteThreshold(null); setDeleteError(null); }}
        />
      )}
    </Modal>
  );
}
