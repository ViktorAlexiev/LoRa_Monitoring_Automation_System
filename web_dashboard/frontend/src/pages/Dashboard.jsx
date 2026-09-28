import { useEffect, useState } from "react";
import { api } from "../api.js";
import ZoneCard from "../components/ZoneCard.jsx";
import ErrorList from "../components/ErrorList.jsx";
import SiteMap from "../components/SiteMap.jsx";

const remember = (key, fallback) => {
  try { return localStorage.getItem(key) || fallback; } catch (e) { return fallback; }
};
const store = (key, value) => {
  try { localStorage.setItem(key, value); } catch (e) { /* private mode - just don't remember */ }
};

export default function Dashboard() {
  const [zones, setZones] = useState([]);
  const [siteMap, setSiteMap] = useState({ zones: [], objects: [] });
  const [networkErrors, setNetworkErrors] = useState([]);
  const [error, setError] = useState(null);
  const [refreshSeconds, setRefreshSeconds] = useState(15);
  const [view, setView] = useState(() => remember("dashView", "list")); // "list" | "map"
  const [showLocked, setShowLocked] = useState(false);

  async function load() {
    try {
      const [z, ne, m] = await Promise.all([api.zones.list(), api.errors.network(), api.site.map().catch(() => null)]);
      setZones(z);
      setNetworkErrors(ne);
      if (m) setSiteMap(m);
      setError(null);
    } catch (e) {
      setError(e.message);
    }
  }

  useEffect(() => {
    api.config.get().then((c) => setRefreshSeconds(c.refresh_intervals.dashboard_seconds)).catch(() => {});
  }, []);

  useEffect(() => {
    load();
    const t = setInterval(load, refreshSeconds * 1000);
    return () => clearInterval(t);
  }, [refreshSeconds]);

  function chooseView(v) {
    setView(v);
    store("dashView", v);
  }

  // The zones you may open come first; the rest are greyed out and only
  // appear after "Покажи".
  // (the map always shows every zone - locked ones grey and inert - so the site picture is complete)
  const mine = zones.filter((z) => z.accessible);
  const locked = zones.filter((z) => !z.accessible);

  return (
    <main className="view">
      <div className="page-head">
        <h1>Общ преглед</h1>
        <p>{mine.length} зони · кликни зона за детайли и управление · обновява се на всеки {refreshSeconds} сек</p>
      </div>
      {error && <div className="error-note">Няма връзка с API-то: {error}</div>}
      <ErrorList errors={networkErrors} />

      <div className="period-switch" role="group" aria-label="Изглед">
        <button className={`btn btn-sm ${view === "list" ? "btn-primary" : ""}`} aria-pressed={view === "list"} onClick={() => chooseView("list")}>Списък</button>
        <button className={`btn btn-sm ${view === "map" ? "btn-primary" : ""}`} aria-pressed={view === "map"} onClick={() => chooseView("map")}>Карта на обекта</button>
      </div>

      {view === "list" ? (
        <div className="zone-grid">
          {mine.map((z) => <ZoneCard key={z.id} zone={z} />)}
          {mine.length === 0 && <p className="muted">Още нямаш достъп до нито една зона.</p>}
        </div>
      ) : (
        <SiteMap zones={zones} map={siteMap} showLocked />
      )}

      {locked.length > 0 && view === "list" && (
        <section className="locked-zones">
          <div className="locked-head">
            <span className="muted">Други зони на обекта ({locked.length}) — нямаш достъп до тях</span>
            <button className="btn btn-sm" onClick={() => setShowLocked((v) => !v)} aria-expanded={showLocked}>
              {showLocked ? "Скрий" : "Покажи"}
            </button>
          </div>
          {showLocked && (
            <div className="zone-grid">
              {locked.map((z) => (
                <div key={z.id} className="zone-card zone-card-locked" aria-disabled="true">
                  <h3>{z.name}</h3>
                  <div className="zone-sub">Нямаш достъп до тази зона</div>
                </div>
              ))}
            </div>
          )}
        </section>
      )}
    </main>
  );
}
