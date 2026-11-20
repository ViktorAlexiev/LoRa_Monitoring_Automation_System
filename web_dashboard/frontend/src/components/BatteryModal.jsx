import { useEffect, useState } from "react";
import { api } from "../api.js";
import Modal from "./Modal.jsx";
import SensorChart from "./SensorChart.jsx";
import BatteryChip from "./BatteryChip.jsx";
import { bucketSeries } from "../utils/series.js";

const KIND_LABEL = { sensor: "Сензор", executor: "Модул за управление", repeater: "Повторител" };
const RANGES = { "24h": { label: "24 часа", hours: 24, buckets: 48 }, "7d": { label: "7 дни", hours: 168, buckets: 56 }, "30d": { label: "30 дни", hours: 720, buckets: 60 } };

// History of one device's battery voltage.
export default function BatteryModal({ item, onClose }) {
  const [range, setRange] = useState("7d");
  const [points, setPoints] = useState([]);
  const [end, setEnd] = useState(() => Date.now());
  const r = RANGES[range];

  useEffect(() => {
    let alive = true;
    api.battery.history(item.kind, item.id, r.hours)
      .then((rows) => { if (alive) { setPoints(rows.map((p) => ({ v: p.mv / 1000, recorded_at: p.recorded_at }))); setEnd(Date.now()); } })
      .catch(() => {});
    return () => { alive = false; };
  }, [item.kind, item.id, r.hours]);

  return (
    <Modal title={`Батерия — ${item.name || item.id} (${item.id})`} onClose={onClose} width="720px">
      <p className="muted">{KIND_LABEL[item.kind]}{item.zones.length ? ` · ${item.zones.join(", ")}` : ""}</p>
      <p><BatteryChip info={item} />{item.age_minutes != null && <span className="muted"> · последно отчитане преди {item.age_minutes} мин</span>}</p>
      <div className="period-switch" role="group" aria-label="Период">
        {Object.entries(RANGES).map(([key, x]) => (
          <button key={key} className={`btn btn-sm ${range === key ? "btn-primary" : ""}`} aria-pressed={range === key} onClick={() => setRange(key)}>{x.label}</button>
        ))}
      </div>
      <div className="tile" style={{ marginTop: 12 }}>
        <SensorChart label="Напрежение на батерията" unit=" V" height={220} minSpan={0.2} color="#0b5cad"
                     points={bucketSeries(points, "v", end - r.hours * 3600000, end, r.buckets)} />
      </div>
      <div className="form-actions"><button className="btn btn-primary" onClick={onClose}>Затвори</button></div>
    </Modal>
  );
}
