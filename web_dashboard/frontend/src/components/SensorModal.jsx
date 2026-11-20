import { useEffect, useState } from "react";
import { api } from "../api.js";
import Modal from "./Modal.jsx";
import Icon from "./Icon.jsx";
import ExternalSensorCharts from "./ExternalSensorCharts.jsx";
import { PERIODS } from "../utils/series.js";

// Click on "Графика" of a normal (inside) sensor: its live values and charts
// for soil and air over 24 h / 7 d / 30 d.
export default function SensorModal({ sensor, onClose }) {
  const [last, setLast] = useState(null);
  const [period, setPeriod] = useState("24h");

  useEffect(() => {
    let alive = true;
    const load = () => api.sensors.readings(sensor.id, 1).then((r) => alive && setLast(r[r.length - 1] || null)).catch(() => {});
    load();
    const t = setInterval(load, 20000);
    return () => { alive = false; clearInterval(t); };
  }, [sensor.id]);

  const num = (v, d = 1, unit = "") => (v != null && v !== -32768 ? `${v.toFixed(d)}${unit}` : "—");
  return (
    <Modal title={`Сензор — ${sensor.name || sensor.id} (${sensor.id})`} onClose={onClose} width="760px">
      <p className="muted"><span className="where-tag where-in">вътре в зоната</span> Мери почвата и въздуха на мястото си.</p>
      <div className="ext-values">
        <div><span className="val">{num(last?.soil_t, 1, "°")}</span><span className="lbl"><Icon name="thermometer" size={14} /> Почва T°</span></div>
        <div><span className="val">{num(last?.soil_h, 1, "%")}</span><span className="lbl"><Icon name="droplet" size={14} /> Почва RH</span></div>
        <div><span className="val">{num(last?.air_t, 1, "°")}</span><span className="lbl"><Icon name="thermometer" size={14} /> Въздух T°</span></div>
        <div><span className="val">{num(last?.air_h, 0, "%")}</span><span className="lbl"><Icon name="droplet" size={14} /> Въздух RH</span></div>
      </div>
      <div className="period-switch" role="group" aria-label="Период на графиките">
        {Object.entries(PERIODS).map(([key, p]) => (
          <button key={key} className={`btn btn-sm ${period === key ? "btn-primary" : ""}`} aria-pressed={period === key}
                  onClick={() => setPeriod(key)}>{p.label}</button>
        ))}
      </div>
      <ExternalSensorCharts sensorId={sensor.id} periodKey={period} inside height={180} />
      <div className="form-actions"><button className="btn btn-primary" onClick={onClose}>Затвори</button></div>
    </Modal>
  );
}
