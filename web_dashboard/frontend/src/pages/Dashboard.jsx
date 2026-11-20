import { useEffect, useState } from "react";
import { useLocation } from "react-router-dom";
import { api } from "../api.js";
import ZoneCard from "../components/ZoneCard.jsx";
import ErrorList from "../components/ErrorList.jsx";
import SiteMap from "../components/SiteMap.jsx";
import ExternalPanel from "../components/ExternalPanel.jsx";
import ZonePeekModal from "../components/ZonePeekModal.jsx";
import Icon from "../components/Icon.jsx";
import BatteryPanel from "../components/BatteryPanel.jsx";
import ListSearch, { matches } from "../components/ListSearch.jsx";
import ExternalSensorModal from "../components/ExternalSensorModal.jsx";

const remember = (key, fallback) => {
  try { return localStorage.getItem(key) || fallback; } catch (e) { return fallback; }
};
const store = (key, value) => {
  try { localStorage.setItem(key, value); } catch (e) { /* private mode - just don't remember */ }
};

export default function Dashboard() {
  const [zones, setZones] = useState([]);
  const location = useLocation();
  const [siteMap, setSiteMap] = useState({ zones: [], objects: [], sensors: [] });
  const [extModal, setExtModal] = useState(null); // external sensor whose chart is open
  const [zonePeek, setZonePeek] = useState(null); // zone clicked on the site map
  const [networkErrors, setNetworkErrors] = useState([]);
  const [error, setError] = useState(null);
  const [refreshSeconds, setRefreshSeconds] = useState(15);
  const [view, setView] = useState(() => location.state?.view || remember("dashView", "list")); // "list" | "map" | "external"
  const [showLocked, setShowLocked] = useState(false);
  const [query, setQuery] = useState(""); // search in the zone list

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
  const inQuery = (z) => matches(query, z.name, z.description, ...(z.modules || []).flatMap((m) => [m.id, m.name]));
  const mine = zones.filter((z) => z.accessible && inQuery(z));
  const locked = zones.filter((z) => !z.accessible && matches(query, z.name));

  return (
    <main className="view">
      <div className="page-head">
        <h1>Общ преглед</h1>
        <p>{zones.filter((z) => z.accessible).length} зони · кликни зона за детайли и управление · обновява се на всеки {refreshSeconds} сек</p>
      </div>
      {error && <div className="error-note">Няма връзка с API-то: {error}</div>}
      <ErrorList errors={networkErrors} />

      <div className="folder-tabs" role="tablist" aria-label="Изглед">
        {[["list", "list", "Обекти"], ["map", "map", "Карта на обекта"], ["external", "sun", "Външни сензори"], ["battery", "battery", "Батерии"]].map(([key, icon, label]) => (
          <button key={key} role="tab" aria-selected={view === key} className={`folder-tab ${view === key ? "active" : ""}`} onClick={() => chooseView(key)}>
            <Icon name={icon} size={20} /> {label}
          </button>
        ))}
      </div>
      <div className="folder-panel">

      {view === "list" && <ListSearch value={query} onChange={setQuery} placeholder="Търси обект по име, описание или клапан…" />}
      {view === "list" && (
        <div className="zone-grid">
          {mine.map((z) => <ZoneCard key={z.id} zone={z} />)}
          {mine.length === 0 && <p className="muted">{query.trim() ? "Няма обект по това търсене." : "Още нямаш достъп до нито една зона."}</p>}
        </div>
      )}
      {view === "map" && <SiteMap zones={zones} map={siteMap} showLocked onSelectExternal={setExtModal} onSelectZone={(z) => setZonePeek(z.id)} />}
      {view === "external" && <ExternalPanel />}
      {view === "battery" && <BatteryPanel />}
      {zonePeek != null && zones.find((z) => z.id === zonePeek) && (
        <ZonePeekModal zone={zones.find((z) => z.id === zonePeek)} onClose={() => setZonePeek(null)} />
      )}
      {extModal && (
        <ExternalSensorModal sensor={extModal} onClose={() => setExtModal(null)}
                             onShowAll={() => { setExtModal(null); chooseView("external"); }} />
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
      </div>
    </main>
  );
}
