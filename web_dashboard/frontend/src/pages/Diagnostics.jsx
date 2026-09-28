import { useEffect, useState } from "react";
import { Link } from "react-router-dom";
import { api } from "../api.js";
import { errorTitle } from "../errorLabels.js";
import AuditTable from "../components/AuditTable.jsx";

const SEV_RANK = { critical: 3, error: 2, warning: 1 };
const SEV_LABEL = { critical: "Сериозен", error: "Проблем", warning: "Внимание" };
const fmt = (iso) =>
  new Date(iso).toLocaleString("bg-BG", { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit" });

const deviceOf = (e) =>
  [e.sensor_id && `сензор ${e.sensor_id}`, e.valve_id && `клапан ${e.valve_id}`, e.pump_id && `помпа ${e.pump_id}`,
    e.executor_id && `модул ${e.executor_id}`, e.repeater_id && `усилвател ${e.repeater_id}`].filter(Boolean).join(", ");

function Problem({ e }) {
  return (
    <div className={`error-banner sev-${e.severity}`}>
      <span>{e.severity === "warning" ? "⚠" : "✖"}</span>
      <div>
        <div><b>{errorTitle(e.error_code)}</b> · {SEV_LABEL[e.severity]}</div>
        <div>{e.description}</div>
        <div className="meta">
          Открита {fmt(e.detected_at)}{deviceOf(e) && ` · ${deviceOf(e)}`}
          {e.resolved_at && ` · решена ${fmt(e.resolved_at)}`}
        </div>
      </div>
    </div>
  );
}

// Admin-only: every open problem and warning grouped into "general" (not tied
// to one zone - gateway, modules ...) and per zone, plus what was resolved
// recently and the history of actions.
export default function Diagnostics() {
  const [tab, setTab] = useState("now");
  const [zones, setZones] = useState([]);
  const [open, setOpen] = useState([]);
  const [history, setHistory] = useState([]);
  const [moreHistory, setMoreHistory] = useState(false);
  const [severity, setSeverity] = useState("");
  const [zoneFilter, setZoneFilter] = useState(""); // "" all, "general", or a zone id
  const [error, setError] = useState(null);
  const PAGE = 100;

  async function loadOpen() {
    try {
      const [z, o] = await Promise.all([api.zones.list(), api.errors.open()]);
      setZones(z);
      setOpen(o);
      setError(null);
    } catch (err) {
      setError(err.message);
    }
  }

  async function loadHistory(before) {
    try {
      const rows = await api.errors.history({ limit: PAGE, beforeId: before });
      setHistory((old) => (before ? [...old, ...rows] : rows));
      setMoreHistory(rows.length === PAGE);
    } catch (err) {
      setError(err.message);
    }
  }

  useEffect(() => {
    loadOpen();
    const t = setInterval(loadOpen, 20000);
    return () => clearInterval(t);
  }, []);

  useEffect(() => {
    if (tab === "resolved" && history.length === 0) loadHistory();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tab]);

  const zoneName = (id) => zones.find((z) => z.id === id)?.name || `Зона #${id}`;
  const keep = (e) =>
    (!severity || e.severity === severity) &&
    (!zoneFilter || (zoneFilter === "general" ? e.zone_id == null : e.zone_id === Number(zoneFilter)));
  const worstFirst = (a, b) => SEV_RANK[b.severity] - SEV_RANK[a.severity] || (a.detected_at < b.detected_at ? 1 : -1);

  const shown = open.filter(keep).sort(worstFirst);
  const general = shown.filter((e) => e.zone_id == null);
  const byZone = {};
  shown.filter((e) => e.zone_id != null).forEach((e) => { (byZone[e.zone_id] ||= []).push(e); });
  const count = (sev) => open.filter((e) => e.severity === sev).length;

  return (
    <main className="view">
      <div className="page-head">
        <h1>Диагностика</h1>
        <p>Всички проблеми и предупреждения на системата — общите и тези по зони — и историята им.</p>
      </div>
      {error && <div className="error-note">{error}</div>}

      <div className="diag-summary">
        <span className="chip chip-problem sev-critical"><span className="dot dot-problem"></span>Сериозни: {count("critical")}</span>
        <span className="chip chip-problem sev-error"><span className="dot dot-problem"></span>Проблеми: {count("error")}</span>
        <span className="chip chip-problem sev-warning"><span className="dot dot-problem"></span>Внимание: {count("warning")}</span>
        {open.length === 0 && <span className="chip"><span className="dot dot-ok"></span>Всичко работи</span>}
      </div>

      <div className="subtabs">
        <button className={`subtab ${tab === "now" ? "active" : ""}`} onClick={() => setTab("now")}>Проблеми сега ({open.length})</button>
        <button className={`subtab ${tab === "resolved" ? "active" : ""}`} onClick={() => setTab("resolved")}>Решени проблеми</button>
        <button className={`subtab ${tab === "actions" ? "active" : ""}`} onClick={() => setTab("actions")}>История на действията</button>
      </div>

      {tab === "now" && (
        <>
          <div className="zone-filter">
            <label htmlFor="diag-sev">Степен:</label>
            <select id="diag-sev" value={severity} onChange={(e) => setSeverity(e.target.value)}>
              <option value="">Всички</option>
              <option value="critical">Сериозни</option>
              <option value="error">Проблеми</option>
              <option value="warning">Внимание</option>
            </select>
            <label htmlFor="diag-zone">Къде:</label>
            <select id="diag-zone" value={zoneFilter} onChange={(e) => setZoneFilter(e.target.value)}>
              <option value="">Навсякъде</option>
              <option value="general">Общи проблеми</option>
              {zones.map((z) => <option key={z.id} value={z.id}>{z.name}</option>)}
            </select>
          </div>

          {shown.length === 0 && <p className="muted">Няма проблеми по избраните филтри.</p>}

          {general.length > 0 && (
            <section className="diag-group">
              <h2>Общи проблеми <span className="muted">({general.length}) — не са за една конкретна зона</span></h2>
              {general.map((e) => <Problem key={e.id} e={e} />)}
            </section>
          )}
          {Object.entries(byZone).map(([zid, list]) => (
            <section className="diag-group" key={zid}>
              <h2><Link to={`/zones/${zid}`}>{zoneName(Number(zid))}</Link> <span className="muted">({list.length})</span></h2>
              {list.map((e) => <Problem key={e.id} e={e} />)}
            </section>
          ))}
        </>
      )}

      {tab === "resolved" && (
        <div className="table-card">
          <div className="table-wrap">
            <table>
              <thead><tr><th>Открит</th><th>Решен</th><th>Къде</th><th>Проблем</th><th>Подробности</th></tr></thead>
              <tbody>
                {history.map((e) => (
                  <tr key={e.id}>
                    <td className="muted mono">{fmt(e.detected_at)}</td>
                    <td className="muted mono">{fmt(e.resolved_at)}</td>
                    <td className="muted">{e.zone_id != null ? zoneName(e.zone_id) : "Общ"}</td>
                    <td>{errorTitle(e.error_code)} · {SEV_LABEL[e.severity]}</td>
                    <td>{e.description}{deviceOf(e) && <div className="muted">{deviceOf(e)}</div>}</td>
                  </tr>
                ))}
                {history.length === 0 && <tr><td colSpan={5} className="muted">Още няма решени проблеми.</td></tr>}
              </tbody>
            </table>
          </div>
          {moreHistory && (
            <div className="row-actions">
              <button className="btn btn-sm" onClick={() => loadHistory(history[history.length - 1].id)}>Покажи по-стари</button>
            </div>
          )}
        </div>
      )}

      {tab === "actions" && <AuditTable />}
    </main>
  );
}
