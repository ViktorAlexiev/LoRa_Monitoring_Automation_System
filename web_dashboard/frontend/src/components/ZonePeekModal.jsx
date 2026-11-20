import { Link } from "react-router-dom";
import Modal from "./Modal.jsx";
import DewPoint from "./DewPoint.jsx";
import DataAge from "./DataAge.jsx";
import Gauge from "./Gauge.jsx";

const MODE_LABEL = { manual: "Ръчен", clock: "По време", threshold: "По прагове" };

// Click on a zone of the whole-site map: a quick look (state, readings,
// problems) with a button to open the zone itself.
export default function ZonePeekModal({ zone, onClose }) {
  const r = zone.readings || {};
  return (
    <Modal title={zone.name} onClose={onClose} width="560px">
      {zone.description && <p className="muted">{zone.description}</p>}
      <div className="status-row">
        <span className="chip"><span className={`dot ${zone.is_active ? "dot-on" : "dot-off"}`}></span>{zone.is_active ? "Активна" : "Неактивна"}</span>
        <span className="chip">Режим: {MODE_LABEL[zone.regime] || zone.regime}</span>
        {zone.open_error_count > 0 && (
          <span className={`chip chip-problem sev-${zone.worst_open_severity || "warning"}`}>
            {zone.worst_open_severity === "warning" ? "⚠" : "✖"} {zone.open_error_count} проблем(и)
          </span>
        )}
      </div>
      <div className="section-title">Вътре в зоната</div>
      <div className="gauge-row">
        <Gauge label="Почва T°" value={r.soil_t} unit="°" />
        <Gauge label="Почва RH" value={r.soil_h} unit="%" warnBelow={zone.humidity_warn_min} warnAbove={zone.humidity_warn_max} />
        <Gauge label="Въздух T°" value={r.air_t} unit="°" />
        <Gauge label="Въздух RH" value={r.air_h} unit="%" />
      </div>
      <DewPoint readings={r} />
      <DataAge minutes={r.data_age_minutes} />
      {zone.modules?.length > 0 && (
        <div className="status-row">
          {zone.modules.map((m) => (
            <span key={`${m.kind}-${m.id}`} className="chip">
              <span className={`dot ${m.state === "on" ? "dot-on" : "dot-off"}`}></span>
              {m.kind === "pump" ? "Помпа " : ""}{m.id}
            </span>
          ))}
        </div>
      )}
      <div className="form-actions">
        <button className="btn" onClick={onClose}>Затвори</button>
        <Link className="btn btn-primary" to={`/zones/${zone.id}`}>Отвори зоната →</Link>
      </div>
    </Modal>
  );
}
