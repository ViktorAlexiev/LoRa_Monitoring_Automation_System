import { useLayoutEffect, useRef, useState } from "react";
import { objectLabel } from "../utils/mapObjects.js";

// Full tile size in px. If any two sensors would overlap at this size, the
// whole map switches to small pills (id + soil humidity); tap one for details.
const TILE_W = 108;
const TILE_H = 104;

// The zone's sensors laid out the way the admin arranged them on the site
// (see ZoneLayoutModal). Read-only: live values at each spot.
export default function SensorMap({ sensors, layout, objects = [], readings, errors }) {
  const mapRef = useRef(null);
  const [size, setSize] = useState({ w: 680, h: 453 });
  const [open, setOpen] = useState(null); // sensor id whose detail card is open (compact mode)

  // Measure right away (before paint) and again on resize / rotation.
  useLayoutEffect(() => {
    const el = mapRef.current;
    if (!el) return undefined;
    const measure = () => {
      const r = el.getBoundingClientRect();
      if (r.width > 0) setSize((old) => (old.w === r.width && old.h === r.height ? old : { w: r.width, h: r.height }));
    };
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    window.addEventListener("resize", measure);
    return () => { ro.disconnect(); window.removeEventListener("resize", measure); };
  }, []);

  const at = Object.fromEntries(layout.map((i) => [i.sensor_id, i]));
  const placed = sensors.filter((s) => at[s.id]);

  let compact = false;
  for (let i = 0; i < placed.length && !compact; i += 1) {
    for (let j = i + 1; j < placed.length; j += 1) {
      const a = at[placed[i].id];
      const b = at[placed[j].id];
      if (Math.abs(a.x - b.x) * size.w / 100 < TILE_W && Math.abs(a.y - b.y) * size.h / 100 < TILE_H) {
        compact = true;
        break;
      }
    }
  }

  const fmt = (v, d = 1) => (v != null ? v.toFixed(d) : "—");

  return (
    <div className="site-map-scroll">
      <div className="site-map" ref={mapRef} onClick={() => setOpen(null)}>
        {objects.map((o, i) => (
          <div key={i} className={`map-obj obj-${o.kind} ${o.color ? `col-${o.color}` : ""}`}
               style={{ left: `${o.x}%`, top: `${o.y}%`, width: `${o.w}%`, height: `${o.h}%` }}>
            <span className="map-obj-label">{objectLabel(o)}</span>
          </div>
        ))}
        {placed.map((s) => {
          const last = (readings[s.id] || []).slice(-1)[0];
          const err = errors.find((e) => e.sensor_id === s.id);
          const sev = err ? `map-tile-${err.severity}` : "";
          const flag = err ? (err.severity === "warning" ? "⚠ " : "✖ ") : "";
          const title = `${s.name || s.id}${err ? " — " + err.description : ""}`;
          const style = { left: `${at[s.id].x}%`, top: `${at[s.id].y}%` };

          if (compact) {
            const isOpen = open === s.id;
            // keep the pill fully inside the frame even for a sensor at the very edge
            const pillStyle = { ...style, left: `clamp(54px, ${at[s.id].x}%, calc(100% - 54px))`, top: `clamp(14px, ${at[s.id].y}%, calc(100% - 14px))` };
            return (
              <div key={s.id} className={`map-pill ${sev} ${isOpen ? "map-pill-open" : ""}`} style={pillStyle} title={title}
                   onClick={(e) => { e.stopPropagation(); setOpen(isOpen ? null : s.id); }} role="button" tabIndex={0}
                   onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") setOpen(isOpen ? null : s.id); }}>
                <span className="mono map-pill-id">{flag}{s.id}</span>
                <span className="map-pill-val">{last?.soil_h != null ? `${last.soil_h.toFixed(0)}%` : "—"}</span>
                {isOpen && (
                  <div className={`map-pill-card ${at[s.id].x > 62 ? "card-right" : at[s.id].x < 38 ? "card-left" : ""}`} onClick={(e) => e.stopPropagation()}>
                    <b>{s.name || s.id}</b>
                    <div>Почва: {fmt(last?.soil_h, 0)}% · {fmt(last?.soil_t)}°</div>
                    <div>Въздух: {fmt(last?.air_h, 0)}% · {fmt(last?.air_t)}°</div>
                    {err && <div className="map-pill-err">{err.description}</div>}
                  </div>
                )}
              </div>
            );
          }

          return (
            <div key={s.id} className={`map-tile ${sev}`} style={style} title={title}>
              <div className="map-tile-head">
                <span className="mono map-tile-id">{flag}{s.id}</span>
              </div>
              <div className="map-tile-main">{last?.soil_h != null ? `${last.soil_h.toFixed(0)}%` : "—"}</div>
              <div className="map-tile-sub">почва {last?.soil_t != null ? `${last.soil_t.toFixed(1)}°` : "—"}</div>
              <div className="map-tile-sub">въздух {last?.air_t != null ? `${last.air_t.toFixed(1)}°` : "—"}</div>
              <div className="map-tile-name">{s.name}</div>
            </div>
          );
        })}
      </div>
      {compact && <div className="chart-legend">Сензорите са близо един до друг — натисни сензор за подробности.</div>}
    </div>
  );
}
