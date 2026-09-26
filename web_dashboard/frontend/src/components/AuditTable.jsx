import { useEffect, useState } from "react";
import { api } from "../api.js";

// Plain-language names of the logged actions.
const ACTION_LABEL = {
  valve_command: "Клапан",
  pump_command: "Помпа",
  regime_change: "Смяна на режим",
  zone_activate: "Включване на зона",
  zone_deactivate: "Изключване на зона",
  limits_change: "Граници на влажност",
  schedule_create: "Нов интервал",
  schedule_update: "Промяна на интервал",
  schedule_delete: "Изтрит интервал",
  threshold_set: "Праг",
  threshold_delete: "Изтрит праг",
  emergency_stop: "Аварийно спиране",
};

const fmt = (iso) =>
  new Date(iso).toLocaleString("bg-BG", { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit" });

// The history of human actions. With zoneId: that zone only (agronomists and
// admins); without: everything (admin only, shows the zone column).
export default function AuditTable({ zoneId }) {
  const [rows, setRows] = useState([]);
  const [error, setError] = useState(null);
  const [more, setMore] = useState(false);
  const PAGE = 100;

  async function load(before) {
    try {
      const data = await api.audit.list({ zoneId, limit: PAGE, beforeId: before });
      setRows((old) => (before ? [...old, ...data] : data));
      setMore(data.length === PAGE);
      setError(null);
    } catch (err) {
      setError(err.message);
    }
  }

  useEffect(() => {
    load();
    const t = setInterval(() => load(), 30000);
    return () => clearInterval(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [zoneId]);

  if (error) return <div className="error-note">{error}</div>;
  return (
    <div className="table-card">
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Кога</th>
              <th>Кой</th>
              <th>Какво</th>
              {!zoneId && <th>Зона</th>}
              <th>Подробности</th>
            </tr>
          </thead>
          <tbody>
            {rows.map((r) => (
              <tr key={r.id} className={r.action === "emergency_stop" ? "audit-emergency" : ""}>
                <td className="muted mono">{fmt(r.at)}</td>
                <td>{r.display_name || r.username || "—"}</td>
                <td>{ACTION_LABEL[r.action] || r.action}</td>
                {!zoneId && <td className="muted">{r.zone_name || "—"}</td>}
                <td>{r.detail}</td>
              </tr>
            ))}
            {rows.length === 0 && (
              <tr><td colSpan={zoneId ? 4 : 5} className="muted">Още няма записани действия.</td></tr>
            )}
          </tbody>
        </table>
      </div>
      {more && (
        <div className="row-actions">
          <button className="btn btn-sm" onClick={() => load(rows[rows.length - 1].id)}>Покажи по-стари</button>
        </div>
      )}
    </div>
  );
}
