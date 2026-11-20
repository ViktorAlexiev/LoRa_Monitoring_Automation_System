import { useEffect, useState } from "react";
import { api } from "../api.js";
import Icon from "./Icon.jsx";
import BatteryModal from "./BatteryModal.jsx";
import ListSearch, { matches } from "./ListSearch.jsx";

const KIND_LABEL = { sensor: "Сензор", executor: "Модул за управление", repeater: "Повторител" };
// what kind of module a card is: normal sensor, external sensor, executor, repeater
const typeOf = (i) => (i.kind === "sensor" ? (i.external ? "external" : "sensor") : i.kind);
const TYPE_OPTIONS = [["", "Всички модули"], ["sensor", "Сензори"], ["external", "Външни сензори"], ["executor", "Модули за управление"], ["repeater", "Повторители"]];
const LEVEL_OPTIONS = [["", "Всички състояния"], ["critical", "Почти изтощени"], ["low", "Ниски"], ["nodata", "Без данни"], ["ok", "Добри"]];
const LEVEL_TEXT = { ok: "Добра", low: "Ниска", critical: "Почти изтощена", nodata: "Няма данни" };

// Dashboard tab "Батерии": every device with a battery, lowest first.
export default function BatteryPanel() {
  const [items, setItems] = useState(null);
  const [error, setError] = useState(null);
  const [open, setOpen] = useState(null);
  const [query, setQuery] = useState("");
  const [type, setType] = useState("");
  const [level, setLevel] = useState("");

  useEffect(() => {
    let alive = true;
    const load = () => api.battery.list().then((l) => { if (alive) { setItems(l); setError(null); } }).catch((e) => alive && setError(e.message));
    load();
    const t = setInterval(load, 30000);
    return () => { alive = false; clearInterval(t); };
  }, []);

  if (error) return <div className="error-note">{error}</div>;
  if (items === null) return <p className="muted">Зареждане…</p>;
  if (items.length === 0) {
    return <p className="muted">Още няма устройства с батерия. Админът отбелязва „Има батерия“ във „Физически модули“.</p>;
  }
  const count = (lvl) => items.filter((i) => i.level === lvl).length;
  const shown = items.filter((i) => (!type || typeOf(i) === type) && (!level || i.level === level) && matches(query, i.name, i.id, ...i.zones));
  return (
    <div>
      <div className="bat-summary">
        {[["critical", "Почти изтощени"], ["low", "Ниски"], ["nodata", "Без данни"], ["ok", "Добри"]].map(([lvl, label]) => (
          <div key={lvl} className={`bat-sum bat-${lvl}`}><span className="val">{count(lvl)}</span><span className="lbl">{label}</span></div>
        ))}
      </div>
      <div className="list-filters">
        <ListSearch value={query} onChange={setQuery} placeholder="Търси модул по име, ID или обект…" />
        <select value={type} onChange={(e) => setType(e.target.value)} aria-label="Вид модул">
          {TYPE_OPTIONS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
        </select>
        <select value={level} onChange={(e) => setLevel(e.target.value)} aria-label="Състояние на батерията">
          {LEVEL_OPTIONS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
        </select>
      </div>
      {shown.length === 0 && <p className="muted">Няма модул по тези филтри.</p>}
      <div className="ext-grid">
        {shown.map((i) => (
          <button type="button" key={`${i.kind}-${i.id}`} className="tile bat-card" onClick={() => setOpen(i)}>
            <div className="tile-head">
              <span className="tile-name">{i.name || i.id}</span>
              <span className="id-tag mono">{i.id}</span>
            </div>
            <div className="muted">{KIND_LABEL[i.kind]}{i.zones.length ? ` · ${i.zones.join(", ")}` : ""}</div>
            <div className="bat-big">
              <span className={`bat-icon bat-${i.level}`}><Icon name="battery" size={26} /></span>
              <span className="val">{i.percent != null ? `${i.percent}%` : "—"}</span>
              <span className="muted">{i.mv != null ? `${(i.mv / 1000).toFixed(2)} V` : ""}</span>
            </div>
            <div className="bat-bar"><div className={`bat-fill bat-${i.level}`} style={{ width: `${i.percent ?? 0}%` }} /></div>
            <div className={`bat-status bat-${i.level}`}>
              {LEVEL_TEXT[i.level]}{i.age_minutes != null && <span className="muted"> · преди {i.age_minutes} мин</span>}
            </div>
          </button>
        ))}
      </div>
      {open && <BatteryModal item={open} onClose={() => setOpen(null)} />}
    </div>
  );
}
