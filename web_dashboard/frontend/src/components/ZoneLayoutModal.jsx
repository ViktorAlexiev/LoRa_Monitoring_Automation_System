import { useEffect, useRef, useState } from "react";
import { api } from "../api.js";
import Modal from "./Modal.jsx";
import { MAP_KINDS, MAP_COLORS, objectLabel } from "../utils/mapObjects.js";

const clamp = (v, lo = 0, hi = 100) => Math.min(hi, Math.max(lo, v));
const STEP = 5; // snap step in percent of the map (grid lines are every 10 %)
let nextKey = 1;
const newKey = () => `o${nextKey++}`;

// Admin arranges the zone's sensors - and rough landmarks (buildings, gate,
// well ...) - on a map that mirrors the real layout of the site; the worker
// then sees the same picture in the zone view.
export default function ZoneLayoutModal({ zone, sensors, onClose, onSaved = () => {} }) {
  const mapRef = useRef(null);
  const drag = useRef(null); // { kind: "sensor"|"object"|"resize", id, dx, dy }
  const [pos, setPos] = useState({}); // sensor_id -> { x, y } (percent)
  const [objects, setObjects] = useState([]); // { key, kind, label, x, y, w, h }
  const [selected, setSelected] = useState(null); // object key
  const [loaded, setLoaded] = useState(false);
  const [error, setError] = useState(null);
  const [saving, setSaving] = useState(false);
  const [snap, setSnap] = useState(true); // snap positions/sizes to the grid
  const [newName, setNewName] = useState(""); // text of the object being added
  const [newShape, setNewShape] = useState("rect");
  const [newColor, setNewColor] = useState("");
  const q = (v) => (snap ? Math.round(v / STEP) * STEP : v);

  useEffect(() => {
    Promise.all([api.zones.layout(zone.id), api.zones.mapObjects(zone.id)])
      .then(([items, objs]) => {
        setPos(Object.fromEntries(items.map((i) => [i.sensor_id, { x: i.x, y: i.y }])));
        setObjects(objs.map((o) => ({ ...o, key: newKey() })));
        setLoaded(true);
      })
      .catch((err) => setError(err.message));
  }, [zone.id]);

  const placed = sensors.filter((s) => pos[s.id]);
  const tray = sensors.filter((s) => !pos[s.id]);
  const sel = objects.find((o) => o.key === selected);

  function pct(e) {
    const rect = mapRef.current.getBoundingClientRect();
    return { px: ((e.clientX - rect.left) / rect.width) * 100, py: ((e.clientY - rect.top) / rect.height) * 100 };
  }

  function placeSensor(id) {
    const n = placed.length;
    setPos((p) => ({ ...p, [id]: { x: q(20 + ((n * 17) % 60)), y: q(25 + ((n * 23) % 50)) } }));
  }

  function removeSensor(id) {
    setPos((p) => {
      const next = { ...p };
      delete next[id];
      return next;
    });
  }

  function addObject(kind, label = "", color = "") {
    const k = MAP_KINDS[kind];
    const o = { key: newKey(), kind, label, color, x: q(50), y: q(50), w: k.w, h: k.h };
    setObjects((list) => [...list, o]);
    setSelected(o.key);
  }

  // "Add object": the admin types what it is and picks a rough shape.
  function addNamed(e) {
    e.preventDefault();
    if (!newName.trim()) return;
    addObject(newShape, newName.trim(), newColor);
    setNewName("");
  }

  function removeObject(key) {
    setObjects((list) => list.filter((o) => o.key !== key));
    setSelected(null);
  }

  function updateObject(key, patch) {
    setObjects((list) => list.map((o) => (o.key === key ? { ...o, ...patch } : o)));
  }

  function startDrag(e, kind, id, current, resize = false) {
    e.stopPropagation();
    const { px, py } = pct(e);
    drag.current = { kind: resize ? "resize" : kind, id, dx: current.x - px, dy: current.y - py };
    if (kind === "object") setSelected(id);
    e.currentTarget.setPointerCapture(e.pointerId);
  }

  function onMove(e) {
    const d = drag.current;
    if (!d) return;
    const { px, py } = pct(e);
    if (d.kind === "sensor") {
      setPos((p) => ({ ...p, [d.id]: { x: clamp(q(px + d.dx)), y: clamp(q(py + d.dy)) } }));
    } else if (d.kind === "object") {
      updateObject(d.id, { x: clamp(q(px + d.dx)), y: clamp(q(py + d.dy)) });
    } else {
      // resize: the handle sits at the bottom-right corner; object x/y is its centre
      setObjects((list) => list.map((o) => (o.key === d.id
        ? { ...o, w: clamp(q((px - o.x) * 2), 3, 100), h: clamp(q((py - o.y) * 2), 3, 100) }
        : o)));
    }
  }

  function endDrag() {
    drag.current = null;
  }

  async function save() {
    setSaving(true);
    const r1 = (v) => Math.round(v * 10) / 10;
    try {
      await api.zones.saveLayout(
        zone.id,
        placed.map((s) => ({ sensor_id: s.id, x: r1(pos[s.id].x), y: r1(pos[s.id].y) }))
      );
      await api.zones.saveMapObjects(
        zone.id,
        objects.map((o) => ({ kind: o.kind, label: o.label, color: o.color || "", x: r1(o.x), y: r1(o.y), w: r1(o.w), h: r1(o.h) }))
      );
      onSaved();
      onClose();
    } catch (err) {
      setError(err.message);
      setSaving(false);
    }
  }

  return (
    <Modal title={`Карта на обекта — ${zone.name}`} onClose={onClose} width="800px">
      <p className="muted">
        Подреди сензорите така, както са на обекта, и добави ориентири (сгради, вход, кладенец…),
        за да се ориентира работникът. Влачи с мишка или пръст.
      </p>

      <label className="snap-toggle">
        <input type="checkbox" checked={snap} onChange={(e) => setSnap(e.target.checked)} />
        Залепване към мрежата (по-лесно за подреждане)
      </label>

      <div className="section-title">Добави обект на картата</div>
      <form className="obj-add" onSubmit={addNamed}>
        <label>Какво е? (напр. Склад, Кладенец, Ограда, Път)
          <input value={newName} maxLength={64} onChange={(e) => setNewName(e.target.value)} placeholder="Напиши име…" />
        </label>
        <label>Форма
          <select value={newShape} onChange={(e) => setNewShape(e.target.value)}>
            {Object.entries(MAP_KINDS).filter(([, k]) => k.generic).map(([kind, k]) => (
              <option key={kind} value={kind}>{k.label}</option>
            ))}
          </select>
        </label>
        <label>Цвят
          <span className="color-pick">
            <span className="color-swatch" style={{ background: MAP_COLORS[newColor]?.swatch }} />
            <select value={newColor} onChange={(e) => setNewColor(e.target.value)}>
              {Object.entries(MAP_COLORS).map(([key, c]) => <option key={key} value={key}>{c.label}</option>)}
            </select>
          </span>
        </label>
        <button type="submit" className="btn btn-primary btn-sm" disabled={!newName.trim()}>+ Добави обект</button>
      </form>
      <div className="checks obj-presets">
        <span className="muted">Бързо:</span>
        {Object.entries(MAP_KINDS).filter(([, k]) => !k.generic && !k.hidden).map(([kind, k]) => (
          <button type="button" key={kind} className="check-chip" onClick={() => addObject(kind, "", newColor)}>+ {k.label}</button>
        ))}
      </div>

      <div className="site-map-scroll">
        <div className="site-map site-map-edit" ref={mapRef} onPointerMove={onMove} onPointerUp={endDrag} onPointerCancel={endDrag}
             onPointerDown={() => setSelected(null)}>
          {objects.map((o) => (
            <div
              key={o.key} className={`map-obj obj-${o.kind} ${o.color ? `col-${o.color}` : ""} ${selected === o.key ? "map-obj-selected" : ""}`}
              style={{ left: `${o.x}%`, top: `${o.y}%`, width: `${o.w}%`, height: `${o.h}%` }}
              onPointerDown={(e) => startDrag(e, "object", o.key, o)}
            >
              <span className="map-obj-label">{objectLabel(o)}</span>
              {selected === o.key && (
                <span className="map-obj-handle" onPointerDown={(e) => startDrag(e, "object", o.key, o, true)} aria-label="Промени размера" />
              )}
            </div>
          ))}
          {placed.map((s) => (
            <div
              key={s.id} className="map-chip"
              style={{ left: `${pos[s.id].x}%`, top: `${pos[s.id].y}%` }}
              onPointerDown={(e) => { if (!e.target.closest(".map-chip-x")) startDrag(e, "sensor", s.id, pos[s.id]); }}
            >
              <span className="map-chip-id mono" title={s.name || s.id}>{s.id}</span>
              <button type="button" className="map-chip-x" aria-label={`Махни ${s.name || s.id} от картата`} onClick={() => removeSensor(s.id)}>&#10005;</button>
            </div>
          ))}
          {loaded && placed.length === 0 && objects.length === 0 && (
            <div className="site-map-empty">Добави сензори от списъка отдолу</div>
          )}
        </div>
      </div>

      {sel && (
        <div className="obj-editor">
          <label>Какво е
            <input value={sel.label} maxLength={64} placeholder={MAP_KINDS[sel.kind]?.label}
                   onChange={(e) => updateObject(sel.key, { label: e.target.value })} />
          </label>
          <label>Форма
            <select value={sel.kind} onChange={(e) => updateObject(sel.key, { kind: e.target.value })}>
              {Object.entries(MAP_KINDS).filter(([kind, k]) => !k.hidden || kind === sel.kind).map(([kind, k]) => (
                <option key={kind} value={kind}>{k.label}</option>
              ))}
            </select>
          </label>
          <label>Цвят
            <span className="color-pick">
              <span className="color-swatch" style={{ background: MAP_COLORS[sel.color || ""]?.swatch }} />
              <select value={sel.color || ""} onChange={(e) => updateObject(sel.key, { color: e.target.value })}>
                {Object.entries(MAP_COLORS).map(([key, c]) => <option key={key} value={key}>{c.label}</option>)}
              </select>
            </span>
          </label>
          <button type="button" className="btn btn-sm" onClick={() => removeObject(sel.key)}>Изтрий ориентира</button>
          <span className="muted">Размерът се сменя от точката в ъгъла.</span>
        </div>
      )}

      <div className="section-title">Сензори, които още не са на картата</div>
      <div className="checks">
        {tray.map((s) => (
          <button type="button" key={s.id} className="check-chip" onClick={() => placeSensor(s.id)}>
            + <span className="mono" title={s.name || s.id}>{s.id}</span> <span className="muted">{s.name}</span>
          </button>
        ))}
        {tray.length === 0 && <span className="muted">Всички сензори са на картата.</span>}
      </div>
      {sensors.length === 0 && <p className="muted">В тази зона още няма сензори — добави ги от „Сензори“.</p>}

      {error && <div className="error-note">{error}</div>}
      <div className="form-actions">
        <button className="btn" onClick={onClose}>Отказ</button>
        <button className="btn btn-primary" onClick={save} disabled={saving || !loaded}>Запази</button>
      </div>
    </Modal>
  );
}
