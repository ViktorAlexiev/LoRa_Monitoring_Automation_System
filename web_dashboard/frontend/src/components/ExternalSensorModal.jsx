import { useEffect, useState } from "react";
import { api } from "../api.js";
import Modal from "./Modal.jsx";
import Icon from "./Icon.jsx";
import ExternalSensorCharts from "./ExternalSensorCharts.jsx";
import { PERIODS } from "../utils/series.js";

// Click on an external sensor anywhere (maps, zone view): its live outdoor
// values and charts, with a button over to the tab that shows all of them.
export default function ExternalSensorModal({ sensor, onClose, onShowAll }) {
  const [info, setInfo] = useState(null);
  const [period, setPeriod] = useState("24h");

  useEffect(() => {
    let alive = true;
    const load = () => api.site.externalSensors()
      .then((list) => alive && setInfo(list.find((s) => s.id === sensor.id) || null))
      .catch(() => {});
    load();
    const t = setInterval(load, 20000);
    return () => { alive = false; clearInterval(t); };
  }, [sensor.id]);

  const name = info?.name || sensor.name || sensor.id;
  return (
    <Modal title={`Външен сензор — ${name} (${sensor.id})`} onClose={onClose} width="760px">
      <p className="muted">
        <span className="where-tag where-out">вън на обекта</span>{" "}
        Мери въздуха вън от растенията — служи като референция за точката на оросяване, не е част от нито една зона.
      </p>
      <div className="ext-values">
        <div><span className="val">{info?.air_t != null ? `${info.air_t.toFixed(1)}°` : "—"}</span><span className="lbl"><Icon name="thermometer" size={14} /> Температура</span></div>
        <div><span className="val">{info?.air_h != null ? `${info.air_h.toFixed(0)}%` : "—"}</span><span className="lbl"><Icon name="droplet" size={14} /> Влажност</span></div>
        <div><span className="val">{info?.dew_point != null ? `${info.dew_point.toFixed(1)}°` : "—"}</span><span className="lbl"><Icon name="droplet" size={14} /> Точка на оросяване</span></div>
      </div>
      {info && info.age_minutes != null && info.air_t == null && (
        <div className="error-note">Няма пресни данни — последните са отпреди {info.age_minutes} мин.</div>
      )}
      {info && info.zones.length > 0 && (
        <p className="muted">Участва в: {info.zones.map((z) => z.name).join(", ")}</p>
      )}
      <div className="period-switch" role="group" aria-label="Период на графиките">
        {Object.entries(PERIODS).map(([key, p]) => (
          <button key={key} className={`btn btn-sm ${period === key ? "btn-primary" : ""}`} aria-pressed={period === key}
                  onClick={() => setPeriod(key)}>{p.label}</button>
        ))}
      </div>
      <ExternalSensorCharts sensorId={sensor.id} periodKey={period} />
      <div className="form-actions">
        {onShowAll && <button className="btn" onClick={onShowAll}>Всички външни сензори →</button>}
        <button className="btn btn-primary" onClick={onClose}>Затвори</button>
      </div>
    </Modal>
  );
}
