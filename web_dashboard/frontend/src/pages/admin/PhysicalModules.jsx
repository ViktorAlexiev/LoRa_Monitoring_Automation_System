import { useEffect, useState } from "react";
import { api } from "../../api.js";
import Modal from "../../components/Modal.jsx";
import ConfirmDialog from "../../components/ConfirmDialog.jsx";
import { SearchBox, Th } from "../../components/TableControls.jsx";
import { useTable } from "../../utils/useTable.js";

const TABS = [
  { key: "sensors", label: "Сензори" },
  { key: "executors", label: "Изпълнители" },
  { key: "repeaters", label: "Повторители" },
  { key: "valves", label: "Клапани" },
  { key: "pumps", label: "Помпи" },
  { key: "gateway", label: "Gateway" },
];

const TITLE = {
  sensors: ["Нов сензор", "Редакция на сензор"],
  executors: ["Нов изпълнител", "Редакция на изпълнител"],
  repeaters: ["Нов повторител", "Редакция на повторител"],
  valves: ["Нов клапан", "Редакция на клапан"],
  pumps: ["Нова помпа", "Редакция на помпа"],
  gateway: ["Нов gateway", "Редакция на gateway"],
};

function StateChip({ value }) {
  if (value === undefined || value === null) return <span className="muted">—</span>;
  const on = value === "on" || value === true || value === "Активен";
  return (
    <span className="chip">
      <span className={`dot ${on ? "dot-on" : "dot-off"}`}></span>
      {typeof value === "boolean" ? (value ? "Активен" : "Неактивен") : value}
    </span>
  );
}

function fmtTime(iso) {
  if (!iso) return "никога";
  // iso is a real UTC instant now (backend appends "Z") - convert to the
  // viewer's own local time instead of just reformatting the raw string,
  // which used to show the server's UTC clock numbers labeled as if local.
  return new Date(iso).toLocaleString("bg-BG", { day: "2-digit", month: "2-digit", year: "numeric", hour: "2-digit", minute: "2-digit" });
}

