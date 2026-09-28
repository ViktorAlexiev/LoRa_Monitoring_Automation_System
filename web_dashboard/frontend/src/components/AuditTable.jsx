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
  executor_restart: "Рестартиран модул",
  zone_create: "Нова зона",
  zone_delete: "Изтрита зона",
  sensor_assign: "Добавен сензор",
  sensor_unassign: "Махнат сензор",
  valve_assign: "Добавен клапан",
  valve_unassign: "Махнат клапан",
  transition_continue: "Продължен преход",
  transition_deactivate: "Прекъснат преход",
  site_map_change: "Карта на обекта",
};

// Ready-made filters: which action codes each one means.
const GROUPS = {
  important: {
    label: "Важни неща",
    actions: ["emergency_stop", "executor_restart", "regime_change", "zone_activate", "zone_deactivate",
      "transition_continue", "transition_deactivate", "limits_change"],
  },
  manual: { label: "Ръчно управление (клапани, помпи)", actions: ["valve_command", "pump_command"] },
  settings: {
    label: "Настройки (режим, интервали, прагове, граници)",
    actions: ["regime_change", "limits_change", "schedule_create", "schedule_update", "schedule_delete",
      "threshold_set", "threshold_delete", "site_map_change"],
  },
  structure: {
    label: "Устройства и зони (добавяне, махане)",
    actions: ["zone_create", "zone_delete", "sensor_assign", "sensor_unassign", "valve_assign", "valve_unassign"],
  },
  system: { label: "Системни събития (рестарти)", actions: ["executor_restart"] },
};

const fmt = (iso) =>
  new Date(iso).toLocaleString("bg-BG", { day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit" });

// The history of human actions. With zoneId: that zone only (agronomists and
// admins); without: everything (admin only, shows the zone column).
export default function AuditTable({ zoneId }) {
  const [rows, setRows] = useState([]);
  const [error, setError] = useState(null);
  const [more, setMore] = useState(false);
  const [what, setWhat] = useState(""); // "", "g:<group>" or "a:<action>"
  const [who, setWho] = useState("");
  const [text, setText] = useState("");
  const [textNow, setTextNow] = useState(""); // debounced
  const [whoNow, setWhoNow] = useState("");
  const PAGE = 100;

  const actions = what.startsWith("g:") ? GROUPS[what.slice(2)].actions : what.startsWith("a:") ? [what.slice(2)] : [];
  const filtered = what || whoNow || textNow;

  // wait for a pause in typing before asking the server
  useEffect(() => {
    const t = setTimeout(() => { setTextNow(text.trim()); setWhoNow(who.trim()); }, 350);
    return () => clearTimeout(t);
  }, [text, who]);

  async function load(before) {
    try {
      const data = await api.audit.list({ zoneId, limit: PAGE, beforeId: before, actions, who: whoNow, q: textNow });
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
  }, [zoneId, what, whoNow, textNow]);

  if (error) return <div className="error-note">{error}</div>;
  return (
    <div className="table-card">
      <div className="audit-filters">
        <label>Какво
          <select value={what} onChange={(e) => setWhat(e.target.value)}>
            <option value="">Всички действия</option>
            <optgroup label="Групи">
              {Object.entries(GROUPS).map(([key, g]) => <option key={key} value={`g:${key}`}>{g.label}</option>)}
            </optgroup>
            <optgroup label="Конкретно действие">
              {Object.entries(ACTION_LABEL).map(([code, label]) => <option key={code} value={`a:${code}`}>{label}</option>)}
            </optgroup>
          </select>
        </label>
        <label>Кой
          <input value={who} onChange={(e) => setWho(e.target.value)} placeholder="име или „Системно“" />
        </label>
        <label>Търси в подробностите
          <input value={text} onChange={(e) => setText(e.target.value)} placeholder="напр. V01 или Домати" />
        </label>
        {filtered && (
          <button className="btn btn-sm" onClick={() => { setWhat(""); setWho(""); setText(""); }}>Изчисти филтрите</button>
        )}
      </div>
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
              <tr><td colSpan={zoneId ? 4 : 5} className="muted">{filtered ? "Няма записи по тези филтри." : "Още няма записани действия."}</td></tr>
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
