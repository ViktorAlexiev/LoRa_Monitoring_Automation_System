import { useEffect, useState } from "react";
import { Link, useParams } from "react-router-dom";
import { api } from "../api.js";
import ErrorList from "../components/ErrorList.jsx";
import ZoneSettingsModal from "../components/ZoneSettingsModal.jsx";
import ConfirmDialog from "../components/ConfirmDialog.jsx";
import Modal from "../components/Modal.jsx";
import SensorChart from "../components/SensorChart.jsx";
import SensorMap from "../components/SensorMap.jsx";
import { sortByMap, gridPlacement } from "../utils/mapObjects.js";
import DewPoint from "../components/DewPoint.jsx";
import Gauge from "../components/Gauge.jsx";
import { nextTransition, fmtTransition } from "../utils/schedule.js";
import { useAuth, zoneAccessLevel } from "../AuthContext.jsx";

const MODE_LABEL = { manual: "Ръчен", clock: "По време", threshold: "По прагове" };
const MODE_CLASS = { manual: "pill-manual", clock: "pill-clock", threshold: "pill-threshold" };
const TABS = [
  { key: "overview", label: "Преглед" },
  { key: "sensors", label: "Сензори" },
  { key: "control", label: "Управление" },
];

const SENSOR_FAULT_VALUE = 255.0; // manual 2.1: "invalid this cycle" marker - never a real value, excluded everywhere

// Arithmetic mean, but a value that's a leave-one-out outlier against the
// rest of the group is excluded first - same test as the backend's
// _robust_mean (routers/zones.py) and health_checker.py's SENSOR_OUTLIER
// check: each value's deviation from the mean/stddev of the OTHER values,
// not a population stat that includes itself. A single glitching sensor
// can't drag the bucket's value off; when everything agrees (the normal
// case) this is just a plain mean, so real variation between sensors isn't
// smoothed away the way a straight median would. Needs >=3 values to judge
// anything - falls back to a plain mean below that.
function robustMean(values, zThresh = 2.0) {
  if (values.length < 3) return values.reduce((a, b) => a + b, 0) / values.length;
  const keep = values.filter((v, i) => {
    const others = values.filter((_, j) => j !== i);
    const meanOther = others.reduce((a, b) => a + b, 0) / others.length;
    const varOther = others.reduce((a, b) => a + (b - meanOther) ** 2, 0) / others.length;
    const stdOther = Math.sqrt(varOther);
    return stdOther === 0 ? Math.abs(v - meanOther) <= 0.01 : Math.abs(v - meanOther) <= zThresh * stdOther;
  });
  if (keep.length === 0) return values.reduce((a, b) => a + b, 0) / values.length;
  return keep.reduce((a, b) => a + b, 0) / keep.length;
}

// Averages readings from ALL of a zone's sensors into one time series for a
// given field. Sensors report on their own independent schedules, so this
// can't just zip together each sensor's i-th reading (that pairs up
// unrelated moments in time) - instead it bins every (timestamp, value)
// pair from every sensor into `bucketCount` equal-width time windows
// spanning the earliest to latest reading present, and takes the robust
// mean (see above) of whatever fell into each window. Empty windows are
// left as gaps (null) rather than interpolated, since we have no real data
// for a period nothing reported for.
function buildAverageSeries(sensorList, readingsMap, attr, tStart, tEnd, bucketCount) {
  const span = tEnd - tStart || 1;
  const buckets = Array.from({ length: bucketCount }, () => []);
  let any = false;
  for (const s of sensorList) {
    for (const r of readingsMap[s.id] || []) {
      const v = r[attr];
      if (v === null || v === undefined || v === SENSOR_FAULT_VALUE) continue;
      const t = new Date(r.recorded_at).getTime();
      if (t < tStart || t > tEnd) continue;
      const idx = Math.min(bucketCount - 1, Math.floor(((t - tStart) / span) * bucketCount));
      buckets[idx].push(v);
      any = true;
    }
  }
  if (!any) return [];
  return buckets.map((vals, i) => ({
    value: vals.length > 0 ? robustMean(vals) : null,
    t: new Date(tStart + ((i + 0.5) / bucketCount) * span),
  }));
}

// Chart periods: hours of history and how many time buckets to draw
// (24 h -> 30-min buckets, 7 d -> 3-hour buckets, 30 d -> 12-hour buckets).
const PERIODS = {
  "24h": { label: "24 часа", hours: 24, buckets: 48 },
  "7d": { label: "7 дни", hours: 24 * 7, buckets: 56 },
  "30d": { label: "30 дни", hours: 24 * 30, buckets: 60 },
};