export default function PhysicalModules() {
  const [tab, setTab] = useState("sensors");
  const [sensors, setSensors] = useState([]);
  const [executors, setExecutors] = useState([]);
  const [repeaters, setRepeaters] = useState([]);
  const [valves, setValves] = useState([]);
  const [pumps, setPumps] = useState([]);
  const [gateway, setGateway] = useState([]);
  const [formOpen, setFormOpen] = useState(false);
  const [editingId, setEditingId] = useState(null); // null = add mode
  const [form, setForm] = useState({});
  const [error, setError] = useState(null);
  const [confirmDelete, setConfirmDelete] = useState(null); // { kind, id, label }

  const sensorsTable = useTable(sensors, { searchFields: ["id", "name"] });
  const executorsTable = useTable(executors, { searchFields: ["id", "name"] });
  const repeatersTable = useTable(repeaters, { searchFields: ["id", "name"] });
  const valvesTable = useTable(valves, { searchFields: ["id", "name"] });
  const pumpsTable = useTable(pumps, { searchFields: ["id", "name"] });
  const gatewayTable = useTable(gateway, { searchFields: ["id", "name"] });

  async function loadAll() {
    const [s, e, r, v, p, g] = await Promise.all([
      api.sensors.list(),
      api.executors.list(),
      api.repeaters.list(),
      api.valves.list(),
      api.pumps.list(),
      api.gateway.list(),
    ]);
    setSensors(s);
    setExecutors(e);
    setRepeaters(r);
    setValves(v);
    setPumps(p);
    setGateway(g);
  }

  useEffect(() => {
    loadAll();
  }, []);

  useEffect(() => {
    closeForm();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tab]);

  function field(name, value) {
    setForm((f) => ({ ...f, [name]: value }));
  }

  function closeForm() {
    setFormOpen(false);
    setEditingId(null);
    setForm({});
    setError(null);
  }

  function openAdd() {
    setEditingId(null);
    setForm({});
    setError(null);
    setFormOpen(true);
  }

  function openEdit(kind, item) {
    setEditingId(item.id);
    setForm({ ...item });
    setError(null);
    setFormOpen(true);
  }

  async function submit(e) {
    e.preventDefault();
    try {
      if (editingId) {
        if (tab === "sensors") {
          await api.sensors.update(editingId, { name: form.name || "", repeater_id: form.repeater_id || null });
        } else if (tab === "executors") {
          await api.executors.update(editingId, { name: form.name || "" });
        } else if (tab === "repeaters") {
          await api.repeaters.update(editingId, { name: form.name || "" });
        } else if (tab === "valves") {
          await api.valves.update(editingId, {
            name: form.name || "",
            pump_id: form.pump_id || null,
            executor_id: form.executor_id || null,
            opening_time_s: Number(form.opening_time_s ?? 3),
            closing_time_s: Number(form.closing_time_s ?? 3),
          });
        } else if (tab === "pumps") {
          await api.pumps.update(editingId, {
            name: form.name || "",
            executor_id: form.executor_id || null,
            max_simultaneous_valves: Number(form.max_simultaneous_valves || 1),
            startup_time_s: Number(form.startup_time_s ?? 3),
            shutdown_time_s: Number(form.shutdown_time_s ?? 5),
          });
        } else if (tab === "gateway") {
          await api.gateway.update(editingId, { name: form.name || "" });
        }
      } else {
        if (tab === "sensors") {
          await api.sensors.create({ id: form.id, name: form.name || "", repeater_id: form.repeater_id || null });
        } else if (tab === "executors") {
          await api.executors.create({ id: form.id, name: form.name || "" });
        } else if (tab === "repeaters") {
          await api.repeaters.create({ id: form.id, name: form.name || "" });
        } else if (tab === "valves") {
          await api.valves.create({
            id: form.id,
            name: form.name || "",
            pump_id: form.pump_id || null,
            executor_id: form.executor_id || null,
            opening_time_s: Number(form.opening_time_s ?? 3),
            closing_time_s: Number(form.closing_time_s ?? 3),
          });
        } else if (tab === "pumps") {
          await api.pumps.create({
            id: form.id,
            name: form.name || "",
            executor_id: form.executor_id || null,
            max_simultaneous_valves: Number(form.max_simultaneous_valves || 1),
            startup_time_s: Number(form.startup_time_s ?? 3),
            shutdown_time_s: Number(form.shutdown_time_s ?? 5),
          });
        } else if (tab === "gateway") {
          await api.gateway.create({ id: form.id, name: form.name || "" });
        }
      }
      closeForm();
      loadAll();
    } catch (err) {
      setError(err.message);
    }
  }

  function describeDependents(kind, id) {
    const notes = [];
    if (kind === "sensors") {
      const s = sensors.find((x) => x.id === id);
      if (s?.zone_id) notes.push(`Ще бъде премахнат от Зона #${s.zone_id}.`);
    }
    if (kind === "executors") {
      const affectedValves = valves.filter((v) => v.executor_id === id);
      const affectedPumps = pumps.filter((p) => p.executor_id === id);
      const zoned = affectedValves.filter((v) => v.zone_id);
      if (affectedValves.length) {
        notes.push(
          `Ще откачи изпълнителя от ${affectedValves.length} клапан(и)` +
          (zoned.length ? ` — ${zoned.length} от тях ще бъдат премахнати от зоната си (нужен е изпълнител, за да остане клапан в зона).` : ".")
        );
      }
      if (affectedPumps.length) notes.push(`Ще откачи изпълнителя от ${affectedPumps.length} помпа/и.`);
    }
    if (kind === "pumps") {
      const affectedValves = valves.filter((v) => v.pump_id === id);
      const zoned = affectedValves.filter((v) => v.zone_id);
      if (affectedValves.length) {
        notes.push(
          `Ще откачи помпата от ${affectedValves.length} клапан(и)` +
          (zoned.length ? ` — ${zoned.length} от тях ще бъдат премахнати от зоната си (нужна е помпа, за да остане клапан в зона).` : ".")
        );
      }
    }
    if (kind === "valves") {
      const v = valves.find((x) => x.id === id);
      if (v?.zone_id) notes.push(`Ще бъде премахнат от Зона #${v.zone_id} и от всички графици/прагове, в които участва.`);
    }
    if (kind === "repeaters") {
      const r = repeaters.find((x) => x.id === id);
      if (r?.sensor_ids?.length) notes.push(`${r.sensor_ids.length} сензор(а) ще останат без свързан повторител.`);
    }
    return notes.join(" ");
  }

  function askDelete(kind, id, label) {
    setConfirmDelete({ kind, id, label, extra: describeDependents(kind, id) });
  }

  async function doDelete() {
    const { kind, id } = confirmDelete;
    if (kind === "sensors") await api.sensors.remove(id);
    if (kind === "executors") await api.executors.remove(id);
    if (kind === "repeaters") await api.repeaters.remove(id);
    if (kind === "valves") await api.valves.remove(id);
    if (kind === "pumps") await api.pumps.remove(id);
    if (kind === "gateway") await api.gateway.remove(id);
    setConfirmDelete(null);
    loadAll();
  }

  return (
    <section>
      <div className="admin-head">
        <div>
          <h1>Физически модули</h1>
          <p>Хардуерни устройства и връзките между тях. Зоновата принадлежност се задава в раздел „Зони“.</p>
        </div>
      </div>

      <div className="subtabs">
        {TABS.map((t) => (
          <button key={t.key} className={`subtab ${tab === t.key ? "active" : ""}`} onClick={() => setTab(t.key)}>
            {t.label}
          </button>
        ))}
      </div>

      {tab === "sensors" && (
        <div className="table-card">
          <div className="table-toolbar">
            <h2>Сензори</h2>
            <div>
              <SearchBox value={sensorsTable.search} onChange={sensorsTable.setSearch} options={sensorsTable.searchOptions} />
              <button className="btn btn-primary btn-sm" onClick={openAdd}>+ Добави сензор</button>
            </div>
          </div>
          <div className="table-wrap">
            <table>
              <thead><tr>
                <Th label="ID" sortKey="id" sort={sensorsTable.sort} onSort={sensorsTable.toggleSort} />
                <Th label="Име" sortKey="name" sort={sensorsTable.sort} onSort={sensorsTable.toggleSort} />
                <Th label="Зона" sortKey="zone_id" sort={sensorsTable.sort} onSort={sensorsTable.toggleSort} />
                <th>Repeater</th>
                <Th label="Статус" sortKey="is_active" sort={sensorsTable.sort} onSort={sensorsTable.toggleSort} />
                <th></th>
              </tr></thead>
              <tbody>
                {sensorsTable.rows.map((s) => (
                  <tr key={s.id}>
                    <td><span className="id-tag mono">{s.id}</span></td>
                    <td>{s.name}</td>
                    <td className="muted">{s.zone_id ? `Зона #${s.zone_id}` : "—"}</td>
                    <td className="muted">{s.repeater_id || "—"}</td>
                    <td><StateChip value={s.is_active} /></td>
                    <td>
                      <button className="btn btn-sm" onClick={() => openEdit("sensors", s)}>Редактирай</button>
                      <button className="btn btn-sm" onClick={() => askDelete("sensors", s.id, s.id)}>Изтрий</button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      {tab === "executors" && (
        <div className="table-card">
          <div className="table-toolbar">
            <h2>Изпълнители</h2>
            <div>
              <SearchBox value={executorsTable.search} onChange={executorsTable.setSearch} options={executorsTable.searchOptions} />
              <button className="btn btn-primary btn-sm" onClick={openAdd}>+ Добави изпълнител</button>
            </div>
          </div>
          <div className="table-wrap">
            <table>
              <thead><tr>
                <Th label="ID" sortKey="id" sort={executorsTable.sort} onSort={executorsTable.toggleSort} />
                <Th label="Описание" sortKey="name" sort={executorsTable.sort} onSort={executorsTable.toggleSort} />
                <Th label="Последен heartbeat" sortKey="last_heartbeat_at" sort={executorsTable.sort} onSort={executorsTable.toggleSort} />
                <Th label="Статус" sortKey="is_active" sort={executorsTable.sort} onSort={executorsTable.toggleSort} />
                <th></th>
              </tr></thead>
              <tbody>
                {executorsTable.rows.map((e) => (
                  <tr key={e.id}>
                    <td><span className="id-tag mono">{e.id}</span></td>
                    <td>{e.name}</td>
                    <td className="muted">{fmtTime(e.last_heartbeat_at)}</td>
                    <td><StateChip value={e.is_active} /></td>
                    <td>
                      <button className="btn btn-sm" onClick={() => openEdit("executors", e)}>Редактирай</button>
                      <button className="btn btn-sm" onClick={() => askDelete("executors", e.id, e.id)}>Изтрий</button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      {tab === "repeaters" && (
        <div className="table-card">
          <div className="table-toolbar">
            <h2>Повторители</h2>
            <div>
              <SearchBox value={repeatersTable.search} onChange={repeatersTable.setSearch} options={repeatersTable.searchOptions} />
              <button className="btn btn-primary btn-sm" onClick={openAdd}>+ Добави повторител</button>
            </div>
          </div>
          <div className="table-wrap">
            <table>
              <thead><tr>
                <Th label="ID" sortKey="id" sort={repeatersTable.sort} onSort={repeatersTable.toggleSort} />
                <Th label="Описание" sortKey="name" sort={repeatersTable.sort} onSort={repeatersTable.toggleSort} />
                <th>Свързани сензори</th>
                <Th label="Статус" sortKey="is_active" sort={repeatersTable.sort} onSort={repeatersTable.toggleSort} />
                <th></th>
              </tr></thead>
              <tbody>
                {repeatersTable.rows.map((r) => (
                  <tr key={r.id}>
                    <td><span className="id-tag mono">{r.id}</span></td>
                    <td>{r.name}</td>
                    <td className="muted">{r.sensor_ids.join(", ") || "—"}</td>
                    <td><StateChip value={r.is_active} /></td>
                    <td>
                      <button className="btn btn-sm" onClick={() => openEdit("repeaters", r)}>Редактирай</button>
                      <button className="btn btn-sm" onClick={() => askDelete("repeaters", r.id, r.id)}>Изтрий</button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      {tab === "valves" && (
        <div className="table-card">
          <div className="table-toolbar">
            <h2>Клапани</h2>
            <div>
              <SearchBox value={valvesTable.search} onChange={valvesTable.setSearch} options={valvesTable.searchOptions} />
              <button className="btn btn-primary btn-sm" onClick={openAdd}>+ Добави клапан</button>
            </div>
          </div>
          <div className="table-wrap">
            <table>
              <thead><tr>
                <Th label="ID" sortKey="id" sort={valvesTable.sort} onSort={valvesTable.toggleSort} />
                <Th label="Име" sortKey="name" sort={valvesTable.sort} onSort={valvesTable.toggleSort} />
                <Th label="Зона" sortKey="zone_id" sort={valvesTable.sort} onSort={valvesTable.toggleSort} />
                <Th label="Помпа" sortKey="pump_id" sort={valvesTable.sort} onSort={valvesTable.toggleSort} />
                <th>Изпълнител</th>
                <th>Време за отваряне</th>
                <th>Време за затваряне</th>
                <Th label="Състояние" sortKey="current_state" sort={valvesTable.sort} onSort={valvesTable.toggleSort} />
                <th>Последна промяна</th>
                <th></th>
              </tr></thead>
              <tbody>
                {valvesTable.rows.map((v) => {
                  return (
                    <tr key={v.id}>
                      <td><span className="id-tag mono">{v.id}</span></td>
                      <td>{v.name}</td>
                      <td className="muted">{v.zone_id ? `Зона #${v.zone_id}` : "—"}</td>
                      <td className="muted">{v.pump_id || "—"}</td>
                      <td className="muted">{v.executor_id || "—"}</td>
                      <td className="muted mono">{v.opening_time_s}s</td>
                      <td className="muted mono">{v.closing_time_s}s</td>
                      <td><StateChip value={v.current_state === "on" ? "Отворен" : "Затворен"} /></td>
                      <td className="muted mono">{fmtTime(v.current_updated_at)}</td>
                      <td>
                        <button className="btn btn-sm" onClick={() => openEdit("valves", v)}>Редактирай</button>
                        <button className="btn btn-sm" onClick={() => askDelete("valves", v.id, v.name || v.id)}>Изтрий</button>
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        </div>
      )}

      {tab === "pumps" && (
        <div className="table-card">
          <div className="table-toolbar">
            <h2>Помпи</h2>
            <div>
              <SearchBox value={pumpsTable.search} onChange={pumpsTable.setSearch} options={pumpsTable.searchOptions} />
              <button className="btn btn-primary btn-sm" onClick={openAdd}>+ Добави помпа</button>
            </div>
          </div>
          <div className="table-wrap">
            <table>
              <thead><tr>
                <Th label="ID" sortKey="id" sort={pumpsTable.sort} onSort={pumpsTable.toggleSort} />
                <Th label="Описание" sortKey="name" sort={pumpsTable.sort} onSort={pumpsTable.toggleSort} />
                <th>Изпълнител</th>
                <Th label="Макс. клапани" sortKey="max_simultaneous_valves" sort={pumpsTable.sort} onSort={pumpsTable.toggleSort} />
                <th>Време за стартиране</th>
                <th>Време за изключване</th>
                <Th label="Състояние" sortKey="current_state" sort={pumpsTable.sort} onSort={pumpsTable.toggleSort} />
                <th>Последна промяна</th>
                <th></th>
              </tr></thead>
              <tbody>
                {pumpsTable.rows.map((p) => (
                  <tr key={p.id}>
                    <td><span className="id-tag mono">{p.id}</span></td>
                    <td>{p.name}</td>
                    <td className="muted">{p.executor_id || "—"}</td>
                    <td className="muted">{p.max_simultaneous_valves}</td>
                    <td className="muted mono">{p.startup_time_s}s</td>
                    <td className="muted mono">{p.shutdown_time_s}s</td>
                    <td><StateChip value={p.current_state === "on" ? "Включена" : "Изключена"} /></td>
                    <td className="muted mono">{fmtTime(p.current_updated_at)}</td>
                    <td>
                      <button className="btn btn-sm" onClick={() => openEdit("pumps", p)}>Редактирай</button>
                      <button className="btn btn-sm" onClick={() => askDelete("pumps", p.id, p.name || p.id)}>Изтрий</button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      {tab === "gateway" && (
        <div className="table-card">
          <div className="table-toolbar">
            <h2>Gateway</h2>
            <div>
              <SearchBox value={gatewayTable.search} onChange={gatewayTable.setSearch} options={gatewayTable.searchOptions} />
              <button className="btn btn-primary btn-sm" onClick={openAdd}>+ Добави gateway</button>
            </div>
          </div>
          <div className="table-wrap">
            <table>
              <thead><tr>
                <Th label="ID" sortKey="id" sort={gatewayTable.sort} onSort={gatewayTable.toggleSort} />
                <Th label="Описание" sortKey="name" sort={gatewayTable.sort} onSort={gatewayTable.toggleSort} />
                <Th label="Последен heartbeat" sortKey="last_heartbeat_at" sort={gatewayTable.sort} onSort={gatewayTable.toggleSort} />
                <Th label="Статус" sortKey="is_active" sort={gatewayTable.sort} onSort={gatewayTable.toggleSort} />
                <th></th>
              </tr></thead>
              <tbody>
                {gatewayTable.rows.map((g) => (
                  <tr key={g.id}>
                    <td><span className="id-tag mono">{g.id}</span></td>
                    <td>{g.name}</td>
                    <td className="muted">{fmtTime(g.last_heartbeat_at)}</td>
                    <td><StateChip value={g.is_active} /></td>
                    <td>
                      <button className="btn btn-sm" onClick={() => openEdit("gateway", g)}>Редактирай</button>
                      <button className="btn btn-sm" onClick={() => askDelete("gateway", g.id, g.name || g.id)}>Изтрий</button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </div>
      )}

      {formOpen && (
        <Modal title={editingId ? TITLE[tab][1] : TITLE[tab][0]} onClose={closeForm} width="440px">
          <form className="form-grid" onSubmit={submit}>
            <div className="field full">
              <label>ID</label>
              <input
                required autoFocus={!editingId} disabled={!!editingId} value={form.id || ""}
                onChange={(e) => field("id", e.target.value)}
                pattern="[A-Za-z0-9_\-]{1,16}"
                title="Само букви, цифри, - и _ (без интервали и специални знаци)"
              />
            </div>

            {tab !== "gateway" && (
              <div className="field full"><label>{tab === "valves" ? "Име (напр. „Мъглуване капково“)" : "Име / описание"}</label>
                <input required={tab === "valves"} autoFocus={!!editingId} value={form.name || ""} onChange={(e) => field("name", e.target.value)} />
              </div>
            )}
            {tab === "gateway" && (
              <div className="field full"><label>Описание</label><input value={form.name || ""} onChange={(e) => field("name", e.target.value)} /></div>
            )}

            {tab === "sensors" && (
              <div className="field full">
                <label>Свързан repeater (по избор)</label>
                <select value={form.repeater_id || ""} onChange={(e) => field("repeater_id", e.target.value)}>
                  <option value="">— директно към Gateway —</option>
                  {repeaters.map((r) => <option key={r.id} value={r.id}>{r.id} — {r.name}</option>)}
                </select>
              </div>
            )}

            {tab === "valves" && (
              <>
                <div className="field">
                  <label>Зависи от помпа</label>
                  <select value={form.pump_id || ""} onChange={(e) => field("pump_id", e.target.value)}>
                    <option value="">— избери помпа —</option>
                    {pumps.map((p) => <option key={p.id} value={p.id}>{p.id} — {p.name}</option>)}
                  </select>
                </div>
                <div className="field">
                  <label>Управлява се от изпълнител</label>
                  <select value={form.executor_id || ""} onChange={(e) => field("executor_id", e.target.value)}>
                    <option value="">— избери изпълнител —</option>
                    {executors.map((ex) => <option key={ex.id} value={ex.id}>{ex.id} — {ex.name}</option>)}
                  </select>
                </div>
                <div className="field">
                  <label>Време за отваряне, сек</label>
                  <input type="number" min="0" value={form.opening_time_s ?? 3} onChange={(e) => field("opening_time_s", e.target.value)} />
                </div>
                <div className="field">
                  <label>Време за затваряне, сек</label>
                  <input type="number" min="0" value={form.closing_time_s ?? 3} onChange={(e) => field("closing_time_s", e.target.value)} />
                </div>
              </>
            )}

            {tab === "pumps" && (
              <>
                <div className="field full">
                  <label>Управлява се от изпълнител</label>
                  <select value={form.executor_id || ""} onChange={(e) => field("executor_id", e.target.value)}>
                    <option value="">— избери изпълнител —</option>
                    {executors.map((ex) => <option key={ex.id} value={ex.id}>{ex.id} — {ex.name}</option>)}
                  </select>
                </div>
                <div className="field"><label>Макс. едновременни клапани</label><input type="number" min="1" value={form.max_simultaneous_valves || 1} onChange={(e) => field("max_simultaneous_valves", e.target.value)} /></div>
                <div className="field"><label>Време за стартиране, сек</label><input type="number" min="0" value={form.startup_time_s ?? 3} onChange={(e) => field("startup_time_s", e.target.value)} /></div>
                <div className="field full"><label>Време за изключване, сек</label><input type="number" min="0" value={form.shutdown_time_s ?? 5} onChange={(e) => field("shutdown_time_s", e.target.value)} /></div>
              </>
            )}

            {error && <div className="error-note field full">{error}</div>}
            <div className="field full">
              <button className="btn" type="button" onClick={closeForm}>Отказ</button>
              <button className="btn btn-primary" type="submit">Запази</button>
            </div>
          </form>
        </Modal>
      )}

      {confirmDelete && (
        <ConfirmDialog
          title="Изтриване"
          message={`Сигурен ли си, че искаш да изтриеш ${confirmDelete.label}? ${confirmDelete.extra ? confirmDelete.extra + " " : ""}Това е необратимо.`}
          confirmLabel="Изтрий"
          danger
          onConfirm={doDelete}
          onCancel={() => setConfirmDelete(null)}
        />
      )}
    </section>
  );
}
