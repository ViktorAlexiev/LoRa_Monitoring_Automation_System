import { Link } from "react-router-dom";
import DewPoint from "./DewPoint.jsx";
import Gauge from "./Gauge.jsx";

const MODE_LABEL = { manual: "Ръчен", clock: "По време", threshold: "По прагове" };
const MODE_CLASS = { manual: "pill-manual", clock: "pill-clock", threshold: "pill-threshold" };

export default function ZoneCard({ zone }) {
  const r = zone.readings;
  return (
    <Link to={`/zones/${zone.id}`} className="zone-card">
      {!zone.is_active && <div className="alert-strip">&#9679; Зоната е неактивна</div>}
      {zone.open_error_count > 0 && (
        <div className={`alert-strip sev-${zone.worst_open_severity || "warning"}`}>
          {zone.worst_open_severity === "warning" ? "⚠" : "✖"} {zone.open_error_count} нерешен(и) проблем(и)
        </div>
      )}
      <div className="zone-card-head">
        <div>
          <h3>{zone.name}</h3>
          <div className="zone-sub">{zone.description}</div>
        </div>
        <span className={`pill ${MODE_CLASS[zone.regime]}`}>{MODE_LABEL[zone.regime]}</span>
      </div>
      <div className="gauge-row">
        <Gauge label="Почва T°" value={r.soil_t} unit="°" />
        <Gauge label="Почва RH" value={r.soil_h} unit="%"
               warnBelow={zone.humidity_warn_min} warnAbove={zone.humidity_warn_max} />
        <Gauge label="Въздух T°" value={r.air_t} unit="°" />
        <Gauge label="Въздух RH" value={r.air_h} unit="%" />
      </div>
      <DewPoint readings={r} />
      <div className="status-row">
        {zone.modules.map((m) => (
          <span key={`${m.kind}-${m.id}`} className="chip">
            <span className={`dot ${m.state === "on" ? "dot-on" : "dot-off"}`}></span>
            {m.kind === "pump" ? "Помпа " : ""}{m.id}
          </span>
        ))}
        {zone.modules.length === 0 && <span className="muted">Няма клапани в зоната</span>}
      </div>
    </Link>
  );
}
