import { Link } from "react-router-dom";
import { MAP_KINDS, objectLabel } from "../utils/mapObjects.js";
import PanZoom, { mapBounds } from "./PanZoom.jsx";

// Status of a zone box on the whole-site map, from the dashboard's zone list.
export function zoneTone(z) {
  if (!z.accessible) return "locked";
  if (z.open_error_count > 0) return z.worst_open_severity === "warning" ? "warn" : "bad";
  if (!z.is_active) return "idle";
  return "ok";
}

// The whole site on one picture: every zone as a box, plus landmarks. A zone
// the user may open is a link; one they may not is a grey, inert box.
export default function SiteMap({ zones, map, showLocked = true }) {
  const byId = Object.fromEntries(zones.map((z) => [z.id, z]));
  const placed = map.zones.filter((p) => byId[p.zone_id] && (showLocked || byId[p.zone_id].accessible));
  return (
    <PanZoom className="site-map-whole" bounds={mapBounds([...map.objects, ...placed])}>
      {() => (
        <>
        {map.objects.map((o, i) => (
          <div key={i} className={`map-obj obj-${o.kind} ${o.color ? `col-${o.color}` : ""}`}
               style={{ left: `${o.x}%`, top: `${o.y}%`, width: `${o.w}%`, height: `${o.h}%` }}>
            <span className="map-obj-label">{objectLabel(o) || MAP_KINDS[o.kind]?.label}</span>
          </div>
        ))}
        {placed.map((p) => {
          const z = byId[p.zone_id];
          const tone = zoneTone(z);
          const style = { left: `${p.x}%`, top: `${p.y}%`, width: `${p.w}%`, height: `${p.h}%` };
          const inner = (
            <>
              <span className="site-zone-name">{z.name}</span>
              {z.accessible && z.open_error_count > 0 && (
                <span className="site-zone-flag">{z.worst_open_severity === "warning" ? "⚠" : "✖"} {z.open_error_count}</span>
              )}
              {z.accessible && z.readings?.soil_h != null && (
                <span className="site-zone-val">{z.readings.soil_h.toFixed(0)}%</span>
              )}
              {!z.accessible && <span className="site-zone-lock">няма достъп</span>}
            </>
          );
          return z.accessible ? (
            <Link key={p.zone_id} to={`/zones/${z.id}`} className={`site-zone site-zone-${tone}`} style={style}>{inner}</Link>
          ) : (
            <div key={p.zone_id} className={`site-zone site-zone-${tone}`} style={style} aria-disabled="true">{inner}</div>
          );
        })}
        {placed.length === 0 && <div className="site-map-empty">Картата на обекта още не е подредена — админът я подрежда от „Зони“.</div>}
        </>
      )}
    </PanZoom>
  );
}
