import { useEffect, useRef, useState } from "react";
import { api } from "../api.js";
import Modal from "./Modal.jsx";
import { MAP_KINDS, MAP_COLORS, objectLabel } from "../utils/mapObjects.js";
import PanZoom, { mapBounds } from "./PanZoom.jsx";

// The map can grow past the first screen: positions may go anywhere in this range (percent of the base canvas).
const clamp = (v, lo = -300, hi = 400) => Math.min(hi, Math.max(lo, v));
const STEP = 5;
let nextKey = 1;
const newKey = () => `s${nextKey++}`;

// Admin arranges the whole site: each zone is a box (drag to move, corner dot
// to resize) and landmarks (roads, buildings, gate ...) go around them.
export default function SiteMapEditor({ zones, onClose, onSaved = () => {} }) {
  const mapRef = useRef(null);
  const drag = useRef(null);
  const [placed, setPlaced] = useState({}); // zone_id -> { x, y, w, h }
  const [objects, setObjects] = useState([]);
  const [selected, setSelected] = useState(null); // "z<id>" or object key
  const [dragging, setDragging] = useState(false); // map bounds stay put while something is being dragged
  const [snap, setSnap] = useState(true);
  const [newName, setNewName] = useState("");
  const [newShape, setNewShape] = useState("rect");
  const [newColor, setNewColor] = useState("");
  const [loaded, setLoaded] = useState(false);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState(null);
  const q = (v) => (snap ? Math.round(v / STEP) * STEP : v);

  useEffect(() => {
    api.site.map()
      .then((m) => {
        setPlaced(Object.fromEntries(m.zones.map((z) => [z.zone_id, { x: z.x, y: z.y, w: z.w, h: z.h }])));
        setObjects(m.objects.map((o) => ({ ...o, key: newKey() })));
        setLoaded(true);
      })
      .catch((err) => setError(err.message));
  }, []);

  const tray = zones.filter((z) => !placed[z.id]);
  const selZone = typeof selected === "string" && selected.startsWith("z") ? zones.find((z) => `z${z.id}` === selected) : null;
  const selObj = objects.find((o) => o.key === selected);

  function pct(e) {
    const r = mapRef.current.getBoundingClientRect();
    return { px: ((e.clientX - r.left) / r.width) * 100, py: ((e.clientY - r.top) / r.height) * 100 };
  }

  function addZone(id) {
    const n = Object.keys(placed).length;
    setPlaced((p) => ({ ...p, [id]: { x: q(25 + ((n * 20) % 60)), y: q(25 + ((n * 25) % 50)), w: 25, h: 25 } }));
    setSelected(`z${id}`);
  }

  function addObject(kind, label, color) {
    const k = MAP_KINDS[kind];
    const o = { key: newKey(), kind, label, color, x: q(50), y: q(50), w: k.w, h: k.h };
    setObjects((l) => [...l, o]);
    setSelected(o.key);
  }

  function startDrag(e, type, id, cur, resize = false) {
    e.stopPropagation();
    const { px, py } = pct(e);
    drag.current = { type: resize ? `${type}-resize` : type, id, dx: cur.x - px, dy: cur.y - py };
    setSelected(type === "zone" ? `z${id}` : id);
    e.currentTarget.setPointerCapture(e.pointerId);
    setDragging(true);
  }

  function onMove(e) {
    const d = drag.current;
    if (!d) return;
    const { px, py } = pct(e);
    if (d.type === "zone") {
      setPlaced((p) => ({ ...p, [d.id]: { ...p[d.id], x: clamp(q(px + d.dx)), y: clamp(q(py + d.dy)) } }));
    } else if (d.type === "zone-resize") {
      setPlaced((p) => ({ ...p, [d.id]: { ...p[d.id], w: clamp(q((px - p[d.id].x) * 2), 5, 400), h: clamp(q((py - p[d.id].y) * 2), 5, 400) } }));
    } else if (d.type === "object") {
      setObjects((l) => l.map((o) => (o.key === d.id ? { ...o, x: clamp(q(px + d.dx)), y: clamp(q(py + d.dy)) } : o)));
    } else {
      setObjects((l) => l.map((o) => (o.key === d.id ? { ...o, w: clamp(q((px - o.x) * 2), 3, 400), h: clamp(q((py - o.y) * 2), 3, 400) } : o)));
    }
  }

  const endDrag = () => { setDragging(false); drag.current = null; };

  async function save() {
    setSaving(true);
    const r1 = (v) => Math.round(v * 10) / 10;
    try {
      await api.site.saveMap({
        zones: Object.entries(placed).map(([id, p]) => ({ zone_id: Number(id), x: r1(p.x), y: r1(p.y), w: r1(p.w), h: r1(p.h) })),
        objects: objects.map((o) => ({ kind: o.kind, label: o.label, color: o.color || "", x: r1(o.x), y: r1(o.y), w: r1(o.w), h: r1(o.h) })),
      });
      onSaved();
      onClose();
    } catch (err) {
      setError(err.message);
      setSaving(false);
    }
  }

  return (
    <Modal title="Карта на целия обект" onClose={onClose} width="900px">
      <p className="muted">
        Подреди зоните така, както са на терена, и добави ориентири (път, сгради, вход…). Всеки потребител
        ще вижда картата, но само зоните, до които има достъп, ще се отварят.
      </p>
      <label className="snap-toggle">
        <input type="checkbox" checked={snap} onChange={(e) => setSnap(e.target.checked)} />
        Залепване към мрежата
      </label>

      <div className="section-title">Добави ориентир</div>
      <form className="obj-add" onSubmit={(e) => { e.preventDefault(); if (newName.trim()) { addObject(newShape, newName.trim(), newColor); setNewName(""); } }}>
        <label>Какво е? (напр. Главен път, Склад, Вход)
          <input value={newName} maxLength={64} onChange={(e) => setNewName(e.target.value)} placeholder="Напиши име…" />
        </label>
        <label>Форма
          <select value={newShape} onChange={(e) => setNewShape(e.target.value)}>
            {Object.entries(MAP_KINDS).filter(([, k]) => k.generic).map(([kind, k]) => <option key={kind} value={kind}>{k.label}</option>)}
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
        {["road", "trees", "well"].map((kind) => (
          <button type="button" key={kind} className="check-chip" onClick={() => addObject(kind, "", newColor)}>+ {MAP_KINDS[kind].label}</button>
        ))}
      </div>

      <PanZoom
        stageRef={mapRef} className="site-map-whole site-map-edit" freezeBounds={dragging}
        bounds={mapBounds([...objects, ...Object.values(placed)], { margin: 10, include: { x0: 0, y0: 0, x1: 100, y1: 100 } })}
        onPointerMove={onMove} onPointerUp={endDrag} onPointerCancel={endDrag} onPointerDown={() => setSelected(null)}
      >
        <>
          {objects.map((o) => (
            <div key={o.key} data-nodrag className={`map-obj obj-${o.kind} ${o.color ? `col-${o.color}` : ""} ${selected === o.key ? "map-obj-selected" : ""}`}
                 style={{ left: `${o.x}%`, top: `${o.y}%`, width: `${o.w}%`, height: `${o.h}%` }}
                 onPointerDown={(e) => startDrag(e, "object", o.key, o)}>
              <span className="map-obj-label">{objectLabel(o)}</span>
              {selected === o.key && <span className="map-obj-handle" onPointerDown={(e) => startDrag(e, "object", o.key, o, true)} />}
            </div>
          ))}
          {zones.filter((z) => placed[z.id]).map((z) => {
            const p = placed[z.id];
            return (
              <div key={z.id} data-nodrag className={`site-zone site-zone-ok site-zone-edit ${selected === `z${z.id}` ? "map-obj-selected" : ""}`}
                   style={{ left: `${p.x}%`, top: `${p.y}%`, width: `${p.w}%`, height: `${p.h}%` }}
                   onPointerDown={(e) => startDrag(e, "zone", z.id, p)}>
                <span className="site-zone-name">{z.name}</span>
                {selected === `z${z.id}` && <span className="map-obj-handle" onPointerDown={(e) => startDrag(e, "zone", z.id, p, true)} />}
              </div>
            );
          })}
          {loaded && Object.keys(placed).length === 0 && objects.length === 0 && (
            <div className="site-map-empty">Добави зоните от списъка отдолу</div>
          )}
        </>
      </PanZoom>

      {(selZone || selObj) && (
        <div className="obj-editor">
          {selObj && (
            <>
              <label>Какво е
                <input value={selObj.label} maxLength={64} placeholder={MAP_KINDS[selObj.kind]?.label}
                       onChange={(e) => setObjects((l) => l.map((o) => (o.key === selObj.key ? { ...o, label: e.target.value } : o)))} />
              </label>
              <label>Цвят
                <span className="color-pick">
                  <span className="color-swatch" style={{ background: MAP_COLORS[selObj.color || ""]?.swatch }} />
                  <select value={selObj.color || ""} onChange={(e) => setObjects((l) => l.map((o) => (o.key === selObj.key ? { ...o, color: e.target.value } : o)))}>
                    {Object.entries(MAP_COLORS).map(([key, c]) => <option key={key} value={key}>{c.label}</option>)}
                  </select>
                </span>
              </label>
              <button type="button" className="btn btn-sm" onClick={() => { setObjects((l) => l.filter((o) => o.key !== selObj.key)); setSelected(null); }}>Изтрий ориентира</button>
            </>
          )}
          {selZone && (
            <button type="button" className="btn btn-sm" onClick={() => { setPlaced((p) => { const n = { ...p }; delete n[selZone.id]; return n; }); setSelected(null); }}>
              Махни „{selZone.name}“ от картата
            </button>
          )}
          <span className="muted">Размерът се сменя от точката в ъгъла.</span>
        </div>
      )}

      <div className="section-title">Зони, които още не са на картата</div>
      <div className="checks">
        {tray.map((z) => <button type="button" key={z.id} className="check-chip" onClick={() => addZone(z.id)}>+ {z.name}</button>)}
        {tray.length === 0 && <span className="muted">Всички зони са на картата.</span>}
      </div>

      {error && <div className="error-note">{error}</div>}
      <div className="form-actions">
        <button className="btn" onClick={onClose}>Отказ</button>
        <button className="btn btn-primary" onClick={save} disabled={saving || !loaded}>Запази</button>
      </div>
    </Modal>
  );
}
