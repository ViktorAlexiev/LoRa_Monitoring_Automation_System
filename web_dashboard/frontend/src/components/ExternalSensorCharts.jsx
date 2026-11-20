import { useEffect, useState } from "react";
import { api } from "../api.js";
import SensorChart from "./SensorChart.jsx";
import { PERIODS, bucketSeries } from "../utils/series.js";

// Outdoor air temperature and humidity of ONE external (reference) sensor over
// the chosen period. Refreshes once a minute, like the zone charts.
export default function ExternalSensorCharts({ sensorId, periodKey = "24h", height = 200, inside = false }) {
  const [readings, setReadings] = useState([]);
  const [end, setEnd] = useState(() => Date.now());
  const p = PERIODS[periodKey];

  useEffect(() => {
    let cancelled = false;
    async function load() {
      try {
        const r = await api.sensors.readingsSince(sensorId, p.hours);
        if (!cancelled) { setReadings(r); setEnd(Date.now()); }
      } catch (err) { /* keep what is on screen */ }
    }
    load();
    const t = setInterval(load, 60000);
    return () => { cancelled = true; clearInterval(t); };
  }, [sensorId, p.hours]);

  const from = end - p.hours * 3600000;
  // [label, field, unit, minSpan, colour]
  const charts = inside
    ? [["Почва T°", "soil_t", "°", 2, "#8a5a2b"], ["Почва RH", "soil_h", "%", 10, "#0b5cad"],
       ["Въздух T°", "air_t", "°", 2, "#d9480f"], ["Въздух RH", "air_h", "%", 10, "#0b8a8a"]]
    : [["Температура вън", "air_t", "°", 2, "#d9480f"], ["Влажност вън", "air_h", "%", 10, "#0b8a8a"]];
  return (
    <div className="overview-charts">
      {charts.map(([label, attr, unit, minSpan, color]) => (
        <div className="tile" key={attr}>
          <SensorChart label={label} unit={unit} height={height} minSpan={minSpan} color={color}
                       points={bucketSeries(readings, attr, from, end, p.buckets)} />
        </div>
      ))}
    </div>
  );
}
