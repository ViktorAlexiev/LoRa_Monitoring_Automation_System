import { useState } from "react";
import { api } from "../api.js";
import Modal from "./Modal.jsx";

const KIND_LABEL = { sensor: "сензор", valve: "клапан" };
const KIND_LABEL_PLURAL = { sensor: "Сензори", valve: "Клапани" };

export default function ZoneModulesModal({
  zone, kind, allItems, pumps, executors, repeaters, locked, onClose, onSaved,
}) {
  const [localItems, setLocalItems] = useState(allItems);
  const [selected, setSelected] = useState(
    () => new Set(allItems.filter((it) => it.zone_id === zone.id).map((it) => it.id))
  );
  const [creating, setCreating] = useState(false);
  const [form, setForm] = useState({ id: "", name: "", pump_id: "", executor_id: "", repeater_id: "" });
  const [error, setError] = useState(null);
  const [saving, setSaving] = useState(false);

  function toggle(id) {
    setSelected((s) => {
      const next = new Set(s);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  }

  async function submitNew(e) {
    e.preventDefault();
    try {
      let created;
      if (kind === "sensor") {
        created = await api.sensors.create({ id: form.id, name: form.name, repeater_id: form.repeater_id || null });
      } else {
        if (!form.pump_id || !form.executor_id) {
          setError("Нов клапан трябва да има избрана помпа и изпълнител, за да може да влезе в зона.");
          return;
        }
        created = await api.valves.create({
          id: form.id, name: form.name,
          pump_id: form.pump_id || null, executor_id: form.executor_id || null,
        });
      }
      setLocalItems((prev) => [...prev, created]);
      setSelected((s) => new Set(s).add(created.id));
      setForm({ id: "", name: "", pump_id: "", executor_id: "", repeater_id: "" });
      setCreating(false);
      setError(null);
    } catch (err) {
      setError(err.message);
    }
  }

  async function save() {
    setSaving(true);
    setError(null);
    const originalIds = new Set(allItems.filter((it) => it.zone_id === zone.id).map((it) => it.id));
    const toAdd = [...selected].filter((id) => !originalIds.has(id));
    const toRemove = [...originalIds].filter((id) => !selected.has(id));
    try {
      if (toAdd.length) {
        if (kind === "sensor") await api.zones.assignSensors(zone.id, toAdd);
        else await api.zones.assignValves(zone.id, toAdd);
      }
      for (const id of toRemove) {
        if (kind === "sensor") await api.zones.unassignSensor(zone.id, id);
        else await api.zones.unassignValve(zone.id, id);
      }
      onSaved();
      onClose();
    } catch (err) {
      setError(err.message);
      setSaving(false);
    }
  }

  return (
    <Modal title={`${KIND_LABEL_PLURAL[kind]} — ${zone.name}`} onClose={onClose} width="480px">
      {locked && (
        <div className="locked-note">Зоната е активна — деактивирай я, за да променяш {KIND_LABEL[kind]}ите ѝ.</div>
      )}

      <div className="module-list">
        {localItems.map((it) => {
          const elsewhere = it.zone_id && it.zone_id !== zone.id;
          const notWired = kind === "valve" && !elsewhere && (!it.pump_id || !it.executor_id);
          const disabled = locked || elsewhere || notWired;
          return (
            <label
              key={it.id}
              className={`module-row ${disabled ? "module-row-disabled" : ""}`}
            >
              <input
                type="checkbox"
                checked={selected.has(it.id)}
                disabled={disabled}
                onChange={() => toggle(it.id)}
                
              />
              <span className="id-tag mono">{it.id}</span>
              <span>{it.name}</span>
              {elsewhere && <span className="muted">в друга зона</span>}
              {notWired && <span className="muted">няма помпа/изпълнител</span>}
            </label>
          );
        })}
        {localItems.length === 0 && <span className="muted">Все още няма {KIND_LABEL[kind]}и в системата.</span>}
      </div>

      <div>
        <button className="btn btn-sm" disabled={locked} onClick={() => setCreating((c) => !c)}>
          + Нов {KIND_LABEL[kind]}
        </button>
      </div>

      {creating && (
        <form className="form-grid" onSubmit={submitNew}>
          <div className="field">
            <label>ID</label>
            <input
              required autoFocus value={form.id}
              onChange={(e) => setForm({ ...form, id: e.target.value })}
              pattern="[A-Za-z0-9_\-]{1,16}"
              title="Само букви, цифри, - и _ (без интервали и специални знаци)"
            />
          </div>
          <div className="field"><label>Име</label><input required value={form.name} onChange={(e) => setForm({ ...form, name: e.target.value })} /></div>

          {kind === "sensor" && (
            <div className="field full">
              <label>Свързан repeater (по избор)</label>
              <select value={form.repeater_id} onChange={(e) => setForm({ ...form, repeater_id: e.target.value })}>
                <option value="">— директно към Gateway —</option>
                {repeaters.map((r) => <option key={r.id} value={r.id}>{r.id} — {r.name}</option>)}
              </select>
            </div>
          )}

          {kind === "valve" && (
            <>
              <div className="field">
                <label>Помпа</label>
                <select value={form.pump_id} onChange={(e) => setForm({ ...form, pump_id: e.target.value })}>
                  <option value="">— избери —</option>
                  {pumps.map((p) => <option key={p.id} value={p.id}>{p.id}</option>)}
                </select>
              </div>
              <div className="field">
                <label>Изпълнител</label>
                <select value={form.executor_id} onChange={(e) => setForm({ ...form, executor_id: e.target.value })}>
                  <option value="">— избери —</option>
                  {executors.map((ex) => <option key={ex.id} value={ex.id}>{ex.id}</option>)}
                </select>
              </div>
            </>
          )}

          <div className="field full">
            <button className="btn btn-primary btn-sm" type="submit">Създай</button>
            <button className="btn btn-sm" type="button" onClick={() => setCreating(false)}>Отказ</button>
          </div>
        </form>
      )}

      {error && <div className="error-note">{error}</div>}

      <div>
        <button className="btn" onClick={onClose}>Отказ</button>
        <button className="btn btn-primary" disabled={locked || saving} onClick={save}>Запази</button>
      </div>
    </Modal>
  );
}
