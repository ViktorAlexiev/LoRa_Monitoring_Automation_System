import { useState } from "react";
import { objectLabel } from "../utils/mapObjects.js";
import PanZoom, { mapBounds } from "./PanZoom.jsx";

// Full tile size in px. If any two sensors would overlap at this size, the
// whole map switches to small pills (id + soil humidity); tap one for details.
// Zooming in spreads them apart again, so the full tiles come back.
const TILE_W = 108;
const TILE_H = 104;

// The zone's sensors laid out the way the admin arranged them on the site
// (see ZoneLayoutModal). Read-only: live values at each spot. The map can be
// zoomed and dragged, and ends where its elements end.
export default function SensorMap({ sensors, layout, objects = [], readings, errors }) {
  const [open, setOpen] = useState(null); // sensor id whose detail card is open (compact mode)

  const at = Object.fromEntries(layout.map((i) => [i.sensor_id, i]));
  const placed = sensors.filter((s) => at[s.id]);
  const bounds = mapBounds([...objects, ...placed.map((s) => ({ x: at[s.id].x, y: at[s.id].y, w: 14, h: 20 }))]);

  const fmt = (v, d = 1) => (v != null ? v.toFixed(d) : "—");

  const map = (
    <PanZoom bounds={bounds} onClick={() => setOpen(null)}>
      {({ scale, width, height }) => {
        let compact = false;
        for (let i = 0; i < placed.length && !compact; i += 1) {
          for (let j = i + 1; j < placed.length; j += 1) {
            const a = at[placed[i].id];
            const b = at[placed[j].id];
            if (Math.abs(a.x - b.x) * width * scale / 100 < TILE_W && Math.abs(a.y - b.y) * height * scale / 100 < TILE_H) {
              compact = true;
              break;
            }
          }
        }
        return (
          <>
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
                return (
                  <div key={s.id} className={`map-pill ${sev} ${isOpen ? "map-pill-open" : ""}`} style={style} title={title}
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
          </>
        );
      }}
    </PanZoom>
  );

  return map;
}
