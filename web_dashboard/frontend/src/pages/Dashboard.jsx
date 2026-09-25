import { useEffect, useState } from "react";
import { api } from "../api.js";
import ZoneCard from "../components/ZoneCard.jsx";
import ErrorList from "../components/ErrorList.jsx";

export default function Dashboard() {
  const [zones, setZones] = useState([]);
  const [networkErrors, setNetworkErrors] = useState([]);
  const [error, setError] = useState(null);
  const [refreshSeconds, setRefreshSeconds] = useState(15);

  async function load() {
    try {
      const [z, ne] = await Promise.all([api.zones.list(), api.errors.network()]);
      setZones(z);
      setNetworkErrors(ne);
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

  return (
    <main className="view">
      <div className="page-head">
        <h1>Общ преглед</h1>
        <p>{zones.length} зони · кликни зона за детайли и управление · обновява се на всеки {refreshSeconds} сек</p>
      </div>
      {error && <div className="error-note">Няма връзка с API-то: {error}</div>}
      <ErrorList errors={networkErrors} />
      <div className="zone-grid">
        {zones.map((z) => (
          <ZoneCard key={z.id} zone={z} />
        ))}
      </div>
    </main>
  );
}