export default function ZoneDetail() {
  const { id } = useParams();
  const zoneId = Number(id);
  const { user } = useAuth();

  const [tab, setTab] = useState("overview");
  const [zone, setZone] = useState(null);
  const [sensors, setSensors] = useState([]);
  const [readings, setReadings] = useState({}); // sensor_id -> [readings]
  const [layout, setLayout] = useState([]); // sensor positions on the site map
  const [mapObjects, setMapObjects] = useState([]); // landmarks drawn on the site map
  const [sensorView, setSensorView] = useState("list"); // "list" (default) | "map"
  const [period, setPeriod] = useState("24h");
  const [chartReadings, setChartReadings] = useState({}); // sensor_id -> readings for the chosen period
  const [chartEnd, setChartEnd] = useState(() => Date.now());
  const [valves, setValves] = useState([]);
  const [allValves, setAllValves] = useState([]);
  const [allZones, setAllZones] = useState([]);
  const [pumps, setPumps] = useState([]);
  const [schedules, setSchedules] = useState([]);
  const [errors, setErrors] = useState([]);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [busyPopup, setBusyPopup] = useState(null); // message string | null — manual-mode block, no DB warning
  const [confirmRegime, setConfirmRegime] = useState(null); // newRegime string
  const [regimeError, setRegimeError] = useState(null);
  const [regimeNote, setRegimeNote] = useState(null); // refusal shown under the mode select
  const [loadError, setLoadError] = useState(null);
  const [refreshSeconds, setRefreshSeconds] = useState(20);

  async function loadAll() {
    try {
      const [z, allSensors, valvesEverywhere, allPumps, sch, errs, zonesEverywhere, lay, objs] = await Promise.all([
        api.zones.get(zoneId),
        api.sensors.list(),
        api.valves.list(),
        api.pumps.list(),
        api.zones.schedules(zoneId),
        api.zones.errors(zoneId),
        api.zones.list(),
        api.zones.layout(zoneId).catch(() => []),
        api.zones.mapObjects(zoneId).catch(() => []),
      ]);
      setLayout(lay);
      setMapObjects(objs);
      setZone(z);
      const zoneSensors = allSensors.filter((s) => s.zone_id === zoneId);
      setSensors(zoneSensors);
      setValves(valvesEverywhere.filter((v) => v.zone_id === zoneId));
      setAllValves(valvesEverywhere);
      setAllZones(zonesEverywhere);
      setPumps(allPumps);
      setSchedules(sch);
      setErrors(errs);
      setLoadError(null);

      const readingEntries = await Promise.all(
        zoneSensors.map(async (s) => [s.id, await api.sensors.readings(s.id)])
      );
      setReadings(Object.fromEntries(readingEntries));
    } catch (err) {
      setLoadError(err.message);
    }
  }

  useEffect(() => {
    api.config.get().then((c) => setRefreshSeconds(c.refresh_intervals.zone_detail_seconds)).catch(() => {});
  }, []);

  // Chart history for the selected period - separate from the 20 s refresh
  // above (30 days of readings is too heavy to re-fetch that often).
  useEffect(() => {
    if (sensors.length === 0) return undefined;
    let cancelled = false;
    async function loadChart() {
      try {
        const entries = await Promise.all(
          sensors.map(async (s) => [s.id, await api.sensors.readingsSince(s.id, PERIODS[period].hours)])
        );
        if (!cancelled) {
          setChartReadings(Object.fromEntries(entries));
          setChartEnd(Date.now());
        }
      } catch (err) { /* keep the previous chart on a failed refresh */ }
    }
    loadChart();
    const t = setInterval(loadChart, 60000);
    return () => { cancelled = true; clearInterval(t); };
  }, [period, sensors.map((s) => s.id).join(",")]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    loadAll();
    const t = setInterval(loadAll, refreshSeconds * 1000);
    return () => clearInterval(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [zoneId, refreshSeconds]);

  async function changeRegime(regime) {
    await api.zones.update(zoneId, { regime });
    loadAll();
  }

  async function doContinueTransition() {
    await api.zones.continueTransition(zoneId);
    loadAll();
  }

  async function doDeactivateTransition() {
    await api.zones.deactivateFromTransition(zoneId);
    loadAll();
  }

  function requestRegimeChange(newRegime) {
    if (newRegime === zone.regime) return;
    setRegimeNote(null);
    if (zone.is_active) {
      setConfirmRegime(newRegime);
      setRegimeError(null);
    } else {
      changeRegime(newRegime).catch((err) => setRegimeNote(err.message));
    }
  }

  async function doConfirmRegimeChange() {
    try {
      await changeRegime(confirmRegime);
      setConfirmRegime(null);
      setRegimeError(null);
    } catch (err) {
      setRegimeError(err.message);
    }
  }

  async function toggleValve(valve, state) {
    try {
      await api.valves.command(valve.id, state);
      loadAll();
    } catch (err) {
      setBusyPopup(err.message);
    }
  }

  async function togglePump(pump, state) {
    try {
      await api.pumps.command(pump.id, state);
      loadAll();
    } catch (err) {
      setBusyPopup(err.message);
    }
  }

  function pumpOccupants(pumpId, excludeValveId) {
    return allValves
      .filter((v) => v.pump_id === pumpId && v.id !== excludeValveId && v.current_state === "on")
      .map((v) => {
        const z = allZones.find((zz) => zz.id === v.zone_id);
        return { id: v.id, name: v.name, zoneName: z?.name ?? "—", zoneRegime: z ? MODE_LABEL[z.regime] : "—" };
      });
  }

  if (!zone) {
    return (
      <main className="view">
        {loadError ? (
          <div className="error-note">{loadError}</div>
        ) : (
          <p className="muted">Зареждане…</p>
        )}
      </main>
    );
  }

  // grid cell of a sensor in the "same arrangement as the map" list
  const gp = gridPlacement(sensors, layout);
  function mapCell(id) {
    const p = gp.place[id];
    return p ? { "--r": p.row, "--c": p.col } : { "--r": gp.rows + 1 }; // not on the map: below the grid
  }

  const zonePumpIds = new Set(valves.map((v) => v.pump_id).filter(Boolean));
  const zonePumps = pumps.filter((p) => zonePumpIds.has(p.id));
  const transitioning = zone.transition_status !== "none";
  const canManage = zoneAccessLevel(user, zoneId) === "control";
  const visibleTabs = TABS.filter((t) => t.key !== "control" || canManage);

  return (
    <main className="view">
      <div className="zone-detail-head">
        <div>
          <h1>{zone.name}</h1>
          <p className="muted">{zone.description}</p>
        </div>
        <span className={`pill ${MODE_CLASS[zone.regime]}`}>{MODE_LABEL[zone.regime]}</span>
      </div>

      {transitioning && (
        <div className={`transition-banner ${zone.transition_status}`}>
          {zone.transition_status === "waiting" ? (
            <div>⏳ Изчаква се изключване на всички консуматори...</div>
          ) : (
            <>
              <div>✖ Не всички консуматори успяха да се изключат — виж грешките по-долу.</div>
              <div>
                <button className="btn btn-sm btn-primary" onClick={doContinueTransition}>Продължи с работещите</button>
                <button className="btn btn-sm" onClick={doDeactivateTransition}>Деактивирай зоната</button>
              </div>
            </>
          )}
        </div>
      )}

      <ErrorList errors={errors} />

      <div className="subtabs">
        {visibleTabs.map((t) => (
          <button key={t.key} className={`subtab ${tab === t.key ? "active" : ""}`} onClick={() => setTab(t.key)}>
            {t.label}
          </button>
        ))}
      </div>

      {tab === "overview" && (
        <div>
          <DewPoint readings={zone.readings} />
          <div className="gauge-row">
            <Gauge label="Почва T°" value={zone.readings?.soil_t} unit="°" />
            <Gauge label="Почва RH" value={zone.readings?.soil_h} unit="%"
                   warnBelow={zone.humidity_warn_min} warnAbove={zone.humidity_warn_max} />
            <Gauge label="Въздух T°" value={zone.readings?.air_t} unit="°" />
            <Gauge label="Въздух RH" value={zone.readings?.air_h} unit="%" />
          </div>
          <div className="period-switch" role="group" aria-label="Период на графиките">
            {Object.entries(PERIODS).map(([key, p]) => (
              <button key={key} className={`btn btn-sm ${period === key ? "btn-primary" : ""}`}
                      aria-pressed={period === key} onClick={() => setPeriod(key)}>{p.label}</button>
            ))}
          </div>
          <div className="overview-charts">
            {[
              ["Почва T° (средно за зоната)", "soil_t", "°", 2, { color: "#8a5a2b" }],
              ["Почва RH (средно за зоната)", "soil_h", "%", 10,
                { color: "#0b5cad", bandMin: zone.humidity_warn_min ?? undefined, bandMax: zone.humidity_warn_max ?? undefined }],
              ["Въздух T° (средно за зоната)", "air_t", "°", 2, { color: "#d9480f" }],
              ["Въздух RH (средно за зоната)", "air_h", "%", 10, { color: "#0b8a8a" }],
            ].map(([label, attr, unit, minSpan, extra]) => (
              <div className="tile" key={attr}>
                <SensorChart label={label} unit={unit} height={220} minSpan={minSpan} {...extra}
                  points={buildAverageSeries(sensors, chartReadings, attr, chartEnd - PERIODS[period].hours * 3600000, chartEnd, PERIODS[period].buckets)} />
              </div>
            ))}
          </div>
          {sensors.length === 0 && <p className="muted">Няма сензори в тая зона.</p>}
        </div>
      )}

      {tab === "sensors" && (
        <div>
          <DewPoint readings={zone.readings} />
          {layout.length > 0 && (
            <div className="period-switch" role="group" aria-label="Изглед на сензорите">
              <button className={`btn btn-sm ${sensorView === "list" ? "btn-primary" : ""}`} aria-pressed={sensorView === "list"}
                      onClick={() => setSensorView("list")}>Списък</button>
              <button className={`btn btn-sm ${sensorView === "map" ? "btn-primary" : ""}`} aria-pressed={sensorView === "map"}
                      onClick={() => setSensorView("map")}>Карта на обекта</button>
            </div>
          )}
          {layout.length > 0 && sensorView === "map" && (
            <SensorMap sensors={sensors} layout={layout} objects={mapObjects} readings={readings} errors={errors} />
          )}
          {layout.length > 0 && sensorView === "map" && sensors.some((s) => !layout.find((l) => l.sensor_id === s.id)) && (
            <div className="section-title">Без място на картата</div>
          )}
          <div className={`tile-grid ${layout.length > 0 ? "map-list" : ""}`} style={layout.length > 0 ? { "--cols": gp.cols } : undefined}>
          {sortByMap(sensors, layout).filter((s) => !(layout.length > 0 && sensorView === "map" && layout.find((l) => l.sensor_id === s.id))).map((s) => {
            const last = (readings[s.id] || []).slice(-1)[0];
            const sensorError = errors.find((e) => e.sensor_id === s.id);
            return (
              <div className={`tile ${sensorError ? `tile-sev-${sensorError.severity}` : ""}`} key={s.id}
                   style={layout.length > 0 ? mapCell(s.id) : undefined}>
                <div className="tile-head">
                  <span className="tile-name">{s.name}</span>
                  <span className="id-tag mono">{s.id}</span>
                </div>
                {sensorError && (
                  <div className={`tile-flag sev-${sensorError.severity}`}>
                    {sensorError.severity === "warning" ? "⚠" : "✖"} {sensorError.description}
                  </div>
                )}
                <div className="tile-readings">
                  <div className="tile-reading"><span className="val">{last?.soil_t?.toFixed(1) ?? "—"}°</span><span className="lbl">Почва T°</span></div>
                  <div className="tile-reading"><span className="val">{last?.soil_h?.toFixed(1) ?? "—"}%</span><span className="lbl">Почва RH</span></div>
                  <div className="tile-reading"><span className="val">{last?.air_t?.toFixed(1) ?? "—"}°</span><span className="lbl">Въздух T°</span></div>
                  <div className="tile-reading"><span className="val">{last?.air_h?.toFixed(1) ?? "—"}%</span><span className="lbl">Въздух RH</span></div>
                </div>
              </div>
            );
          })}
          {sensors.length === 0 && <p className="muted">Няма сензори в тая зона.</p>}
          </div>
        </div>
      )}

      {tab === "control" && canManage && (
        <fieldset disabled={transitioning}>
          <div className="field">
            <label>Режим на управление</label>
            <select value={zone.regime} onChange={(e) => requestRegimeChange(e.target.value)}>
              <option value="manual">Ръчен</option>
              <option value="clock">По време</option>
              <option value="threshold">По прагове</option>
            </select>
            {regimeNote && <div className="error-note">{regimeNote}</div>}
          </div>
          <button className="btn btn-sm" onClick={() => setSettingsOpen(true)}>Настройки на управлението</button>

          <div className="section-title">Клапани</div>
          <div className="tile-grid">
            {valves.map((v) => {
              const on = v.current_state === "on";
              const pending = v.desired_state !== v.current_state;
              const next = zone.regime === "clock" ? nextTransition(schedules, v.id) : null;
              const manualBlocked = zone.regime === "threshold";
              const pump = pumps.find((p) => p.id === v.pump_id);
              const occupants = pump ? pumpOccupants(pump.id, v.id) : [];
              const noSlot = !on && pump && occupants.length >= pump.max_simultaneous_valves;
              return (
                <div className="tile" key={v.id}>
                  <div className="tile-head">
                    <span className="tile-name">{v.name}</span>
                    <span className="id-tag mono">{v.id}</span>
                  </div>
                  <div className="muted">
                    Помпа: {pump ? <span className="mono">{pump.id}</span> : "—"}{pump && ` (${pump.name})`}
                  </div>
                  <span className={`chip`}><span className={`dot ${on ? "dot-on" : "dot-off"}`}></span>{on ? "Отворен" : "Затворен"}</span>
                  {next && <div className="tile-next">{fmtTransition(next)}</div>}
                  {manualBlocked && pending && <div className="tile-waiting">чака слот на помпата…</div>}
                  {noSlot && (
                    <div className="tile-waiting">
                      Няма свободен слот — помпа {pump.id} е заета от: {occupants.map((o) => `${o.id} — ${o.zoneName} (${o.zoneRegime})`).join("; ")}.
                      {" "}Изчакай малко или изключи ръчно някой от тях, за да освободи слот.
                    </div>
                  )}
                  {!manualBlocked && !noSlot && pending && (
                    <div className="tile-waiting">Изчаква потвърждение от устройството…</div>
                  )}
                  {manualBlocked ? (
                    <div className="tile-next">Ръчното управление е забранено в режим по прагове.</div>
                  ) : (
                    <div className="tile-actions">
                      <button className="btn btn-sm" disabled={on || noSlot || pending} onClick={() => toggleValve(v, "on")}>Включи</button>
                      <button className="btn btn-sm" disabled={!on || pending} onClick={() => toggleValve(v, "off")}>Изключи</button>
                    </div>
                  )}
                </div>
              );
            })}
            {valves.length === 0 && <p className="muted">Няма клапани в тая зона.</p>}
          </div>

          <div className="section-title">Помпи</div>
          <div className="tile-grid">
            {zonePumps.map((p) => {
              const on = p.current_state === "on";
              const pending = p.desired_state !== p.current_state;
              return (
                <div className="tile" key={p.id}>
                  <div className="tile-head">
                    <span className="tile-name">{p.name}</span>
                    <span className="id-tag mono">{p.id}</span>
                  </div>
                  <span className="chip"><span className={`dot ${on ? "dot-on" : "dot-off"}`}></span>{on ? "Включена" : "Изключена"}</span>
                  {pending && <div className="tile-waiting">Изчаква потвърждение от устройството…</div>}
                  <div className="tile-actions">
                    <button className="btn btn-sm" disabled={on || pending} onClick={() => togglePump(p, "on")}>Включи</button>
                    <button className="btn btn-sm" disabled={!on || pending} onClick={() => togglePump(p, "off")}>Изключи</button>
                  </div>
                </div>
              );
            })}
            {zonePumps.length === 0 && <p className="muted">Няма помпи, обслужващи тая зона.</p>}
          </div>
        </fieldset>
      )}

      {settingsOpen && <ZoneSettingsModal zone={zone} valves={valves} onClose={() => setSettingsOpen(false)} onSaved={loadAll} />}

      {confirmRegime && (
        <ConfirmDialog
          title="Смяна на режим на активна зона"
          message={`Зоната е активна — всички нейни клапани и помпи ще бъдат изведени в изключено състояние, преди да премине на режим „${MODE_LABEL[confirmRegime]}“.`}
          confirmLabel="Смени режима"
          error={regimeError}
          onConfirm={doConfirmRegimeChange}
          onCancel={() => { setConfirmRegime(null); setRegimeError(null); }}
        />
      )}

      {busyPopup && (
        <Modal title="Помпата е заета" onClose={() => setBusyPopup(null)} width="380px">
          <p>{busyPopup}</p>
          <div>
            <button className="btn btn-primary" onClick={() => setBusyPopup(null)}>ОК</button>
          </div>
        </Modal>
      )}
    </main>
  );
}
