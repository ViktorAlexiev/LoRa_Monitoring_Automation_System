import { useEffect, useState } from "react";
import { api } from "../api.js";
import Icon from "./Icon.jsx";
import BatteryChip from "./BatteryChip.jsx";
import ListSearch, { matches } from "./ListSearch.jsx";
import ExternalSensorCharts from "./ExternalSensorCharts.jsx";
import { PERIODS } from "../utils/series.js";

// Dashboard tab "Външни": every external (reference) sensor with its live
// outdoor values, the zones it takes part in, and its charts.
export default function ExternalPanel() {
  const [sensors, setSensors] = useState(null);
  const [batteries, setBatteries] = useState({}); // "sensor:SX01" -> latest battery reading
  const [query, setQuery] = useState("");
  const [period, setPeriod] = useState("24h");
  const [error, setError] = useState(null);

  useEffect(() => {
    let alive = true;
    const load = () => api.site.externalSensors()
      .then((l) => { if (alive) { setSensors(l); setError(null); } })
      .catch((e) => alive && setError(e.message));
    const loadBat = () => api.battery.list().then((l) => alive && setBatteries(Object.fromEntries(l.map((b) => [`${b.kind}:${b.id}`, b])))).catch(() => {});
    load();
    loadBat();
    const t = setInterval(() => { load(); loadBat(); }, 20000);
    return () => { alive = false; clearInterval(t); };
  }, []);

  if (error) return <div className="error-note">{error}</div>;
  if (sensors === null) return <p className="muted">Зареждане…</p>;
  if (sensors.length === 0) {
    return <p className="muted">Още няма външни сензори. Админът ги добавя от „Физически модули“ (тип „Външен“).</p>;
  }
  return (
    <div>
      <p className="muted">
        <span className="where-tag where-out">вън на обекта</span>{" "}
        Измерват въздуха вън от растенията. Не са част от зона и не влизат в нейните средни стойности —
        служат за точката на оросяване.
      </p>
      <div className="period-switch" role="group" aria-label="Период на графиките">
        {Object.entries(PERIODS).map(([key, p]) => (
          <button key={key} className={`btn btn-sm ${period === key ? "btn-primary" : ""}`} aria-pressed={period === key}
                  onClick={() => setPeriod(key)}>{p.label}</button>
        ))}
      </div>
      <ListSearch value={query} onChange={setQuery} placeholder="Търси външен сензор по име или ID…" />
      <div className="ext-grid">
      {sensors.filter((s) => matches(query, s.name, s.id)).map((s) => (
        <section className="tile ext-card" key={s.id}>
          <div className="tile-head">
            <span className="tile-name">{s.name || s.id}</span>
            <span className="tile-head-right">
              <BatteryChip compact has={`sensor:${s.id}` in batteries} info={batteries[`sensor:${s.id}`]} />
              <span className="id-tag mono">{s.id}</span>
            </span>
          </div>
          <div className="ext-values">
            <div><span className="val">{s.air_t != null ? `${s.air_t.toFixed(1)}°` : "—"}</span><span className="lbl"><Icon name="thermometer" size={14} /> Температура</span></div>
            <div><span className="val">{s.air_h != null ? `${s.air_h.toFixed(0)}%` : "—"}</span><span className="lbl"><Icon name="droplet" size={14} /> Влажност</span></div>
            <div><span className="val">{s.dew_point != null ? `${s.dew_point.toFixed(1)}°` : "—"}</span><span className="lbl"><Icon name="droplet" size={14} /> Точка на оросяване</span></div>
          </div>
          {s.air_t == null && (
            <div className="error-note">
              {s.age_minutes == null ? "Още няма данни от този сензор." : `Няма пресни данни — последните са отпреди ${s.age_minutes} мин.`}
            </div>
          )}
          <p className="muted">{s.zones.length ? `Участва в: ${s.zones.map((z) => z.name).join(", ")}` : "Не е добавен в нито една зона."}</p>
          <ExternalSensorCharts sensorId={s.id} periodKey={period} />
        </section>
      ))}
      </div>
      {sensors.length > 0 && !sensors.some((s) => matches(query, s.name, s.id)) && <p className="muted">Няма външен сензор по това търсене.</p>}
    </div>
  );
}
